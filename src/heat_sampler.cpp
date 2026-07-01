#include "heat_sampler.h"
#include "wave_optimiser.h"
#include "wave_optimiser_signatures.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>
#include <Windows.h>

// Raycast and actor->entity resolution require the game SDK (UKismetSystemLibrary,
// UGameplayStatics, etc.) which is only available in client/server SDK builds.
// Server builds get safe no-op stubs below.
#ifdef MODLOADER_CLIENT_BUILD

#include "Engine_classes.hpp"
#include "Engine_structs.hpp"
#include "CoreUObject_structs.hpp"
#include "MassActors_classes.hpp"
#include "MassSignals_classes.hpp"

using namespace SDK;

namespace HeatSampler
{
    namespace
    {
        // FCrMassActorReplicationHelper::FCrMassActorReplicationHelper(AActor*)
        // Per IDA (confirmed struct dump): [vtable ptr:8][FMassNetworkID NetID:4]
        // [TWeakObjectPtr<AActor,FWeakObjectPtr> Actor:8][pad:4]. This is NOT an
        // entity-handle resolver (that was our original wrong assumption) - it's a
        // network replication payload (has a NetSerialize vtable slot), used when
        // replacing/destroying building actors. We only reuse it here as a
        // convenient way to get the actor's own {ObjectIndex, ObjectSerialNumber}
        // identity at offset 0x0C - that's bit-identical to TObjectKey<AActor const>
        // (both just wrap FObjectKey), which is the key GetEntityHandleFromActor
        // below actually needs.
        using MassActorHelperCtor_t = void(__fastcall*)(void* self, AActor* actor);
        MassActorHelperCtor_t s_helperCtor = nullptr;

        // FMassActorManager::GetEntityHandleFromActor(FMassActorManager*, FMassEntityHandle* result, TObjectKey<AActor const> Actor)
        // The real actor -> Mass entity resolver. UMassActorSubsystem::GetEntityHandleFromActor
        // is just a thin forwarder into this and too small to get a unique AOB on its own.
        // ::FMassEntityHandle (global, from mass_entity_set.h) explicitly qualified below -
        // MassActors_classes.hpp pulls in the SDK's own SDK::FMassEntityHandle (identical
        // layout, distinct type), which is otherwise ambiguous with `using namespace SDK;` in effect.
        using GetEntityHandleFromActor_t = ::FMassEntityHandle*(__fastcall*)(void* actorManager, ::FMassEntityHandle* result, uint64_t objectKey);
        GetEntityHandleFromActor_t s_getEntityHandleFromActor = nullptr;

        // UMassSignalSubsystem::SignalEntities(FName SignalName, TArrayView<const FMassEntityHandle, int32> Entities)
        // Stock Epic MassGameplay code - see the AOB comment in wave_optimiser_signatures.h
        // for why this exists: writing CurrentHeat directly skips whatever signal
        // processor would normally react to a real temperature change, so the
        // reset-all-heat tool fires the equivalent signal itself afterwards.
        // TArrayView<T,int32> is just {T* DataPtr; int32 ArrayNum} in engine memory -
        // mirrored here the same way mass_entity_set.h mirrors TSet.
        struct ArrayViewMirror
        {
            const ::FMassEntityHandle* DataPtr;
            int32_t ArrayNum;
        };
        using SignalEntities_t = void(__fastcall*)(void* signalSubsystem, FName signalName, const ArrayViewMirror* entities);
        SignalEntities_t s_signalEntities = nullptr;

        std::atomic<bool> s_enabled{ false };
        std::atomic<bool> s_resetHeatPending{ false };

        Result         s_lastResult{};
        std::mutex     s_resultMutex;

        // Dedup key for the handle-resolution debug log below - avoids spamming
        // once per tick while the crosshair rests on the same actor.
        std::string s_lastLoggedActor;

        APawn* GetLocalPawn()
        {
            UWorld* world = UWorld::GetWorld();
            if (!world)
                return nullptr;
            APlayerController* pc = UGameplayStatics::GetPlayerController(world, 0);
            return pc ? pc->Pawn : nullptr;
        }

        // FMassActorManager* lives at offset 0x38 of UMassActorSubsystem - a
        // TSharedPtr<FMassActorManager> in Dumper-7's Pad_38[0x10] region (not a
        // reflected UPROPERTY, but the first 8 bytes of any TSharedPtr are the raw
        // pointee pointer). UMassActorSubsystem itself is resolved through the SDK's
        // own reflected USubsystemBlueprintLibrary::GetWorldSubsystem - a real
        // UFUNCTION call via ProcessEvent, the same mechanism already used for the
        // UGameplayStatics/UKismetSystemLibrary calls elsewhere in this file - so no
        // AOB scanning is needed for this step at all.
        void* ResolveActorManager(UWorld* world)
        {
            UWorldSubsystem* subsystem = USubsystemBlueprintLibrary::GetWorldSubsystem(world, UMassActorSubsystem::StaticClass());
            if (!subsystem)
                return nullptr;
            return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(subsystem) + 0x38);
        }

        // UMassSignalSubsystem needs no AOB to locate - like UMassActorSubsystem
        // above, it's a real UWorldSubsystem reachable through the SDK's own
        // reflected GetWorldSubsystem. Only SignalEntities() itself (native,
        // non-reflected) needs an AOB.
        void* ResolveSignalSubsystem(UWorld* world)
        {
            return static_cast<void*>(USubsystemBlueprintLibrary::GetWorldSubsystem(world, UMassSignalSubsystem::StaticClass()));
        }

        // Builds a live FName from an ASCII config string. Deliberately not cached
        // (unlike STATIC_NAME_IMPL's GetStaticName pattern, which only resolves
        // once ever) - the signal name is config-driven and re-read every reset so
        // the user can retune it without rebuilding the plugin.
        FName MakeFNameFromAscii(const char* ascii)
        {
            wchar_t wbuf[128];
            int i = 0;
            for (; ascii[i] != '\0' && i < 127; ++i)
                wbuf[i] = static_cast<wchar_t>(static_cast<unsigned char>(ascii[i]));
            wbuf[i] = L'\0';
            return BasicFilesImplUtils::StringToName(wbuf);
        }

        // Handles resolved from an arbitrary hit actor (unlike the wave-replay path,
        // which only ever touches handles fresh from AddEntitiesToWave) can be stale -
        // e.g. the actor lingers a frame after its Mass entity was destroyed.
        // InternalGetFragmentDataPtr hard-asserts (via IsEntityBuilt) on a stale handle
        // instead of failing softly, which crashes the game. UE's check() raises a real
        // SEH exception, so a __try/__except here degrades that to "no fragment" instead.
        // WaveOptimiser::IsEntityValid() exists but its AOB match is unverified/likely
        // wrong (confirmed rejecting known-valid entities) - do not gate on it until fixed.
        // No unwind-requiring locals allowed in this frame - keep it minimal.
        void* SafeGetFragmentDataPtr(WaveOptimiser::FragDataFn fn, void* entityMgr,
            ::FMassEntityHandle handle, const void* structPtr)
        {
            __try
            {
                return fn(entityMgr, handle, structPtr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        // Resolves an actor's Mass entity handle the same way the cursor sampler
        // does - see the comments on s_helperCtor/s_getEntityHandleFromActor
        // above. Returns a zeroed handle (Index == 0 && SerialNumber == 0) if the
        // actor has no Mass entity or actorManager is null.
        ::FMassEntityHandle ResolveHandleForActor(AActor* actor, void* actorManager)
        {
            ::FMassEntityHandle handle{};
            if (!actor || !actorManager)
                return handle;

            alignas(8) uint8_t helperBuf[0x18] = {};
            s_helperCtor(helperBuf, actor);
            uint64_t objectKey = 0;
            std::memcpy(&objectKey, helperBuf + 0x0C, sizeof(objectKey));

            s_getEntityHandleFromActor(actorManager, &handle, objectKey);
            return handle;
        }

        // Walks every actor currently loaded in the world and zeroes CurrentHeat
        // on whichever ones resolve to a Mass entity with the temperature
        // fragment. Deliberately no filtering by wave history or building-grid
        // membership - this is meant to clear/inspect heat independent of
        // whatever our wave hook has or hasn't touched.
        void PerformResetAllHeat(UWorld* world, void* entityMgr, const WaveOptimiser::HeatQueryContext& ctx, void* actorManager)
        {
            TArray<AActor*> actors;
            UGameplayStatics::GetAllActorsOfClass(world, AActor::StaticClass(), &actors);

            std::vector<::FMassEntityHandle> zeroedHandles;
            zeroedHandles.reserve(static_cast<size_t>(actors.Num()));

            for (int i = 0; i < actors.Num(); ++i)
            {
                AActor* actor = actors[i];
                if (!actor || !UKismetSystemLibrary::IsValid(actor))
                    continue;

                ::FMassEntityHandle handle = ResolveHandleForActor(actor, actorManager);
                if (handle.Index == 0 && handle.SerialNumber == 0)
                    continue;

                void* fragData = SafeGetFragmentDataPtr(ctx.getFragDataFn, entityMgr, handle, ctx.tempStructPtr);
                if (fragData)
                {
                    *static_cast<float*>(fragData) = 0.0f;
                    zeroedHandles.push_back(handle);
                }
            }

            LOG_INFO("HeatSampler: reset heat to zero on %zu of %d loaded actors", zeroedHandles.size(), actors.Num());

            // Writing CurrentHeat directly (above) skips whatever signal processor
            // would normally react to a real temperature change (see the AOB comment
            // on SignalEntities in wave_optimiser_signatures.h) - fire it ourselves,
            // batched, so the game re-evaluates the "Hold E to lower temperature"
            // prompt instead of it staying stuck from before the reset.
            if (s_signalEntities && !zeroedHandles.empty())
            {
                void* signalSubsystem = ResolveSignalSubsystem(world);
                if (signalSubsystem)
                {
                    ArrayViewMirror view{ zeroedHandles.data(), static_cast<int32_t>(zeroedHandles.size()) };
                    FName signalName = MakeFNameFromAscii(WaveOptimiserSignatures::TemperatureSignalName);
                    s_signalEntities(signalSubsystem, signalName, &view);
                    LOG_INFO("HeatSampler: signalled %zu entities with '%s'",
                        zeroedHandles.size(), WaveOptimiserSignatures::TemperatureSignalName);
                }
                else
                    LOG_WARN("HeatSampler: reset-all-heat signal skipped - UMassSignalSubsystem not resolved");
            }
        }
    }

    void Init(IPluginSelf* self)
    {
        IPluginScanner* scanner = self->scanner;

        // AOB shared with WAILA plugin — FCrMassActorReplicationHelper ctor.
        uintptr_t addr = scanner->FindPatternInMainModule(
            "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 33 ED "
            "48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8B DA");
        if (addr)
        {
            s_helperCtor = reinterpret_cast<MassActorHelperCtor_t>(addr);
            LOG_DEBUG("HeatSampler: FCrMassActorReplicationHelper ctor at 0x%llX",
                static_cast<unsigned long long>(addr));
        }
        else
            LOG_WARN("HeatSampler: FCrMassActorReplicationHelper ctor not found - heat sampling inactive");

        uintptr_t entityFromActorAddr = scanner->FindPatternInMainModule(
            "4C 89 44 24 ?? 53 55 57 41 55 41 56 48 83 EC ?? 49 8B F8");
        if (entityFromActorAddr)
        {
            s_getEntityHandleFromActor = reinterpret_cast<GetEntityHandleFromActor_t>(entityFromActorAddr);
            LOG_DEBUG("HeatSampler: FMassActorManager::GetEntityHandleFromActor at 0x%llX",
                static_cast<unsigned long long>(entityFromActorAddr));
        }
        else
            LOG_WARN("HeatSampler: FMassActorManager::GetEntityHandleFromActor not found - heat sampling inactive");

        uintptr_t signalEntitiesAddr = scanner->FindPatternInMainModule(WaveOptimiserSignatures::SignalEntities);
        if (signalEntitiesAddr)
        {
            s_signalEntities = reinterpret_cast<SignalEntities_t>(signalEntitiesAddr);
            LOG_DEBUG("HeatSampler: UMassSignalSubsystem::SignalEntities at 0x%llX",
                static_cast<unsigned long long>(signalEntitiesAddr));
        }
        else
            LOG_WARN("HeatSampler: SignalEntities not found - reset-all-heat will zero heat but won't notify the temperature signal processor");
    }

    void Shutdown()
    {
        s_helperCtor = nullptr;
        s_getEntityHandleFromActor = nullptr;
        s_signalEntities = nullptr;
        s_enabled.store(false);
        s_resetHeatPending.store(false);
        std::lock_guard<std::mutex> lock(s_resultMutex);
        s_lastResult = {};
    }

    void OnTick(float /*deltaSeconds*/)
    {
        if (!s_helperCtor || !s_getEntityHandleFromActor)
            return;

        UWorld* world = UWorld::GetWorld();
        if (!world)
            return;

        // Runs regardless of whether the cursor sampler itself is enabled -
        // it's driven by its own "Reset All Heat" button.
        if (s_resetHeatPending.exchange(false))
        {
            WaveOptimiser::HeatQueryContext resetCtx = WaveOptimiser::GetHeatQueryContext();
            void* resetEntityMgr = resetCtx.entityMgr ? resetCtx.entityMgr : WaveOptimiser::ResolveEntityManagerFromWorld(world);
            void* actorManager = ResolveActorManager(world);
            if (resetEntityMgr && resetCtx.getFragDataFn && resetCtx.tempStructPtr && actorManager)
                PerformResetAllHeat(world, resetEntityMgr, resetCtx, actorManager);
            else
                LOG_WARN("HeatSampler: reset-all-heat skipped - entity manager/fragment ptr/actor manager not resolved");
        }

        if (!s_enabled.load())
            return;

        WaveOptimiser::HeatQueryContext ctx = WaveOptimiser::GetHeatQueryContext();
        void* entityMgr = ctx.entityMgr ? ctx.entityMgr : WaveOptimiser::ResolveEntityManagerFromWorld(world);
        if (!entityMgr || !ctx.getFragDataFn || !ctx.tempStructPtr)
            return;

        APawn* pawn = GetLocalPawn();
        if (!pawn)
            return;

        FVector   eyeLocation;
        FRotator  eyeRotation;
        pawn->GetActorEyesViewPoint(&eyeLocation, &eyeRotation);

        FVector direction     = UKismetMathLibrary::GetForwardVector(eyeRotation);
        FVector startLocation = eyeLocation + direction * 80.0;
        FVector endLocation   = eyeLocation + direction * 5000.0;

        TArray<AActor*> ignoreActors;
        ignoreActors.Add(pawn);

        FHitResult   hitResult;
        FLinearColor noColor{ 0.f, 0.f, 0.f, 0.f };

        bool bHit = UKismetSystemLibrary::LineTraceSingle(
            world, startLocation, endLocation,
            ETraceTypeQuery::TraceTypeQuery1, false,
            ignoreActors, EDrawDebugTrace::None,
            &hitResult, true, noColor, noColor, 0.f);

        Result result{};

        if (bHit)
        {
            AActor* hitActor = nullptr;
            if (UPrimitiveComponent* comp = hitResult.Component.Get())
                hitActor = comp->GetOwner();

            if (hitActor && UKismetSystemLibrary::IsValid(hitActor))
            {
                std::string name = UKismetSystemLibrary::GetObjectName(hitActor).ToString();
                std::snprintf(result.actorName, sizeof(result.actorName), "%s", name.c_str());
                result.valid = true;

                void* actorManager = ResolveActorManager(world);
                ::FMassEntityHandle handle = ResolveHandleForActor(hitActor, actorManager);

                result.handleResolved = actorManager && (handle.Index != 0 || handle.SerialNumber != 0);
                result.handleIndex    = handle.Index;
                result.handleSerial   = handle.SerialNumber;

                if (name != s_lastLoggedActor)
                {
                    s_lastLoggedActor = name;
                    LOG_DEBUG("HeatSampler: actor '%s' -> handle Index=%d SerialNumber=%d (actorManager=%s)",
                        result.actorName, handle.Index, handle.SerialNumber, actorManager ? "ok" : "null");
                }

                if (result.handleResolved)
                {
                    // Diagnostic only - see the comment on SafeGetFragmentDataPtr.
                    result.entityValid = WaveOptimiser::IsEntityValid(entityMgr, handle);

                    void* fragData = SafeGetFragmentDataPtr(ctx.getFragDataFn, entityMgr, handle, ctx.tempStructPtr);
                    if (fragData)
                    {
                        result.heat        = *static_cast<float*>(fragData);
                        result.hasFragment = true;
                    }
                }
            }
        }

        std::lock_guard<std::mutex> lock(s_resultMutex);
        s_lastResult = result;
    }

    void SetEnabled(bool enabled)
    {
        s_enabled.store(enabled);
        if (!enabled)
        {
            std::lock_guard<std::mutex> lock(s_resultMutex);
            s_lastResult = {};
        }
    }

    bool IsEnabled() { return s_enabled.load(); }

    Result GetLastResult()
    {
        std::lock_guard<std::mutex> lock(s_resultMutex);
        return s_lastResult;
    }

    void RequestResetAllHeat()
    {
        s_resetHeatPending.store(true);
    }

} // namespace HeatSampler

#else // !MODLOADER_CLIENT_BUILD

// Server builds: no local player, no raycast. All stubs.
namespace HeatSampler
{
    void   Init(IPluginSelf*) {}
    void   Shutdown()         {}
    void   OnTick(float)      {}
    void   SetEnabled(bool)   {}
    bool   IsEnabled()        { return false; }
    Result GetLastResult()    { return {}; }
    void   RequestResetAllHeat() {}
}

#endif
