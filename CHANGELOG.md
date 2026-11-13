# Changelog

All notable changes to the **SteelTempSaint Donkey Kong Bananza PC Recomp (DKB)** project are documented in this file.

---

## [Alpha 1g] - 2026-11-13

### Native resolution, DLSS Super Resolution, and safe defaults
- The picture and HUD follow Game Render Resolution. 1920×1080, 2560×1440, and 3840×2160 are real render sizes. A new install stays at 1920×1080 windowed.
- NVIDIA DLSS Super Resolution follows the size the game is already drawing. DLAA, Quality, and Balanced use preset K. Performance uses M. Ultra Performance uses L. The FPS line shows the live mode.
- Motion vectors are dilated camera reprojection, matched to the previous frame where something moves on its own.
- Opening the in-game menu no longer aborts, and hit-effect textures are drawn again.
- Replay lookups, vertex rebinds, command-buffer walks, and all-zero texture uploads do less work. The picture is unchanged.
- A new install, and Restore Defaults, uses upscaling off, frame generation off, Reflex off, and the default color profile. An update keeps the `dkb.ini` already on that PC. No pipeline cache is shipped.

---

## [Alpha 1f] - 2026-11-08

### 🍌 Native Nodix Super Motion, Handheld/Docked Scaling & Optical Flow Fixes (Alpha 1f)
- **100% Native In-Engine Nodix Super Motion SMRS + Frame Generation:**
  - Integrated Nodix Super Motion directly into `dkb.exe` and `DKBLauncher.exe` with zero external apps.
  - Eliminated all dependencies on `Mfg Super Motion.exe`, preventing window focus conflicts, alt-tab capture drops, and background process clutter.
- **Dynamic Handheld (1080p) & Docked (1620p) SMRS Workload Scaling:**
  - Resolved the recomp's fixed 1620p virtual display limitation by hooking SMRS presets directly into `nn::oe::GetOperationMode()` in `runtime/ov_system.c`.
  - **Balanced & Performance Presets:** Switch the engine dynamically to Handheld mode (`1920x1080`), reducing GPU pixel fill rate by **55.6%** for an immediate **35–50% base FPS boost**.
  - **Quality & Native (DLAA) Presets:** Switch the engine dynamically to Docked mode (`2880x1620`) for full 1620p resolution with edge reconstruction.
  - Relaxed FSR/RCAS bypass logic in `runtime/vk_fsr.c` to allow edge sharpening on 1080p 1:1 scale.
- **Dedicated Sharpness & Optical Flow Controls:**
  - Fixed duplicate "FSR sharpness" slider bug in the in-game menu (`runtime/ui/menu.cpp`).
  - Added dedicated text identifiers: `UI_MENU_SMRS_SHARPNESS` ("Super Motion sharpness") and `UI_MENU_FG_FLOW` ("Optical flow sensitivity") in English and Russian.
- **Ongoing Reverse-Engineering Research:**
  - Actively analyzing `al::getVirtualDisplayWidth`/`Height` and `agl` texture heap allocators in Bananza's retail Switch 2 code to unlock arbitrary native dynamic resolutions (from 720p up to true 4K/8K) in upcoming releases.

---

## [Alpha 1d] - 2026-11-08

### 🍌 Upscaler Architecture Overhaul & Memory/Pipeline Performance (Alpha 1d)
- **Super Motion Frame Generation Gate (`runtime/gfx_present.c`):**
  - Decoupled `DKB_FG_MODE` (`dlss_fg`, `fsr3_fg`, `xefg`, `msm_fg`) from legacy `frame_gen = 0` configuration blocks so Frame Generation engages reliably in all gameplay scenarios.
  - Active generator and multiplier badges now display accurately in the in-game performance overlay.
- **Upstream Memory & Driver Stall Optimizations (`runtime/vk_memory.c`, `runtime/vk_pipeline.c`):**
  - Ported `TOTK_POOL_LAZY=1` and `TOTK_HOST_PROPS_CACHE=1` from upstream, deferring guest memory imports to first GPU use and slashing driver allocate stalls.
  - Defaulted `TOTK_POOL_FINI_WAIT=fence` to eliminate CPU queue idle wait bottlenecks.
  - Added per-stage sampler capacity validation (`push_fits`) and geometry stage support guards (`vk_geometry_supported`).
- **Ampere Shader Recompiler Lowering (`shader_recompiler`):**
  - Lowered 12 Ampere compute and texture fetching stages for native Ampere instruction compatibility.
- **Audio Voice Fix (`runtime/audren.c`):**
  - Applied unused voice slot recording fix preventing voice 0 wiping during stereo playback.
- **DLSS Pipeline Decoupling & Depth Sanitization (`runtime/nvn_api_dlss.c`, `runtime/vk_dlaa.c`):**
  - Decoupled depth buffer copying from camera motion vector generation so depth memory is guaranteed clean and valid.
  - Removed inverted depth flags and added automatic 1:1 DLAA mode resolution scaling.
  - Restored camera reprojection VP locking across uniform banks `c4` and `c8`.
- **Anti-Aliasing & Ambient Occlusion Stack (`runtime/vk_smaa.c`, `runtime/vk_ao.c`):**
  - Integrated Subpixel Morphological Anti-Aliasing (SMAA 1x) with automatic bypass during native DLAA/DLSS.
  - Added SSAO / HBAO ambient occlusion pipelines with in-game overlay and launcher controls.

---

## [Alpha 1c] - 2026-11-07

### 🍌 Upstream Engine Integration & Cutscene Stability (Alpha 1c)
- **SPIR-V Fused-Multiply-Zero (FMZ) Hardware Emulation (`runtime/vk_pipeline.c`, `runtime/cfg.c`):**
  - Implemented automatic SPIR-V instruction rewrites for 32-bit float `OpFMul` and GLSL.std.450 `Fma` so `0 * x = 0` holds strictly even when `x` is `±Infinity` or `NaN` (NVIDIA hardware FMZ behavior).
  - Enabled by default for all vertex stages (`DKB_FMZ=vertex`), resolving the end-of-tutorial cutscene device loss mathematically on NVIDIA GPUs without dropping visual meshes.
  - Added bisection control `TOTK_FMZ_SEL=<mask:val>` and stage selector `TOTK_FMZ=0/vertex/all`.
- **Live GPU Checkpoints & Fault Diagnostics (`runtime/vk_device.c`, `runtime/vk_crumbs.c`, `runtime/vk_pipeline.c`):**
  - Integrated `VK_NV_device_diagnostic_checkpoints` under `DKB_GPU_CHECKPOINTS=1`.
  - Added automatic driver binary fault dump generation (`logs/dkb-fault-<time>.bin`) when `VK_ERROR_DEVICE_LOST` is encountered for zero-guesswork debugging.
  - Added `DKB_SKIP_VS` / `DKB_SKIP_FS` stage origin filters, SPIR-V bytecode dumping (`DKB_DUMP_SPV=1`), and hot-override support (`DKB_SPV_OVERRIDE`).
- **Vulkan Pipeline & Command Recorder Optimizations (`runtime/vk_rec.c`, `runtime/vk_pipeline.c`):**
  - Cache-line aligned (`alignas(64)`) command ring buffer `head` and `tail` pointers to eliminate false sharing.
  - Added `TOTK_REC_BATCH` recording batching for reduced CPU overhead.
  - Declared `StorageImageReadWithoutFormat` capability for unknown-format storage images.
- **Portuguese Language Normalization (`runtime/locale.c`):**
  - Mapped base Portuguese `pt` locale code to `pt-BR` for Bananza.
- **Proprietary Core Features Preserved:**
  - Retained AMD RX 9070 XT 1 MiB zero dummy buffer & full range SSBO binding (`ZERO_BYTES 1048576u`) in `vk_exec_draw.c`.
  - Retained 1500ms fence wait (`ONESHOT_WAIT_MS 1500u`) preventing Windows 2-second TDR driver resets.
  - Retained AMD recorder default 0 for stutter-free command dispatch.
  - Retained NVIDIA DLSS 4.5, DLAA, Reflex, Intel XeSS 1.3, Frame Generation, DualSense haptics/adaptive triggers, F10 HUD, and 1-Click Launcher Auto-Updater.

---

## [Alpha 1b] - 2026-11-07

### 🔥 Community Tester Hotfixes (Alpha 1b)
- **Cross-Vendor Cutscene Fault Protection (`runtime/vk_exec_draw.c`):**
  - Enabled `light_quad_fault_skip` across all GPU vendors (AMD RDNA 4 / RX 9070 XT / Radeon 890M, NVIDIA RTX, and Intel Arc) instead of restricting to NVIDIA (`vendorID == 0x10de`), fixing `VK_ERROR_DEVICE_LOST` during the opening cutscene on AMD Radeon GPUs.
- **Native 30 / 60 FPS Timing Cap (`runtime/frame_rate.c`, `runtime/cfg.c`, `runtime/ui/menu.cpp`, `launcher/DKBLauncher.py`):**
  - Locked selectable and parsed Frame Rate Limits to **30 FPS** and **60 FPS** (clamping any legacy `120` or `display` values in `dkb.ini` to `60`). Prevents game speed-up above 60 FPS and fixes the `"Golden Bananas?!"` intro transition freeze.
- **Installer & Launcher RomFS Packing + Pre-Flight Validation (`installer/modern_gui.py`, `launcher/DKBLauncher.py`):**
  - Fixed loose `romfs/` to `romfs/romfs.bin` packing in `modern_gui.py` and added 1-click loose `romfs/` auto-packing in `DKBLauncher.exe`.
  - Added smart dump directory resolution and strict verification for `exefs/main`, `exefs/sdk`, and `romfs/romfs.bin`.

---

## [Alpha 1] - 2026-11-07

### 🌟 Initial Alpha 1 Release Highlights
- **Modern Dark Jungle & Banana Gold Setup Wizard (`DKB-Setup.exe` / `Donkey-Kong-Bananza-Setup.exe`):**
  - Standalone zero-dependency PyInstaller bundle (`customtkinter` + `darkdetect` embedded) with high-res multi-mipmap `dkb.ico` icon.
  - Native unpacking support for `.nup` containers, unpacked `exefs/` + `romfs/` (or `base/exefs` + `base/romfs`) dump folders, and `.nsp` / `.xci` containers.
  - Automatic `romfs.bin` indexed image builder from loose RomFS files for instant startup and streaming.
  - 1-click deployment of NVIDIA DLSS 4.5, DLSS-NR, Streamline, Reflex, and Intel XeSS 1.3+ runtime libraries.
  - Built-in Controller UI selector (PS5 DualSense Monochrome/Color, Xbox Series Monochrome/Color, Nintendo Switch) and Desktop shortcut creator.

- **Standalone PC Launcher (`DKBLauncher.exe` - Alpha 1):**
  - Branded as **Alpha 1** (`ALPHA 1 · DKB`) with full two-way synchronization against `dkb.ini` and the in-game F1 overlay menu.
  - Added Display & Graphics controls for Upscaler (`Off`, `DLSS`, `FSR`, `XeSS`, `DLAA`), Upscaler Quality Preset, DLSS Preset (`Default`, `Preset K`, `Preset L`, `Preset M`), Frame Generation, NVIDIA Reflex (`Off`, `On`, `On + Boost`), AMD Anti-Lag 2, Render Scale, Shadow & Anisotropic Filtering, FOV (`70`–`120`), and Color Grading (`Default`, `Vivid`, `Warm`, `Cool`, `Cinema`).
  - Added Vulkan Shader Pre-Warm utility, Save Backup Vault with safety snapshots, and 1-click Mod Manager.

- **Vulkan Renderer & Cutscene Stability Fixes:**
  - **Sky, Cave Walls & Distant World Restoration (`runtime/vk_exec_draw.c`):** Fixed `light_quad_fault_skip` so that only true per-instance light quads (`count <= 6`, `>= 8` active vertex streams, all active streams having `divisor != 0`) or known faulting cutscene vertex stage origins (`cae0ef8e9fe9147f`, `a05d707357ffc917`, `c081588c78ee16f3`, `89300537d4fff15e`, `fb6e80e4f0bd1ac5`, `140a1eac75f7b5c0`, `163bd4735277403a`) are skipped on NVIDIA GPUs. Legitimate 3D world shaders with vertex input masks `0x03ff`–`0x7fff` (skybox, cave walls, distant terrain) use per-vertex streams (`divisor == 0`) and `count > 6`, so zero 3D meshes are ever skipped.
  - **Indirect Reset & Texture Write-Back Integrity (`runtime/vk_exec_draw.c`, `runtime/vk_texture.c`, `runtime/vk_backend.c`):** Restored `vk_exec_read_buffer` in `vk_exec_reset_indirect` and synchronous fence write-back limits so GPU-generated LUTs and destructible terrain indirect records never read stale guest memory.
  - **Upstream Bananza Engine Sync:**
    - Enabled native passthrough geometry shader replay by default (`NATIVE_GS=1`) so destructible terrain renders properly.
    - Integrated Ampere lowering support for RGB-mask `TXD` instructions (`lower_to_maxwell.h`) and shader translator patch `0019` (allowing code after unpredicated `CAL` instructions to run, restoring the opening tutorial wall).
    - Allowed passthrough geometry shaders to read unforwarded inputs (`runtime/gfx_shader.c`).
    - Added indexed pending GPU write tracking during shadow memory sync (`runtime/vk_memory.c`).
    - Rounded alpha-test pipeline key references to nearest `1/255` step (`runtime/vk_pipeline.c`) to eliminate per-frame pipeline rebuild stutter during fades.
    - Added mutable sRGB storage-image view support (`VK_IMAGE_CREATE_EXTENDED_USAGE_BIT` in `runtime/vk_texture.c` and format-matched storage views in `runtime/vk_exec_draw.c`).
