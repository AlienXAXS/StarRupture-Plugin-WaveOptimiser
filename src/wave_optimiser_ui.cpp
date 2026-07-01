#include "wave_optimiser_ui.h"
#include "wave_optimiser.h"
#include "heat_sampler.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <cstdio>
#include <cstring>

namespace WaveOptimiserUI
{
    namespace
    {
        WidgetHandle g_widget = nullptr;

        void RenderWidget(IModLoaderImGui* imgui)
        {
            const WaveOptimiser::Stats stats = WaveOptimiser::GetStats();

            imgui->SeparatorText("Hook status");
            imgui->Text(stats.hookInstalled ? "AddEntitiesToWave hook: installed" : "AddEntitiesToWave hook: NOT installed");

            if (!WaveOptimiserConfig::Config::IsEnabled())
                imgui->TextDisabled("Plugin disabled in config - hooks are passing through unmodified (vanilla wave)");

            imgui->SeparatorText("Live queue");

            char line[160];
            std::snprintf(line, sizeof(line), "Queue depth: %zu", stats.queueDepth);
            imgui->Text(line);

            std::snprintf(line, sizeof(line), "Max entities per tick: %d", WaveOptimiserConfig::Config::GetMaxEntitiesPerTick());
            imgui->Text(line);

            std::snprintf(line, sizeof(line), "Last wave type: %u", static_cast<unsigned>(stats.lastWaveType));
            imgui->Text(line);

            if (stats.currentBurstTotal > 0)
            {
                const size_t done = stats.currentBurstTotal - stats.queueDepth;
                const float fraction = static_cast<float>(done) / static_cast<float>(stats.currentBurstTotal);

                std::snprintf(line, sizeof(line), "%zu / %zu", done, stats.currentBurstTotal);
                imgui->ProgressBar(fraction, 0.0f, 0.0f, line);
            }
            else
            {
                imgui->TextDisabled("No batch in progress");
            }

            imgui->SeparatorText("Heat reset");

            if (imgui->Button("Reset All Heat to Zero"))
                HeatSampler::RequestResetAllHeat();

            imgui->SeparatorText("Heat sampler");

            if (imgui->Button(HeatSampler::IsEnabled() ? "Stop sampling" : "Sample heat at cursor"))
                HeatSampler::SetEnabled(!HeatSampler::IsEnabled());

            if (HeatSampler::IsEnabled())
            {
                const HeatSampler::Result result = HeatSampler::GetLastResult();
                if (result.valid)
                {
                    std::snprintf(line, sizeof(line), "Actor: %s", result.actorName);
                    imgui->Text(line);
                    if (result.hasFragment)
                    {
                        std::snprintf(line, sizeof(line), "CurrentHeat: %.6f", result.heat);
                        imgui->Text(line);
                    }
                    else if (!result.handleResolved)
                        imgui->TextDisabled("No Mass entity mapped to this actor");
                    else
                        imgui->TextDisabled("No temperature fragment on this actor");

                    // Diagnostic only. Cross-check Index/SerialNumber against the
                    // "captured Add entity Index=... SerialNumber=..." Trace log line for
                    // this same actor (logged by Detour_AddEntitiesToWave, ground truth
                    // from the engine) to confirm the helper's offset-0 handle assumption.
                    // IsEntityValid's AOB match is separately unverified and has been
                    // observed rejecting known-valid entities - not used to gate the
                    // fragment read above.
                    if (result.handleResolved)
                    {
                        std::snprintf(line, sizeof(line), "(debug) Handle: Index=%d SerialNumber=%d",
                            result.handleIndex, result.handleSerial);
                        imgui->TextDisabled(line);

                        std::snprintf(line, sizeof(line), "(debug) IsEntityValid: %s",
                            result.entityValid ? "true" : "false");
                        imgui->TextDisabled(line);
                    }
                }
                else
                    imgui->TextDisabled("Not looking at an actor");
            }

            imgui->SeparatorText("Lifetime totals");

            std::snprintf(line, sizeof(line), "Entities captured: %llu", static_cast<unsigned long long>(stats.totalCaptured));
            imgui->Text(line);
            std::snprintf(line, sizeof(line), "Entities replayed: %llu", static_cast<unsigned long long>(stats.totalProcessed));
            imgui->Text(line);

            std::snprintf(line, sizeof(line), "Batches replayed: %llu", static_cast<unsigned long long>(stats.totalBatchesReplayed));
            imgui->Text(line);
        }

        // Registers the widget on first use - cheap to call repeatedly, no-ops
        // once g_widget is already set.
        void EnsureRegistered(IPluginSelf* self)
        {
            if (g_widget || !self->hooks->UI)
                return;

            // Must outlive this call - RegisterWidget stores this pointer rather
            // than copying the struct, so a stack-local desc would leave the
            // modloader holding a dangling pointer once we return.
            static PluginWidgetDesc desc{};
            desc.name = "WaveOptimiser Debug";
            desc.renderFn = &RenderWidget;
            desc.windowHints = nullptr;

            g_widget = self->hooks->UI->RegisterWidget(&desc);
            if (g_widget)
            {
                LOG_INFO("WaveOptimiserUI: debug widget registered");
                self->hooks->UI->SetWidgetVisible(g_widget, true);
            }
            else
                LOG_WARN("WaveOptimiserUI: failed to register debug widget");
        }

        void Unregister(IPluginSelf* self)
        {
            if (g_widget && self->hooks->UI)
            {
                self->hooks->UI->UnregisterWidget(g_widget);
                g_widget = nullptr;
            }
        }

        // Fired whenever any plugin config value changes via the modloader UI -
        // lets the widget appear/disappear live instead of only being checked
        // once at PluginInit (which misses toggling it on after the game has
        // already started).
        void OnConfigChanged(const char* section, const char* key, const char* newValue)
        {
            if (std::strcmp(section, "WaveOptimiser") != 0 || std::strcmp(key, "ShowDebugPanel") != 0)
                return;

            IPluginSelf* self = GetSelf();
            if (!self)
                return;

            const bool enabled = std::strcmp(newValue, "true") == 0;
            LOG_DEBUG("WaveOptimiserUI: ShowDebugPanel changed to %s", enabled ? "true" : "false");

            if (enabled)
                EnsureRegistered(self);
            else
                Unregister(self);
        }
    } // namespace

    void Init(IPluginSelf* self)
    {
        if (!self->hooks->UI)
        {
            LOG_DEBUG("WaveOptimiserUI: UI hooks unavailable (server/generic build) - debug panel not available");
            return;
        }

        self->hooks->UI->RegisterOnConfigChanged(&OnConfigChanged);

        if (WaveOptimiserConfig::Config::GetShowDebugPanel())
            EnsureRegistered(self);
        else
            LOG_DEBUG("WaveOptimiserUI: debug panel disabled in config - not registering yet");
    }

    void Shutdown(IPluginSelf* self)
    {
        if (self->hooks->UI)
            self->hooks->UI->UnregisterOnConfigChanged(&OnConfigChanged);

        Unregister(self);
    }
} // namespace WaveOptimiserUI
