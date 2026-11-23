<div align="center">

# GNU-Upscaler

**A Linux and Proton fork of OptiScaler with 10x Multi-Frame Generation, closed-loop Auto-DRS, sub-millisecond frame pacing, and native Steam deployment**

[![Build GNU-Upscaler](https://github.com/not1cyyy/GNU-Upscaler/actions/workflows/build.yml/badge.svg)](https://github.com/not1cyyy/GNU-Upscaler/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/not1cyyy/GNU-Upscaler?include_prereleases&label=Release&color=blue)](https://github.com/not1cyyy/GNU-Upscaler/releases/tag/latest)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%2F%20Proton%20%2F%20SteamOS-informational)](#installation)
[![License](https://img.shields.io/badge/License-GPL--3.0-green)](#)

*Bundles OptiScaler, OptiPatcher (`OptiPatcher.asi`), AMD FidelityFX (FSR 4 / 3.1 / FG), and Intel XeSS v3.0.2 (`libxess_fg.dll` + `libxell.dll`) in one automated package.*

</div>

<p align="center">
  <img src="docs/screenshots/stellar-blade-showcase.jpg" alt="Stellar Blade running under VKD3D-Proton at 1440p with GNU-Upscaler" width="92%" />
  <br />
  <sub><b>Stellar Blade</b> (2560x1440, VKD3D-Proton) on RX 7800 XT and R7 5700X: 181 FPS (5.5 ms) with a flat frametime graph (5.3 to 8.7 ms)</sub>
</p>

## Why GNU-Upscaler

Upstream OptiScaler targets Windows. Running modern upscaling and frame generation under Wine, Proton, and VKD3D-Proton brings coarse timer jitter, swapchain color-space crashes, `WINELOADER` Streamline blocks, and missing vendor unlocks. GNU-Upscaler fixes those at the engine and presentation layers and adds several features of its own:

| Feature | What it does | Best suited for |
| :--- | :--- | :--- |
| **10x XeFG unlock** | Patches `libxess_fg.dll` in memory (SIMD AOB) to unlock `2x`, `3x`, `4x`, `6x`, and `10x` on any GPU | High-refresh monitors (144Hz to 360Hz+) |
| **Adaptive Dynamic FG (ADFG)** | Shared `FG_DynamicController` for XeFG, FSR-FG, and DLSS-G with a `MinBaseFps` latency floor guard | Keeping input lag low in heavy combat |
| **GNU Adaptive Quality (GAQ)** | GPU-timestamp and sleep-aware PID Auto-DRS with CPU-bound detection and `MotionBoost` | Steam Deck, handhelds, and 4K targets |
| **Front-edge frame pacing** | JIT, VBlank PLL, `VK_EXT_present_timing`, `VK_GOOGLE_display_timing`, and GPU completion sync | Reducing frametime jitter under Proton |
| **FSR 4 auto-step-down** | Monitors FSR 4 GPU pass time (`Fsr4MaxGpuMs`) and steps models (`FP8`, `INT8`, `INT8 Lite`) and presets | Avoiding GPU-bound stutters |
| **Zero-copy & tile-skip HUDFix** | `ZeroCopyHudless`, 16x16 super-tile `HudCopyTileSkip`, and `PackedBuffers` (`R11G11B10F`) | Cutting frame gen overhead and VRAM bandwidth |
| **Integrated OptiPatcher** | SIMD (`memchr`) pattern scanner, generic UE unlock, `patches.ini`, and Nixxes `WINELOADER` bypass | Native AMD/Intel shader paths without spoofing |
| **`GnuUi` overlay** | Six-tab ImGui menu with one-click presets and live pipeline status | Quick in-game tuning with `Insert` |

## Contents

- [Installation](#installation)
- [Core features](#core-features)
  - [Multi-frame generation up to 10x and adaptive dynamic FG](#multi-frame-generation-up-to-10x-and-adaptive-dynamic-fg)
  - [GNU Adaptive Quality (GAQ Auto-DRS)](#gnu-adaptive-quality-gaq-auto-drs)
  - [Frame pacing, latency, and Vulkan parity](#frame-pacing-latency-and-vulkan-parity)
  - [FSR 4 model management and auto-step-down](#fsr-4-model-management-and-auto-step-down)
  - [Zero-copy HUDFix, DX12, and HDR pipeline](#zero-copy-hudfix-dx12-and-hdr-pipeline)
  - [OptiPatcher and game-specific Linux fixes](#optipatcher-and-game-specific-linux-fixes)
  - [Anti-cheat safety guard](#anti-cheat-safety-guard)
- [In-game overlay (`GnuUi`) and hotkeys](#in-game-overlay-gnuui-and-hotkeys)
- [Configuration reference (`OptiScaler.ini`)](#configuration-reference-optiscalerini)
- [Recommended game engine tweaks](#recommended-game-engine-tweaks)
- [Building from source](#building-from-source)

## Installation

### One-line Linux installer

You can run the installer without cloning the repository. It downloads the latest CI release from GitHub, caches it in `~/.cache/gnu-upscaler/`, finds your Steam libraries (native and Flatpak), resolves nested binary folders (such as `*/Binaries/Win64`, `bin/x64`, or `Client/WindowsNoEditor/HT/Binaries/Win64`), and can inject `WINEDLLOVERRIDES` into Steam's `localconfig.vdf` after making a `.bak` backup.

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/not1cyyy/GNU-Upscaler/main/install.sh)
```

Or pass flags and a game path directly:

```bash
# FSR 4 + Frame Generation with automatic Steam launch option injection
bash <(curl -fsSL https://raw.githubusercontent.com/not1cyyy/GNU-Upscaler/main/install.sh) --fsr4 --fg --auto-steam "/path/to/game"
```

### Common installer recipes

```bash
# Universal Frame Generation (OptiFG) for games without native FG toggles
./install.sh --optifg "/path/to/Stellar Blade"

# XeFG 6x Multi-Frame Generation + GAQ Auto-DRS
./install.sh --xefg --mfg=6x --gaq "/path/to/game"

# Fetch FSR 4 INT8 / FP8 extra model variants into custom/ (recommended, 'all', or tag filter)
./install.sh --fsr4 --fetch-extras "/path/to/game"

# Use an alternate proxy DLL (default: dxgi.dll)
./install.sh --proxy=version.dll "/path/to/game"

# Force re-download of the latest CI build
./install.sh --update "/path/to/game"

# Cleanly remove GNU-Upscaler and Steam launch overrides from a game
./install.sh --uninstall "/path/to/game"
```

<details>
<summary><b>All <code>install.sh</code> CLI flags and supported proxy DLLs</b></summary>

| Flag | Description |
| :--- | :--- |
| `--proxy=<name>` | Proxy DLL name: `dxgi.dll` (default), `winmm.dll`, `version.dll`, `d3d12.dll`, `dbghelp.dll`, `wininet.dll`, `winhttp.dll` |
| `--fsr4` / `--fsr3` / `--xess` / `--dlss` | Pre-configure `OptiScaler.ini` upscaler backend across DX12, DX11, and Vulkan |
| `--fg` | Enable Frame Generation (`FGOutput=auto`, `FGInput=auto`) |
| `--optifg` | Enable universal upscaler-driven Frame Generation (`FGInput=upscaler`, `HUDFix=true`) |
| `--xefg` | Set Frame Generation output to Intel XeFG |
| `--mfg=<mult>` | Enable XeFG Multi-Frame Generation (`2x`, `3x`, `4x`, `6x`, `10x`) |
| `--gaq` | Enable GNU Adaptive Quality (`[GAQ] Enabled=true`) |
| `--fetch-extras[=filter]` | Download FSR 4 INT8 & FP8 variants from `Optiscaler-Client` into `custom/` (`recommended`, `all`, or tag substring) |
| `--auto-steam` | Automatically inject `WINEDLLOVERRIDES` into Steam `localconfig.vdf` (with `.bak` backup) |
| `-u`, `--update` | Force re-check and download of the latest GitHub release |
| `--uninstall` | Remove proxy DLLs, companion DLLs, `OptiPatcher.asi`, and Steam launch overrides |

</details>

### Steam launch option

If you skip `--auto-steam` or decline the prompt, add the DLL override in Steam game properties:

```text
WINEDLLOVERRIDES="dxgi=n,b" %command%
```

Replace `dxgi` if you chose another proxy (for example `version=n,b`). To avoid double-pacing under Proton, unset external limiters like `DXVK_FRAME_RATE` or MangoHud `fps_limit`, and consider adding `PROTON_USE_NTSYNC=1`.

## Core features

### Multi-frame generation up to 10x and adaptive dynamic FG

Upstream Intel XeFG caps interpolation at 4x and locks it to Intel Arc GPUs. GNU-Upscaler scans `libxess_fg.dll` in memory with AOB patterns and unlocks `2x`, `3x`, `4x`, `6x`, and `10x` (`InterpolationCount` `1` through `9`) on AMD, Intel, and NVIDIA GPUs without an external ASI loader.

To keep 3x+ XeFG stable under VKD3D-Proton, it scales swapchain `minImageCount`, staggers sub-frames with an open-loop Virtual VSync clock (`FramerateLimitFgSubframeMode`), and bounds DXGI latency waits (`PaceOnSwapchain=true`, `MaxWaitMs=2`).

A shared dynamic frame generation controller (`FG_DynamicController`) works across XeFG (`DynamicMFG`), FSR-FG (DX12 and Vulkan), and DLSS-G (`DynamicFG`):

- Modulates multipliers and `2X <-> 1X` passthrough with hysteresis around `DynamicTargetFps` (or your refresh rate / FPS cap).
- Enforces a latency floor (`MinBaseFps`, default `45.0`) that drops Frame Generation down when native base rendering dips below the threshold.
- Detects camera cuts to reset interpolation history cleanly and avoid smearing.

### GNU Adaptive Quality (GAQ Auto-DRS)

GAQ (`GaqController.h`) is a closed-loop PID Dynamic Resolution Scaling engine that works across all upscaler inputs even when a game has no native DRS:

- Reads active GPU-timestamp and present frametimes (ignoring limiter and VSync sleep) and adjusts render scale in `0.05` steps (`dwellMs = 500ms`) between `MinRatio` and `MaxRatio`.
- Detects CPU-bound scenes (`gaqCpuBound`) so it stops lowering resolution when the GPU is not the bottleneck.
- Adds temporary `MotionBoost` (`0.20` by default) during fast camera or mouse turns, then smoothly restores quality when the camera settles.

### Frame pacing, latency, and Vulkan parity

GNU-Upscaler replaces coarse Wine/Windows timer sleeps with dedicated frame pacing across DX12 and Vulkan.

Three limiter modes are available via `FramerateLimitMode`:
- `0`: Front-Edge JIT (default). Sleeps post-present minus measured CPU work lead, then trims pre-present.
- `1`: Front-Edge Sync. RTSS-style pre-present wait aligned to VBlank/compositor phase (`FramerateLimitSyncMarginMs = 0.6ms`).
- `2`: Back-Edge. Drift-free post-present sleep.

On Vulkan (`VulkanFrameSync`), the pacer supports `VK_EXT_present_timing` with adaptive spin-tail pacing, `VK_GOOGLE_display_timing` (`desiredPresentTime` scheduling), `VK_KHR_present_wait` in-flight limits (`FramerateLimitMaxInFlight`, bypassed during active FG), calibrated GPU completion sync (`FramerateLimitGpuSync`), and MMCSS thread priority boost (`FramerateLimitPriorityBoost`).

All helper passes now have precompiled SPIR-V compute shaders on Vulkan, including Auto Exposure (`AE_Vk`), Bias (`Bias_Vk`), HUDless Compare Compute (`HCC_Shader_Vk`), Format Transfer, Output Scaling, and RCAS. Low-latency paths support NVIDIA Reflex, AMD Anti-Lag 2 (DX12) and Anti-Lag VK, Intel XeLL (`libxell.dll` with `xefg_xell` FPS cap sync), and LatencyFleX.

### FSR 4 model management and auto-step-down

`FSR4Models` supports `FP8`, `INT8`, `INT8 RDNA2`, and `INT8 Lite` providers, along with custom INT8/FP8 builds downloaded via `--fetch-extras`. If you set a millisecond budget in `Fsr4MaxGpuMs` (for example `2.5`), GNU-Upscaler tracks the FSR 4 GPU pass duration and steps down to a lighter model or preset when it exceeds the budget, stepping back up when headroom returns.

### Zero-copy HUDFix, DX12, and HDR pipeline

Several DX12 and shader optimizations reduce frame generation overhead and fix VKD3D-Proton issues:

- `ZeroCopyHudless` passes matching-format, swapchain-sized HUDless render targets directly to Frame Generation without a full-screen GPU copy.
- `HudCopyTileSkip` (default `true`) uses a 16x16 super-tile GPU activity mask in `HudCopy` to skip static non-HUD screen regions and identical-pixel writes.
- `PackedBuffers` (default `true`) allocates 32-bit `DXGI_FORMAT_R11G11B10_FLOAT` intermediate buffers instead of 64-bit `RGBA16F` when alpha is not needed.
- `CmdListFastPath` and `UseShards` provide a lock-free fast bypass for D3D12 command list hooks when tracking is inactive, plus sharded mutexes for multithreaded resource tracking.
- XeFG HUD mask support isolates UI elements inside XeFG to stop ghosting at 6x/10x, and DLSS 3.5 Ray Reconstruction spoofing exposes Ray Reconstruction on non-NVIDIA GPUs.
- Under VKD3D-Proton and HDR, GNU-Upscaler fixes Unreal Engine `SetColorSpace1` crashes with 10-bit `DXGI_FORMAT_R10G10B10A2_UNORM` swapchains, auto-detects HDR10 PQ color transfer curves and inverted depth buffers, and preserves specular highlights during RCAS sharpening.

### OptiPatcher and game-specific Linux fixes

Many games hide DLSS, Reflex, or Frame Generation toggles if an NVIDIA GPU is missing. Traditional DXGI spoofing (`0x10DE`) often crashes Unreal Engine under Proton (`D3D12 not supported`) or pushes games onto slower NVIDIA-specific shader paths on Mesa/RADV.

OptiPatcher (`plugins/OptiPatcher.asi`) patches vendor checks in memory so games stay on native AMD or Intel driver paths:

- Uses a SIMD `memchr` (AVX2/SSE2) pattern scanner across executable sections.
- Automatically detects and unlocks unlisted UE4/UE5 binaries.
- Reads `plugins/patches.ini` for custom AOB offsets without recompiling.
- Fixes **Ratchet & Clank: Rift Apart** on Wine/Proton by unlocking both DLSS and Reflex vendor checks (legacy and `v1.727.0.0+` binaries) and bypassing Nixxes' `WINELOADER` check that otherwise disables Streamline.

### Anti-cheat safety guard

GNU-Upscaler monitors running processes for active anti-cheat runtimes such as Easy Anti-Cheat and aborts DLL hooks if a protected multiplayer session is detected.

## In-game overlay (`GnuUi`) and hotkeys

The ImGui overlay (`GnuUi`) uses filled-track sliders, segmented toggles, inline hover tips, and six tabs:

1. **Quick Start**: Live Pipeline Status (upscaler, scale factor, active FG multiplier, Reflex markers), One-Click Presets (`Quality`, `Balanced`, `Performance`, `Battery Saver`, `Native AA`), and Quick Controls.
2. **Upscaling**: Backend selection, FSR 4 model/budget, upscale ratio, output scaling, and input settings.
3. **Frame Gen**: Output backend (FSR-FG / XeFG / DLSS-G), Multi-Frame Gen multiplier (`2x` to `10x`), Dynamic FG, Frame Limiter mode, Low Latency, and VSync.
4. **Image**: Sharpness (RCAS), API & texture tweaks, init flags, and magnifier.
5. **Interface**: Performance/FPS telemetry overlay, keybinds, UI scale, and themes.
6. **Advanced**: Game quirks, DX12/Vulkan engine tweaks, and logging.

| Default key | Action |
| :--- | :--- |
| `Insert` | Open or close the GNU-Upscaler overlay |
| `End` | Quick-toggle Frame Generation |
| `Page Up` | Toggle FPS and frametime telemetry HUD |
| `Page Down` | Cycle FPS telemetry overlay detail |

## Configuration reference (`OptiScaler.ini`)

`install.sh` creates `OptiScaler.ini` next to your proxy DLL and preserves it on updates. Key settings:

```ini
[Upscalers]
; auto, ffx (FSR 4 / 3.1), xess, dlss, fsr22
Dx12Upscaler=auto

[FSR]
; auto (FSR 4 on supported GPUs), 1 (forces FSR 3.1)
UpscalerIndex=auto
; 0=FP8, 1=INT8, 2=INT8 RDNA2, 3=INT8 Lite
Fsr4Model=auto
; GPU time budget in ms before auto-stepping down FSR 4 model/preset (0 = disabled)
Fsr4MaxGpuMs=auto

[FrameGen]
Enabled=true
; auto, fsrfg, xefg, dlssg
FGOutput=auto
; auto, dlssg (Streamline), upscaler (OptiFG), fsrfg
FGInput=auto
; Dynamic FG 2X <-> 1X modulation for FSR-FG & DLSS-G
DynamicFG=auto
DynamicTargetFps=auto
MinBaseFps=45.0

[XeFG]
; 1=2x, 2=3x, 3=4x, 5=6x, 9=10x
InterpolationCount=auto
DynamicMFG=false
PaceOnSwapchain=true
MaxWaitMs=2

[Framerate]
FramerateLimit=auto
; 0=Front-Edge JIT, 1=Front-Edge Sync (VBlank PLL), 2=Back-Edge
FramerateLimitMode=0
; 0=Off, 1=Auto GPU flush, 2=Strict RTSS SyncFlush
FramerateLimitGpuSync=1
FramerateLimitFgSubframeMode=1

[GAQ]
Enabled=false
TargetFps=60.0
MinRatio=1.0
MaxRatio=2.5
MotionBoost=0.20

[DX12]
PackedBuffers=true
CmdListFastPath=true

[HUDFix]
ZeroCopyHudless=false
HudCopyTileSkip=true
UseShards=true

[Plugins]
LoadAsiPlugins=true
```

## Recommended game engine tweaks

### Unreal Engine (`Engine.ini`)

Unreal Engine titles sometimes dilate motion vectors before passing them to upscalers, which can cause ghosting with XeFG or Output Scaling. Disable dilation in the game's `Engine.ini`:

```ini
[SystemSettings]
r.NGX.DLSS.DilateMotionVectors=0
r.Streamline.DilateMotionVectors=0
```

If an Unreal Engine game crashes when using FSR2 inputs in DX12 mode, add:

```ini
[SystemSettings]
r.FidelityFX.FSR2.UseNativeDX12=1
```

## Building from source

Automated builds compile on Windows via GitHub Actions (`.github/workflows/build.yml`) using Visual Studio 2022 (`Release | x64`) and publish `GNU-Upscaler-latest.zip` to the `latest` release.

```cmd
:: Build OptiScaler
cd OptiScaler
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiScaler.sln

:: Build OptiPatcher
cd ..\OptiPatcher
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiPatcher.sln
```

---

<div align="center">
  <sub>Built on <a href="https://github.com/cdozdil/OptiScaler">OptiScaler</a> for Linux, Proton, and SteamOS</sub>
</div>
