# GNU-Upscaler

GNU-Upscaler is a high-performance in-game resolution upscaling, frame generation, and anti-lag overlay system built for modern graphics APIs (DirectX 11, DirectX 12, and Vulkan), based on an enhanced, human-designed OptiScaler architecture.

---

## Key Features & Customizations

### 1. Modern In-Game Overlay UI
* **Clean Horizontal Navigation**: Replaced the cluttered multi-column sidebar with a sleek, text-only top navigation tab bar:
  * **Quick Start**: Pipeline telemetry & one-click performance presets (*Quality*, *Balanced*, *Performance*, *Battery Saver / Handheld*, *Native AA*).
  * **Upscaling**: Fine-grained backend controls (XeSS, AMD FSR 2/3.1, NVIDIA DLSS), custom scale overrides, and input masks.
  * **Frame Gen**: Multi-vendor frame generation selection, frame pacing & caps, low-latency injection (Reflex, Anti-Lag 2, XeLL).
  * **Image**: CAS & Contrast Adaptive Sharpening, Anisotropic Filtering, automatic Mipmap Bias tuning, and Magnifier.
  * **Interface**: Telemetry HUD customization (presets, custom rows, coordinates, graphs) and keybindings.
  * **Advanced**: Engine-specific quirks, DX11/12 synchronization barriers, and diagnostic logging.
* **Proportional 190px Control Geometry**: Controls maintain fixed, comfortable widths across 1080p, 1440p, 4K, and ultra-wide aspect ratios.
* **Widget System (`GnuUi`)**:
  * **`FillSlider`**: Modern progress-style sliders with right-aligned numeric readouts inside the track.
  * **Minimalist `Checkbox`**: 12px clean square indicator with instant visual feedback.
  * **Hover-Activated Documentation**: Seamless tooltips on label hover without noisy `(?)` buttons.
  * **Live Background Blur**: Real-time compute blurred game frame behind the transparent window.
* **Signature GNU Emerald Palette**: Dark theme with `#14171c` backdrop, `#23272e` frames, and vibrant `#2ecc71` GNU Emerald accents.

### 2. Anti-Ghosting & Input Stability
* **Toggle Gate & Press Latch**: Implements debounced shortcut handling (`ToggleGate.h` and `KeyPressLatch.h`) to completely prevent double-toggle bugs and dropped keystrokes in high-framerate or heavy-stutter scenes.

---

## Repository Structure

* `OptiScaler/`: Core C++ native upscaler hook and proxy DLLs.
  * `menu/`: In-game overlay rendering and input subsystems.
  * `menu/gnu_ui/`: GNU-Upscaler's custom ImGui widget suite (`GnuUi.h` / `GnuUi.cpp`).
  * `menu/input/`: Debounced input handling and edge latches.
* `AMD-NR---OptiScaler/`: Cloned reference architecture for inspection and inspiration.

---

## License

GPL-3.0-or-later
