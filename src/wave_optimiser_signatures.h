#pragma once

// AOB patterns for WaveOptimiser. Centralised here so a future game update
// that shifts a function's prologue only requires touching one file.
namespace WaveOptimiserSignatures
{
    // UCrMassEnviroWaveSubsystem::AddEntitiesToWave prologue. Confirmed
    // register layout via IDA: mov rbx, rdx (EntitiesToAdd) / mov r13, rcx
    // (this) - matches our __fastcall(this, EntitiesToAdd, WaveType) detour
    // signature in wave_optimiser.cpp.
    static const char* AddEntitiesToWave =
        "48 89 5C 24 ?? 48 89 54 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B DA 4C 8B E9";

    // UE5 engine function containing a call to UWorld::GetSubsystem<UMassEntitySubsystem>.
    // Stable across game updates because it lives in engine code, not game code.
    // Scan to get function start, add CallOffset to reach the call instruction,
    // resolve rel32 to get the UWorld::GetSubsystem<UMassEntitySubsystem> pointer.
    // Call it with UWorld* (= UCrMassEnviroWaveSubsystem::OuterPrivate at +0x20)
    // to get UMassEntitySubsystem*; FMassEntityManager* is then at [result + 0x38].
    static const char* GetSubsystemMassEntity_Fn =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC ?? ?? ?? ?? 4C 8B F1 48 8B CA";
    static const size_t GetSubsystemMassEntity_CallOffset = 0x2F;

    // XRef site that calls FMassEntityManager::InternalGetFragmentDataPtr inside
    // UMassEnemyRepresentationSubsystem::SynchronizeActorStateToMass. Scan to
    // function start, add CallOffset to reach the call instruction, resolve
    // rel32 to get InternalGetFragmentDataPtr. Signature:
    //   void* InternalGetFragmentDataPtr(FMassEntityManager*, FMassEntityHandle, const UScriptStruct*)
    static const char* InternalGetFragmentDataPtr_XRef =
        "40 53 56 41 56 48 81 EC ?? ?? ?? ?? 48 83 B9 ?? ?? ?? ?? ?? 4D 8B F0";
    static const size_t InternalGetFragmentDataPtr_CallOffset = 0x27D;

    // FCrMassTemperatureFragment::StaticStruct — too small for a unique AOB;
    // resolved via module base + RVA (confirmed in IDA, default image base 0x140000000).
    static const uintptr_t TemperatureFragment_StaticStructRVA = 0x72C76A0;

    // UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave prologue. Signature
    // (UCrMassEnviroWaveSubsystem*, const TSet<FMassEntityHandle>*) - no
    // WaveType param, unlike AddEntitiesToWave.
    static const char* RemoveEntitiesFromWave =
        "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B DA 48 89 54 24";

    // FMassEntityView constructor. Signature:
    //   FMassEntityView::FMassEntityView(FMassEntityView*, FMassEntityManager*, FMassEntityHandle)
    // Used to build a view from a raw entity manager + handle pair so we can
    // call HasTag on the result.
    static const char* FMassEntityViewCtor =
        "4C 8B DC 49 89 5B ?? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ?? 45 33 F6";

    // FMassEntityView::HasTag(FMassEntityView*, const UScriptStruct*)
    // Returns true if the entity's archetype composition includes the given tag.
    static const char* FMassEntityViewHasTag =
        "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B D9 48 8B FA 48 83 C1 ?? 48 8B 53";

    // FCrMassSimpleBuildingTag::StaticStruct - too small for a unique AOB;
    // resolved via module base + this RVA (confirmed in IDA, default image base
    // 0x140000000). Confirmed via IDA disassembly (not guessed) to be added
    // unconditionally by UCrMassSimpleBuildingTrait::BuildTemplate - the trait
    // used exclusively by grid-snapped structural pieces (foundations,
    // platforms, walls, stairs, connectors, pillars, cooler/drill/water-
    // extractor foundations) - and never by UCrMassBuildingTrait::BuildTemplate
    // (furniture/machines/storages/crafting stations). Replaces the previously
    // used FCrMassBuildingGridElementTag, which turned out to have zero native
    // code xrefs anywhere in the engine (effectively dead/unused) and wrongly
    // excluded legitimate structural buildings like BP_Modular_BasePlatform_C.
    static const uintptr_t SimpleBuildingTag_StaticStructRVA = 0x72804B0;

    // FMassActorFragment::StaticStruct - too small for a unique AOB; resolved
    // via module base + this RVA (confirmed in IDA, default image base
    // 0x140000000). FMassActorFragment (MassActors module, stock Epic layout:
    // {TWeakObjectPtr<AActor> Actor; uint8 Pad[4]}) is what lets OnTick's
    // diagnostic trace resolve a replayed entity handle back to its actor
    // name/class via the same InternalGetFragmentDataPtr accessor already
    // used for the temperature fragment.
    static const uintptr_t ActorFragment_StaticStructRVA = 0x6631960;

    // FMassEntityManager::IsEntityValid(FMassEntityManager*, FMassEntityHandle)
    // Returns whether a handle still refers to a live entity. Must be checked
    // before InternalGetFragmentDataPtr on any handle that isn't fresh off
    // AddEntitiesToWave (historical handles, or handles resolved from an
    // arbitrary hit actor) - that function hard-asserts (crashes the game) on a
    // stale/destroyed entity instead of failing softly.
    static const char* IsEntityValid =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? 57 48 83 EC ?? 48 8B DA 85 D2";

    // UMassSignalSubsystem::SignalEntities(FName SignalName, TArrayView<const FMassEntityHandle, int32> Entities)
    // Stock Epic MassGameplay plugin code (not game-specific), confirmed via IDA.
    // Batches signal delivery for multiple entities in a single call instead of
    // calling SignalEntity per-entity. Used by the heat-reset tool to notify
    // whatever signal processor (e.g. UCrMassTemperatureSignalProcessor) would
    // normally react to a real temperature change, since writing CurrentHeat
    // directly via InternalGetFragmentDataPtr bypasses that notification path
    // entirely (see PerformResetAllHeat in heat_sampler.cpp).
    static const char* SignalEntities =
        "48 8B C4 48 89 50 ?? 55 48 81 EC ?? ?? ?? ?? 41 83 78";

    // Signal name UCrMassTemperatureSignalProcessor listens for. Fired via
    // SignalEntities (above) after any direct CurrentHeat write - reset-all-heat
    // (heat_sampler.cpp) and wave heat injection (wave_optimiser.cpp) - so the
    // game's own signal processor re-evaluates the "Hold E to lower temperature"
    // prompt instead of it going stale. Hardcoded rather than user-configurable:
    // it names a specific native processor, not a tunable.
    static const char* TemperatureSignalName = "TemperatureChanged";
}
