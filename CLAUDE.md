# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is SDRangel

SDRangel is an open-source Qt5/Qt6, OpenGL 3.0+ SDR (Software Defined Radio) and signal analyzer frontend. It processes I/Q samples from various SDR hardware, applies signal processing and demodulation/modulation plugins, and exposes a WebAPI for remote control. It runs either as a desktop GUI application (`app/`) or a headless server (`appsrv/`).

Current version: **7.23.2**. C++17, CMake build system.

## Build

CMake presets are defined in `CMakePresets.json`. The default preset targets Linux with dependencies under `/opt/install/`.

```bash
# Configure (Linux, Qt5)
cmake --preset default

# Build
cmake --build --preset default -j$(nproc)

# Install (optional)
cmake --install build --prefix /opt/install/sdrangel

# Qt6 build
cmake --preset default-qt6
cmake --build --preset default-qt6 -j$(nproc)

# Windows
cmake --preset default-windows
cmake --build --preset default-windows
```

Key CMake options:
- `BUILD_GUI` / `BUILD_SERVER` — toggle GUI/server flavors
- `RX_SAMPLE_24BIT` — 24-bit internal DSP (default ON; affects `SDR_RX_SAMP_SZ` and `FixReal` type in `sdrbase/dsp/dsptypes.h`)
- `ENABLE_QT6` — build with Qt6 instead of Qt5
- `DEBUG_OUTPUT` — enable debug log messages
- `SANITIZE_ADDRESS` / `SANITIZE_MEMORY` — address/memory sanitizers

External dependencies are pointed to via `CMakePresets.json` cache variables (`AIRSPY_DIR`, `LIMESUITE_DIR`, `UHD_DIR`, etc.). Use the `sdrangel-docker` project for reproducible dependency environments.

## Testing

There is no central unit-test runner. Tests are exercised by building and running individual plugins or benchmark programs (`appbench/`, `sdrbench/`). Architecture SIMD detection tests live in `cmake/test/`. CI helper scripts are under `cmake/ci/`.

## Top-level Directory Structure

| Directory | Purpose |
|---|---|
| `app/` | GUI application entry point (`main.cpp`) |
| `appsrv/` | Headless server entry point (`main.cpp`) |
| `appbench/` | Benchmark application entry point |
| `sdrbase/` | Core framework shared by all apps and plugins |
| `sdrgui/` | Shared GUI components (Qt widgets, channel/device/feature GUI base classes) |
| `sdrsrv/` | Shared server-mode components |
| `settings/` | Application-level settings management |
| `plugins/` | All plugins — the primary extension mechanism |
| `devices/` | Hardware-specific device utility code (BladeRF, HackRF, LimeSDR, etc.) |
| `wdsp/` | Core DSP library (AGC, filters, demodulators) called by plugins |
| `ft8/` | FT8 modem module |
| `modemm17/` | M17 modem module |
| `qrtplib/` | RTP library used by modem plugins |
| `swagger/` | OpenAPI spec + generated Qt client/server code (WebAPI) |
| `httpserver/` | HTTP server powering the WebAPI |
| `logging/` | Logging framework |
| `scriptsapi/` | Python scripts for WebAPI automation |
| `doc/` | Documentation images (`img/`) and architecture models (`model/`) |

## Plugin System Architecture

Plugins live under `plugins/` and are divided into:
- `samplesource/` — SDR hardware receivers (single Rx I/Q stream)
- `samplesink/` — SDR hardware transmitters (single Tx I/Q stream)
- `samplemimo/` — MIMO devices (multiple synchronized Rx+Tx streams)
- `channelrx/` — receive channel demodulators/processors
- `channeltx/` — transmit channel modulators
- `channelmimo/` — MIMO channel processors
- `feature/` — standalone feature plugins (map, rotator control, satellite tracker, etc.)

Each plugin follows a consistent structure. Using `plugins/channelrx/demodam/` as a canonical example:
- `amdemodplugin.{h,cpp}` — `PluginInterface` implementation; registers the plugin with the system
- `amdemodsettings.{h,cpp}` — serializable settings class (saved/restored in presets)
- `amdemod.{h,cpp}` — `ChannelAPI` + `BasebandSampleSink`; the main channel object
- `amdemodbaseband.{h,cpp}` — baseband-thread processing object
- `amdemodsink.{h,cpp}` — innermost DSP sink (runs in baseband thread)
- `amdemodgui.{h,cpp,ui}` — Qt widget GUI (compiled only when `NOT SERVER_MODE`)
- `amdemodwebapiadapter.{h,cpp}` — maps WebAPI requests to plugin settings

The `SERVER_MODE` CMake variable controls whether GUI files are compiled; plugin `CMakeLists.txt` files use this to define two targets (`demodam` vs `demodamsrv`).

## Core Abstractions

**DSP types** (`sdrbase/dsp/dsptypes.h`):
- `FixReal` — `qint32` (24-bit mode) or `qint16` (16-bit mode)
- `Sample` — packed `{FixReal real, FixReal imag}` IQ sample
- `SampleVector` — `std::vector<Sample>`
- `Real` — `float`, `Complex` — `std::complex<float>`
- Scale constants: `SDR_RX_SCALEF` (8388608.0 or 32768.0)

**Message passing** (`sdrbase/util/message.h`):
- All inter-component communication uses `Message`-derived classes
- Naming: `Msg*` inner classes declared with `MESSAGE_CLASS_DECLARATION` macro
- `MessageQueue` (`sdrbase/util/messagequeue.h`) is used throughout; push with `.push()`, consume in handler
- `pushMessage()` is the thread-safe entry point on sinks/channels

**DSP Engines** (`sdrbase/dsp/`):
- `DSPDeviceSourceEngine` — drives a single Rx device, feeds baseband sinks
- `DSPDeviceSinkEngine` — drives a single Tx device, reads from sources
- `DSPDeviceMIMOEngine` — drives MIMO devices
- Each engine runs in its own thread

**DeviceSet** (`sdrbase/device/deviceset.h`):
- Groups a `DeviceAPI` (hardware interface) with its engine and channel list
- Holds `DSPDeviceSourceEngine`, `DSPDeviceSinkEngine`, or `DSPDeviceMIMOEngine`

**Pipes** (`sdrbase/pipes/`):
- `MessagePipes` / `DataPipes` — inter-plugin communication for sharing decoded data (e.g., AIS positions, APRS packets) between channel plugins and feature plugins

## WebAPI

- OpenAPI spec: `swagger/sdrangel/api/swagger/swagger.yaml`
- Generated Qt client code: `swagger/sdrangel/code/qt5/client/` — **do not edit manually**
- To regenerate: `swagger-codegen generate -i api/swagger/swagger.yaml -l qt5cpp -c qt5cpp-config.json -o code/qt5` (from `swagger/sdrangel/`)
- Each plugin exposes a `*WebAPIAdapter` class that translates REST calls to/from settings
- Python automation scripts are in `scriptsapi/`; see `scriptsapi/Readme.md`

## Key Conventions

- **Settings classes**: every plugin has a `*Settings` class with `serialize()`/`deserialize()` for preset persistence. Adding/removing settings fields requires careful handling for backwards compatibility with saved presets.
- **Worker threads**: hardware and long-running DSP tasks run in `*Worker` or `*Baseband` classes on Qt threads. Communication back to the main thread uses `MessageQueue`.
- **Generated code**: never edit files under `swagger/sdrangel/code/`. Modify the OpenAPI spec or generator config (`swagger/sdrangel/config/`) instead.
- **Plugin documentation**: each plugin has a `readme.md` explaining parameters — use these as the primary reference for plugin-specific behavior.
- **Reference plugin for new channel**: `plugins/channelrx/wdsprx/` is a well-documented canonical example.
