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
    // Call it with UWorld* (= the UCrMassEnviroWaveSubsystem Outer)
    // to get UMassEntitySubsystem*; FMassEntityManager* is then read through WaveOptimiserFields::EntityManager.
    static const char* GetSubsystemMassEntity_Fn =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC ?? ?? ?? ?? 4C 8B F1 48 8B CA";
    static const size_t GetSubsystemMassEntity_CallOffset = 0x2F;
    // No pattern on GetSubsystem<UMassEntitySubsystem> itself: every
    // GetSubsystem<T> instantiation has the same bytes apart from the call to
    // T::StaticClass, a per-build displacement, so the body matches 130+ places.
    // The key stays on the caller above, which the loader follows.

    // FMassEntityManager::InternalGetFragmentDataPtr:
    //   void* InternalGetFragmentDataPtr(FMassEntityManager*, FMassEntityHandle, const UScriptStruct*)
    // Found by its own pattern, whole function up to the second assert's line
    // number.
    //
    // It is one of three adjacent, near-identical template instantiations -
    // InternalGetConstSharedFragmentPtr, InternalGetFragmentDataChecked and
    // InternalGetFragmentDataPtr sit back to back and share a byte-identical
    // prologue. Picking the wrong one is not a soft failure:
    // InternalGetFragmentDataChecked hard-asserts and kills the game on any
    // entity that lacks the fragment, which is exactly the case this plugin
    // relies on failing softly. So the pattern has to reach the bytes that tell
    // them apart, and it does:
    //   - InternalGetConstSharedFragmentPtr keeps its local at rsp+0x48, where
    //     this function has it at rsp+0x40 (C6 44 24 40 00 / 48 8D 4C 24 40);
    //   - InternalGetFragmentDataChecked differs from this function in one
    //     byte only: the line number the second assert reports, 0x70A there and
    //     0x714 here (41 B8 14 07 00 00).
    // Every call and RIP-relative displacement is wildcarded, so the pattern
    // reads the same on every build; it matches exactly once on CL-127004 and
    // CL-128786.
    //
    // This used to be resolved as the rel32 target of the call at
    // AddTemperatureToEntity + 0x40, and before that from a call site inside
    // UMassEnemyRepresentationSubsystem::SynchronizeActorStateToMass at a
    // hardcoded +0x27D, an offset that silently rotted into a call to a
    // different function (InternalGetConstSharedFragmentPtr) and surfaced as
    // "can't find the heat fragment" everywhere. A pattern on the function
    // itself has no offset to rot, and the map's row for this key is the same
    // address (the loader refuses a pattern that disagrees with its row).
    static const char* InternalGetFragmentDataPtr =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? 57 48 83 EC 30 49 8B F0 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0 75 32 "
        "8B 44 24 4C 4C 8D 0D ?? ?? ?? ?? 89 44 24 28 48 8D 15 ?? ?? ?? ?? 41 B8 4A 07 00 00 89 5C 24 20 48 8D 0D ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 84 C0 74 02 90 CC 0F B6 47 60 48 8D 57 20 4C 8D 05 ?? ?? ?? ?? C6 44 24 40 00 48 8D 4C 24 40 4D 8B 04 C0 "
        "41 FF D0 8B D3 48 8B 08 4C 8B 41 10 48 8B C8 41 FF D0 48 8B F8 48 85 C0 75 26 4C 8D 0D ?? ?? ?? ?? 41 B8 14 07 00 00";

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
