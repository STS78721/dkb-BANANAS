# Testing DKB Recomp

This guide is for running DKB Recomp on your own PC and reporting what happens. You build it from source with your own
copy of the game. Nothing from the game is in this repository, and nothing you build from it may be shared: not the
executable, not the generated code, not the shader cache.

## What you need

- Windows 10 or 11 on an x86-64 CPU with AVX2.
- 16 GB of RAM. The game uses about 13 GB, so close other large programs first.
- A GPU with Vulkan 1.3 and a current driver.
- About 15 GB free for the build (the packed RomFS is about 9 GB), besides the space your dump takes.
- *Donkey Kong Bananza* base version 1.0.0, dumped from your own Switch 2: the ExeFS (`main`, `sdk`, `main.npdm`) and
  the RomFS as a folder. Update 458752 is not supported yet. This project does not explain how to dump the game.
  If your dump is a single `.nup` file (the container Eglantine and Eden load), `tools/extract_nup.py` writes both
  folders from its base package: `python tools\extract_nup.py <game>.nup --list` shows the packages, and
  `python tools\extract_nup.py <game>.nup --out build\extracted\dkb100-dump` writes `exefs` and `romfs` there, checking
  every file's SHA-256. Pass those two folders to the commands in the README.

## Build

Install the toolchain from [building.md](building.md) (MSYS2 UCRT64 Clang, Ninja, Python 3.12 in `.venv`, .NET SDK),
then run the commands in the README, "Building from source". The executable ends up in `build\dkb100\dkb.exe`.

The generation command must include `--patches recomp\patches-dkb100.txt`. This enables the controller setup patch
that bypasses the game's HID server proxy, which the host does not implement. If you generated the code without
that argument, rerun the README's generation command with it and then rebuild with `tools\build_game.py`.
Rebuilding alone leaves the unpatched generated code in place and the game stops during controller setup.

## Run

```
build\dkb100\dkb.exe
```

Run it from inside the repository: it finds the shader translator (`build\xlate\totk_xlate.exe`) by looking up from
its own folder. The first run translates every shader it meets and is slow; later runs use the cache.

Useful settings, as environment variables (`set NAME=value` in the same Command Prompt):

| Setting | Effect |
| --- | --- |
| `TOTK_PROFILE_DIR=<folder>` | Use this folder for the save and settings. A new, empty folder gives a fresh start, which is what reports should use. |
| `TOTK_GPU_NAME=<text>` | Use the first GPU whose name contains this text, on machines with more than one. |
| `TOTK_GPU=null` | Run without a picture (no Vulkan). Useful to tell CPU and OS bugs from graphics bugs. |
| `TOTK_XLATE_PROCS=<n>` | Shader translator processes at run time (default 2). Lower it if RAM runs out. |
| `TOTK_TRACE=perf` | Log the frame time, what holds frames back and the GPU time every 60 frames. For stalls and low frame rates. |
| `TOTK_PERF_SLOW_GPU_MS=<ms>` | With `TOTK_TRACE=perf`, a replay part that runs this long on the GPU (default 100) gets a line with its draw and dispatch counts and the NVN 2 names of its stages, a second line with the arguments of its indirect draws as read back afterwards, and a third saying whether a shader in that part wrote the largest one's arguments, and which. |
| `TOTK_FIX_SCREEN_UV=0` (or `DKB_FIX_SCREEN_UV=0`) | Leaves the map's mask shader as the game wrote it. By default its screen position is computed from `gl_FragCoord` instead of read from varyings the vertex stage divided by w: the camera sits inside the box that shader draws, and on AMD (Eglantine as well) the faces behind the camera turned the varyings into long cyan wedges across the map. The shader is recognised by its NVN 2 name or the hash of its translated SPIR-V, and the log names it when it is rewritten (`vk: fs 544677ae4ec509be ... rebuilt from gl_FragCoord`). |
| `TOTK_FIX_INDIRECT_RESET=0` | Draws the terrain's indirect-record reset program as a shader instead of the host doing its stores (#48). The shader's stores are lost, so the records keep zero or stale contents; for comparisons only. |
| `TOTK_EXPERIMENTAL_GEOMETRY=1` | Replays draws with an ordinary geometry stage through it (#52). Off by default; other geometry draws are rejected once in the log. |
| `TOTK_NATIVE_GS=0` (or `DKB_NATIVE_GS=0`) | Turns off the replay of the game's native passthrough geometry stages on the portable path. That replay draws the destructible terrain (#45) and is on by default; with it off the terrain is black. The varyings are inferred, not read from the hardware skip mask. |
| `TOTK_REC_BATCH=N` (or `DKB_REC_BATCH=N`) | Publishes the Vulkan recorder ring's head every N packets (1..64) instead of after each one (#93), so the recorder thread's spin reads a cache line the producer rewrites less often. 0 (default) publishes after every packet. Waits and handoffs publish the rest. With large N the last packets are recorded inside the final drain while the submitting thread waits, so watch `drain_ms` in `TOTK_TRACE=perf`; start with 8. |
| `DKB_GEOMETRY_CONTEXT=1` | Logs the first 12 draws with a geometry stage and their buffers (#58). `DKB_GEOMETRY_CONTEXT_ORIGIN=<hex>` keeps only draws with that stage origin. |
| `DKB_GPU_CHECKPOINTS=1` | On NVIDIA (VK_NV_device_diagnostic_checkpoints), marks every pipeline bind, compute dispatch, copy, blit and colour clear so that a lost device logs the last checkpoints the GPU reached, with the NVN 2 names of the shaders of each pipeline (#75). With VK_EXT_device_address_binding_report it also keeps the last 4096 GPU address bind/unbind events and, for each address in the device fault report, logs the bindings that contain it or the nearest one ending within 1 MiB below it. Also saves the driver's fault dump to `logs\dkb-fault-<time>.bin`; it can contain shader code, never attach it. With VK_KHR_pipeline_executable_properties it also logs, the first time a pipeline is bound, one line per driver executable (stage, NVN 2 stage names, every statistic such as binary size) and saves its internal representation (ISA) to `logs\dkb-isa-<pipeline handle>-<stage>.txt` (or `.bin`; at most 4096 pipelines and 512 MiB; derived from the game, never attach). The capture runs on the recorder thread at each new pipeline's first bind, so it can slow recording, and toggling the variable costs one cold pipeline-cache run. Off by default. AMD always has its own breadcrumbs. |
| `DKB_SKIP_VS=<origin>[,<origin>...]`, `DKB_SKIP_FS=<origin>[,<origin>...]` | Debug only, for diagnosing #75: skips every draw whose NVN 2 vertex (`DKB_SKIP_VS`) or fragment (`DKB_SKIP_FS`) stage origin (the 16-digit hex name logged by `DKB_GPU_CHECKPOINTS=1`) is listed, up to 16. Off by default. |
| `DKB_DUMP_SPV=1` | Debug only, for diagnosing #75: writes the exact SPIR-V passed to `vkCreateShaderModule` for each graphics and compute shader (after every rewrite) to `spv/<vs\|fs\|gs\|cs>-<NVN 2 stage origin>-<FNV-1a 64>.spv` next to `dkb.log`, so a SPIR-V hash from an Nsight Aftermath dump maps to its NVN 2 stage; up to 1 GiB, one `vk: spv dump` log line per file. Off by default. The files can hold code derived from the game: never attach them. |
| `DKB_SPV_OVERRIDE=<hash>:<path.spv>[,<hash>:<path.spv>...]` | Debug only, for #75: a shader module whose final SPIR-V has this FNV-1a 64 hash (16 hex digits, the last part of a `DKB_DUMP_SPV` file name) is created from the given file instead (multiple of 4 bytes, up to 16 MiB; max 8 entries). One `vk: SPIR-V override` log line per replaced module. Off by default. |
| `DKB_NAN_GUARD=zero` (default), `mark` or `0` | Fragment stages store 0 in place of NaN in their float colour outputs (`spirv_nan_guard.c`). A NaN texel in the HDR scene colour is spread by the bloom into a glowing band crossed by black lines; two frame dumps from a challenge room on an RX 9070 XT had NaN right after the lit opaque pass, which the console does not show. `mark` writes a colour named by the stage instead: r, g and b are each 0, 1, 4 or 16 from bits of the stage's SPIR-V hash (the `fs` hash in a dump's `passes.txt`), and the log lists `vk: fs <hash>: NaN marked as <r> <g> <b>` for each stage, so a dump's `_tm` image shows which stage writes the NaN. `0` stores what the shader computed. Tested by `tests/nan_guard_spirv_test.py --cc clang --spirv-val spirv-val --out <dir>`. |
| `DKB_FMZ=0`, `vertex` (default) or `all` | Workaround for #75, on for vertex stages by default (`0` turns it off): rewrites 32-bit float `OpFMul` and GLSL.std.450 `Fma` in the final SPIR-V so `0 * x = 0` even for Inf and NaN (NVIDIA's fused-multiply-zero rule). `vertex` touches vertex-stage modules only, `all` every stage. One `vk: FMZ rewrite <hash> mul=<n> fma=<m>` log line per rewritten module; `DKB_DUMP_SPV` shows the rewritten bytes. `DKB_FMZ_SEL=<mask>:<value>` (hex) limits it to the modules whose FNV-1a 64 hash before the rewrite satisfies `(hash & mask) == value`, so `1:0` and `1:1` split the set in halves for a bisection. |
| `TOTK_TRACE=codec` | Log each call of the video decoder that moves a buffer or a picture. For guide movies that stop or show a flat colour. |

The settings keep TotK Recomp's `TOTK_` names, so its fixes apply without a rename. Each one can also be written with
`DKB_` in its place (`DKB_GPU=null`); when both are set, the `DKB_` one wins.

Command-line options: `--seconds S` stops the run after S seconds of game time, and `--shot frame.png` with it saves
that frame. A shot is for your own debugging; do not post it if it shows game content.

## What to report

Open an issue with the issue form, or comment on [#3](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/3)
(GPU testing) or [#7](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/7) (playing further):

- the commit you built (`git rev-parse --short HEAD`);
- GPU, driver version and Windows version;
- how far the game got and what you did just before it stopped;
- the log, `logs\totk.log` next to `dkb.exe`, as an attachment. After a crash, copy it before you start the game
  again: the next run renames it to `totk.1.log`.

Never attach game files, keys, builds or the shader cache, and never paste key values into a log or an issue.
