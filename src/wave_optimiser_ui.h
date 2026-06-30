#pragma once

#include "plugin_interface.h"

namespace WaveOptimiserUI
{
    // Registers the debug panel if WaveOptimiser/ShowDebugPanel is enabled and
    // the UI hooks are available (client only - hooks->UI is null on a
    // dedicated server build). No-op otherwise.
    void Init(IPluginSelf* self);

    void Shutdown(IPluginSelf* self);
}
