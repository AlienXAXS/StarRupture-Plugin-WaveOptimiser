#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "wave_optimiser.h"
#include "wave_optimiser_ui.h"

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// Wave entity processing is server-authoritative, but this DLL is built for
// both Client and Server configurations (see Shared.props) - declare
// whichever target matches the actual build so the modloader only loads this
// binary into the matching executable.
#if defined(MODLOADER_SERVER_BUILD)
#define WAVE_OPTIMISER_PLUGIN_TARGET PLUGIN_TARGET_SERVER_ONLY
#elif defined(MODLOADER_CLIENT_BUILD)
#define WAVE_OPTIMISER_PLUGIN_TARGET PLUGIN_TARGET_CLIENT_ONLY
#else
#error "WaveOptimiser must be built with either MODLOADER_SERVER_BUILD or MODLOADER_CLIENT_BUILD defined (see Shared.props)"
#endif

static PluginInfo s_pluginInfo = {
	"WaveOptimiser",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Optimises wave/horde spawning in StarRupture",
	PLUGIN_INTERFACE_VERSION,
	WAVE_OPTIMISER_PLUGIN_TARGET
};

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		// Initialize config system (optional - creates config file with defaults)
		WaveOptimiserConfig::Config::Initialize(self);

		// These always run regardless of enabled state - UI handles missing hooks
		// gracefully, diagnostic hooks need to observe the unhooked code path.
		WaveOptimiserUI::Init(self);
		IsEntityValidDiag::Init(self);
		AddTemperatureDiag::Init(self);

		// Check if plugin is enabled via config
		if (!WaveOptimiserConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file - wave batching inactive, diagnostic hook only");
			return true;
		}

		LOG_DEBUG("Config: MaxEntitiesPerTick=%d ShowDebugPanel=%s",
			WaveOptimiserConfig::Config::GetMaxEntitiesPerTick(),
			WaveOptimiserConfig::Config::GetShowDebugPanel() ? "true" : "false");

		WaveOptimiser::Init(self);
		self->hooks->Engine->RegisterOnTick(&WaveOptimiser::OnTick);

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_self)
		{
			WaveOptimiser::Shutdown(g_self);
			g_self->hooks->Engine->UnregisterOnTick(&WaveOptimiser::OnTick);
			WaveOptimiserUI::Shutdown(g_self);
			IsEntityValidDiag::Shutdown(g_self);
			AddTemperatureDiag::Shutdown(g_self);
		}

		g_self = nullptr;
	}

} // extern "C"
