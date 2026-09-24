#include "wave_optimiser.h"
#include "mass_entity_set.h"
#include "wave_optimiser_signatures.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

// Only needed for SDK::UObject/SDK::FWeakObjectPtr (actor name/class
// diagnostics in OnTick's trace logging) - deliberately not `using namespace
// SDK;` here, since mass_entity_set.h's global FMassEntityHandle would clash
// with SDK::FMassEntityHandle pulled in transitively (same issue noted in
// heat_sampler.cpp). Available on both client and server SDKs - these are
// core UObject reflection accessors, not client-only actor/raycast APIs.
#include "CoreUObject_classes.hpp"

// Also provides UGameplayStatics::GetTimeSeconds, used to stamp
// FCrMassTemperatureFragment::LastUpdateTime when applying wave heat (see
// SetEntityHeat). Reflected UFUNCTION, present in both SDK dumps.
//
// UMassSignalSubsystem::StaticClass()/USubsystemBlueprintLibrary - needed to
// fire the same temperature-change signal the reset-all-heat tool fires (see
// heat_sampler.cpp's PerformResetAllHeat) after this file's direct CurrentHeat
// write during wave replay, so the "Hold E to lower temperature" prompt reacts
// to wave-applied heat too instead of only reacting to the debug reset button.
// Confirmed present in both the Client and Server SDK dumps (MassSignals is a
// gameplay module, not client-visual) - safe to include unconditionally here,
// same as CoreUObject_classes.hpp above.
#include "Engine_classes.hpp"
#include "MassSignals_classes.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>

namespace WaveOptimiser
{
    std::mutex g_statsMutex;

    namespace
    {
        enum class WaveOp : uint8_t { Add, Remove };

        struct QueuedEntity
        {
            FMassEntityHandle handle;
            uint8_t waveType; // only meaningful when op == Add
            void* subsystem;
            WaveOp op;
        };

        typedef void(__fastcall* AddEntitiesToWave_t)(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToAdd, uint8_t waveType);
        typedef void(__fastcall* RemoveEntitiesFromWave_t)(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToRemove);

        AddEntitiesToWave_t       g_original = nullptr;
        RemoveEntitiesFromWave_t  g_originalRemove = nullptr;
        HookHandle                g_hook = nullptr;
        HookHandle                g_hookRemove = nullptr;
        std::deque<QueuedEntity>  g_queue;

        // FMassEntityManager::InternalGetFragmentDataPtr - the generic fragment accessor
        // underlying all GetFragmentDataPtr<T> template instantiations. Resolved via
        // an xref in SynchronizeActorStateToMass. Null if not found.
        // Signature: void*(FMassEntityManager*, FMassEntityHandle, const UScriptStruct*)
        typedef void* (__fastcall* GetFragmentDataPtr_t)(void* entityMgr, FMassEntityHandle handle, const void* fragStruct);
        GetFragmentDataPtr_t g_getFragmentDataPtr = nullptr;
        void*                g_temperatureFragStruct = nullptr; // FCrMassTemperatureFragment UScriptStruct*

        // FMassActorFragment UScriptStruct* - a regular fragment (stock Epic
        // layout: {TWeakObjectPtr<AActor> Actor; pad}) queried through the same
        // g_getFragmentDataPtr accessor. Diagnostic-only: lets OnTick's trace
        // logging resolve a replayed entity handle back to its actor name/class.
        void* g_actorFragStruct = nullptr;

        // FMassEntityManager::IsEntityValid - must be checked before
        // InternalGetFragmentDataPtr on any handle that isn't fresh off
        // AddEntitiesToWave; that function hard-asserts (crashes) on a
        // stale/destroyed entity rather than failing softly.
        typedef bool(__fastcall* IsEntityValid_t)(void* entityMgr, FMassEntityHandle handle);
        IsEntityValid_t g_isEntityValid = nullptr;

        // GetSubsystem<UMassEntitySubsystem>(UWorld*) resolved via AOB + rel32.
        typedef void* (__fastcall* GetSubsystemMassEntity_t)(void* world);
        GetSubsystemMassEntity_t g_getSubsystemFn = nullptr;

        // Cached wave subsystem pointer — stable for the session lifetime.
        // At reset time we read OuterPrivate (+0x20) to get UWorld*, then call
        // g_getSubsystemFn to reach UMassEntitySubsystem* and from there the
        // entity manager at +0x38.
        void* g_cachedWaveSubsystem = nullptr;

        // UMassSignalSubsystem::SignalEntities - see the AOB comment in
        // wave_optimiser_signatures.h. Writing CurrentHeat directly (below) skips
        // whatever signal processor (UCrMassTemperatureSignalProcessor) would
        // normally react to a real temperature change, so this fires the same
        // signal the reset-all-heat tool fires (heat_sampler.cpp), just for the
        // entities this file heated during wave replay. TArrayView<T,int32> is
        // just {T* DataPtr; int32 ArrayNum} in engine memory.
        struct ArrayViewMirror
        {
            const FMassEntityHandle* DataPtr;
            int32_t ArrayNum;
        };
        typedef void(__fastcall* SignalEntities_t)(void* signalSubsystem, SDK::FName signalName, const ArrayViewMirror* entities);
        SignalEntities_t g_signalEntities = nullptr;

        // Cached UMassSignalSubsystem* - resolved lazily the same way as
        // g_cachedEntityMgr (both derive from the same UWorld*).
        void* g_cachedSignalSubsystem = nullptr;

        // UCrBuildingFunctionLibrary::AddTemperatureToEntity - the game's own
        // heat-application primitive. Writes the clamped value *and* pushes
        // AddTag<FCrMassTemperatureUpdatingTag>, which is what lets
        // UCrMassTemperatureProcessor cool the building afterwards. See the AOB
        // comment in wave_optimiser_signatures.h for why a raw fragment write
        // leaves entities permanently overheated.
        typedef void(__fastcall* AddTemperatureToEntity_t)(void* world, FMassEntityHandle handle, float value);
        AddTemperatureToEntity_t g_addTemperatureToEntity = nullptr;

        // Heat a wave applies to each affected building. Originally derived
        // empirically via HeatSampler against vanilla (passthrough) waves; since
        // confirmed to be UCrTemperatureDeveloperSettings::BuildingsHeatCapacity -
        // CrMassTemperatureHelpers::UpdateHeatFromTemperature assigns
        // `CurrentHeat = ±BuildingsHeatCapacity` outright whenever the local
        // temperature exceeds ±100, which is exactly the in-wave case. Since
        // AddTemperatureToEntity clamps to that same cap, a game update that
        // retunes it leaves us landing on the new cap rather than overshooting.
        constexpr float kWaveHeat = 600.0f;

        // Byte offset of FCrMassTemperatureFragment::LastUpdateTime. The fragment
        // is {float CurrentHeat @0x00; FCrThermalHeatModifiers Modifiers @0x08;
        // TArray<FMassEntityHandle> CoresActiveCooling @0x68; float LastUpdateTime
        // @0x78} (sizeof 0x80), confirmed in IDA from the temperature processor's
        // own access - `movss xmm3, dword ptr [rdi+78h]` at 0x147719E65. There is
        // no engine setter for it, hence the direct write.
        constexpr size_t kLastUpdateTimeOffset = 0x78;

        // Interior-only furnishings that must never be heated. Fragment presence
        // alone is NOT the filter: these get a FCrMassTemperatureFragment from
        // the base building trait, but their DA_* configs (all under
        // Content/Chimera/Buildings/Interiors in the FModel export) have no
        // CrMassTemperatureTrait, they can only be placed inside an interior,
        // and vanilla never heats them. Every exterior building config (all 230
        // DA_* assets, incl. machines/drone poles/storages) does carry the
        // trait and should heat. No shared native parent exists (BP_ItemPrinter
        // and BP_Furnace are both CrCrafter), so this is a manual per-BP-class
        // exclusion list, matched with UObject::IsA against the actor from
        // FMassActorFragment - IsA walks the Super chain, so derived variants
        // (BP_CloningBed_Lander_C, BP_PersonalStorage_NonDeconstructible_C, ...)
        // are covered by their base class entry.
        const char* const kHeatExcludedClassNames[] = {
            "BP_PersonalStorage_C",
            "BP_PersonalStorageBig_C",
            "BP_ResearchTerminal_C",
            "BP_SuitWorkshop_C",
            "BP_CloningBed_C",
            "BP_Armory_C",
            "BP_ItemPrinter_C",
            "BP_UpgradeStation_C",
            "BP_Recycler_C",
            "BP_FoodProcessor_C",
        };
        constexpr size_t kHeatExcludedClassCount = sizeof(kHeatExcludedClassNames) / sizeof(kHeatExcludedClassNames[0]);

        // UClass* cache for the list above, resolved lazily via the SDK's
        // FindClassFast (one GObjects scan per class per world) - reset on
        // world teardown since BlueprintGeneratedClasses can be GC'd/reloaded.
        SDK::UClass* g_heatExcludedClasses[kHeatExcludedClassCount] = {};
        bool         g_heatExcludedClassesResolved = false;

        // Cached FMassEntityManager* — derived once from the wave subsystem's outer
        // UWorld and reused each tick for heat injection.
        void* g_cachedEntityMgr = nullptr;

        // Lifetime/live counters backing GetStats() - see header for field meanings.
        size_t   g_currentBurstTotal = 0;
        uint64_t g_totalCaptured = 0;
        uint64_t g_totalProcessed = 0;
        uint64_t g_totalBatchesReplayed = 0;
        uint8_t  g_lastWaveType = 0;

        // Detour for UCrMassEnviroWaveSubsystem::AddEntitiesToWave. Copies the
        // (potentially huge) incoming TSet's entity handles into our own queue
        // and returns immediately instead of running the original synchronous
        // per-entity loop - that loop is what freezes the game on a large save.
        // OnTick() replays the captured handles through the real original
        // function in small batches across many ticks.
        void __fastcall Detour_AddEntitiesToWave(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToAdd, uint8_t waveType)
        {
            if (!entitiesToAdd)
                return;

            if (!g_cachedWaveSubsystem)
                g_cachedWaveSubsystem = thisPtr;

            // Config::IsEnabled() off means "run vanilla" - hand the original,
            // unmodified TSet straight to the real function instead of queueing it,
            // so the real wave is unaffected (useful for A/B-ing our heat values
            // against the game's own). Hooks stay installed either way (see
            // PluginInit) purely so g_cachedWaveSubsystem above and the
            // entity-manager/fragment-ptr resolution in Init() keep happening -
            // that's what HeatSampler needs to keep working while batching is off.
            if (!WaveOptimiserConfig::Config::IsEnabled())
            {
                g_original(thisPtr, entitiesToAdd, waveType);
                return;
            }

            const int numBits = entitiesToAdd->Elements.AllocationFlags.NumBits;
            const uint32_t* bits = entitiesToAdd->Elements.AllocationFlags.SecondaryData
                ? entitiesToAdd->Elements.AllocationFlags.SecondaryData
                : entitiesToAdd->Elements.AllocationFlags.InlineWords;
            const MassEntitySet::TSetElement* elements = entitiesToAdd->Elements.Data.Data;

            std::lock_guard<std::mutex> lock(g_statsMutex);

            // A fresh capture burst starts whenever the queue was empty -
            // used purely to size the debug panel's progress bar.
            if (g_queue.empty())
                g_currentBurstTotal = 0;

            int captured = 0;
            for (int i = 0; i < numBits; ++i)
            {
                if (bits[i / 32] & (1u << (i % 32)))
                {
                    g_queue.push_back({ elements[i].Value, waveType, thisPtr, WaveOp::Add });
                    LOG_TRACE("WaveOptimiser: captured Add entity Index=%d SerialNumber=%d (wave type %u)",
                        elements[i].Value.Index, elements[i].Value.SerialNumber, static_cast<unsigned>(waveType));
                    ++captured;
                }
            }

            g_currentBurstTotal += static_cast<size_t>(captured);
            g_totalCaptured += static_cast<uint64_t>(captured);
            g_lastWaveType = waveType;

            LOG_DEBUG("WaveOptimiser: deferred %d Add entities (wave type %u) - queue depth now %zu",
                captured, static_cast<unsigned>(waveType), g_queue.size());
        }

        // Detour for UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave. Must be
        // hooked alongside AddEntitiesToWave and pushed into the *same* ordered
        // queue: RemoveEntitiesFromWave mutates this->EntitiesInWave (a set
        // difference) and removes the wave-stage tag, which is the same state
        // AddEntitiesToWave establishes. If Remove ran synchronously while
        // matching Adds were still sitting in the queue (e.g. the wave reverses
        // direction mid-drain), entities would get tagged/added to EntitiesInWave
        // *after* they should already have been removed - corrupting wave-stage
        // membership and the heat ramp that depends on it.
        void __fastcall Detour_RemoveEntitiesFromWave(void* thisPtr, const MassEntitySet::ScriptSet* entitiesToRemove)
        {
            if (!entitiesToRemove)
                return;

            // See the matching check in Detour_AddEntitiesToWave - pass through
            // unmodified when the plugin is disabled in config.
            if (!WaveOptimiserConfig::Config::IsEnabled())
            {
                g_originalRemove(thisPtr, entitiesToRemove);
                return;
            }

            const int numBits = entitiesToRemove->Elements.AllocationFlags.NumBits;
            const uint32_t* bits = entitiesToRemove->Elements.AllocationFlags.SecondaryData
                ? entitiesToRemove->Elements.AllocationFlags.SecondaryData
                : entitiesToRemove->Elements.AllocationFlags.InlineWords;
            const MassEntitySet::TSetElement* elements = entitiesToRemove->Elements.Data.Data;

            std::lock_guard<std::mutex> lock(g_statsMutex);

            if (g_queue.empty())
                g_currentBurstTotal = 0;

            int captured = 0;
            for (int i = 0; i < numBits; ++i)
            {
                if (bits[i / 32] & (1u << (i % 32)))
                {
                    g_queue.push_back({ elements[i].Value, 0, thisPtr, WaveOp::Remove });
                    LOG_TRACE("WaveOptimiser: captured Remove entity Index=%d SerialNumber=%d",
                        elements[i].Value.Index, elements[i].Value.SerialNumber);
                    ++captured;
                }
            }

            g_currentBurstTotal += static_cast<size_t>(captured);
            g_totalCaptured += static_cast<uint64_t>(captured);

            LOG_DEBUG("WaveOptimiser: deferred %d Remove entities - queue depth now %zu",
                captured, g_queue.size());
        }
    } // namespace

    void* ResolveEntityManagerFromWorld(void* world)
    {
        if (!g_getSubsystemFn || !world)
            return nullptr;
        void* massEntitySubsystem = g_getSubsystemFn(world);
        if (!massEntitySubsystem)
            return nullptr;
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(massEntitySubsystem) + 0x38);
    }

    bool IsEntityValid(void* entityMgr, FMassEntityHandle handle)
    {
        return g_isEntityValid && entityMgr && g_isEntityValid(entityMgr, handle);
    }

    namespace
    {
        // Reads OuterPrivate (+0x20) off the cached wave subsystem to get UWorld*.
        // Shared by ResolveEntityManager() and the signal-subsystem resolution below.
        void* ResolveWorld()
        {
            if (!g_cachedWaveSubsystem)
                return nullptr;
            return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_cachedWaveSubsystem) + 0x20);
        }

        // Walks the cached subsystem pointer chain to reach FMassEntityManager*.
        // Returns null if any pointer in the chain is missing.
        void* ResolveEntityManager()
        {
            void* world = ResolveWorld();
            return world ? ResolveEntityManagerFromWorld(world) : nullptr;
        }

        // Builds a live FName from an ASCII config string - mirrors
        // HeatSampler::MakeFNameFromAscii (heat_sampler.cpp). Deliberately not
        // cached: the signal name is config-driven and re-read every batch so the
        // user can retune it without rebuilding the plugin.
        SDK::FName MakeFNameFromAscii(const char* ascii)
        {
            wchar_t wbuf[128];
            int i = 0;
            for (; ascii[i] != '\0' && i < 127; ++i)
                wbuf[i] = static_cast<wchar_t>(static_cast<unsigned char>(ascii[i]));
            wbuf[i] = L'\0';
            return SDK::BasicFilesImplUtils::StringToName(wbuf);
        }

        // UMassSignalSubsystem needs no AOB to locate - it's a real UWorldSubsystem
        // reachable through the SDK's own reflected GetWorldSubsystem (same
        // approach as HeatSampler::ResolveSignalSubsystem in heat_sampler.cpp).
        void* ResolveSignalSubsystem(void* world)
        {
            return static_cast<void*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
                reinterpret_cast<SDK::UObject*>(world), SDK::UMassSignalSubsystem::StaticClass()));
        }

        // Notifies UCrMassTemperatureSignalProcessor about entities this file just
        // heated directly via InternalGetFragmentDataPtr (see the Add-branch heat
        // injection in OnTick below) - same signal the reset-all-heat tool fires
        // in heat_sampler.cpp, just for wave-applied heat instead of a manual
        // reset, so the "Hold E to lower temperature" prompt reacts either way.
        void FireTemperatureSignal(const std::vector<FMassEntityHandle>& handles)
        {
            if (!g_signalEntities || handles.empty())
                return;

            if (!g_cachedSignalSubsystem)
            {
                void* world = ResolveWorld();
                if (world)
                    g_cachedSignalSubsystem = ResolveSignalSubsystem(world);
            }

            if (!g_cachedSignalSubsystem)
            {
                LOG_TRACE("WaveOptimiser: heat signal skipped - UMassSignalSubsystem not resolved");
                return;
            }

            ArrayViewMirror view{ handles.data(), static_cast<int32_t>(handles.size()) };
            SDK::FName signalName = MakeFNameFromAscii(WaveOptimiserSignatures::TemperatureSignalName);
            g_signalEntities(g_cachedSignalSubsystem, signalName, &view);
            LOG_TRACE("WaveOptimiser: signalled %zu entities with '%s' after wave heat injection",
                handles.size(), WaveOptimiserSignatures::TemperatureSignalName);
        }

        // SEH guard for the fragment lookup below. The engine hard-asserts (a
        // real SEH exception, not a C++ one) when the entity manager's storage
        // has been torn down ("EntityStorageInterface without initialization" -
        // seen in the wild when a world ended while entities were still queued)
        // or when a handle is stale. Same pattern as heat_sampler.cpp's
        // SafeGetFragmentDataPtr: no unwind-requiring locals in this frame.
        void* GuardedGetFragmentDataPtr(void* entityMgr, FMassEntityHandle handle, const void* fragStruct)
        {
            __try
            {
                return g_getFragmentDataPtr(entityMgr, handle, fragStruct);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        // SEH guard for the engine call itself - AddTemperatureToEntity does its
        // own InternalGetFragmentDataPtr, which hard-asserts on a stale handle
        // rather than returning null. Deferred replay means a handle can die
        // between capture and replay, so this can't be assumed safe. Same
        // no-unwinding-locals constraint as GuardedGetFragmentDataPtr above.
        bool GuardedAddTemperature(void* world, FMassEntityHandle handle, float delta)
        {
            __try
            {
                g_addTemperatureToEntity(world, handle, delta);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Diagnostic-only: resolves a replayed entity handle to its actor's
        // name/class for OnTick's trace logging. FMassActorFragment mirrors the
        // stock Epic layout {TWeakObjectPtr<AActor> Actor; pad} - queried through
        // the same generic g_getFragmentDataPtr accessor used for the temperature
        // fragment, then TWeakObjectPtr::Get() (real SDK code, no extra AOB)
        // resolves the live AActor*. The handles are always fresh off
        // AddEntitiesToWave/RemoveEntitiesFromWave replay, but the *manager*
        // can still be stale (world torn down with entries queued), so the
        // lookup goes through the SEH guard above regardless.
        std::string DescribeEntityActor(FMassEntityHandle handle)
        {
            if (!g_cachedEntityMgr || !g_getFragmentDataPtr || !g_actorFragStruct)
                return "actor=<unresolved>";

            void* fragData = GuardedGetFragmentDataPtr(g_cachedEntityMgr, handle, g_actorFragStruct);
            if (!fragData)
                return "actor=<no FMassActorFragment>";

            const SDK::FWeakObjectPtr* weakActor = static_cast<const SDK::FWeakObjectPtr*>(fragData);
            SDK::UObject* actor = weakActor->Get();
            if (!actor)
                return "actor=<none, no live actor for this entity>";

            char buf[192];
            std::snprintf(buf, sizeof(buf), "actor='%s' class='%s'",
                actor->GetName().c_str(),
                actor->Class ? actor->Class->GetName().c_str() : "<no class>");
            return buf;
        }

        // Resolves the fragment UScriptStruct*s by object name from GObjects.
        // Previously these came from hardcoded client-binary RVAs, which crashed
        // the dedicated server (separate link, different addresses -> the blind
        // indirect call fastfailed with 0xc0000409). A name lookup works on any
        // binary. Unlike calling StaticStruct(), FindObjectFast only sees a
        // struct once the engine has registered it, so null results here are
        // retried from OnTick's Add branch until both resolve.
        void ResolveFragmentStructs()
        {
            if (!g_getFragmentDataPtr)
                return;

            if (!g_temperatureFragStruct)
            {
                g_temperatureFragStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
                    "CrMassTemperatureFragment", SDK::EClassCastFlags::ScriptStruct);
                if (g_temperatureFragStruct)
                    LOG_DEBUG("WaveOptimiser: FCrMassTemperatureFragment UScriptStruct* = 0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_temperatureFragStruct)));
                else
                    LOG_DEBUG("WaveOptimiser: FCrMassTemperatureFragment not in GObjects yet - will retry (heat ops inert until resolved)");
            }

            if (!g_actorFragStruct)
            {
                g_actorFragStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
                    "MassActorFragment", SDK::EClassCastFlags::ScriptStruct);
                if (g_actorFragStruct)
                    LOG_DEBUG("WaveOptimiser: FMassActorFragment UScriptStruct* = 0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_actorFragStruct)));
                else
                    LOG_DEBUG("WaveOptimiser: FMassActorFragment not in GObjects yet - will retry (OnTick trace won't include actor name/class)");
            }
        }

        // Fills g_heatExcludedClasses from kHeatExcludedClassNames. Classes that
        // aren't loaded yet stay null and are retried on the next call (a class
        // can't have live instances before it's loaded, so a null slot can't
        // cause a wrongly-heated entity in the meantime).
        void ResolveHeatExcludedClasses()
        {
            if (g_heatExcludedClassesResolved)
                return;

            // Tracks how many slots were still null after the previous attempt,
            // so the "still unresolved" warning fires when the count changes
            // rather than spamming every replayed batch.
            static size_t s_lastUnresolvedCount = SIZE_MAX;

            size_t unresolved = 0;
            for (size_t i = 0; i < kHeatExcludedClassCount; ++i)
            {
                if (g_heatExcludedClasses[i])
                    continue;
                g_heatExcludedClasses[i] = SDK::UObject::FindClassFast(kHeatExcludedClassNames[i]);
                if (g_heatExcludedClasses[i])
                    LOG_INFO("WaveOptimiser: heat-excluded class '%s' = 0x%llX",
                        kHeatExcludedClassNames[i],
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_heatExcludedClasses[i])));
                else
                    ++unresolved;
            }
            g_heatExcludedClassesResolved = (unresolved == 0);

            if (unresolved != s_lastUnresolvedCount)
            {
                if (g_heatExcludedClassesResolved)
                    LOG_INFO("WaveOptimiser: all %zu heat-excluded classes resolved", kHeatExcludedClassCount);
                else
                {
                    // Not necessarily a problem - a class that was never loaded
                    // (its BP not present in this world/save) has no instances
                    // to exclude - but list the names so a typo'd entry is
                    // visible instead of silently never matching.
                    for (size_t i = 0; i < kHeatExcludedClassCount; ++i)
                        if (!g_heatExcludedClasses[i])
                            LOG_INFO("WaveOptimiser: heat-excluded class '%s' not found in GObjects yet "
                                "(not loaded, or misspelled) - will retry on next wave batch",
                                kHeatExcludedClassNames[i]);
                }
                s_lastUnresolvedCount = unresolved;
            }
        }

        // True if the entity's live actor IsA() any class in the exclusion list
        // (interior-only furnishing - vanilla never heats these). Entities whose
        // actor can't be resolved are treated as heatable (default allow), same
        // as vanilla would.
        bool IsHeatExcludedEntity(FMassEntityHandle handle, const char** matchedName)
        {
            *matchedName = nullptr;

            if (!g_actorFragStruct)
                return false;

            void* fragData = GuardedGetFragmentDataPtr(g_cachedEntityMgr, handle, g_actorFragStruct);
            if (!fragData)
                return false;

            const SDK::FWeakObjectPtr* weakActor = static_cast<const SDK::FWeakObjectPtr*>(fragData);
            SDK::UObject* actor = weakActor->Get();
            if (!actor)
                return false;

            for (size_t i = 0; i < kHeatExcludedClassCount; ++i)
            {
                if (g_heatExcludedClasses[i] && actor->IsA(g_heatExcludedClasses[i]))
                {
                    *matchedName = kHeatExcludedClassNames[i];
                    return true;
                }
            }
            return false;
        }
    }

    bool SetEntityHeat(void* world, FMassEntityHandle handle, float targetHeat, float worldTimeSeconds)
    {
        if (!g_addTemperatureToEntity || !world || !g_getFragmentDataPtr || !g_temperatureFragStruct)
            return false;

        // AddTemperatureToEntity finds the entity manager itself; we only need
        // one to read the current heat for the delta below. Fall back to
        // deriving it from the world so this still works before the first wave
        // has populated g_cachedEntityMgr (the reset-all-heat tool's case).
        void* entityMgr = g_cachedEntityMgr ? g_cachedEntityMgr : ResolveEntityManagerFromWorld(world);
        if (!entityMgr)
            return false;

        // AddTemperatureToEntity adds rather than assigns, so read the current
        // value and pass the difference. It then clamps to
        // BuildingsHeatCapacity, so if the game's own cap is below targetHeat we
        // land on the cap rather than overshooting it - which is what we want.
        float* heat = static_cast<float*>(GuardedGetFragmentDataPtr(entityMgr, handle, g_temperatureFragStruct));
        if (!heat)
            return false;

        if (!GuardedAddTemperature(world, handle, targetHeat - *heat))
            return false;

        // Refresh LastUpdateTime so the cooling processor's first pass charges a
        // normal frame's worth of decay instead of everything since this
        // building last simulated. See the header comment for why this matters.
        // kLastUpdateTimeOffset is a struct field offset, not a module RVA - it's
        // identical on client and server, so it doesn't fall foul of the
        // no-hardcoded-addresses rule.
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(heat) + kLastUpdateTimeOffset) = worldTimeSeconds;
        return true;
    }

    // Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
    uintptr_t g_addrAddEntitiesToWave      = 0;
    uintptr_t g_addrRemoveEntitiesFromWave = 0;

    // Every address this file resolves is a function entry -- two get detours,
    // the rest are called -- so each request declares
    // PLUGIN_SCAN_FUNCTION_START. The loader checks the match against the
    // executable's exception directory, which is what tells "the pattern
    // matched" apart from "the pattern matched the right function".
    static uintptr_t ResolveFunction(IPluginSelf* self, IPluginHookScanner* scanner,
        const char* hookName, const char* pattern)
    {
        PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName = hookName;
        req.pattern  = pattern;
        req.kind     = PLUGIN_SCAN_FUNCTION_START;
        req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

        return scanner->Resolve(self, &req);
    }

    // The same, for a pattern that anchors on a function whose body contains a
    // CALL to the address actually wanted. followRel32At names the byte holding
    // the E8/E9; the loader checks that opcode is really there, decodes the
    // rel32, confirms the target lands inside the image, and then applies the
    // kind check to the TARGET rather than to the anchor. That is the whole of
    // what the hand-rolled decodes below used to do, except that a failure is
    // now a named line in the loader's report instead of a warning in our log.
    static uintptr_t ResolveCalleeOf(IPluginSelf* self, IPluginHookScanner* scanner,
        const char* hookName, const char* pattern, int32_t callOffset)
    {
        PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName      = hookName;
        req.pattern       = pattern;
        req.kind          = PLUGIN_SCAN_FUNCTION_START;
        req.flags         = PLUGIN_SCAN_FLAG_OPTIONAL | PLUGIN_SCAN_FLAG_FOLLOW_REL32;
        req.followRel32At = callOffset;

        return scanner->Resolve(self, &req);
    }

    void ResolvePatterns(IPluginSelf* self, IPluginHookScanner* scanner)
    {
        if (!self || !scanner)
            return;

        // Optional throughout: every miss below degrades one feature and leaves
        // the plugin loaded, which is what the old scan-at-init path did. The
        // loader still lists each miss for the user.
        g_addrAddEntitiesToWave = ResolveFunction(self, scanner,
            "UCrWaveSubsystem::AddEntitiesToWave", WaveOptimiserSignatures::AddEntitiesToWave);
        g_addrRemoveEntitiesFromWave = ResolveFunction(self, scanner,
            "UCrWaveSubsystem::RemoveEntitiesFromWave", WaveOptimiserSignatures::RemoveEntitiesFromWave);


        // Resolve GetSubsystem<UMassEntitySubsystem> for the heat-reset tool. The
        // AOB anchors on an engine function that CALLS it; the loader follows
        // the rel32 at +0x2F and kind-checks what it lands on, where this used
        // to read the displacement itself and trust whatever came back.
        const uintptr_t getSubsysAddr = ResolveCalleeOf(self, scanner,
            "UWorld::GetSubsystem<UMassEntitySubsystem>",
            WaveOptimiserSignatures::GetSubsystemMassEntity_Fn,
            static_cast<int32_t>(WaveOptimiserSignatures::GetSubsystemMassEntity_CallOffset));
        if (getSubsysAddr)
        {
            g_getSubsystemFn = reinterpret_cast<GetSubsystemMassEntity_t>(getSubsysAddr);
            LOG_DEBUG("WaveOptimiser: GetSubsystem<UMassEntitySubsystem> resolved at 0x%llX",
                static_cast<unsigned long long>(getSubsysAddr));
        }
        else
            LOG_WARN("WaveOptimiser: GetSubsystemMassEntity_Fn unresolved - heat reset button will be inert");

        // Resolve UCrBuildingFunctionLibrary::AddTemperatureToEntity. Without it
        // we fall back to writing CurrentHeat directly, which applies heat but
        // never adds FCrMassTemperatureUpdatingTag - so heated buildings stay
        // overheated until something else re-tags them (in practice: the player
        // moving far enough to trigger OnWeatherRegionsChanged). Warn loudly,
        // because the failure mode is a slow-burn gameplay bug, not a crash.
        //
        // Resolved before InternalGetFragmentDataPtr below, which is derived from
        // a call site inside it.
        uintptr_t addTempAddr = ResolveFunction(self, scanner,
            "UCrBuildingFunctionLibrary::AddTemperatureToEntity", WaveOptimiserSignatures::AddTemperatureToEntity);
        if (addTempAddr)
        {
            g_addTemperatureToEntity = reinterpret_cast<AddTemperatureToEntity_t>(addTempAddr);
            LOG_DEBUG("WaveOptimiser: UCrBuildingFunctionLibrary::AddTemperatureToEntity resolved at 0x%llX",
                static_cast<unsigned long long>(addTempAddr));
        }
        else
            LOG_WARN("WaveOptimiser: AddTemperatureToEntity unresolved - falling back to direct CurrentHeat writes. "
                "Heated buildings will NOT cool down on their own (no FCrMassTemperatureUpdatingTag) until the "
                "player moves enough to trigger a weather-region change.");

        // Resolve FMassEntityManager::InternalGetFragmentDataPtr from the call to
        // it inside AddTemperatureToEntity. Non-fatal - heat injection and reset
        // button will be inert if missing. See the long note in
        // wave_optimiser_signatures.h for why this is derived from a call site
        // rather than scanned directly (its two neighbours are byte-identical,
        // and one of them hard-asserts).
        //
        // The 0xE8 opcode check is load-bearing: without it a shifted offset
        // yields a plausible-looking non-null pointer that passes every guard and
        // faults only at call time, which is precisely how this broke before.
        // FOLLOW_REL32 makes that check the loader's, so a shifted offset is a
        // named failure in the report rather than a line in our log that nobody
        // reads until something faults -- and the target gets the FUNCTION_START
        // check on top, which the hand-rolled decode never did.
        //
        // This re-scans the AddTemperatureToEntity pattern rather than working
        // from addTempAddr, because followRel32At is measured from the match.
        // The scan cache makes the second pass free.
        const uintptr_t fragmentDataPtr = ResolveCalleeOf(self, scanner,
            "FMassEntityManager::InternalGetFragmentDataPtr",
            WaveOptimiserSignatures::AddTemperatureToEntity,
            static_cast<int32_t>(WaveOptimiserSignatures::InternalGetFragmentDataPtr_CallOffsetFromAddTemp));
        if (fragmentDataPtr)
        {
            g_getFragmentDataPtr = reinterpret_cast<GetFragmentDataPtr_t>(fragmentDataPtr);
            LOG_DEBUG("WaveOptimiser: InternalGetFragmentDataPtr resolved at 0x%llX",
                static_cast<unsigned long long>(fragmentDataPtr));
        }
        else
            LOG_WARN("WaveOptimiser: InternalGetFragmentDataPtr unresolved - heat injection and reset will be inert");

        uintptr_t isEntityValidAddr = ResolveFunction(self, scanner,
            "FMassEntityManager::IsEntityValid", WaveOptimiserSignatures::IsEntityValid);
        if (isEntityValidAddr)
        {
            g_isEntityValid = reinterpret_cast<IsEntityValid_t>(isEntityValidAddr);
            LOG_DEBUG("WaveOptimiser: FMassEntityManager::IsEntityValid resolved at 0x%llX",
                static_cast<unsigned long long>(isEntityValidAddr));
        }
        else
            LOG_WARN("WaveOptimiser: FMassEntityManager::IsEntityValid unresolved - stale-entity checks will fail closed (skip)");

        // Resolve UMassSignalSubsystem::SignalEntities - lets wave-applied heat
        // notify UCrMassTemperatureSignalProcessor the same way the reset-all-heat
        // tool does (see FireTemperatureSignal above). Non-fatal if missing: heat
        // injection still runs, it just won't refresh the temperature prompt.
        uintptr_t signalEntitiesAddr = ResolveFunction(self, scanner,
            "UMassSignalSubsystem::SignalEntities", WaveOptimiserSignatures::SignalEntities);
        if (signalEntitiesAddr)
        {
            g_signalEntities = reinterpret_cast<SignalEntities_t>(signalEntitiesAddr);
            LOG_DEBUG("WaveOptimiser: UMassSignalSubsystem::SignalEntities resolved at 0x%llX",
                static_cast<unsigned long long>(signalEntitiesAddr));
        }
        else
            LOG_WARN("WaveOptimiser: SignalEntities unresolved - wave heat injection won't notify the temperature signal processor");

    }

    bool Init(IPluginSelf* self)
    {
        const uintptr_t addAddr = g_addrAddEntitiesToWave;
        if (!addAddr)
        {
            LOG_WARN("WaveOptimiser: AddEntitiesToWave unresolved - wave batching not installed");
            return false;
        }

        const uintptr_t removeAddr = g_addrRemoveEntitiesFromWave;
        if (!removeAddr)
        {
            LOG_WARN("WaveOptimiser: RemoveEntitiesFromWave unresolved - wave batching not installed "
                "(refusing to hook Add alone, that would desync EntitiesInWave ordering)");
            return false;
        }

        g_hook = self->hooks->Hooks->Install(
            addAddr,
            reinterpret_cast<void*>(&Detour_AddEntitiesToWave),
            reinterpret_cast<void**>(&g_original));

        if (!g_hook)
        {
            LOG_WARN("WaveOptimiser: AddEntitiesToWave hook installation failed");
            return false;
        }

        g_hookRemove = self->hooks->Hooks->Install(
            removeAddr,
            reinterpret_cast<void*>(&Detour_RemoveEntitiesFromWave),
            reinterpret_cast<void**>(&g_originalRemove));

        if (!g_hookRemove)
        {
            LOG_WARN("WaveOptimiser: RemoveEntitiesFromWave hook installation failed");
            self->hooks->Hooks->Remove(g_hook);
            g_hook = nullptr;
            g_original = nullptr;
            return false;
        }
        

        // Cache UScriptStruct* for the fragment types by name from GObjects.
        // Both structs may not be registered yet this early; ResolveFragmentStructs
        // is retried lazily from OnTick's Add branch until both resolve.
        ResolveFragmentStructs();

        // Resolve the heat-excluded UClass*es up front so their addresses are in
        // the log at startup rather than mid-wave. Classes not loaded yet (e.g.
        // if init happens before a save is loaded) stay null here and are
        // retried by the lazy call in OnTick's Add branch - which is a no-op
        // once everything is resolved.
        ResolveHeatExcludedClasses();

        LOG_INFO("WaveOptimiser: AddEntitiesToWave hooked at 0x%llX, RemoveEntitiesFromWave hooked at 0x%llX",
            static_cast<unsigned long long>(addAddr), static_cast<unsigned long long>(removeAddr));
        return true;
    }

    void Shutdown(IPluginSelf* self)
    {
        if (g_hook && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hook);
            g_hook = nullptr;
            g_original = nullptr;
        }

        if (g_hookRemove && self->hooks->Hooks)
        {
            self->hooks->Hooks->Remove(g_hookRemove);
            g_hookRemove = nullptr;
            g_originalRemove = nullptr;
        }

        LOG_DEBUG("WaveOptimiser: hooks removed");

        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_queue.clear();
        g_currentBurstTotal = 0;
        g_getFragmentDataPtr = nullptr;
        g_temperatureFragStruct = nullptr;
        g_isEntityValid = nullptr;
        g_getSubsystemFn = nullptr;
        g_cachedWaveSubsystem = nullptr;
        for (size_t i = 0; i < kHeatExcludedClassCount; ++i)
            g_heatExcludedClasses[i] = nullptr;
        g_heatExcludedClassesResolved = false;
        g_actorFragStruct = nullptr;
        g_cachedEntityMgr = nullptr;
        g_signalEntities = nullptr;
        g_cachedSignalSubsystem = nullptr;
        g_addTemperatureToEntity = nullptr;
    }

    void OnWorldTeardown()
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);

        if (!g_queue.empty())
            LOG_INFO("WaveOptimiser: world teardown with %zu queued entities - dropping them (their entity manager is going away)", g_queue.size());

        g_queue.clear();
        g_currentBurstTotal = 0;

        // World-scoped pointers - stale the moment the world ends. OnTick and
        // Detour_AddEntitiesToWave re-resolve them lazily for the next world.
        // The exclusion-class cache is dropped too: BlueprintGeneratedClasses
        // can be GC'd on world unload; ResolveHeatExcludedClasses re-resolves
        // them from GObjects on the next replayed Add batch.
        for (size_t i = 0; i < kHeatExcludedClassCount; ++i)
            g_heatExcludedClasses[i] = nullptr;
        g_heatExcludedClassesResolved = false;
        g_cachedEntityMgr = nullptr;
        g_cachedWaveSubsystem = nullptr;
        g_cachedSignalSubsystem = nullptr;
    }

    void OnTick(float /*deltaSeconds*/)
    {
        // Lazily resolve FMassEntityManager* — stable after the first wave fires
        // and g_cachedWaveSubsystem is populated by Detour_AddEntitiesToWave.
        if (!g_cachedEntityMgr && g_cachedWaveSubsystem)
        {
            g_cachedEntityMgr = ResolveEntityManager();
            LOG_TRACE("WaveOptimiser: OnTick lazily resolved entity manager -> %s",
                g_cachedEntityMgr ? "ok" : "NULL (heat injection cannot run this tick)");
        }

        if (!g_original)
        {
            LOG_TRACE("WaveOptimiser: OnTick abort - AddEntitiesToWave original function pointer not resolved (hook not installed)");
            return;
        }

        if (!g_originalRemove)
        {
            LOG_TRACE("WaveOptimiser: OnTick abort - RemoveEntitiesFromWave original function pointer not resolved (hook not installed)");
            return;
        }

        if (g_queue.empty())
        {
            return;
        }

        const int maxPerTick = WaveOptimiserConfig::Config::GetMaxEntitiesPerTick();
        int processed = 0;

        // Needed by SetEntityHeat below - resolved once per tick rather than per
        // entity. Null before the first wave populates g_cachedWaveSubsystem, in
        // which case heat injection degrades to the direct-write fallback.
        void* world = ResolveWorld();

        // GetTimeSeconds goes through UFUNCTION reflection, so it's sampled once
        // per tick and reused for every entity in every batch below. Sub-frame
        // drift doesn't matter: the value only has to be recent enough that the
        // cooling processor's first pass sees a small delta.
        const float worldTimeSeconds = world
            ? static_cast<float>(SDK::UGameplayStatics::GetTimeSeconds(reinterpret_cast<SDK::UObject*>(world)))
            : 0.0f;

        MassEntitySet::Batch batch;

        // Entities heated directly below (Add branch) across every batch this
        // tick - signalled once at the end via FireTemperatureSignal instead of
        // per-batch, since UMassSignalSubsystem is a single world subsystem
        // shared across all wave subsystem instances/wave types.
        std::vector<FMassEntityHandle> heatedHandles;

        std::unique_lock<std::mutex> lock(g_statsMutex);

        while (processed < maxPerTick && !g_queue.empty())
        {
            const QueuedEntity first = g_queue.front();
            void* subsystem = first.subsystem;
            uint8_t waveType = first.waveType;
            WaveOp op = first.op;

            batch.Reset();

            // Group consecutive entries sharing the same subsystem instance,
            // op (Add/Remove) and wave type into one batch - stop at a
            // mismatched op/subsystem/waveType, batch capacity, or per-tick
            // budget. Never merging across an op boundary is what preserves
            // the original Add/Remove call order on replay.
            while (!g_queue.empty()
                && processed < maxPerTick
                && !batch.IsFull()
                && g_queue.front().subsystem == subsystem
                && g_queue.front().op == op
                && (op == WaveOp::Remove || g_queue.front().waveType == waveType))
            {
                batch.Add(g_queue.front().handle);
                g_queue.pop_front();
                ++processed;
            }

            if (batch.Count() > 0)
            {
                LOG_TRACE("WaveOptimiser: replaying %s batch of %d entities (wave type %u, queue depth now %zu)",
                    op == WaveOp::Add ? "Add" : "Remove",
                    batch.Count(), static_cast<unsigned>(waveType), g_queue.size());

                // Drop the lock while calling into engine code - GetStats() is a
                // quick poll from the render thread and shouldn't be blocked for
                // the duration of the original call.
                lock.unlock();
                if (op == WaveOp::Add)
                {
                    //g_original(subsystem, batch.AsScriptSet(), waveType);

                    // We deliberately don't replay the vanilla AddEntitiesToWave.
                    // It adds FCrMassEnviroWaveStageMovingTag and lets
                    // UCrMassTemperatureProcessor ramp heat over the wave's
                    // lifetime - but by the time a deferred batch is replayed the
                    // wave has usually moved on, so the ramp never completes.
                    // Instead we apply the end-state heat directly, per entity,
                    // skipping the interior-only furnishings in
                    // kHeatExcludedClassNames (they carry the temperature fragment
                    // from the base building trait but vanilla never heats them -
                    // see the list's comment for the full reasoning).
                    //
                    // Skipping the original also means EntitiesInWave is never
                    // populated, so OnWaveStarted/OnWaveFinished (which iterate it)
                    // are no-ops for us. That's consistent rather than broken:
                    // those exist to add/strip the moving tag we never set. The
                    // one thing we must not skip is FCrMassTemperatureUpdatingTag,
                    // which SetEntityHeat below takes care of - without it the
                    // cooling processor never matches the entity at all.
                    ResolveHeatExcludedClasses();
                    ResolveFragmentStructs();

                    const bool canFilter = g_cachedEntityMgr
                        && g_getFragmentDataPtr
                        && g_temperatureFragStruct;

                    // Dumps which prerequisite is missing whenever heat injection can't
                    // run at all - the fastest way to spot "why is nothing getting hot".
                    if (!canFilter)
                    {
                        LOG_TRACE("WaveOptimiser: heat injection disabled this batch - entityMgr=%s "
                            "getFragmentDataPtr=%s tempFragStruct=%s",
                            g_cachedEntityMgr ? "ok" : "NULL",
                            g_getFragmentDataPtr ? "ok" : "NULL",
                            g_temperatureFragStruct ? "ok" : "NULL");
                    }

                    for (int i = 0; i < batch.Count(); ++i)
                    {
                        FMassEntityHandle handle = batch.GetEntity(i);

                        if (!canFilter)
                        {
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "heat injection skipped, prerequisites unresolved",
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType),
                                DescribeEntityActor(handle).c_str());
                            continue;
                        }

                        const char* excludedAs = nullptr;
                        if (IsHeatExcludedEntity(handle, &excludedAs))
                        {
                            // LOG_INFO (not TRACE) so exclusion hits are visible at the
                            // default log level - the quickest way to confirm interior
                            // furnishings are being protected during a real wave.
                            LOG_INFO("WaveOptimiser: heat EXCLUDED - %s matched IsA(%s), interior-only furnishing "
                                "(entity Index=%d SerialNumber=%d, wave type %u)",
                                DescribeEntityActor(handle).c_str(), excludedAs,
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType));
                            continue;
                        }

                        float* heat = static_cast<float*>(GuardedGetFragmentDataPtr(g_cachedEntityMgr, handle, g_temperatureFragStruct));
                        if (heat)
                        {
                            const float before = *heat;

                            // Route through the game's own AddTemperatureToEntity
                            // rather than assigning *heat, so the entity also gets
                            // FCrMassTemperatureUpdatingTag and UCrMassTemperatureProcessor
                            // will actually cool it back down afterwards. The direct
                            // write below is only a degraded fallback for when the
                            // signature didn't resolve - it heats but never cools.
                            const bool tagged = SetEntityHeat(world, handle, kWaveHeat, worldTimeSeconds);
                            if (!tagged)
                                *heat = kWaveHeat;

                            heatedHandles.push_back(handle);
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "CurrentHeat %.1f -> %.1f (%s)",
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType),
                                DescribeEntityActor(handle).c_str(), before, *heat,
                                tagged ? "tagged for cooling" : "DIRECT WRITE, will not cool");
                        }
                        else
                        {
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "no CurrentHeat fragment, heat skipped",
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType),
                                DescribeEntityActor(handle).c_str());
                        }
                    }
                }
                else
                {
                    g_originalRemove(subsystem, batch.AsScriptSet());

                    for (int i = 0; i < batch.Count(); ++i)
                    {
                        FMassEntityHandle handle = batch.GetEntity(i);
                        LOG_TRACE("WaveOptimiser: Remove entity Index=%d SerialNumber=%d %s",
                            handle.Index, handle.SerialNumber, DescribeEntityActor(handle).c_str());
                    }
                }
                lock.lock();

                g_totalProcessed += static_cast<uint64_t>(batch.Count());
                ++g_totalBatchesReplayed;
            }
        }

        if (g_queue.empty())
        {
            g_currentBurstTotal = 0;
            LOG_DEBUG("WaveOptimiser: queue drained");
        }

        lock.unlock();
        FireTemperatureSignal(heatedHandles);
    }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);

        Stats stats{};
        stats.hookInstalled = g_hook != nullptr;
        stats.queueDepth = g_queue.size();
        stats.currentBurstTotal = g_currentBurstTotal;
        stats.totalCaptured = g_totalCaptured;
        stats.totalProcessed = g_totalProcessed;
        stats.totalBatchesReplayed = g_totalBatchesReplayed;
        stats.lastWaveType = g_lastWaveType;
        return stats;
    }

    HeatQueryContext GetHeatQueryContext()
    {
        HeatQueryContext ctx{};
        ctx.entityMgr    = g_cachedEntityMgr;
        ctx.getFragDataFn = reinterpret_cast<FragDataFn>(g_getFragmentDataPtr);
        ctx.tempStructPtr = g_temperatureFragStruct;
        return ctx;
    }
} // namespace WaveOptimiser
