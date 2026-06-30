#pragma once

#include "plugin_interface.h"

namespace WaveOptimiserConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable WaveOptimiser"
		},
		{
			"WaveOptimiser",
			"MaxEntitiesPerTick",
			ConfigValueType::Integer,
			"256",
			"Max wave entities processed per tick when batching a large AddEntitiesToWave call",
			1.0f,
			512.0f
		},
		{
			"WaveOptimiser",
			"ShowDebugPanel",
			ConfigValueType::Boolean,
			"false",
			"Show the WaveOptimiser debug panel in the modloader UI (client only)"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;

			// Initialize config from schema - creates file with defaults if missing
			if (s_self)
			{
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		static int GetMaxEntitiesPerTick()
		{
			int value = s_self ? s_self->config->ReadInt(s_self, "WaveOptimiser", "MaxEntitiesPerTick", 32) : 32;
			if (value < 1) value = 1;
			if (value > 128) value = 128; // batch capacity cap - see mass_entity_set.h kMaxBatchCapacity
			return value;
		}

		static bool GetShowDebugPanel()
		{
			return s_self ? s_self->config->ReadBool(s_self, "WaveOptimiser", "ShowDebugPanel", false) : false;
		}

	private:
		static IPluginSelf* s_self;
	};
}
