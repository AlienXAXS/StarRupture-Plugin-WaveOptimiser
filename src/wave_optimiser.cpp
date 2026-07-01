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

        // FMassEntityView constructor and HasTag — used for the structural-building
        // filter applied during Add batch replay. FCrMassSimpleBuildingTag is added
        // unconditionally by UCrMassSimpleBuildingTrait::BuildTemplate (grid-snapped
        // structural pieces: foundations, platforms, walls, stairs, connectors,
        // pillars, cooler/drill/water-extractor foundations) and never by
        // UCrMassBuildingTrait::BuildTemplate (furniture/machines/storages/crafting
        // stations) - confirmed via IDA disassembly of both trait BuildTemplate
        // functions. Entities lacking it are skipped; structural buildings are
        // heated directly.
        typedef void(__fastcall* FMassEntityViewCtor_t)(void* view, void* entityMgr, FMassEntityHandle handle);
        typedef bool(__fastcall* FMassEntityViewHasTag_t)(void* view, const void* tagStruct);
        FMassEntityViewCtor_t   g_entityViewCtor    = nullptr;
        FMassEntityViewHasTag_t g_entityViewHasTag  = nullptr;
        void*                   g_simpleBuildingTagStruct = nullptr; // FCrMassSimpleBuildingTag UScriptStruct*

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

        // Diagnostic-only: resolves a replayed entity handle to its actor's
        // name/class for OnTick's trace logging. FMassActorFragment mirrors the
        // stock Epic layout {TWeakObjectPtr<AActor> Actor; pad} - queried through
        // the same generic g_getFragmentDataPtr accessor used for the temperature
        // fragment, then TWeakObjectPtr::Get() (real SDK code, no extra AOB)
        // resolves the live AActor*. Safe to call unguarded here because these
        // handles are always fresh off AddEntitiesToWave/RemoveEntitiesFromWave
        // replay, never the arbitrary/possibly-stale handles heat_sampler.cpp
        // has to SEH-guard against.
        std::string DescribeEntityActor(FMassEntityHandle handle)
        {
            if (!g_cachedEntityMgr || !g_getFragmentDataPtr || !g_actorFragStruct)
                return "actor=<unresolved>";

            void* fragData = g_getFragmentDataPtr(g_cachedEntityMgr, handle, g_actorFragStruct);
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
    }

    bool Init(IPluginSelf* self)
    {
        IPluginScanner* scanner = self->scanner;

        LOG_DEBUG("WaveOptimiser: scanning for AddEntitiesToWave...");

        uintptr_t addAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::AddEntitiesToWave);
        if (!addAddr)
        {
            LOG_WARN("WaveOptimiser: AddEntitiesToWave pattern not found - wave batching not installed");
            return false;
        }

        LOG_DEBUG("WaveOptimiser: scanning for RemoveEntitiesFromWave...");

        uintptr_t removeAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::RemoveEntitiesFromWave);
        if (!removeAddr)
        {
            LOG_WARN("WaveOptimiser: RemoveEntitiesFromWave pattern not found - wave batching not installed "
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
        

        // Resolve GetSubsystem<UMassEntitySubsystem> for the heat-reset tool.
        uintptr_t getSubsysFn = scanner->FindPatternInMainModule(WaveOptimiserSignatures::GetSubsystemMassEntity_Fn);
        if (getSubsysFn)
        {
            uintptr_t callSite = getSubsysFn + WaveOptimiserSignatures::GetSubsystemMassEntity_CallOffset;
            int32_t rel = *reinterpret_cast<int32_t*>(callSite + 1);
            g_getSubsystemFn = reinterpret_cast<GetSubsystemMassEntity_t>(callSite + 5 + rel);
            LOG_DEBUG("WaveOptimiser: GetSubsystem<UMassEntitySubsystem> resolved at 0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_getSubsystemFn)));
        }
        else
            LOG_WARN("WaveOptimiser: GetSubsystemMassEntity_Fn not found - heat reset button will be inert");

        // Resolve FMassEntityManager::InternalGetFragmentDataPtr via xref in
        // SynchronizeActorStateToMass. Non-fatal — heat injection and reset button
        // will be inert if missing.
        uintptr_t fragXRef = scanner->FindPatternInMainModule(WaveOptimiserSignatures::InternalGetFragmentDataPtr_XRef);
        if (fragXRef)
        {
            uintptr_t callSite = fragXRef + WaveOptimiserSignatures::InternalGetFragmentDataPtr_CallOffset;
            int32_t rel = *reinterpret_cast<int32_t*>(callSite + 1);
            g_getFragmentDataPtr = reinterpret_cast<GetFragmentDataPtr_t>(callSite + 5 + rel);
            LOG_DEBUG("WaveOptimiser: InternalGetFragmentDataPtr resolved at 0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_getFragmentDataPtr)));
        }
        else
            LOG_WARN("WaveOptimiser: InternalGetFragmentDataPtr_XRef not found - heat injection and reset will be inert");

        uintptr_t isEntityValidAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::IsEntityValid);
        if (isEntityValidAddr)
        {
            g_isEntityValid = reinterpret_cast<IsEntityValid_t>(isEntityValidAddr);
            LOG_DEBUG("WaveOptimiser: FMassEntityManager::IsEntityValid resolved at 0x%llX",
                static_cast<unsigned long long>(isEntityValidAddr));
        }
        else
            LOG_WARN("WaveOptimiser: FMassEntityManager::IsEntityValid not found - stale-entity checks will fail closed (skip)");

        // Resolve UMassSignalSubsystem::SignalEntities - lets wave-applied heat
        // notify UCrMassTemperatureSignalProcessor the same way the reset-all-heat
        // tool does (see FireTemperatureSignal above). Non-fatal if missing: heat
        // injection still runs, it just won't refresh the temperature prompt.
        uintptr_t signalEntitiesAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::SignalEntities);
        if (signalEntitiesAddr)
        {
            g_signalEntities = reinterpret_cast<SignalEntities_t>(signalEntitiesAddr);
            LOG_DEBUG("WaveOptimiser: UMassSignalSubsystem::SignalEntities resolved at 0x%llX",
                static_cast<unsigned long long>(signalEntitiesAddr));
        }
        else
            LOG_WARN("WaveOptimiser: SignalEntities not found - wave heat injection won't notify the temperature signal processor");

        // Resolve FMassEntityView constructor and HasTag for the building-grid filter.
        uintptr_t entityViewCtorAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::FMassEntityViewCtor);
        if (entityViewCtorAddr)
        {
            g_entityViewCtor = reinterpret_cast<FMassEntityViewCtor_t>(entityViewCtorAddr);
            LOG_DEBUG("WaveOptimiser: FMassEntityView ctor resolved at 0x%llX",
                static_cast<unsigned long long>(entityViewCtorAddr));
        }
        else
            LOG_WARN("WaveOptimiser: FMassEntityView ctor not found - building-grid heat filter will be inactive");

        uintptr_t entityViewHasTagAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::FMassEntityViewHasTag);
        if (entityViewHasTagAddr)
        {
            g_entityViewHasTag = reinterpret_cast<FMassEntityViewHasTag_t>(entityViewHasTagAddr);
            LOG_DEBUG("WaveOptimiser: FMassEntityView::HasTag resolved at 0x%llX",
                static_cast<unsigned long long>(entityViewHasTagAddr));
        }
        else
            LOG_WARN("WaveOptimiser: FMassEntityView::HasTag not found - building-grid heat filter will be inactive");

        // Cache UScriptStruct* for both fragment/tag types via module base + RVA.
        // All StaticStruct() fns are too small for unique AOBs.
        {
            HMODULE hMain = GetModuleHandleA(nullptr);
            if (hMain)
            {
                using StaticStruct_t = void*(*)();
                const uintptr_t base = reinterpret_cast<uintptr_t>(hMain);

                if (g_getFragmentDataPtr)
                {
                    auto fn = reinterpret_cast<StaticStruct_t>(base + WaveOptimiserSignatures::TemperatureFragment_StaticStructRVA);
                    g_temperatureFragStruct = fn();
                    if (g_temperatureFragStruct)
                        LOG_DEBUG("WaveOptimiser: FCrMassTemperatureFragment UScriptStruct* = 0x%llX",
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_temperatureFragStruct)));
                    else
                        LOG_WARN("WaveOptimiser: FCrMassTemperatureFragment StaticStruct returned null - heat ops inert");
                }

                if (g_entityViewCtor && g_entityViewHasTag)
                {
                    auto fn = reinterpret_cast<StaticStruct_t>(base + WaveOptimiserSignatures::SimpleBuildingTag_StaticStructRVA);
                    g_simpleBuildingTagStruct = fn();
                    if (g_simpleBuildingTagStruct)
                        LOG_DEBUG("WaveOptimiser: FCrMassSimpleBuildingTag UScriptStruct* = 0x%llX",
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_simpleBuildingTagStruct)));
                    else
                        LOG_WARN("WaveOptimiser: FCrMassSimpleBuildingTag StaticStruct returned null - building filter inactive");
                }

                if (g_getFragmentDataPtr)
                {
                    auto fn = reinterpret_cast<StaticStruct_t>(base + WaveOptimiserSignatures::ActorFragment_StaticStructRVA);
                    g_actorFragStruct = fn();
                    if (g_actorFragStruct)
                        LOG_DEBUG("WaveOptimiser: FMassActorFragment UScriptStruct* = 0x%llX",
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_actorFragStruct)));
                    else
                        LOG_WARN("WaveOptimiser: FMassActorFragment StaticStruct returned null - OnTick trace won't include actor name/class");
                }
            }
        }

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
        g_entityViewCtor = nullptr;
        g_entityViewHasTag = nullptr;
        g_simpleBuildingTagStruct = nullptr;
        g_actorFragStruct = nullptr;
        g_cachedEntityMgr = nullptr;
        g_signalEntities = nullptr;
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

                    // The Mass processor that applies heat via EnviroWaveStatusChanged_0
                    // may have unsubscribed by the time our deferred batch is replayed
                    // (it unsubscribes when the wave finishes). Inject heat directly for
                    // structural buildings. Furniture/machines/storages lack
                    // FCrMassSimpleBuildingTag (only added by
                    // UCrMassSimpleBuildingTrait::BuildTemplate, confirmed via IDA) and
                    // are intentionally skipped.
                    const bool canFilter = g_cachedEntityMgr
                        && g_getFragmentDataPtr
                        && g_temperatureFragStruct
                        && g_entityViewCtor
                        && g_entityViewHasTag
                        && g_simpleBuildingTagStruct;

                    // Dumps which prerequisite is missing whenever heat injection can't
                    // run at all - the fastest way to spot "why is nothing getting hot".
                    if (!canFilter)
                    {
                        LOG_TRACE("WaveOptimiser: heat injection disabled this batch - entityMgr=%s "
                            "getFragmentDataPtr=%s tempFragStruct=%s entityViewCtor=%s entityViewHasTag=%s "
                            "simpleBuildingTag=%s",
                            g_cachedEntityMgr ? "ok" : "NULL",
                            g_getFragmentDataPtr ? "ok" : "NULL",
                            g_temperatureFragStruct ? "ok" : "NULL",
                            g_entityViewCtor ? "ok" : "NULL",
                            g_entityViewHasTag ? "ok" : "NULL",
                            g_simpleBuildingTagStruct ? "ok" : "NULL");
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

                        alignas(8) uint8_t viewBuf[128] = {};
                        g_entityViewCtor(viewBuf, g_cachedEntityMgr, handle);
                        const bool hasSimpleBuildingTag = g_entityViewHasTag(viewBuf, g_simpleBuildingTagStruct);

                        if (!hasSimpleBuildingTag)
                        {
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "no FCrMassSimpleBuildingTag, treated as non-structural furnishing, heat skipped",
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType),
                                DescribeEntityActor(handle).c_str());
                            continue;
                        }

                        float* heat = static_cast<float*>(g_getFragmentDataPtr(g_cachedEntityMgr, handle, g_temperatureFragStruct));
                        if (heat)
                        {
                            const float before = *heat;
                            *heat = 600.0f; // confirmed via HeatSampler against vanilla (passthrough) wave behavior
                            heatedHandles.push_back(handle);
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "structural building, CurrentHeat %.1f -> 600.0",
                                handle.Index, handle.SerialNumber, static_cast<unsigned>(waveType),
                                DescribeEntityActor(handle).c_str(), before);
                        }
                        else
                        {
                            LOG_TRACE("WaveOptimiser: Add entity Index=%d SerialNumber=%d (wave type %u) %s - "
                                "structural building but GetFragmentDataPtr returned NULL, no CurrentHeat fragment on this entity",
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
