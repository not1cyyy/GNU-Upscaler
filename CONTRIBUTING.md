# Contributing to GNU-Upscaler

Thanks for wanting to help out with GNU-Upscaler. Most of the work here is focused on Linux/Proton compatibility, frame pacing, and keeping upscaler/frame-gen overhead low under Wine and VKD3D-Proton.

## Ways to contribute

- **Testing and bug reports:** Test the frame pacer (`Front-Edge Sync`, `Front-Edge JIT`), XeFG/FSR-FG/DLSS-G, and `install.sh` on different GPUs (RDNA 2/3/4, Intel Arc, NVIDIA), Steam Deck, and compositors (Gamescope, KWin, Mutter, Hyprland, Sway).
- **Game patches:** Add or update AOB signatures in `OptiPatcher` (or `patches.ini` entries) for games that block DLSS, Reflex, or Streamline under Proton.
- **Installer improvements:** Improve game/binary detection or distro compatibility in `install.sh`.
- **Code changes:** Fixes and optimizations in `OptiScaler/` or `OptiPatcher/`.

## Reporting bugs

When opening an issue, include:

1. **System info:** GPU, Mesa/driver version, distro, compositor/DE, and Proton version.
2. **Game and API:** Game name, rendering API (`DX12`, `DX11`, or `Vulkan`), and whether it runs through VKD3D-Proton or DXVK.
3. **Config and logs:** Your `OptiScaler.ini` and `OptiScaler.log` from the game's binary folder.
4. **Reproduction steps:** What settings were enabled (upscaler, FG mode, pacing mode, FPS limit) and what happened.

## Repository layout

- `OptiScaler/`: Main `OptiScaler.dll` source (hooks, upscalers, framegen, `GnuUi` overlay, `FrameLimit`, `VulkanFrameSync`, `GaqController`).
- `OptiPatcher/`: `OptiPatcher.asi` source (`Scanner.cpp`, `dllmain.cpp`) for in-memory vendor-check and Wine/Proton bypasses.
- `install.sh`: Linux installer script (fetches CI builds, detects Steam/Flatpak libraries, configures `OptiScaler.ini` and `localconfig.vdf`).
- `.github/workflows/build.yml`: Automated Windows MSVC build workflow.

## Building locally

### Windows (Visual Studio 2022 / MSBuild)

Both solutions target `Release | x64` and require submodules (`git submodule update --init --recursive`):

```cmd
:: Build OptiScaler
cd OptiScaler
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiScaler.sln

:: Build OptiPatcher
cd ..\OptiPatcher
msbuild /m /p:Configuration=Release /p:Platform=x64 OptiPatcher.sln
```

Note that `OptiScaler` expects `libxess_fg.dll` and `libxell.dll` (Intel XeSS v3.0.2) in `OptiScaler/external/xess/bin/` if you want the XeFG runtime bundled.

### Linux

There is no native Linux MSVC build; CI compiles on `windows-latest` via `.github/workflows/build.yml`. If you only edit `install.sh` or documentation, you can test `install.sh` locally on Linux without rebuilding the DLLs.

## Pull requests

- Keep PRs focused on one fix or feature so they are easy to review.
- Test your changes in-game under Proton before opening a PR when possible, and mention what hardware/game you tested on.
- For `install.sh` changes, test both fresh installs and `--uninstall` / existing-install prompts.
- Match the repository's commit style (short lowercase imperative messages, e.g. `fix vulkan present wait edge case`).
- Do not commit game binaries, personal `OptiScaler.log` files, or third-party proprietary DLLs.
