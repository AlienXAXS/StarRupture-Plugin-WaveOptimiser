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

    // UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave prologue. Signature
    // (UCrMassEnviroWaveSubsystem*, const TSet<FMassEntityHandle>*) - no
    // WaveType param, unlike AddEntitiesToWave. Confirmed via IDA: mov rbx, rdx
    // (EntitiesToRemove) lands a bit later than Add's (after the stack-cookie
    // setup), but the leading mov [rsp+8], rbx / push sequence is unique enough
    // on its own.
    // XRef site inside FMassCommandBuffer::AddTag<FCrMassTemperatureUpdatingTag> at
    // function+0x70. Resolving: funcAddr = patternAddr - 0x70.
    static const char* AddTagUpdatingTag_XRef =
        "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? ?? ?? ?? ?? 41 0F 54 C2";

    // XRef site that calls FMassEntityManager::GetFragmentDataPtr<FCrMassTemperatureFragment>.
    // Resolving: read rel32 at xrefAddr+1, target = xrefAddr + 5 + rel32.
    static const char* GetFragmentDataPtr_XRef =
        "E8 ?? ?? ?? ?? 48 85 C0 74 ?? ?? ?? ?? 0F 82 ?? ?? ?? ?? 48 8D 9F";

    // FMassEntityManager::IsEntityValid prologue.
    // Signature: (FMassEntityManager*, FMassEntityHandle) -> bool
    static const char* IsEntityValid =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? 57 48 83 EC 20 48 8B DA 85 D2";

    // UCrBuildingFunctionLibrary::AddTemperatureToEntity prologue.
    // Signature: (UWorld*, FMassEntityHandle, float) - diagnostic logging hook only.
    static const char* AddTemperatureToEntity =
        "48 89 5C 24 ?? 48 89 54 24 ?? 55 48 83 EC";

    static const char* RemoveEntitiesFromWave =
        "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B DA 48 89 54 24";
}
