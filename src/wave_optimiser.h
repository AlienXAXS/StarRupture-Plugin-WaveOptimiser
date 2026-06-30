#pragma once

#include "plugin_interface.h"
#include <cstdint>
#include <cstddef>

namespace WaveOptimiser
{
    // Installs the AddEntitiesToWave and RemoveEntitiesFromWave hooks. Returns
    // false (logged) if either pattern can't be resolved - plugin still
    // loads, just inert. Both calls are deferred into one ordered queue so
    // replay preserves the original Add/Remove call order - hooking only one
    // of the pair would let the real (synchronous) Remove run ahead of
    // queued Adds and corrupt EntitiesInWave membership / wave-stage tag
    // state, which is what drives heat processing downstream.
    bool Init(IPluginSelf* self);

    void Shutdown(IPluginSelf* self);

    // Drains up to Config::GetMaxEntitiesPerTick() queued entities by replaying
    // them through the original engine functions in small batches, preserving
    // the original Add/Remove interleaving.
    void OnTick(float deltaSeconds);

    // Live snapshot for the debug panel / future telemetry. Cheap to call -
    // safe to poll every render frame.
    struct Stats
    {
        bool        hookInstalled;
        size_t      queueDepth;        // entities currently waiting to be replayed
        size_t      currentBurstTotal; // size of the in-progress capture burst (0 once drained)
        uint64_t    totalCaptured;     // lifetime count of entities ever deferred
        uint64_t    totalProcessed;    // lifetime count of entities replayed through the original fn
        uint64_t    totalBatchesReplayed;
        uint8_t     lastWaveType;
    };

    Stats GetStats();
}

// Temporary diagnostic hook — logs every UCrBuildingFunctionLibrary::AddTemperatureToEntity
// call so we can confirm the value passed and whether our wave entities ever receive it.
// Remove once the heat chain is understood.
namespace IsEntityValidDiag
{
    bool  Init(IPluginSelf* self);
    void  Shutdown(IPluginSelf* self);
    void* GetEntityMgr();
}

namespace AddTemperatureDiag
{
    bool Init(IPluginSelf* self);
    void Shutdown(IPluginSelf* self);
}
