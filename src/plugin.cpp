#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "wave_optimiser.h"
#include "wave_optimiser_ui.h"
#include "heat_sampler.h"
#include "wave_optimiser_fields.h"

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
	WAVE_OPTIMISER_PLUGIN_TARGET,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

// The wave queue and the cached FMassEntityManager/subsystem pointers are
// world-scoped: replaying queued entities after their world tore down
// hard-asserts inside FMassEntityManager (crash seen in the wild via
// DescribeEntityActor). Drop everything before the world goes away.
static void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	LOG_DEBUG("World '%s' ending - clearing wave queue and world-scoped caches", worldName ? worldName : "<unknown>");
	WaveOptimiser::OnWorldTeardown();
}

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	// Runs after GetPluginInfo and before PluginInit, and is the only context in
	// which the loader lets a plugin pattern scan. Resolve here, install from
	// PluginInit -- self->hooks is null for the duration of this event.
	__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_self = self;

		WaveOptimiserFields::DeclareFeatures(self, scanner);
		WaveOptimiserFields::Resolve(self, scanner);
		WaveOptimiser::ResolvePatterns(self, scanner);
		HeatSampler::ResolvePatterns(self, scanner);
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// First, before any early return: off its game build the loader refuses
		// a plugin whose PluginInit did not take the globals.
		PluginSdkRuntime::Init(self->hooks);

		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		// Initialize config system (optional - creates config file with defaults)
		WaveOptimiserConfig::Config::Initialize(self);

		// UI always runs regardless of enabled state - handles missing hooks gracefully.
		WaveOptimiserUI::Init(self);

		if (!WaveOptimiserConfig::Config::IsEnabled())
			LOG_WARN("Plugin is disabled in config file - wave batching inactive (hooks run in passthrough mode)");

		LOG_DEBUG("Config: Enabled=%s",
			WaveOptimiserConfig::Config::IsEnabled() ? "true" : "false");

		// Hooks and HeatSampler are installed regardless of the enabled toggle -
		// the detours themselves check Config::IsEnabled() per-call and pass
		// straight through to the original engine functions when disabled (see
		// wave_optimiser.cpp). That keeps the entity-manager/fragment-ptr
		// resolution HeatSampler depends on alive even while batching is off, so
		// the cursor sampler can still read ground-truth heat from the
		// unmodified wave.
		WaveOptimiser::Init(self);
		self->hooks->Engine->RegisterOnTick(&WaveOptimiser::OnTick);
		self->hooks->World->RegisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);

		self->hooks->Engine->RegisterOnTick(&HeatSampler::OnTick);

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
			g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);
			HeatSampler::Shutdown();
			g_self->hooks->Engine->UnregisterOnTick(&HeatSampler::OnTick);
			WaveOptimiserUI::Shutdown(g_self);
		}

		g_self = nullptr;
	}

} // extern "C"
