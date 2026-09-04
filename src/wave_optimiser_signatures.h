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
        "48 89 5C 24 ?? 48 89 54 24 ?? 48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B DA";

    // UE5 engine function containing a call to UWorld::GetSubsystem<UMassEntitySubsystem>.
    // Stable across game updates because it lives in engine code, not game code.
    // Scan to get function start, add CallOffset to reach the call instruction,
    // resolve rel32 to get the UWorld::GetSubsystem<UMassEntitySubsystem> pointer.
    // Call it with UWorld* (= UCrMassEnviroWaveSubsystem::OuterPrivate at +0x20)
    // to get UMassEntitySubsystem*; FMassEntityManager* is then at [result + 0x38].
    static const char* GetSubsystemMassEntity_Fn =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC ?? ?? ?? ?? 4C 8B F1 48 8B CA";
    static const size_t GetSubsystemMassEntity_CallOffset = 0x2F;

    // FMassEntityManager::InternalGetFragmentDataPtr:
    //   void* InternalGetFragmentDataPtr(FMassEntityManager*, FMassEntityHandle, const UScriptStruct*)
    // Resolved as the rel32 target of the call at AddTemperatureToEntity + this
    // offset, NOT by its own AOB and NOT via a separate xref function.
    //
    // Why not its own AOB: it is one of three adjacent, near-identical template
    // instantiations - InternalGetConstSharedFragmentPtr, InternalGetFragmentDataChecked
    // and InternalGetFragmentDataPtr sit back to back and share a byte-identical
    // prologue. Every candidate pattern tried (including one extended past the
    // assert-line immediate) matched all three. Picking the wrong one is not a
    // soft failure: InternalGetFragmentDataChecked hard-asserts and kills the
    // game on any entity that lacks the fragment, which is exactly the case this
    // plugin relies on failing softly.
    //
    // Why not the old xref: this used to be resolved from a call site inside
    // UMassEnemyRepresentationSubsystem::SynchronizeActorStateToMass at a
    // hardcoded +0x27D. That function is ~2.3KB of inlined fragment lookups, and
    // the offset silently rotted - in the 2026-09-04 build +0x27D lands on
    // `mov rdx, rbx`, six bytes shy of a call to a *different* function
    // (InternalGetConstSharedFragmentPtr). The old code read a rel32 from there
    // with no opcode check, so it produced a non-null garbage pointer that passed
    // every null check and only faulted when finally called - surfacing as
    // "can't find the heat fragment" everywhere, because the sampler's
    // __try/__except degrades the access violation to "no fragment".
    //
    // AddTemperatureToEntity is a much safer anchor: it is 0xB5 bytes total, its
    // AOB above is already required for heat writes anyway, and its single call
    // to InternalGetFragmentDataPtr is unambiguous - the caller passes
    // FCrMassTemperatureFragment::StaticStruct() and null-checks the result, so
    // it cannot be the Checked variant. Resolution MUST verify the 0xE8 opcode
    // and fail loudly rather than trusting the offset (see ResolvePatterns).
    static const size_t InternalGetFragmentDataPtr_CallOffsetFromAddTemp = 0x40;

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

    // UCrBuildingFunctionLibrary::AddTemperatureToEntity(UWorld*, FMassEntityHandle, float)
    // The game's own "apply heat to one building" primitive. Does two things:
    //   CurrentHeat = min(CurrentHeat + Value, UCrTemperatureDeveloperSettings::BuildingsHeatCapacity)
    //   CommandBuffer.AddTag<FCrMassTemperatureUpdatingTag>(Entity)
    // That second half is the whole reason we call it instead of writing the
    // fragment ourselves. UCrMassTemperatureProcessor::ConfigureQueries requires
    // FCrMassTemperatureProcessorTag (presence All) plus *either*
    // FCrMassEnviroWaveStageMovingTag or FCrMassTemperatureUpdatingTag (both
    // presence Any). Since we replace the vanilla AddEntitiesToWave body (which
    // is what would normally add the moving tag) with a direct heat write, an
    // entity we heat carries neither tag - so the temperature processor never
    // matches its archetype and the building stays overheated forever. Players
    // saw this as "production never restarts while AFK, but everything cools the
    // moment I move around": moving triggers
    // UCrMassTemperatureSubsystem::OnWeatherRegionsChanged, which blanket-adds
    // FCrMassTemperatureUpdatingTag to every registered temperature entity and
    // accidentally unsticks them.
    // The same tag also gates UCrHeaterCoolerProcessor, so heater/cooler
    // buildings were inert on wave-heated entities too.
    // Not a UFUNCTION (no exec thunk in the binary), hence the AOB rather than
    // an SDK reflection call. Takes UWorld* and resolves the entity manager and
    // the correct deferred command buffer internally, so we don't have to mirror
    // any of those offsets ourselves.
    // Note it does NOT stamp FCrMassTemperatureFragment::LastUpdateTime - callers
    // that apply a large jump in heat have to do that themselves or the cooling
    // processor will bill them for every second since the entity last simulated
    // (see SetEntityHeat in wave_optimiser.cpp).
    static const char* AddTemperatureToEntity =
        "48 89 5C 24 ?? 48 89 54 24 ?? 55 48 83 EC 30 0F 29 74 24 ?? 48 8B DA 0F 28 F2";

    // Signal name UCrMassTemperatureSignalProcessor listens for. Fired via
    // SignalEntities (above) after any direct CurrentHeat write - reset-all-heat
    // (heat_sampler.cpp) and wave heat injection (wave_optimiser.cpp) - so the
    // game's own signal processor re-evaluates the "Hold E to lower temperature"
    // prompt instead of it going stale. Hardcoded rather than user-configurable:
    // it names a specific native processor, not a tunable.
    static const char* TemperatureSignalName = "TemperatureChanged";
}
