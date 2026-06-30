# WaveOptimiser -- StarRupture Plugin

A [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) mod loader plugin.

**Target:** Game client and server

---

## What It Does

On large saves, StarRupture triggers heat waves that add tens of thousands of entities to the temperature system in a single synchronous call. This causes multi-second freezes (or an outright hang) while the game processes every entity before returning control to the player.

WaveOptimiser hooks `UCrMassEnviroWaveSubsystem::AddEntitiesToWave` and `RemoveEntitiesFromWave`, captures the incoming entity handles into an internal queue, and returns immediately. It then drains the queue in small configurable batches each tick, spreading the work across many frames so the game stays responsive.

For each entity added to a wave, the plugin directly injects heat into the entity's `FCrMassTemperatureFragment` and re-adds `FCrMassTemperatureUpdatingTag` via the Mass command buffer, so the temperature processor picks it up correctly on the next tick without relying on the original wave entry path.

---

## Configuration

Settings live in `Plugins\config\WaveOptimiser.ini`, created on first launch.

---

## Installation

1. Download the latest release from the [Releases page](../../releases/latest):
   - **Client:** `WaveOptimiser_Plugin-Client-*.zip`
   - **Server:** `WaveOptimiser_Plugin-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder -- it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\WaveOptimiser.ini` and confirm `Enabled=true`.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Game updated, plugin broken | The hook uses byte-pattern scanning. A game update may shift the patterns -- wait for a plugin update. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open the `.sln` file, and build one of the following configurations:

| Configuration | Output |
|---|---|
| `Client Release\|x64` | Client DLL |
| `Server Release\|x64` | Server DLL |

The output DLL will be placed in `build\<config>\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.
