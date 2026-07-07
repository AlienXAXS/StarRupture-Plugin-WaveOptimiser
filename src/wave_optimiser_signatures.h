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

    // Note: the fragment UScriptStruct*s (FCrMassTemperatureFragment,
    // FMassActorFragment) are intentionally NOT resolved here. Their
    // StaticStruct() functions are too small for unique AOBs, and hardcoded
    // RVAs are client-binary-specific (they crashed the dedicated server -
    // separate link, different addresses). They're resolved by object name
    // via SDK::UObject::FindObjectFast in ResolveFragmentStructs()
    // (wave_optimiser.cpp), which works on both binaries.

    // UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave prologue. Signature
    // (UCrMassEnviroWaveSubsystem*, const TSet<FMassEntityHandle>*) - no
    // WaveType param, unlike AddEntitiesToWave.
    static const char* RemoveEntitiesFromWave =
        "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B DA 48 89 54 24";

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
