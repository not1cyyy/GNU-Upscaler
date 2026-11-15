# GNU-Upscaler

GNU-Upscaler is a fork of OptiScaler for Linux gaming under Proton. It packages OptiPatcher alongside FidelityFX libraries with a native installation script.

Automated builds compile on Windows via GitHub Actions and publish to the repository releases page.

## Contents

- Features unique to GNU-Upscaler
- Installation
- Frame generation architecture
- GNU Adaptive Quality (GAQ Auto-DRS)
- OptiPatcher
- Recommended game engine tweaks
- Manual configuration
- Building from source

## Features unique to GNU-Upscaler

GNU-Upscaler includes several modifications not present in upstream OptiScaler.

### Multi-frame generation up to 10x
Upstream XeFG limits frame generation to 4x on Intel Arc hardware. GNU-Upscaler patches `libxess_fg.dll` in memory to unlock multipliers up to 10x on any GPU architecture.
- In-memory unlocking across `libxess_fg.dll` builds without external ASI loaders.
- Automatic swapchain buffer scaling that expands backbuffer counts to prevent queue starvation at 6x and 10x.
- Multiplier fallback when target frame intervals cannot be sustained.

### Adaptive Dynamic Frame Generation (ADFG)
A real-time frame generation controller that adjusts interpolation multipliers based on GPU load.
- Latency floor guard: when base rendering drops during heavy scenes, the multiplier clamps to 2x to prevent input latency spikes.
- Decouples interpolation pacing from coarse Windows timers under Wine.

### GNU Adaptive Quality (GAQ)
A closed-loop PID controller for Dynamic Resolution Scaling (DRS).
- Adjusts render resolution between configured bounds to maintain a steady framerate target.
- Functions across all upscaler inputs without requiring native game engine DRS support.
- Useful for handheld devices like the Steam Deck.

### VKD3D-Proton and Linux stability fixes
- Unreal Engine swapchain fix: resolves crashes when games invoke `SetColorSpace1` under VKD3D-Proton, configuring 10-bit swapchains (`DXGI_FORMAT_R10G10B10A2_UNORM`).
- Command list submission batching: aggregates DirectX 12 command lists to lower CPU overhead across the Wine-to-Vulkan boundary.
- Auto-detect inverted depth: inspects engine projection matrices at runtime to identify inverted Z-buffers.

### Anti-cheat safety guard
Monitors running processes for anti-cheat runtimes like Easy Anti-Cheat. If a protected multiplayer session is detected, GNU-Upscaler aborts DLL hooks to prevent account penalties.

### XeFG HUD mask support
Separates HUD masks within XeFG to eliminate UI ghosting at high multipliers without game-specific HUD mods.

### Ray reconstruction spoofing
Exposes NVIDIA DLSS 3.5 Ray Reconstruction to supported titles on non-NVIDIA GPUs under Proton.

### HDR pipeline fixes
- RCAS highlight preservation: protects specular highlight peaks from clipping when applying RCAS sharpening on HDR displays.
- Automatic HDR10 PQ color transfer detection: identifies HDR PQ curves and adjusts compute dispatch dimensions for output scaling.

### Integrated OptiPatcher
Bundles an upgraded OptiPatcher ASI plugin with a SIMD byte scanner. It includes generic Unreal Engine pattern detection plus external `patches.ini` support.

## Installation

Run the installer directly without cloning the repository:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/not1cyyy/GNU-Upscaler/main/install.sh)
```

Or pass options and a game path directly:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/not1cyyy/GNU-Upscaler/main/install.sh) --fsr4 --fg "/path/to/game"
```

Alternatively, clone the repository and run `install.sh` locally:

```bash
git clone https://github.com/not1cyyy/GNU-Upscaler.git
cd GNU-Upscaler
./install.sh "/path/to/game"
```

The installer fetches the newest release archive from GitHub and caches it in `~/.cache/gnu-upscaler/`. It detects game binary folders like `Binaries/Win64` automatically, and can auto-discover installed Steam games when run without arguments.

If you run `./install.sh` inside a game directory without arguments, it detects the current folder and prompts for confirmation.

### Common installer options

```bash
# Configure for FSR 4 and frame generation
./install.sh --fsr4 --fg "/path/to/game"

# Universal frame generation for games without native FG options
./install.sh --optifg "/path/to/game"

# Enable XeFG multi-frame generation (2x, 3x, 4x, 6x, 10x) and GAQ Auto-DRS
./install.sh --xefg --mfg=6x --gaq "/path/to/game"

# Use a different proxy DLL (default is dxgi.dll)
./install.sh --proxy=version.dll "/path/to/game"

# Re-download the latest build from GitHub
./install.sh --update "/path/to/game"

# Remove all installed files from the game
./install.sh --uninstall "/path/to/game"
```

### Steam launch option

When using `dxgi.dll` or another proxy under Proton, set the DLL override in Steam game properties:

```text
WINEDLLOVERRIDES="dxgi=n,b" %command%
```

Replace `dxgi` with the proxy name if you selected `version.dll` or `winmm.dll`.

## Frame generation architecture

OptiScaler supports two primary routes on Linux:

1. Games with in-game DLSS frame generation:
   Turn on Frame Generation in the game settings. OptiScaler intercepts the stream through its Streamline hook and outputs interpolated frames through FSR FG (`amd_fidelityfx_framegeneration_dx12.dll`) or XeFG (`libxess_fg.dll`).
2. Games without native frame generation options:
   Install with `--optifg` (or set `FGInput=upscaler` and `HUDFix=true` in `OptiScaler.ini`). This generates interpolated frames directly from the upscaler pass.

## GNU Adaptive Quality (GAQ Auto-DRS)

GAQ monitors frametimes every frame and adjusts the upscaler internal resolution to hit your target framerate.

To configure GAQ in `OptiScaler.ini`:

```ini
[GAQ]
; Enable closed-loop PID dynamic resolution scaling
Enabled=true

; Target framerate
TargetFps=60.0

; Render scale range (1.0 = native, 1.5 = Quality, 2.0 = Performance, 3.0 = Ultra Performance)
MinRatio=1.0
MaxRatio=2.5
```

## OptiPatcher

Many games check your GPU vendor ID on startup and hide DLSS or Frame Generation toggles if an NVIDIA card is absent.

Standard OptiScaler uses DXGI spoofing to pretend the GPU is NVIDIA (`0x10DE`). On Linux, spoofing often causes Unreal Engine titles to crash with `D3D12 not supported` or forces games down NVIDIA-specific shader paths that reduce performance on Mesa and RADV.

GNU-Upscaler bundles OptiPatcher as an ASI plugin (`plugins/OptiPatcher.asi`). Instead of spoofing your hardware, it patches the vendor check in memory on launch so the game exposes DLSS and Frame Generation toggles while continuing to run native AMD or Intel driver paths.

Features:
- Built-in signatures for known titles.
- Generic pattern scanner that identifies and patches unlisted Unreal Engine binaries.
- Optional `patches.ini` support in the plugins folder for custom game offsets without recompilation.

## Recommended game engine tweaks

### Unreal Engine games

Unreal Engine games occasionally dilate motion vectors before sending them to upscalers, causing ghosting with XeFG or Output Scaling. To disable dilation, add the following lines to the game's `Engine.ini`:

```ini
[SystemSettings]
r.NGX.DLSS.DilateMotionVectors=0
r.Streamline.DilateMotionVectors=0
```

If a game crashes when using FSR2 inputs in DX12 mode, add:

```ini
[SystemSettings]
r.FidelityFX.FSR2.UseNativeDX12=1
```

## Manual configuration

The installer creates `OptiScaler.ini` in the game folder next to the proxy DLL. Key settings:

```ini
[Upscalers]
; auto, ffx (FSR 4 / 3.1), xess, dlss, fsr22
Dx12Upscaler=auto

[FSR]
; auto (FSR 4 on supported GPUs), 1 (forces FSR 3.1)
UpscalerIndex=auto

[FrameGen]
; true or false
Enabled=true
; auto, fsrfg, xefg, dlssg
FGOutput=auto
; auto, dlssg (Streamline), upscaler (OptiFG), fsrfg
FGInput=auto

[XeFG]
; 1 = 2x, 2 = 3x, 3 = 4x, 5 = 6x, 9 = 10x
InterpolationCount=auto
; Automatically adjust generation multiplier based on load
DynamicMFG=false

[GAQ]
; Closed-loop PID Dynamic Resolution Scaling
Enabled=false
TargetFps=60.0
MinRatio=1.0
MaxRatio=2.5

[Plugins]
; Loads OptiPatcher.asi from the plugins folder
LoadAsiPlugins=true
```

## Building from source

Automated builds run on Windows using Visual Studio 2022:

```cmd
:: Build OptiScaler
cd OptiScaler
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiScaler.sln

:: Build OptiPatcher
cd ..\OptiPatcher
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiPatcher.sln
```

Artifacts are packaged into `GNU-Upscaler-latest.zip` and published by the repository workflow.
