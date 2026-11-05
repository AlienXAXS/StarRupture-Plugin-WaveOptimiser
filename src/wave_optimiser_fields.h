#pragma once

#include "plugin_interface.h"
#include "Engine_classes.hpp"
#include "Chimera_structs.hpp"
#include "ChimeraMassCommon_structs.hpp"
#include "MassActors_structs.hpp"
#include "plugin_map.h"

#include <cstdint>
#include <cstring>
#include <iterator>

// Every member of a game class the plugin reads, taken from the loader's map
// for the running game build (see plugin_map.h). Resolved once in
// OnPluginLoadHooks; read on the game thread with In(object), which is null
// when the object is null or the member did not resolve, and the feature
// using it is skipped. Engine members (UObject::Outer, UWorld) are not game
// classes and stay compiled in.

#define WAVE_OPTIMISER_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace WaveOptimiserFields
{
	// FCrMassTemperatureFragment, reached through FMassEntityManager's
	// fragment lookup.
	inline auto CurrentHeat    = WAVE_OPTIMISER_FIELD(FCrMassTemperatureFragment, CurrentHeat);
	inline auto LastUpdateTime = WAVE_OPTIMISER_FIELD(FCrMassTemperatureFragment, LastUpdateTime);

	// FMassActorFragment, reached the same way.
	inline auto FragmentActor = WAVE_OPTIMISER_FIELD(FMassActorFragment, Actor);

	// The game's own constructor fills a stack buffer of this struct and the
	// actor's identity is read from it, so its size is checked on the running
	// build.
	inline auto ReplicationHelperActor = WAVE_OPTIMISER_FIELD(FCrMassActorReplicationHelper, Actor);
	inline auto ReplicationHelperSize  = SR_SIZE_CHECK(FCrMassActorReplicationHelper);

	// UMassActorSubsystem::ActorManager is a TSharedPtr the Game-SDK folds into
	// Pad_38 (not reflected), so there is no SDK name to take an offset from. A
	// stale offset would read whatever now sits there, so on a build the loader
	// has no row for, the plugin is refused instead. CL-127004: 0x38, 0x10
	// bytes. The row below is this plugin's own until the loader's map carries
	// it. The entity subsystem's manager is the SDK's PluginMap::MassEntityManagerField.
	inline auto ActorManager = PluginField<PluginMap::SharedRef>{ { "UMassActorSubsystem::ActorManager",
		"UMassActorSubsystem", "ActorManager", PLUGIN_FIELD_NATIVE, sizeof(PluginMap::SharedRef), 0x38 } };

	inline const PluginFieldCandidate kActorManagerRows[] = {
		{ 127004, 0x38, static_cast<uint32_t>(sizeof(PluginMap::SharedRef)), 0, nullptr },
	};

	// The parts of the plugin that can be off while the rest runs. The heat
	// tools reuse the batching side's entity manager, fragment lookup and heat
	// member, so those keys sit in both lists. SignalEntities is shared too.
	inline PluginMap::Feature WaveBatching{ "batching", "Rupture heat batching",
		{ "UCrMassEnviroWaveSubsystem::AddEntitiesToWave", "UCrMassEnviroWaveSubsystem::RemoveEntitiesFromWave",
		  "UWorld::GetSubsystem<UMassEntitySubsystem>", "UCrBuildingFunctionLibrary::AddTemperatureToEntity",
		  "FMassEntityManager::InternalGetFragmentDataPtr", "FMassEntityManager::IsEntityValid",
		  "UMassSignalSubsystem::SignalEntities", PluginMap::MassEntityManagerField.key,
		  "FCrMassTemperatureFragment::CurrentHeat", "FCrMassTemperatureFragment::LastUpdateTime",
		  "FMassActorFragment::Actor" },
		{ "WaveOptimiser.MaxEntitiesPerTick" } };
	inline PluginMap::Feature HeatTools{ "heat-tools", "Heat reset and heat readout under the cursor",
		{ "FCrMassActorReplicationHelper::FCrMassActorReplicationHelper",
		  "FMassActorManager::GetEntityHandleFromActor", "UMassSignalSubsystem::SignalEntities",
		  "UWorld::GetSubsystem<UMassEntitySubsystem>", "FMassEntityManager::InternalGetFragmentDataPtr",
		  PluginMap::MassEntityManagerField.key, "UMassActorSubsystem::ActorManager",
		  "FCrMassActorReplicationHelper::Actor", "FCrMassActorReplicationHelper",
		  "FCrMassTemperatureFragment::CurrentHeat", "FMassActorFragment::Actor" },
		{} };

	// Call before Resolve, so a miss lands on a feature and not on the plugin.
	inline void DeclareFeatures(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, WaveBatching, HeatTools);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool nativeOk = PluginMap::ResolveMassEntityManager(scanner, self);
		nativeOk = PluginMap::ResolveField(scanner, self, ActorManager,
			kActorManagerRows, static_cast<uint32_t>(std::size(kActorManagerRows))) && nativeOk;
		return PluginMap::ResolveFields(scanner, self,
			CurrentHeat, LastUpdateTime, FragmentActor,
			ReplicationHelperActor, ReplicationHelperSize) && nativeOk;
	}

	// The FMassActorManager* a UMassActorSubsystem holds, or null.
	inline void* ActorManagerOf(const void* subsystem)
	{
		const PluginMap::SharedRef* at = ActorManager.In(subsystem);
		return at ? at->Object : nullptr;
	}

	// The live actor an FMassActorFragment points at, or null.
	inline SDK::AActor* ActorOf(const void* fragment)
	{
		const auto* weak = FragmentActor.In(fragment);
		return weak ? weak->Get() : nullptr;
	}

	// The {object index, serial number} identity of the actor in a
	// FCrMassActorReplicationHelper, which is the key the actor manager looks
	// entities up by. Zero when the member did not resolve.
	inline uint64_t ActorKeyOf(const void* helper)
	{
		uint64_t key = 0;
		if (const auto* weak = ReplicationHelperActor.In(helper))
			std::memcpy(&key, weak, sizeof(key));
		return key;
	}
}
