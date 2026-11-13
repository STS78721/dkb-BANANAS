# Donkey Kong Bananza Alpha 1g

**Release date:** 2026-11-13  
**Type:** Native render resolution, NVIDIA DLSS Super Resolution, menu and hit-effect fixes, replay optimizations, and safe defaults (drop-in over Alpha 1f / 1d / 1c / 1b / 1)

This release draws the picture at the resolution you choose, adds NVIDIA DLSS Super Resolution that reports the preset it is actually running, brings back the missing hit-effect textures, and stops the F1 menu from aborting. Fresh installs get a vendor-neutral 1920×1080 windowed profile. An update does not replace the settings already on your PC.

---

## Highlights

- **Native render resolution** — the picture and HUD follow **Game Render Resolution**. 1920×1080, 2560×1440, and 3840×2160 are real render sizes, not a 2880×1620 image scaled up afterwards. A new install stays at **1920×1080 windowed**
- **NVIDIA DLSS Super Resolution** — DLSS follows the size the game is already drawing. **100% DLAA** uses preset **K**, **Quality** and **Balanced** use preset **K**, **Performance** uses preset **M**, and **Ultra Performance** uses preset **L**. The in-game FPS line shows that live mode, for example `DLSS Quality K 67%`
- **Motion for DLSS** — motion vectors are dilated camera reprojection, then matched against the previous frame where something moves on its own. This is super resolution, not frame generation
- **F1 menu and hit effects** — opening the in-game menu no longer aborts, and the textures shown when Donkey Kong hits something are drawn again
- **Same picture, less replay work** — repeated descriptor and address lookups are cached, a draw rebinds only the vertex streams that changed, command buffers are walked again only when they wait or read a deferred bind, and an all-zero texture upload is cleared on the GPU

---

## Changed

### Picture size
- **Render size (`dkb_wide`):** the virtual display and its render-target pool follow `output_res` for 16:9 sizes from 1280×720 through 7680×4320. The shipped default is 1920×1080
- **HUD:** the HUD is drawn in that same size. Choosing a higher resolution does not shrink it

### Upscaling and the FPS meter
- **DLSS Super Resolution (`vk_dlss.c`):** evaluation uses the game's color and the current render size. Presets are **K / K / K / M / L** across DLAA, Quality, Balanced, Performance, and Ultra Performance. `nvngx_dlss.dll` beside the game is the NGX runtime. Streamline is not loaded
- **Live preset (`vk_dlss_meter`):** the FPS overlay and the full performance panel name the mode from the last successful evaluation, including the actual percentage when dynamic resolution moves between the named steps
- **Motion vectors (`dlss_mv.comp`):** dilated camera reprojection, with a bounded luma match to the previous frame. The history stores luma only. The accept test is unchanged

### Stability
- **In-game menu:** a cleared guest system event is rebuilt instead of aborting when the menu opens
- **Hit effects:** the hit-effect draws that were being skipped are drawn again

### Replay cost, without changing the image
- **Descriptor and address caches:** exact descriptor keys and failed address lookups stay cached until a pool mapping changes
- **Vertex binds:** one changed stream is rebound on its own
- **Command buffers:** a recording is marked when it finishes, and a later submit skips the wait and deferred-bind walks when those records are absent
- **Zero uploads:** a texture of 32 MiB or more whose storage is entirely zero is cleared on the GPU

### Safe out-of-the-box defaults
- A new install, and **Restore Defaults** in the launcher, uses **1920×1080 windowed**, **Upscaling = Off**, **Frame generation = Off**, **Reflex = Off**, **field of view 100**, and the **default color profile**
- Updating keeps the `dkb.ini` already on that PC. New keys are added only when that PC does not already have them
- No pipeline cache is included. Each GPU builds its own on first launch
- No display, color, controller, or volume settings from a development PC are included

---

## Install

| Asset | Use when |
|---|---|
| **`SteelTemple-DKB-Alpha1g-Update.zip`** | You already have Alpha 1f / 1d / 1c / 1b / 1. Open **DKBLauncher** and take the update, or extract the zip over the game folder |
| `SteelTemple-DKB-Alpha1g-Portable-StandAlone.zip` | Fresh install. Extract it, then copy in your own `exefs` and `romfs` |

1. Close the game before updating.
2. Update from **DKBLauncher**, or extract `SteelTemple-DKB-Alpha1g-Update.zip` over the game folder. Your `dkb.ini`, saves, and dump stay as they are.
3. Launch the game. The first run on a new GPU prepares shaders once. The launcher title shows **Alpha 1g**.

Your dump (`exefs` and `romfs`), saves, and personal `dkb.ini` stay on your PC. A PC that does not already have a `dkb.ini` receives the 1920×1080 windowed profile above.

DLSS appears in the launcher and in the in-game menu. Leave it off on a GPU that does not support it.

---

## Credits

SteelTempSaint community and testers · Lucas and the DKB Recomp contributors · TotK Recomp · NVIDIA NGX for DLSS Super Resolution · everyone testing resolution, the menu, hit effects, and other GPUs
