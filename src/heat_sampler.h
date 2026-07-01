#pragma once

#include "plugin_interface.h"
#include <cstdint>

namespace HeatSampler
{
    struct Result
    {
        bool    valid          = false; // an actor was under the crosshair
        bool    handleResolved = false; // GetEntityHandleFromActor produced a non-zero handle
        int32_t handleIndex    = 0;     // FMassEntityHandle.Index from GetEntityHandleFromActor
        int32_t handleSerial   = 0;     // FMassEntityHandle.SerialNumber from GetEntityHandleFromActor
        bool    entityValid    = false; // FMassEntityManager::IsEntityValid reported true for that handle
        bool    hasFragment    = false;
        float   heat           = 0.0f;
        char    actorName[128] = {};
    };

    // Scans for FCrMassActorReplicationHelper ctor used to resolve actor -> entity handle.
    // No-op on server builds (stubs return safe defaults).
    void Init(IPluginSelf* self);
    void Shutdown();

    // Must be called each game tick (registered via RegisterOnTick).
    // When enabled, raycasts from the local player's eye and reads
    // FCrMassTemperatureFragment::CurrentHeat on whatever actor is hit.
    void OnTick(float deltaSeconds);

    void   SetEnabled(bool enabled);
    bool   IsEnabled();
    Result GetLastResult();

    // Schedules a heat-reset pass on the next game tick: walks every actor
    // currently loaded in the world (no filtering by wave/building-grid
    // membership), resolves each one's Mass entity handle, and zeroes
    // FCrMassTemperatureFragment::CurrentHeat wherever the fragment exists.
    // Does not consult wave history - purely a snapshot of what's loaded right
    // now. Safe to call from any thread (e.g. the render thread from an ImGui
    // button click). No-op on server builds (no actor list there).
    void RequestResetAllHeat();
}
