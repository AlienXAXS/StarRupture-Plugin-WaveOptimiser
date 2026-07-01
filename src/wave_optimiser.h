#pragma once

#include "plugin_interface.h"
#include "mass_entity_set.h"
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
        size_t      queueDepth;          // entities currently waiting to be replayed
        size_t      currentBurstTotal;   // size of the in-progress capture burst (0 once drained)
        uint64_t    totalCaptured;       // lifetime count of entities ever deferred
        uint64_t    totalProcessed;      // lifetime count of entities replayed through the original fn
        uint64_t    totalBatchesReplayed;
        uint8_t     lastWaveType;
    };

    Stats GetStats();

    // Snapshot of the pointers needed to query FCrMassTemperatureFragment from outside
    // this translation unit (e.g. HeatSampler). All fields are null until the first
    // wave fires and g_cachedEntityMgr is resolved.
    using FragDataFn = void*(*)(void* entityMgr, FMassEntityHandle handle, const void* structPtr);
    struct HeatQueryContext
    {
        void*      entityMgr;     // FMassEntityManager*
        FragDataFn getFragDataFn; // FMassEntityManager::InternalGetFragmentDataPtr
        void*      tempStructPtr; // FCrMassTemperatureFragment UScriptStruct*
    };
    HeatQueryContext GetHeatQueryContext();

    // Resolves FMassEntityManager* directly from a UWorld* (as an opaque void*),
    // bypassing the g_cachedWaveSubsystem lazy-init path used by OnTick/GetHeatQueryContext.
    // Available as soon as Init() has resolved GetSubsystem<UMassEntitySubsystem> -
    // does not require a wave to have fired yet. Returns null if that scan failed
    // or if massEntitySubsystem can't be reached for this world.
    void* ResolveEntityManagerFromWorld(void* world);

    // Checks whether a handle still refers to a live entity in the given entity
    // manager. Call before any fragment/tag lookup on a handle that wasn't just
    // freshly handed to us by AddEntitiesToWave (e.g. historical handles, or
    // handles resolved from an arbitrary actor) - InternalGetFragmentDataPtr
    // hard-asserts (crashes the game) on a stale/destroyed entity instead of
    // failing softly. Fails closed: returns false if the underlying signature
    // wasn't resolved.
    bool IsEntityValid(void* entityMgr, FMassEntityHandle handle);
}
