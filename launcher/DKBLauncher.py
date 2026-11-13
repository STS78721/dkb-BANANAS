"""Donkey Kong Bananza PC Recomp Launcher - Alpha 1.

The layout and interaction model are adapted from the SteelTemple DKB launcher.
See LICENSE-MIT.txt for the upstream notice. DKB Recomp retains inherited dkb.ini/DKB_ runtime names.
"""
from __future__ import annotations
import base64, ctypes, hashlib, json, os, re, shutil, subprocess, sys, tempfile, threading, time, urllib.request, zipfile
from datetime import datetime
from pathlib import Path
from ctypes import wintypes
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from PIL import Image, ImageDraw, ImageTk

CURRENT_VERSION = "Alpha 1g"
APP_TITLE = f"Donkey Kong Bananza PC Recomp · {CURRENT_VERSION}"
VERSION_STRING = f"{CURRENT_VERSION} · DKB"
GITHUB_URL = "https://github.com/STS78721/dkb-BANANAS"
RELEASES_URL = GITHUB_URL + "/releases"
RELEASES_API_URL = "https://api.github.com/repos/STS78721/dkb-BANANAS/releases/latest"
COLOR_BG_DARK = "#0a0d0d"
COLOR_GOLD = "#ffd24d"
COLOR_GOLD_HOVER = "#fff2a3"
COLOR_WHITE = "#ffffff"
COLOR_TEXT_DIM = "#a4ada7"
COLOR_CARD = "#121918"
COLOR_CARD_BORDER = "#2c3932"
COLOR_ACCENT_GREEN = "#65db83"
COLOR_TAB_ACTIVE = "#1d2a23"
COLOR_BUTTON_HOVER = "#28382e"
COLOR_TRACK_BG = "#2e3b34"
COLOR_TRACK_ACTIVE = COLOR_GOLD
MOD_DISABLED_SUFFIX = ".disabled"


def resource_dir():
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))


def app_dir():
    return Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent


def find_asset(filename):
    candidates = [
        resource_dir() / "assets" / filename,
        app_dir() / "assets" / filename,
        Path.cwd() / "assets" / filename,
        resource_dir() / filename,
        app_dir() / filename,
        Path.cwd() / "launcher" / "assets" / filename,
        Path(__file__).resolve().parent / "assets" / filename,
    ]
    for c in candidates:
        if c.is_file(): return c
    return None


def open_folder(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    try:
        if os.name == "nt": os.startfile(str(path))
        elif sys.platform == "darwin": subprocess.Popen(["open", str(path)])
        else: subprocess.Popen([shutil.which("xdg-open") or "xdg-open", str(path)])
    except (OSError, subprocess.SubprocessError): pass


def play_ui_sound(kind):
    if not getattr(play_ui_sound, "enabled", True): return
    try:
        import winsound
        sound = find_asset("hover.wav" if kind == "hover" else "click.wav")
        if sound: winsound.PlaySound(str(sound), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
    except Exception: pass


class _RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG), ("right", wintypes.LONG), ("bottom", wintypes.LONG)]
class _MONITORINFOEX(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", _RECT), ("rcWork", _RECT), ("dwFlags", wintypes.DWORD), ("szDevice", wintypes.WCHAR * 32)]
class _DEVMODE(ctypes.Structure):
    _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD), ("dmDriverVersion", wintypes.WORD),
                ("dmSize", wintypes.WORD), ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                ("dmOrientation", ctypes.c_short), ("dmPaperSize", ctypes.c_short), ("dmPaperLength", ctypes.c_short),
                ("dmPaperWidth", ctypes.c_short), ("dmScale", ctypes.c_short), ("dmCopies", ctypes.c_short),
                ("dmDefaultSource", ctypes.c_short), ("dmPrintQuality", ctypes.c_short), ("dmColor", ctypes.c_short),
                ("dmDuplex", ctypes.c_short), ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short),
                ("dmCollate", ctypes.c_short), ("dmFormName", wintypes.WCHAR * 32), ("dmLogPixels", wintypes.WORD),
                ("dmBitsPerPel", wintypes.DWORD), ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD),
                ("dmDisplayFlags", wintypes.DWORD), ("dmDisplayFrequency", wintypes.DWORD)]


def enumerate_displays():
    displays = []
    if os.name != "nt": return [{"index":1,"device":"PRIMARY","resolution":"1920x1080","refresh":60,"left":0,"top":0,"monitor_width":1920,"monitor_height":1080,"primary":True}]
    try:
        user32 = ctypes.windll.user32
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC, ctypes.POINTER(_RECT), wintypes.LPARAM)
        user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.POINTER(_MONITORINFOEX)]
        user32.EnumDisplaySettingsW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(_DEVMODE)]
        def visit(handle, _dc, _rect, _data):
            info = _MONITORINFOEX(); info.cbSize = ctypes.sizeof(info)
            if not user32.GetMonitorInfoW(handle, ctypes.byref(info)): return True
            mode = _DEVMODE(); mode.dmSize = ctypes.sizeof(mode)
            user32.EnumDisplaySettingsW(info.szDevice, -1, ctypes.byref(mode))
            width = mode.dmPelsWidth or info.rcMonitor.right-info.rcMonitor.left
            height = mode.dmPelsHeight or info.rcMonitor.bottom-info.rcMonitor.top
            displays.append({"index":len(displays)+1,"device":info.szDevice,"resolution":f"{width}x{height}",
                "refresh":int(mode.dmDisplayFrequency or 60),"left":int(info.rcMonitor.left),"top":int(info.rcMonitor.top),
                "monitor_width":int(info.rcMonitor.right-info.rcMonitor.left),"monitor_height":int(info.rcMonitor.bottom-info.rcMonitor.top),
                "primary":bool(info.dwFlags & 1)})
            return True
        user32.EnumDisplayMonitors(None,None,callback_type(visit),0)
    except Exception: pass
    if not displays: displays=[{"index":1,"device":"PRIMARY","resolution":"1920x1080","refresh":60,"left":0,"top":0,"monitor_width":1920,"monitor_height":1080,"primary":True}]
    return displays


def place_window_on_monitor(window, monitor):
    window.update_idletasks(); w,h=window.winfo_width(),window.winfo_height()
    left=monitor["left"]+max(0,(monitor["monitor_width"]-w)//2); top=monitor["top"]+max(0,(monitor["monitor_height"]-h)//2)
    window.geometry(f"{w}x{h}+{left}+{top}")


def classify_gpu_name(name):
    low=name.lower()
    vendor="NVIDIA" if any(v in low for v in ("nvidia","geforce")) else "AMD" if any(v in low for v in ("amd","radeon")) else "Intel" if "intel" in low else "Other"
    return name,vendor

def detect_gpu():
    if hasattr(detect_gpu,"result"):return detect_gpu.result
    if os.name != "nt": result=("GPU detection is available on Windows", "Other")
    else:
        try:
            cmd="Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name | ConvertTo-Json -Compress"
            r=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command",cmd],capture_output=True,text=True,timeout=8,
                             creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
            names=json.loads(r.stdout.strip()) if r.returncode==0 and r.stdout.strip() else []
            if isinstance(names,str):names=[names]
            if names:
                names.sort(key=lambda n:(not any(v in n.lower() for v in ("nvidia","geforce","amd","radeon")),n))
                result=classify_gpu_name(names[0])
            else:result=("GPU not detected","Other")
        except Exception:result=("GPU not detected","Other")
    detect_gpu.result=result
    return result


APP_CONFIG = app_dir() / "launcher_config.json"

def load_config():
    try: return json.loads(APP_CONFIG.read_text(encoding="utf-8"))
    except Exception: return {"show_launcher":True,"launcher_sound_effects":True}

def save_config(cfg):
    APP_CONFIG.parent.mkdir(parents=True,exist_ok=True)
    tmp=APP_CONFIG.with_suffix(".tmp"); tmp.write_text(json.dumps(cfg,indent=2),encoding="utf-8"); tmp.replace(APP_CONFIG)

def valid_game_dir(path):
    if not path: return False
    p = Path(path)
    if not p.is_dir(): return False
    return (p / "dkb.exe").is_file() or (p / "build" / "dkb100" / "dkb.exe").is_file()

def find_game_dir(cfg):
    choices=[]
    if cfg.get("game_dir"): choices.append(Path(cfg["game_dir"]))
    if os.environ.get("DKB_GAME_DIR"): choices.append(Path(os.environ["DKB_GAME_DIR"]))
    a=app_dir(); choices += [a, Path.cwd(), a/"build"/"dkb100", a/"jogo"/"build"/"dkb100", a.parent/"build"/"dkb100", Path.cwd()/"build"/"dkb100"]
    for path in choices:
        if valid_game_dir(path):
            res = path.resolve()
            if (res / "build" / "dkb100" / "dkb.exe").is_file() and not (res / "dkb.exe").is_file():
                res = (res / "build" / "dkb100").resolve()
            return res
    return None

GAME_DIR = None
INI_PATH = None
PROFILE_DIR = None
MODS_DIR = None
ACTIVE_MODS_DIR = None
LOGS_DIR = None
SHADER_CACHE_DIR = None
LAUNCHER_CONFIG = load_config()

def set_game_dir(path):
    global GAME_DIR, INI_PATH, PROFILE_DIR, MODS_DIR, ACTIVE_MODS_DIR, LOGS_DIR, SHADER_CACHE_DIR
    GAME_DIR=Path(path).resolve()
    INI_PATH=GAME_DIR/"dkb.ini"
    PROFILE_DIR=Path(LAUNCHER_CONFIG.get("profile_dir") or (GAME_DIR.parent/"profile-dkb100"))
    if not PROFILE_DIR.exists() and (GAME_DIR.parent/"profile-dkb100").exists(): PROFILE_DIR=GAME_DIR.parent/"profile-dkb100"
    if not PROFILE_DIR.exists() and (GAME_DIR/"profile").exists(): PROFILE_DIR=GAME_DIR/"profile"
    MODS_DIR=GAME_DIR/"mods"
    ACTIVE_MODS_DIR=GAME_DIR/"romfs-mods-active"
    LOGS_DIR=GAME_DIR/"logs"
    SHADER_CACHE_DIR=GAME_DIR/"shader-cache"


def parse_ini(path):
    values={}
    if not path or not Path(path).is_file(): return values
    try:
        for line in Path(path).read_text(encoding="utf-8",errors="ignore").splitlines():
            s=line.strip()
            if not s or s.startswith(("#",";","[")) or "=" not in s: continue
            k,v=s.split("=",1); values[k.strip().lower()]=v.split("#")[0].split(";")[0].strip()
    except OSError: pass
    return values

def config_text_value(value):
    value=str(value).strip()
    if value.casefold() in {"true","on"}: return "1"
    if value.casefold() in {"false","off"}: return "0"
    return value

def save_ini_settings(path, updates):
    path=Path(path); raw_lines=path.read_text(encoding="utf-8",errors="ignore").splitlines(keepends=True) if path.exists() else []
    obsolete={"dlss","dlaa","dlss_preset","xess_preset","antilag2","smaa","ao","ao_intensity"}
    lines=[]
    for line in raw_lines:
        stripped=line.strip()
        if "=" in stripped and not stripped.startswith(("#",";")):
            k=stripped.split("=",1)[0].strip().casefold()
            if k in obsolete:
                continue
        lines.append(line)
    for key,value in updates.items():
        if key.casefold() in obsolete:
            continue
        found=False
        active_section=""
        target_section=next((section for section, keys in {
            "frame rate":{"fps_limit","fps60","realtime","cloth_realtime","highfps_fix"},
            "display":{"fullscreen","window_size","vk_present","output_res","render_scale","upscaler","upscaler_preset","reflex","sharpness","aspect","fov","color_preset","color_brightness","color_contrast","color_saturation","color_gamma","device_profile","assume_focus","low_latency","frame_gen","fg_mode","fg_mult","fg_type","fg_preset","fg_still","fg_flow","perf_overlay","gpu_index","fxaa"},
            "language":{"locale","lang","menu_lang","operation_mode"},
            "audio":{"volume","audio_channels"},
            "input":{"motion","rumble_strength","rumble_curve","rumble_triggers","pad_layout","stick_deadzone","mouse_sens"},
            "performance":{"pipe_wait","pipe_skip","pipe_prebuild","lib_prebuild","experimental_geometry","fix_vs_fault","fix_zero","lod_guard","fix_indirect_reset","astc","memory","tex_alias","astc_bc","native_gs","fix_c0","bind_skip","shader_memo","pipe_memo","tex_memo","desc_template","push_skip","split_draws","split_us","vk_validation","vk_dyn_vertex","vk_dyn_a2c","vk_subgroup_pin","async_preempt","replay_async","timer_1ms","nx2_read_nowait"},
            "files":{"romfs_mods"}
        }.items() if key.casefold() in keys), "")
        if target_section:
            for i,line in enumerate(lines):
                stripped=line.strip()
                if stripped.startswith("[") and stripped.endswith("]"):
                    active_section=stripped[1:-1].casefold()
                    continue
                if active_section!=target_section or stripped.startswith(("#",";")):continue
                if "=" in line and line.split("=",1)[0].strip().casefold()==key.casefold():
                    lines[i]=f"{key} = {value}\n"; found=True; break
            if not found:
                sec_idx = -1
                for idx, line in enumerate(lines):
                    if line.strip().casefold() == f"[{target_section}]":
                        sec_idx = idx
                        break
                if sec_idx != -1:
                    lines.insert(sec_idx + 1, f"{key} = {value}\n")
                else:
                    if lines and lines[-1].strip(): lines.append("\n")
                    lines.append(f"[{target_section}]\n")
                    lines.append(f"{key} = {value}\n")
        else:
            for i,line in enumerate(lines):
                stripped=line.lstrip()
                if stripped.startswith(("#",";","[")):continue
                if "=" in line and line.split("=",1)[0].strip().casefold()==key.casefold():
                    lines[i]=f"{key} = {value}\n"; found=True; break
            if not found:lines.append(f"{key} = {value}\n")
    path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix(path.suffix+".tmp"); tmp.write_text("".join(lines),encoding="utf-8"); tmp.replace(path)
    return True

def runtime_defaults():
    return {"locale":"en-US","menu_lang":"en","experimental_geometry":"0","native_gs":"0"}



def is_same_version(ver1, ver2):
    def norm(v):
        v = re.sub(r"^v", "", str(v or "").strip().lower())
        return re.sub(r"[^a-z0-9]", "", v)
    return norm(ver1) == norm(ver2)

def find_running_game_processes():
    """Find running DKB game processes, excluding the launcher."""
    if os.name != "nt" or not GAME_DIR:
        return []
    target = str((GAME_DIR / "dkb.exe").resolve()).replace("'", "''")
    command = (
        f"$target = '{target}'; "
        "$items = @(Get-CimInstance Win32_Process -Filter \\\"Name='dkb.exe'\\\" -ErrorAction SilentlyContinue | "
        "Where-Object { $_.ExecutablePath -and "
        "[String]::Equals([IO.Path]::GetFullPath($_.ExecutablePath), $target, "
        "[StringComparison]::OrdinalIgnoreCase) } | "
        "Select-Object ProcessId, ExecutablePath); "
        "if ($items.Count -eq 1) { $items[0] | ConvertTo-Json -Compress } "
        "else { $items | ConvertTo-Json -Compress }"
    )
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, timeout=8,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)
        )
        if result.returncode == 0:
            output = result.stdout.strip()
            if not output or output == "null":
                return []
            parsed = json.loads(output)
            if isinstance(parsed, dict):
                return [{"pid": parsed.get("ProcessId"), "path": parsed.get("ExecutablePath")}]
            elif isinstance(parsed, list):
                return [{"pid": item.get("ProcessId"), "path": item.get("ExecutablePath")} for item in parsed if isinstance(item, dict)]
    except Exception:
        pass
    return []

def game_process_warning(action):
    processes = find_running_game_processes()
    if not processes:
        return None
    details = []
    for process in processes:
        label = []
        if process.get("pid") is not None:
            label.append(f"PID {process['pid']}")
        if process.get("path"):
            label.append(str(process["path"]))
        details.append(" - ".join(label) if label else "DKB game process")
    return "A DKB game process is currently running:\n" + "\n".join(details) + f"\n\nPlease close the game before {action}."

def merge_update_ini(current_ini, update_ini, output_ini):
    """Preserve every existing user setting and add only new keys from an update's dkb.ini."""
    current_ini, update_ini, output_ini = Path(current_ini), Path(update_ini), Path(output_ini)
    if not current_ini.is_file():
        shutil.copy2(update_ini, output_ini)
        return
    lines = current_ini.read_text(encoding="utf-8", errors="ignore").splitlines(keepends=True)
    existing = set(parse_ini(current_ini))
    additions = []
    for line in update_ini.read_text(encoding="utf-8", errors="ignore").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";", "[")) or "=" not in stripped:
            continue
        key = stripped.split("=", 1)[0].strip().lower()
        if key and key not in existing:
            additions.append(line.rstrip("\r\n"))
            existing.add(key)
    if additions:
        if lines and not lines[-1].endswith(("\n", "\r")):
            lines[-1] += "\n"
        lines.extend(["\n# New settings supplied by the update; existing values were preserved.\n"]
                     + [line + "\n" for line in additions])
    output_ini.write_text("".join(lines), encoding="utf-8")

def extract_update_archive(archive_path, stage_dir):
    """Validate the official flat update archive and stage safe update files."""
    archive_path, stage_dir = Path(archive_path), Path(stage_dir)
    selected = {}
    with zipfile.ZipFile(archive_path) as archive:
        bad_member = archive.testzip()
        if bad_member:
            raise ValueError(f"Update archive CRC check failed for {bad_member}.")
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename
            path = Path(name.replace("\\", "/"))
            if path.is_absolute() or len(path.parts) != 1 or ".." in path.parts:
                raise ValueError(f"Unexpected path in update archive: {name}")
            mode = (info.external_attr >> 16) & 0o170000
            if mode == 0o120000:
                raise ValueError(f"Symbolic links are not allowed in updates: {name}")
            key = path.name.casefold()
            if key.startswith((".", "exefs", "romfs", "profile", "save")):
                continue
            if key in selected:
                raise ValueError(f"Duplicate update file: {name}")
            if info.file_size <= 0:
                raise ValueError(f"Update file is empty: {name}")
            selected[key] = info

        for required in ("dkb.exe", "dkblauncher.exe"):
            if required not in selected:
                raise ValueError(f"Update archive is missing required binary: {required}.")
        if selected["dkb.exe"].file_size < 1024 * 1024 or selected["dkblauncher.exe"].file_size < 1024 * 1024:
            raise ValueError("Update executable is unexpectedly small.")

        for key, info in selected.items():
            target_name = info.filename
            target = stage_dir / target_name
            if key == "dkb.ini":
                incoming = stage_dir / "_update_dkb.ini"
                with archive.open(info) as source, incoming.open("wb") as dest:
                    shutil.copyfileobj(source, dest)
                merge_update_ini(GAME_DIR / "dkb.ini", incoming, stage_dir / "dkb.ini")
                incoming.unlink(missing_ok=True)
            else:
                with archive.open(info) as source, target.open("wb") as dest:
                    shutil.copyfileobj(source, dest)
    return [path.name for path in stage_dir.iterdir() if path.is_file() and not path.name.startswith("_")]

def validate_update_game_executable(exe_path, game_dir, timeout=30):
    """Run an integrity probe on the staged dkb.exe before permitting installation."""
    exe_path = Path(exe_path)
    if not exe_path.is_file() or exe_path.stat().st_size < 1024 * 1024:
        raise ValueError("The staged dkb.exe binary is missing or invalid.")
    command = [str(exe_path), "--probe-dpi"]
    result = subprocess.run(
        command, cwd=str(game_dir), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=timeout,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)
    )
    output = (result.stdout or "") + "\n" + (result.stderr or "")
    if result.returncode != 0 or "probe-dpi: PASS" not in output:
        excerpt = output[-1000:].strip()
        raise RuntimeError("The update executable did not pass the integrity probe. No files were changed.\n" + excerpt)
    return output

def scan_mod_folders():
    MODS_DIR.mkdir(parents=True,exist_ok=True); found=[]
    for p in MODS_DIR.iterdir():
        if not p.is_dir(): continue
        disabled=p.name.casefold().endswith(MOD_DISABLED_SUFFIX)
        name=p.name[:-len(MOD_DISABLED_SUFFIX)] if disabled else p.name
        if name: found.append({"name":name,"path":p,"enabled":not disabled})
    return sorted(found,key=lambda x:x["name"].casefold())

def set_mod_folder_enabled(folder,enabled):
    folder=Path(folder); off=folder.name.casefold().endswith(MOD_DISABLED_SUFFIX)
    if bool(enabled)==(not off): return folder
    target=folder.with_name(folder.name[:-len(MOD_DISABLED_SUFFIX)] if enabled else folder.name+MOD_DISABLED_SUFFIX)
    if target.exists(): raise FileExistsError(f"Cannot rename because {target.name} already exists.")
    return folder.rename(target)

def create_mod_folder():
    if not GAME_DIR: raise RuntimeError("Choose a DKB game folder first.")
    MODS_DIR.mkdir(parents=True,exist_ok=True)
    name=""; index=1
    while not name or (MODS_DIR/name).exists():
        name=f"New Mod {index}"; index+=1
    return (MODS_DIR/name).mkdir()

def rebuild_mod_overlay():
    """Compose enabled per-mod RomFS trees for the runtime's single overlay directory."""
    mods=scan_mod_folders(); enabled=[m for m in mods if m["enabled"]]
    if not INI_PATH or not INI_PATH.is_file():
        raise FileNotFoundError("The selected DKB game folder is missing its runtime settings file.")
    owner=GAME_DIR/".romfs-mods-active.owner.json"
    if ACTIVE_MODS_DIR.exists() and not owner.is_file():
        raise RuntimeError("The active RomFS overlay exists without this launcher's ownership marker; it was left untouched.")
    stage=GAME_DIR/f".romfs-mods-stage-{os.getpid()}-{threading.get_ident()}"
    shutil.rmtree(stage,ignore_errors=True)
    if enabled:
        stage.mkdir(parents=True)
        seen={}
        for mod in enabled:
            for source in mod["path"].rglob("*"):
                if source.is_symlink():continue
                if not source.is_file():continue
                rel=source.relative_to(mod["path"]); dest=stage/rel
                try:dest.resolve().relative_to(stage.resolve())
                except ValueError:raise ValueError(f"Mod path escapes the overlay folder: {rel}") from None
                dest.parent.mkdir(parents=True,exist_ok=True)
                # Deterministic precedence follows the alphabetical package order shown in the manager.
                shutil.copy2(source,dest); seen[rel]=mod["name"]
        if ACTIVE_MODS_DIR.exists():
            old=GAME_DIR/f".romfs-mods-old-{time.time_ns()}"; ACTIVE_MODS_DIR.rename(old)
            stage.rename(ACTIVE_MODS_DIR); shutil.rmtree(old,ignore_errors=True)
        else: stage.rename(ACTIVE_MODS_DIR)
        owner_tmp=owner.with_name(owner.name+f".{os.getpid()}.tmp")
        owner_tmp.write_text(json.dumps({"owner":"DKBLauncher","format":1},indent=2),encoding="utf-8")
        owner_tmp.replace(owner)
        save_ini_settings(INI_PATH,{"romfs_mods":str(ACTIVE_MODS_DIR)})
    else:
        shutil.rmtree(stage,ignore_errors=True)
        if ACTIVE_MODS_DIR.exists():
            old=GAME_DIR/f".romfs-mods-old-{time.time_ns()}"
            ACTIVE_MODS_DIR.rename(old)
            shutil.rmtree(old,ignore_errors=True)
        owner.unlink(missing_ok=True)
        if INI_PATH and INI_PATH.exists(): save_ini_settings(INI_PATH,{"romfs_mods":""})


def find_running_game_processes():
    if os.name!="nt" or not GAME_DIR: return []
    target=str((GAME_DIR/"dkb.exe").resolve()).replace("'","''")
    cmd=("$target='"+target+"'; @(Get-CimInstance Win32_Process -Filter \"Name='dkb.exe'\" | "
         "Where-Object { $_.ExecutablePath -and [String]::Equals([IO.Path]::GetFullPath($_.ExecutablePath),$target,[StringComparison]::OrdinalIgnoreCase) } | "
         "Select-Object ProcessId,ExecutablePath) | ConvertTo-Json -Compress")
    try:
        r=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command",cmd],capture_output=True,text=True,timeout=10,
                         creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
        data=json.loads(r.stdout.strip()) if r.returncode==0 and r.stdout.strip() else []
        return data if isinstance(data,list) else [data]
    except Exception: return []

def game_process_warning(action):
    procs=find_running_game_processes()
    return ("Close Donkey Kong Bananza before "+action+".\n\n"+"\n".join(f"PID {p.get('ProcessId')}: {p.get('ExecutablePath')}" for p in procs)) if procs else ""

def create_save_snapshot(profile_dir,backups_dir=None,suffix=""):
    profile_dir=Path(profile_dir); save_dir=profile_dir/"save"
    backups_dir=Path(backups_dir) if backups_dir else profile_dir/"backups"
    if not save_dir.is_dir(): raise FileNotFoundError(f"Save data was not found: {save_dir}")
    backups_dir.mkdir(parents=True,exist_ok=True); stamp=datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    base=f"save_{stamp}"+(f"_{suffix}" if suffix else ""); dest=backups_dir/base; n=2
    while dest.exists(): dest=backups_dir/f"{base}_{n}"; n+=1
    try:shutil.copytree(save_dir,dest)
    except Exception:
        shutil.rmtree(dest,ignore_errors=True)
        raise
    return dest

def restore_save_snapshot(profile_dir,snapshot_dir):
    profile_dir=Path(profile_dir); snapshot_dir=Path(snapshot_dir); save_dir=profile_dir/"save"; backups=profile_dir/"backups"
    try:snapshot_dir.resolve().relative_to(backups.resolve())
    except ValueError: raise ValueError("Selected snapshot is outside the DKB backup folder.") from None
    if not snapshot_dir.is_dir() or not any(snapshot_dir.iterdir()): raise ValueError("Selected snapshot is missing or empty.")
    safety=create_save_snapshot(profile_dir,backups,"before-restore")
    staged=profile_dir/f".save-restore-{os.getpid()}-{time.time_ns()}"; old=profile_dir/f".save-previous-{os.getpid()}-{time.time_ns()}"
    try:
        shutil.copytree(snapshot_dir,staged)
        if save_dir.exists(): save_dir.rename(old)
        staged.rename(save_dir)
        if old.exists():
            try:shutil.rmtree(old)
            except OSError:pass
    except Exception:
        if not save_dir.exists() and old.exists(): old.rename(save_dir)
        shutil.rmtree(staged,ignore_errors=True); raise
    return safety

class DKBSlider(tk.Canvas):
    """Gold-tracked value slider used by the source launcher."""
    def __init__(self,parent,from_,to_,initial,on_change=None,width=190,height=28):
        super().__init__(parent,width=width,height=height,bg=COLOR_BG_DARK,highlightthickness=0)
        self.from_,self.to_,self.val,self.on_change=from_,to_,int(initial),on_change
        self.w,self.h,self.pad,self.hovered=width,height,12,False
        self.bind("<Button-1>",self._click); self.bind("<B1-Motion>",self._click)
        self.bind("<Enter>",lambda _e:self._hover(True)); self.bind("<Leave>",lambda _e:self._hover(False))
        self.bind("<MouseWheel>",self._wheel); self.draw()
    def _hover(self,value): self.hovered=value; self.config(cursor="hand2" if value else ""); self.draw()
    def _value(self,x):
        span=self.w-2*self.pad; frac=max(0,min(1,(x-self.pad)/span)); return round(self.from_+frac*(self.to_-self.from_))
    def _click(self,event): self.set(self._value(event.x))
    def _wheel(self,event): self.set(self.val+(5 if event.delta>0 else -5))
    def set(self,value):
        self.val=max(self.from_,min(self.to_,int(value))); self.draw()
        if self.on_change: self.on_change(self.val)
    def draw(self):
        self.delete("all"); y=self.h//2; x=self.pad+(self.val-self.from_)/(self.to_-self.from_)*(self.w-2*self.pad)
        self.create_line(self.pad,y,self.w-self.pad,y,fill=COLOR_TRACK_BG,width=6,capstyle="round")
        if x>self.pad:self.create_line(self.pad,y,x,y,fill=COLOR_TRACK_ACTIVE,width=6,capstyle="round")
        r=10 if self.hovered else 9
        self.create_oval(x-r,y-r,x+r,y+r,fill=COLOR_GOLD_HOVER if self.hovered else COLOR_GOLD,outline=COLOR_WHITE,width=1.5)
        self.create_oval(x-3,y-3,x+3,y+3,fill=COLOR_BG_DARK,outline="")


class DKBSettingsDialog(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent); self.parent_app=parent; self.title(f"DKB {CURRENT_VERSION} · Display & Graphics Settings")
        self.geometry("900x700"); self.minsize(820,630); self.configure(bg=COLOR_BG_DARK); self.transient(parent); self.grab_set()
        self.ini_values=parse_ini(INI_PATH); self.vars={}; self.monitors=enumerate_displays()
        saved=parent.launcher_cfg.get("target_display")
        self.target_monitor=next((m for m in self.monitors if m["device"]==saved),next((m for m in self.monitors if m["primary"]),self.monitors[0]))
        self.display_resolution=self.target_monitor["resolution"]; self.display_refresh=self.target_monitor["refresh"]
        self.gpu_name,self.gpu_vendor=("Detecting graphics adapter…","Other")
        self.setup_ui(); place_window_on_monitor(self,self.target_monitor)
        threading.Thread(target=self._detect_gpu_worker,daemon=True).start()
    def _detect_gpu_worker(self):
        name,vendor=detect_gpu()
        try:self.after(0,lambda:self._show_detected_gpu(name,vendor))
        except tk.TclError:pass
    def _show_detected_gpu(self,name,vendor):
        self.gpu_name,self.gpu_vendor=name,vendor
        self.gpu_header.config(text=f"Detected: {name}",fg=COLOR_ACCENT_GREEN if vendor!="Other" else COLOR_TEXT_DIM)
    def get_val(self,key,default=""): return self.ini_values.get(key.lower(),default)
    def setup_ui(self):
        header=tk.Frame(self,bg=COLOR_CARD,height=58); header.pack(fill=tk.X,side=tk.TOP); header.pack_propagate(False)
        tk.Label(header,text=f"DKB {CURRENT_VERSION.upper()}  ·  GRAPHICS & SYSTEM CONFIGURATION",font=("Segoe UI",12,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD).pack(side=tk.LEFT,padx=24,pady=17)
        self.gpu_header=tk.Label(header,text=f"Detected: {self.gpu_name}",font=("Segoe UI",9),fg=COLOR_ACCENT_GREEN if self.gpu_vendor!="Other" else COLOR_TEXT_DIM,bg=COLOR_CARD)
        self.gpu_header.pack(side=tk.RIGHT,padx=20)
        style=ttk.Style(self); style.theme_use("clam")
        style.configure("DKB.TNotebook",background=COLOR_BG_DARK,borderwidth=0)
        style.configure("DKB.TNotebook.Tab",background=COLOR_CARD,foreground=COLOR_TEXT_DIM,font=("Segoe UI",10,"bold"),padding=[18,10],borderwidth=0)
        style.map("DKB.TNotebook.Tab",background=[("selected",COLOR_TAB_ACTIVE)],foreground=[("selected",COLOR_GOLD)])
        self.notebook=ttk.Notebook(self,style="DKB.TNotebook"); self.notebook.pack(fill=tk.BOTH,expand=True,padx=20,pady=9)
        self.tabs=[]
        for label in ("DISPLAY","UPSCALING & PERFORMANCE","GRAPHICS & LIGHTING","CONTROLS & AUDIO"):
            tab=tk.Frame(self.notebook,bg=COLOR_BG_DARK); self.notebook.add(tab,text=f"  {label}  "); self.tabs.append(self._scroll_area(tab))
        self.tab_display,self.tab_upscale,self.tab_graphics,self.tab_controls=self.tabs
        self.build_display_tab(); self.build_upscale_tab(); self.build_graphics_tab(); self.build_controls_tab()
        bottom=tk.Frame(self,bg=COLOR_CARD,height=62); bottom.pack(fill=tk.X,side=tk.BOTTOM); bottom.pack_propagate(False)
        self._button(bottom,"SAVE & APPLY",self.save_and_close,True).pack(side=tk.RIGHT,padx=(8,22),pady=13)
        self._button(bottom,"CANCEL",self.destroy).pack(side=tk.RIGHT,padx=8,pady=13)
        self._button(bottom,"RESTORE DEFAULTS",self.restore_defaults).pack(side=tk.LEFT,padx=22,pady=13)
    def _button(self,parent,text,cmd,primary=False):
        return tk.Button(parent,text=text,font=("Segoe UI",9,"bold"),fg="#000000" if primary else COLOR_WHITE,
            bg=COLOR_GOLD if primary else COLOR_BUTTON_HOVER,activebackground=COLOR_GOLD_HOVER,activeforeground="#000000",
            bd=0,padx=16,pady=8,cursor="hand2",command=cmd)
    def _scroll_area(self,parent):
        holder=tk.Frame(parent,bg=COLOR_BG_DARK); holder.pack(fill=tk.BOTH,expand=True)
        canvas=tk.Canvas(holder,bg=COLOR_BG_DARK,highlightthickness=0); bar=ttk.Scrollbar(holder,orient="vertical",command=canvas.yview)
        canvas.configure(yscrollcommand=bar.set); bar.pack(side=tk.RIGHT,fill=tk.Y); canvas.pack(side=tk.LEFT,fill=tk.BOTH,expand=True)
        inner=tk.Frame(canvas,bg=COLOR_BG_DARK); win=canvas.create_window((0,0),window=inner,anchor="nw")
        inner.bind("<Configure>",lambda _e:canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",lambda e:canvas.itemconfigure(win,width=e.width))
        canvas.bind("<MouseWheel>",lambda e:canvas.yview_scroll(int(-e.delta/120),"units"))
        return inner
    def _section_note(self,parent,text):
        tk.Label(parent,text=text,font=("Segoe UI",9),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,wraplength=700,justify="left").pack(fill=tk.X,padx=22,pady=(8,4),anchor="w")
    def create_row_dropdown(self,parent,label,key,options,default,desc=""):
        row=tk.Frame(parent,bg=COLOR_BG_DARK,pady=7); row.pack(fill=tk.X,padx=20)
        left=tk.Frame(row,bg=COLOR_BG_DARK); left.pack(side=tk.LEFT,fill=tk.X,expand=True)
        tk.Label(left,text=label,font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(anchor="w")
        if desc:tk.Label(left,text=desc,font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,wraplength=540,justify="left").pack(anchor="w")
        current=config_text_value(self.get_val(key,str(default))); var=tk.StringVar(value=current if current in options else str(default)); self.vars[key]=var
        cb=ttk.Combobox(row,textvariable=var,values=options,state="readonly",width=22,font=("Segoe UI",9))
        cb.pack(side=tk.RIGHT,padx=5)
        if not hasattr(self,"combos"):self.combos={}
        self.combos[key]=cb
        return row
    def create_row_toggle(self,parent,label,key,default="0",desc=""):
        row=tk.Frame(parent,bg=COLOR_BG_DARK,pady=7); row.pack(fill=tk.X,padx=20)
        left=tk.Frame(row,bg=COLOR_BG_DARK); left.pack(side=tk.LEFT,fill=tk.X,expand=True)
        tk.Label(left,text=label,font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(anchor="w")
        if desc:tk.Label(left,text=desc,font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,wraplength=540,justify="left").pack(anchor="w")
        cur=config_text_value(self.get_val(key,str(default))); var=tk.StringVar(value="1" if cur.lower() in ("1","true","on") else "0"); self.vars[key]=var
        tk.Checkbutton(row,variable=var,onvalue="1",offvalue="0",bg=COLOR_BG_DARK,activebackground=COLOR_BG_DARK,selectcolor=COLOR_CARD,bd=0).pack(side=tk.RIGHT,padx=8)
        return row
    def create_row_slider(self,parent,label,key,low,high,default,unit="",desc=""):
        row=tk.Frame(parent,bg=COLOR_BG_DARK,pady=7); row.pack(fill=tk.X,padx=20)
        left=tk.Frame(row,bg=COLOR_BG_DARK); left.pack(side=tk.LEFT,fill=tk.X,expand=True)
        tk.Label(left,text=label,font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(anchor="w")
        if desc:tk.Label(left,text=desc,font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,wraplength=470,justify="left").pack(anchor="w")
        try:value=round(float(self.get_val(key,str(default)))*10) if key=="rumble_curve" else int(float(self.get_val(key,str(default))))
        except (TypeError,ValueError):value=default
        var=tk.IntVar(value=value); self.vars[key]=var
        pill=tk.Frame(row,bg=COLOR_CARD,bd=1,relief="solid",padx=7,pady=3); pill.pack(side=tk.RIGHT,padx=(8,0))
        text=tk.Label(pill,text="",font=("Segoe UI",9,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD,width=8); text.pack()
        def changed(v):
            var.set(v); display=f"{v/10:.1f}" if key in {"rumble_curve","ao_intensity"} else str(v)
            text.config(text=f"{display} {unit}".strip())
        slider=DKBSlider(row,low,high,value,changed,width=180,height=28); slider.pack(side=tk.RIGHT,padx=5); changed(value)
        return row
    def build_display_tab(self):
        f=self.tab_display
        box=tk.LabelFrame(f,text=" Presets ",font=("Segoe UI",9,"bold"),fg=COLOR_GOLD,bg=COLOR_BG_DARK,bd=1,highlightthickness=1,highlightbackground=COLOR_CARD_BORDER,padx=12,pady=8)
        box.pack(fill=tk.X,padx=20,pady=(10,5))
        self.preset_status=tk.Label(box,text="Balanced settings for handheld PCs and small displays.",font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,anchor="w"); self.preset_status.pack(side=tk.LEFT,fill=tk.X,expand=True)
        tk.Button(box,text="BANANZA HANDHELD PRESET",command=self.apply_handheld_preset,font=("Segoe UI",8,"bold"),fg="#000",bg=COLOR_GOLD,activebackground=COLOR_GOLD_HOVER,bd=0,padx=10,pady=7,cursor="hand2").pack(side=tk.RIGHT,padx=(8,0))
        monitor_row=tk.Frame(f,bg=COLOR_BG_DARK,pady=8); monitor_row.pack(fill=tk.X,padx=20)
        tk.Label(monitor_row,text="Target Display Monitor",font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(side=tk.LEFT)
        self.monitor_labels={f"Display {m['index']}: {m['resolution']}"+(" (Primary)" if m['primary'] else ""):m for m in self.monitors}
        self.target_display_var=tk.StringVar(value=next((k for k,v in self.monitor_labels.items() if v['device']==self.target_monitor['device']),next(iter(self.monitor_labels))))
        self.monitor_combo=ttk.Combobox(monitor_row,textvariable=self.target_display_var,values=list(self.monitor_labels),state="readonly",width=28); self.monitor_combo.pack(side=tk.RIGHT)
        self.monitor_combo.bind("<<ComboboxSelected>>",self.on_monitor_changed)
        pathrow=tk.Frame(f,bg=COLOR_BG_DARK,pady=6); pathrow.pack(fill=tk.X,padx=20)
        tk.Label(pathrow,text="DKB Game Folder",font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(side=tk.LEFT)
        tk.Label(pathrow,text=str(GAME_DIR) if GAME_DIR else "Not selected",font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK).pack(side=tk.LEFT,padx=12)
        tk.Button(pathrow,text="BROWSE…",command=self.parent_app.choose_game_directory,font=("Segoe UI",8,"bold"),fg=COLOR_WHITE,bg=COLOR_BUTTON_HOVER,activebackground=COLOR_GOLD,bd=0,padx=10,pady=5).pack(side=tk.RIGHT)
        self.create_row_toggle(f,"Exclusive Fullscreen","fullscreen","0","Start in exclusive fullscreen mode.")
        resolutions=["3840x2160","2560x1440","1920x1080","1600x900","1280x800","1280x720","3440x1440","5120x1440"]
        if self.display_resolution not in resolutions:resolutions.insert(0,self.display_resolution)
        self.create_row_dropdown(f,"Window Resolution","window_size",resolutions,self.get_val("window_size","1920x1080"),"Window size in pixels. A new install uses 1920×1080. Game render resolution is configured separately.")
        rates=["30","60"]
        cur_fps=self.get_val("fps_limit","60")
        self.create_row_dropdown(f,"Frame Rate Limit","fps_limit",rates,"30" if cur_fps=="30" else "60","Target frame rate: 60 FPS (Native Bananza speed) or 30 FPS (Battery Saver / Handheld).")
        self.create_row_dropdown(f,"V-Sync / Presentation Mode","vk_present",["mailbox","fifo","immediate"],"mailbox","Mailbox is low-latency; FIFO is strict V-Sync; Immediate may tear.")
        self.create_row_dropdown(f,"Performance Metrics Overlay","perf_overlay",["off","fps","full"],self.get_val("perf_overlay","off"),"Display in-game real-time FPS and frametime metrics overlay.")
        output_sizes=["1280x720","1600x900","1920x1080","2560x1440","3200x1800","3840x2160","2560x1080","3440x1440","3840x1600","5120x2160","3840x1080","5120x1440","1280x800","1680x1050","1920x1200","2560x1600"]
        if self.display_resolution not in output_sizes:output_sizes.insert(0,self.display_resolution)
        self.create_row_dropdown(f,"Game Render Resolution","output_res",output_sizes,self.get_val("output_res","1920x1080"),"Draw the picture and HUD at this size. A new install uses 1920×1080. Higher sizes need a GPU that can hold them.")
        self.create_row_dropdown(f,"Aspect Ratio Behavior","aspect",["fit","stretch"],"fit","Fit preserves the image; Stretch fills the full display.")
        self.create_row_slider(f,"Field of View","fov",70,140,100,"%","Widen or narrow the camera view where supported by the DKB build. Requires the FOV patch in the selected game build.")
        wide=tk.Frame(f,bg=COLOR_CARD,padx=13,pady=9,highlightthickness=1,highlightbackground=COLOR_CARD_BORDER); wide.pack(fill=tk.X,padx=20,pady=8)
        tk.Label(wide,text="ULTRAWIDE DISPLAY PROFILE",font=("Segoe UI",9,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD).pack(side=tk.LEFT)
        tk.Label(wide,text="Uses DKB's built-in aspect and FOV controls; no game files are patched.",font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_CARD).pack(side=tk.LEFT,padx=12)
        tk.Button(wide,text="APPLY",command=self.apply_ultrawide_profile,font=("Segoe UI",8,"bold"),fg="#000",bg=COLOR_GOLD,activebackground=COLOR_GOLD_HOVER,bd=0,padx=10,pady=6).pack(side=tk.RIGHT)
    def on_monitor_changed(self,_event=None):
        self.target_monitor=self.monitor_labels.get(self.target_display_var.get(),self.target_monitor)
        self.display_resolution=self.target_monitor["resolution"]; self.display_refresh=self.target_monitor["refresh"]
        if "window_size" in self.vars:self.vars["window_size"].set(self.display_resolution)
        if "output_res" in self.vars:self.vars["output_res"].set(self.display_resolution)
        self.parent_app.move_to_monitor(self.target_monitor)
    def apply_handheld_preset(self):
        for k,v in {"device_profile":"deck","window_size":"1280x800","output_res":"1280x800","fps_limit":"60","vk_present":"fifo","upscaler":"fsr","render_scale":"game","sharpness":"80"}.items():
            if k in self.vars:self.vars[k].set(v)
        self.preset_status.config(text="Handheld profile selected: 1280×800, 60 FPS, FSR scaling, and FIFO presentation.",fg=COLOR_ACCENT_GREEN)
    def apply_ultrawide_profile(self):
        if "aspect" in self.vars:self.vars["aspect"].set("fit")
        if "fov" in self.vars:self.vars["fov"].set(110)
        if "window_size" in self.vars:self.vars["window_size"].set(self.target_monitor["resolution"])
        if "output_res" in self.vars:self.vars["output_res"].set(self.target_monitor["resolution"])
        messagebox.showinfo("Display Profile","Applied DKB's fit aspect mode and a wider camera view. This setting does not rewrite RomFS or HUD files.",parent=self)
    def build_upscale_tab(self):
        f=self.tab_upscale
        self.create_row_dropdown(f,"Super Resolution Technology","upscaler",["off","dlss","smrs","fsr","bilinear"],"off","Off draws the game picture as rendered. DLSS is NVIDIA Super Resolution and follows that size: DLAA, Quality, and Balanced use preset K, Performance uses M, Ultra Performance uses L. SMRS and FSR run on other GPUs. A new install leaves this off.")
        self.create_row_dropdown(f,"SMRS / Upscaler Preset","upscaler_preset",["native","ultra_quality","quality","balanced","performance","ultra_performance"],"quality","SMRS preset. DLSS ignores this row and uses the live render size instead. The in-game FPS line shows the DLSS mode that was actually used.")
        self.create_row_dropdown(f,"Frame Generation Mode","fg_mode",["off","msm_fg","dlss_fg","fsr3_fg","xefg"],"off","Frame generation. A new install leaves this off.")
        self.create_row_dropdown(f,"Frame Multiplier","fg_mult",["2x","3x","4x","6x"],"2x","Multiplier: 2x (Double FPS), 3x (Triple FPS), 4x (Quadruple FPS), 6x.")
        self.create_row_dropdown(f,"Multiplier Pacing","fg_type",["fixed","adaptive"],"fixed","Pacing: Fixed Multiplier or Adaptive Frame Pacing.")
        self.create_row_dropdown(f,"Render Scale (Internal)","render_scale",["game","33","50","58","67","75","100","125","150","200"],"game","Scale used by SMRS. DLSS follows the game's own rendered size. A new install leaves this on game.")
        self.create_row_slider(f,"Super Motion Sharpness","sharpness",0,100,80,"%","Nodix SMRS spatial reconstruction and edge enhancement filter.")
        self.create_row_dropdown(f,"Low-Latency Mode","reflex",["off","on","boost"],"off","NVIDIA Reflex. A new install leaves this off so other GPUs are not asked for it.")
        self.create_row_dropdown(f,"Device Profile","device_profile",["auto","desktop","deck"],"auto","Runtime profile for thread priority and system hints.")
        self.create_row_dropdown(f,"Shader Pipeline Policy","pipe_wait",["default","draw"],"default","Default waits only when needed; Draw prioritizes avoiding pipeline stalls.")
        self.create_row_toggle(f,"Prebuild Vulkan Pipelines","pipe_prebuild","1","Build reusable Vulkan pipelines before play when supported.")
        self._section_note(f,"A new install is 1920×1080 windowed, with upscaling, frame generation, and Reflex off. Turn a scaler on here if this PC should use one.")

        def on_preset_changed(_e=None):
            p = self.vars["upscaler_preset"].get()
            scale_map = {"native": "100", "ultra_quality": "75", "quality": "67", "balanced": "58", "performance": "50", "ultra_performance": "33"}
            if p in scale_map and "render_scale" in self.vars:
                self.vars["render_scale"].set(scale_map[p])
            if "operation_mode" in self.vars:
                if p in ("native", "quality", "ultra_quality"):
                    self.vars["operation_mode"].set("docked")
                else:
                    self.vars["operation_mode"].set("handheld")

        def on_scale_changed(_e=None):
            s = self.vars["render_scale"].get()
            rev_map = {"100": "native", "75": "ultra_quality", "67": "quality", "58": "balanced", "50": "performance", "33": "ultra_performance"}
            if s in rev_map and "upscaler_preset" in self.vars:
                self.vars["upscaler_preset"].set(rev_map[s])
            if "operation_mode" in self.vars:
                if s in ("100", "125", "150", "200", "75", "67", "game"):
                    self.vars["operation_mode"].set("docked")
                else:
                    self.vars["operation_mode"].set("handheld")

        if hasattr(self, "combos"):
            if "upscaler_preset" in self.combos:
                self.combos["upscaler_preset"].bind("<<ComboboxSelected>>", on_preset_changed)
            if "render_scale" in self.combos:
                self.combos["render_scale"].bind("<<ComboboxSelected>>", on_scale_changed)
    def build_graphics_tab(self):
        f=self.tab_graphics
        self.create_row_dropdown(f,"Color Profile","color_preset",["default","vivid","clarity","custom"],"default","Choose a display color preset.")
        self.create_row_slider(f,"Color Brightness","color_brightness",-30,30,0,"","Display brightness adjustment.")
        self.create_row_slider(f,"Color Contrast","color_contrast",70,150,100,"%","Display contrast adjustment.")
        self.create_row_slider(f,"Color Saturation","color_saturation",50,200,100,"%","Display saturation adjustment.")
        self.create_row_dropdown(f,"Color Gamma","color_gamma",["0.8","0.9","1.0","1.1","1.2"],"1.0","Gamma adjustment.")
        self.create_row_toggle(f,"FXAA Anti-Aliasing","fxaa","1","Game's built-in FXAA edge smoothing.")
        self.create_row_toggle(f,"High-FPS Realtime Physics","realtime","1","Unlocks game physics calculation tickrate for smooth 60+ FPS without slow motion.")
        self.create_row_toggle(f,"Realtime Cloth & Fur Physics","cloth_realtime","1","Evaluates Donkey Kong fur and cloth physics in real time at high refresh rates.")
        self.create_row_toggle(f,"High-FPS Camera Smoothing Fix","highfps_fix","1","Fixes camera tracking jitter and maintains smooth animation pacing.")
        self.create_row_dropdown(f,"Console Emulation Profile","operation_mode",["docked","handheld"],self.get_val("operation_mode","docked"),"Docked or handheld game mode. Picture size is Game Render Resolution.")
        self.create_row_toggle(f,"Native Vulkan Geometry Shaders","native_gs","1","Directly compiles native geometry shaders on supported Vulkan GPUs.")
        self.create_row_toggle(f,"Experimental Geometry","experimental_geometry","0","Experimental DKB geometry path; leave off if destructible terrain renders incorrectly.")
        self.create_row_toggle(f,"Skip Known NVIDIA Movie Fault","fix_vs_fault","1","Avoids a known NVIDIA GPU fault after the first guide movie.")
        self.create_row_toggle(f,"Zero New GPU Memory","fix_zero","1","Initializes new GPU memory to match the console's behavior.")
        self.create_row_toggle(f,"Guard Level-of-Detail Transitions","lod_guard","1","Prevents temporary terrain and object flicker during transitions.")
        self.create_row_toggle(f,"Reset Indirect Draw State","fix_indirect_reset","1")
        self.create_row_toggle(f,"Prebuild Shader Libraries","lib_prebuild","1")
        panel=tk.Frame(f,bg=COLOR_CARD,padx=14,pady=10,highlightthickness=1,highlightbackground=COLOR_CARD_BORDER); panel.pack(fill=tk.X,padx=20,pady=10)
        self.warm_status=tk.Label(panel,text="Warm the local Vulkan shader cache before playing.",font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_CARD,wraplength=460,justify="left"); self.warm_status.pack(side=tk.LEFT,fill=tk.X,expand=True)
        self.warm_button=tk.Button(panel,text="⚡ PRE-WARM VULKAN SHADERS",command=self.prewarm_shaders,font=("Segoe UI",8,"bold"),fg="#000",bg=COLOR_GOLD,activebackground=COLOR_GOLD_HOVER,bd=0,padx=10,pady=8)
        self.warm_button.pack(side=tk.RIGHT,padx=(8,0))
    def prewarm_shaders(self):
        if not GAME_DIR or not (GAME_DIR/"dkb.exe").is_file():messagebox.showerror("Shader Pre-Warm","Select a DKB game folder first.",parent=self); return
        if getattr(self,"warm_process_active",False):return
        self.warm_process_active=True; self.warm_button.config(state=tk.DISABLED,text="PRE-WARMING…")
        self.warm_status.config(text="Preparing the local DKB Vulkan pipeline cache…",fg=COLOR_GOLD)
        threading.Thread(target=self._prewarm_worker,daemon=True).start()
    def _prewarm_worker(self):
        try:
            logs_dir = GAME_DIR / "logs"
            logs_dir.mkdir(parents=True, exist_ok=True)
            p=subprocess.run([str(GAME_DIR/"dkb.exe"),"--warm-shaders"],cwd=str(GAME_DIR),capture_output=True,text=True,timeout=1800)
            log_content = f"=== DKB Shader Pre-Warm Log ===\nExit Code: {p.returncode}\n\n[STDOUT]\n{p.stdout}\n\n[STDERR]\n{p.stderr}\n"
            (logs_dir / "prewarm.log").write_text(log_content, encoding="utf-8")
            if p.returncode==0:message,color="Shader cache preparation completed.",COLOR_ACCENT_GREEN
            else:message,color=f"Shader pre-warm failed (exit {p.returncode}). Check logs/prewarm.log.","#ff7373"
        except Exception as exc:message,color=f"Shader pre-warm failed: {exc}","#ff7373"
        try:self.after(0,lambda:self._finish_prewarm(message,color))
        except tk.TclError:pass
    def _finish_prewarm(self,message,color):
        self.warm_process_active=False; self.warm_button.config(state=tk.NORMAL,text="⚡ PRE-WARM VULKAN SHADERS"); self.warm_status.config(text=message,fg=color)
    def build_controls_tab(self):
        f=self.tab_controls
        self.create_row_dropdown(f,"Controller Button Prompts","pad_layout",["auto","nintendo","xbox","xbox_prompts"],"auto","Select Nintendo-style, Xbox-style, or automatic prompts.")
        self.create_row_toggle(f,"Motion Controls","motion","1","Enable motion input for supported controllers.")
        self.create_row_slider(f,"Controller Rumble Strength","rumble_strength",0,300,130,"%","Set controller vibration strength.")
        self.create_row_dropdown(f,"Rumble Response Curve","rumble_curve",["0.2","0.4","0.6","0.8","1.0","1.2","1.5"],"0.6","Shape the response to small and large rumble events.")
        self.create_row_toggle(f,"Rumble Triggers","rumble_triggers","0","Enable trigger rumble where the controller supports it.")
        self.create_row_slider(f,"Stick Deadzone","stick_deadzone",0,50,8,"%","Ignore small stick movements below this threshold.")
        self.create_row_slider(f,"Mouse Sensitivity","mouse_sens",0,10,1,"x","Adjust mouse input sensitivity.")
        self.create_row_slider(f,"Game Volume","volume",0,100,100,"%","Set the game's master volume.")
        self.create_row_dropdown(f,"Audio Output Channels","audio_channels",["stereo","5.1"],self.get_val("audio_channels","stereo"),"Choose between stereo or 5.1 surround sound audio mixing.")
        loc = self.get_val("locale") or self.get_val("lang") or "en-US"
        self.create_row_dropdown(f,"Game Language","locale",["en-US","en-GB","ja","fr","de","it","es","nl","ru","pt","pt-BR","zh-Hans","zh-Hant","ko"],loc,"English is selected by default.")
        self.create_row_dropdown(f,"Launcher Menu Language","menu_lang",["en"],"en","The game menu and overlay are kept in English.")
        self.create_row_toggle(f,"Keep Game Active When Unfocused","assume_focus","1","Keep the game running when its window is not in front.")
        row=tk.Frame(f,bg=COLOR_BG_DARK,pady=8); row.pack(fill=tk.X,padx=20)
        self.sound_effects_var=tk.BooleanVar(value=self.parent_app.launcher_cfg.get("launcher_sound_effects",True))
        tk.Checkbutton(row,text="Launcher Sound Effects",variable=self.sound_effects_var,font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK,activebackground=COLOR_BG_DARK,activeforeground=COLOR_GOLD,selectcolor=COLOR_CARD,bd=0,highlightthickness=0).pack(side=tk.LEFT)
    def save_and_close(self):
        if not GAME_DIR:messagebox.showerror("Settings","Choose the DKB game folder first.",parent=self); return
        updates={k:(f"{v.get()/10:.1f}" if k in {"rumble_curve"} else str(v.get())) for k,v in self.vars.items()}
        if "locale" in updates:
            updates["lang"] = updates["locale"]
        updates["reflex"] = updates.get("reflex", "boost")
        updates["low_latency"] = updates.get("reflex", "boost")
        fg_m = updates.get("fg_mode", "msm_fg")
        updates["frame_gen"] = "0" if fg_m == "off" else "1"
        try:
            save_ini_settings(INI_PATH,updates)
            self.parent_app.launcher_cfg["target_display"]=self.target_monitor["device"]
            self.parent_app.launcher_cfg["launcher_sound_effects"]=self.sound_effects_var.get(); save_config(self.parent_app.launcher_cfg)
            play_ui_sound.enabled=self.sound_effects_var.get(); self.parent_app.move_to_monitor(self.target_monitor)
            messagebox.showinfo("Settings Saved","DKB game settings have been saved.",parent=self); self.destroy()
        except Exception as exc:messagebox.showerror("Settings","Could not save the DKB settings:\n"+str(exc),parent=self)
    def restore_defaults(self):
        if not messagebox.askyesno("Restore Defaults","Reset this page to the DKB launcher defaults?",parent=self):return
        defaults={"fullscreen":"0","window_size":"1920x1080","output_res":"1920x1080","fps_limit":"60","vk_present":"mailbox","aspect":"fit","fov":"100","perf_overlay":"off",
            "upscaler":"off","upscaler_preset":"quality","fg_mode":"off","fg_mult":"2x","fg_type":"fixed","render_scale":"game","sharpness":"80","reflex":"off","device_profile":"auto","pipe_wait":"default","pipe_prebuild":"1",
            "color_preset":"default","color_brightness":"0","color_contrast":"100","color_saturation":"100","color_gamma":"1.0",
            "fxaa":"1",
            "experimental_geometry":"0","fix_vs_fault":"1","fix_zero":"1","lod_guard":"1","fix_indirect_reset":"1","lib_prebuild":"1",
            "pad_layout":"auto","motion":"1","rumble_strength":"130","rumble_curve":"0.6","rumble_triggers":"0","stick_deadzone":"8",
            "mouse_sens":"1","volume":"100","locale":"en-US","menu_lang":"en","assume_focus":"1"}
        for k,v in defaults.items():
            if k in self.vars:self.vars[k].set(v)
        self.sound_effects_var.set(True)

_initial_game=find_game_dir(LAUNCHER_CONFIG)
if _initial_game:set_game_dir(_initial_game)


class DKBModsManagerDialog(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent); self.parent_app=parent; self.title("Bananza Mod Manager"); self.geometry("720x500"); self.minsize(600,380)
        self.configure(bg=COLOR_BG_DARK); self.transient(parent)
        head=tk.Frame(self,bg=COLOR_CARD,padx=22,pady=14); head.pack(fill=tk.X)
        tk.Label(head,text="MOD MANAGER",font=("Segoe UI",14,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD).pack(anchor="w")
        tk.Label(head,text="Toggle DKB RomFS overlay packages. Put each mod's RomFS tree in its own folder.",font=("Segoe UI",9),fg=COLOR_TEXT_DIM,bg=COLOR_CARD).pack(anchor="w",pady=(3,0))
        tk.Label(head,text="Enabled folders are composed into a separate overlay. If mods replace the same file, the later name alphabetically takes precedence.",font=("Segoe UI",8),fg=COLOR_TEXT_DIM,bg=COLOR_CARD,wraplength=640,justify="left").pack(anchor="w",pady=(4,0))
        body=tk.Frame(self,bg=COLOR_BG_DARK); body.pack(fill=tk.BOTH,expand=True,padx=20,pady=14)
        self.canvas=tk.Canvas(body,bg=COLOR_BG_DARK,highlightthickness=0); bar=ttk.Scrollbar(body,orient="vertical",command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=bar.set); bar.pack(side=tk.RIGHT,fill=tk.Y); self.canvas.pack(side=tk.LEFT,fill=tk.BOTH,expand=True)
        self.rows=tk.Frame(self.canvas,bg=COLOR_BG_DARK); self.win=self.canvas.create_window((0,0),window=self.rows,anchor="nw")
        self.rows.bind("<Configure>",lambda _e:self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>",lambda e:self.canvas.itemconfigure(self.win,width=e.width))
        foot=tk.Frame(self,bg=COLOR_CARD,height=60,padx=18); foot.pack(fill=tk.X,side=tk.BOTTOM); foot.pack_propagate(False)
        self.button(foot,"OPEN MODS FOLDER",self.open_mods,True).pack(side=tk.LEFT,pady=11)
        self.button(foot,"ADD MOD FOLDER",self.add_mod).pack(side=tk.LEFT,padx=10,pady=11)
        self.button(foot,"REFRESH",self.refresh).pack(side=tk.LEFT,padx=10,pady=11)
        self.button(foot,"CLOSE",self.destroy).pack(side=tk.RIGHT,pady=11); self.refresh()
    def button(self,p,text,cmd,primary=False):
        return tk.Button(p,text=text,command=cmd,font=("Segoe UI",8,"bold"),fg="#000" if primary else COLOR_WHITE,bg=COLOR_GOLD if primary else COLOR_BUTTON_HOVER,activebackground=COLOR_GOLD_HOVER,bd=0,padx=12,pady=7,cursor="hand2")
    def refresh(self):
        for c in self.rows.winfo_children():c.destroy()
        mods=scan_mod_folders()
        if not mods:
            tk.Label(self.rows,text="No mod folders found. Open the folder below and add one folder for each RomFS overlay package.",font=("Segoe UI",10),fg=COLOR_TEXT_DIM,bg=COLOR_BG_DARK,wraplength=560,justify="left").pack(anchor="w",padx=10,pady=18); return
        for mod in mods:
            row=tk.Frame(self.rows,bg=COLOR_CARD,padx=12,pady=10,highlightthickness=1,highlightbackground=COLOR_CARD_BORDER); row.pack(fill=tk.X,pady=4)
            var=tk.BooleanVar(value=mod["enabled"])
            tk.Checkbutton(row,text=mod["name"],variable=var,command=lambda p=mod["path"],v=var:self.toggle(p,v),font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_CARD,activebackground=COLOR_CARD,activeforeground=COLOR_GOLD,selectcolor=COLOR_TAB_ACTIVE,bd=0,highlightthickness=0).pack(side=tk.LEFT)
            tk.Label(row,text="ENABLED" if mod["enabled"] else "DISABLED",font=("Segoe UI",8,"bold"),fg=COLOR_ACCENT_GREEN if mod["enabled"] else COLOR_GOLD,bg=COLOR_CARD).pack(side=tk.RIGHT,padx=8)
    def toggle(self,path,var):
        warning=game_process_warning("changing DKB mods")
        if warning:messagebox.showwarning("Close the Game First",warning,parent=self); self.refresh(); return
        try:set_mod_folder_enabled(path,var.get()); rebuild_mod_overlay(); self.refresh()
        except Exception as exc:messagebox.showerror("Mod Update Failed",str(exc),parent=self); self.refresh()
    def open_mods(self):
        if not GAME_DIR:messagebox.showerror("Mods","Select a DKB game folder first.",parent=self); return
        open_folder(MODS_DIR)
    def add_mod(self):
        if not GAME_DIR:messagebox.showerror("Mods","Select a DKB game folder first.",parent=self); return
        try:create_mod_folder(); self.refresh(); open_folder(MODS_DIR)
        except Exception as exc:messagebox.showerror("Mods",str(exc),parent=self)


class DKBSaveBackupVaultDialog(tk.Toplevel):
    def __init__(self,parent):
        super().__init__(parent); self.title("DKB Save Backup Vault"); self.geometry("680x460"); self.minsize(560,360)
        self.configure(bg=COLOR_BG_DARK); self.transient(parent); self.profile_dir=PROFILE_DIR; self.backups_dir=PROFILE_DIR/"backups"; self.snapshots=[]
        head=tk.Frame(self,bg=COLOR_CARD,padx=20,pady=14); head.pack(fill=tk.X)
        tk.Label(head,text="SAVE BACKUP VAULT",font=("Segoe UI",13,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD).pack(anchor="w")
        tk.Label(head,text=f"Snapshots are stored in {self.backups_dir}.",font=("Segoe UI",9),fg=COLOR_TEXT_DIM,bg=COLOR_CARD,wraplength=630,justify="left").pack(anchor="w",pady=(3,0))
        body=tk.Frame(self,bg=COLOR_BG_DARK); body.pack(fill=tk.BOTH,expand=True,padx=20,pady=14)
        tk.Label(body,text="Available snapshots",font=("Segoe UI",10,"bold"),fg=COLOR_WHITE,bg=COLOR_BG_DARK).pack(anchor="w",pady=(0,8))
        self.list=tk.Listbox(body,bg=COLOR_CARD,fg=COLOR_WHITE,selectbackground=COLOR_TAB_ACTIVE,selectforeground=COLOR_GOLD,font=("Segoe UI",9),highlightthickness=1,highlightbackground=COLOR_CARD_BORDER,activestyle="none"); self.list.pack(fill=tk.BOTH,expand=True)
        foot=tk.Frame(self,bg=COLOR_CARD,padx=14,pady=12); foot.pack(fill=tk.X,side=tk.BOTTOM)
        self.button(foot,"CREATE BACKUP SNAPSHOT",self.create,True).pack(side=tk.LEFT)
        self.button(foot,"RESTORE BACKUP",self.restore).pack(side=tk.LEFT,padx=8)
        self.button(foot,"OPEN BACKUPS FOLDER",self.open_backups).pack(side=tk.RIGHT); self.refresh()
    def button(self,p,text,cmd,primary=False):
        return tk.Button(p,text=text,command=cmd,font=("Segoe UI",8,"bold"),fg="#000" if primary else COLOR_WHITE,bg=COLOR_GOLD if primary else COLOR_BUTTON_HOVER,activebackground=COLOR_GOLD_HOVER,bd=0,padx=11,pady=8)
    def refresh(self):
        self.backups_dir.mkdir(parents=True,exist_ok=True)
        self.snapshots=sorted((p for p in self.backups_dir.iterdir() if p.is_dir() and p.name.startswith("save_")),key=lambda p:p.name,reverse=True)
        self.list.delete(0,tk.END)
        for p in self.snapshots:self.list.insert(tk.END,p.name)
        if not self.snapshots:self.list.insert(tk.END,"No save snapshots yet")
    def create(self):
        warning=game_process_warning("creating a save snapshot")
        if warning:messagebox.showwarning("Close the Game First",warning,parent=self); return
        try:p=create_save_snapshot(self.profile_dir,self.backups_dir); self.refresh(); messagebox.showinfo("Backup Created",f"Save snapshot created:\n{p.name}",parent=self)
        except Exception as exc:messagebox.showerror("Backup Failed",str(exc),parent=self)
    def restore(self):
        sel=self.list.curselection()
        if not sel or sel[0]>=len(self.snapshots):messagebox.showinfo("Select a Backup","Select a save snapshot to restore.",parent=self); return
        warning=game_process_warning("restoring a save snapshot")
        if warning:messagebox.showwarning("Close the Game First",warning,parent=self); return
        snap=self.snapshots[sel[0]]
        if not messagebox.askyesno("Confirm Save Restore",f"Restore {snap.name}? The current save is backed up first.",parent=self):return
        try:safety=restore_save_snapshot(self.profile_dir,snap); self.refresh(); messagebox.showinfo("Backup Restored",f"Restored {snap.name}.\nSafety snapshot: {safety.name}",parent=self)
        except Exception as exc:messagebox.showerror("Restore Failed",str(exc),parent=self)
    def open_backups(self):self.backups_dir.mkdir(parents=True,exist_ok=True); open_folder(self.backups_dir)


class DKBLauncherApp(tk.Tk):
    def __init__(self):
        super().__init__(); self.title(APP_TITLE); self.geometry("1120x630"); self.resizable(False,False); self.configure(bg=COLOR_BG_DARK)
        self.launcher_cfg=load_config(); self.protocol("WM_DELETE_WINDOW",self.safe_quit)
        icon=resource_dir()/"assets"/"dkb.ico"
        if icon.is_file():
            try:self.iconbitmap(str(icon))
            except Exception:pass
        self.update_idletasks(); w,h=1120,630; self.geometry(f"{w}x{h}+{(self.winfo_screenwidth()-w)//2}+{(self.winfo_screenheight()-h)//2}")
        play_ui_sound.enabled=bool(self.launcher_cfg.get("launcher_sound_effects",True))
        monitors=enumerate_displays()
        self.target_monitor=next((m for m in monitors if m["device"]==self.launcher_cfg.get("target_display")),next((m for m in monitors if m["primary"]),monitors[0]))
        self.latest_release=None; self.update_in_progress=False; self.setup_background(); self.setup_ui_overlay(); self.after(600, self.check_for_updates)
        if self.launcher_cfg.get("target_display"):self.move_to_monitor(self.target_monitor)
    def safe_quit(self):self.destroy()
    def move_to_monitor(self,monitor):place_window_on_monitor(self,monitor)
    def setup_background(self):
        self.canvas=tk.Canvas(self,width=1120,height=630,highlightthickness=0,bg=COLOR_BG_DARK); self.canvas.pack(fill=tk.BOTH,expand=True)
        path = find_asset("bananza_background.png")
        if not path: return
        try:
            im=Image.open(path).convert("RGBA").resize((1120,630),Image.Resampling.LANCZOS)
            overlay=Image.new("RGBA",im.size,(0,0,0,0)); draw=ImageDraw.Draw(overlay)
            for x in range(570):
                alpha=int(225*(1-(x/570)**0.82)); draw.line((x,0,x,630),fill=(7,12,10,alpha))
            draw.rectangle((0,500,1120,630),fill=(7,11,9,85))
            self.bg_img=ImageTk.PhotoImage(Image.alpha_composite(im,overlay)); self.canvas.create_image(0,0,image=self.bg_img,anchor="nw")
        except Exception as exc:print("Could not load launcher artwork:",exc)
    def _menu_bind(self,item,callback,primary=False):
        base=COLOR_GOLD if primary else COLOR_WHITE
        item.bind("<Enter>",lambda _e:(play_ui_sound("hover"),item.config(fg=COLOR_GOLD_HOVER if primary else COLOR_GOLD),self.config(cursor="hand2")))
        item.bind("<Leave>",lambda _e:(item.config(fg=base),self.config(cursor="")))
        item.bind("<Button-1>",lambda _e:(play_ui_sound("click"),callback()))
    def setup_ui_overlay(self):
        self.canvas.create_text(70,62,text="DKB RECOMP",font=("Segoe UI",11,"bold"),fill=COLOR_GOLD,anchor="w")
        self.canvas.create_text(70,104,text="DONKEY KONG BANANZA",font=("Impact",34),fill=COLOR_WHITE,anchor="w")
        self.canvas.create_text(70,141,text=f"PC RECOMPILED  ·  {CURRENT_VERSION.upper()}",font=("Segoe UI",10,"bold"),fill=COLOR_ACCENT_GREEN,anchor="w")
        for i,(label,cb,primary) in enumerate([("PLAY",self.launch_game,True),("SETTINGS",self.open_settings,False),("DIGITAL EXTRAS",self.open_extras,False),("QUIT",self.safe_quit,False)]):
            item=tk.Label(self,text=label,font=("Segoe UI",22,"bold"),fg=COLOR_GOLD if primary else COLOR_WHITE,bg=COLOR_BG_DARK,cursor="hand2")
            self.canvas.create_window(70,230+i*62,window=item,anchor="w"); self._menu_bind(item,cb,primary)
        footer=tk.Frame(self,bg=COLOR_CARD,bd=1,relief="solid",padx=16,pady=10)
        self.keep_launcher_var=tk.BooleanVar(value=self.launcher_cfg.get("show_launcher",True))
        tk.Checkbutton(footer,text="KEEP LAUNCHER OPEN",variable=self.keep_launcher_var,command=self._save_keep_launcher,font=("Segoe UI",9,"bold"),fg=COLOR_WHITE,bg=COLOR_CARD,activebackground=COLOR_CARD,activeforeground=COLOR_GOLD,selectcolor=COLOR_CARD_BORDER,bd=0,highlightthickness=0).pack(anchor="e")
        tk.Label(footer,text=VERSION_STRING,font=("Segoe UI",9,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD).pack(anchor="e",pady=(4,0))
        self.update_status=tk.Label(footer,text="",font=("Segoe UI",8),fg=COLOR_ACCENT_GREEN,bg=COLOR_CARD,wraplength=235,justify="right"); self.update_status.pack(anchor="e",pady=(4,0))
        self.update_action_btn=tk.Button(footer,text="⚡ UPDATE NOW (1-CLICK)",command=self.apply_latest_update,font=("Segoe UI",8,"bold"),fg="#000",bg=COLOR_ACCENT_GREEN,activebackground="#8ef0a5",bd=0,padx=10,pady=6,cursor="hand2"); self.update_action_btn.pack_forget()
        tk.Button(footer,text="CHECK FOR UPDATES",command=self.check_for_updates,font=("Segoe UI",8,"bold"),fg=COLOR_WHITE,bg=COLOR_BUTTON_HOVER,activebackground=COLOR_GOLD,activeforeground="#000",bd=0,padx=10,pady=5,cursor="hand2").pack(anchor="e",pady=(6,0))
        self.release_btn=tk.Button(footer,text="VIEW RELEASE NOTES",command=self.open_latest_release,font=("Segoe UI",8,"bold"),fg="#000",bg=COLOR_GOLD,activebackground=COLOR_GOLD_HOVER,bd=0,padx=10,pady=6,cursor="hand2"); self.release_btn.pack_forget()
        self.canvas.create_window(1050,565,window=footer,anchor="se")
        repo=tk.Button(self.canvas,text="★ DKB Recomp Project",font=("Segoe UI",9,"bold"),fg=COLOR_GOLD,bg=COLOR_CARD,activebackground=COLOR_CARD_BORDER,activeforeground=COLOR_GOLD_HOVER,bd=1,relief="solid",padx=14,pady=6,cursor="hand2",command=lambda:webbrowser.open(GITHUB_URL)); self.canvas.create_window(70,570,window=repo,anchor="w")
        self.canvas.create_text(70,608,text=f"{CURRENT_VERSION.upper()} RELEASE  ·  USE YOUR OWN GAME DATA",font=("Segoe UI",8,"bold"),fill=COLOR_TEXT_DIM,anchor="w")
    def _save_keep_launcher(self):self.launcher_cfg["show_launcher"]=self.keep_launcher_var.get(); save_config(self.launcher_cfg)
    def choose_game_directory(self):
        selected=filedialog.askdirectory(title="Select the folder containing dkb.exe")
        if not selected:return
        if not valid_game_dir(selected):messagebox.showerror("DKB Game Folder","Choose the folder that contains dkb.exe.",parent=self); return
        self.launcher_cfg["game_dir"]=str(Path(selected).resolve()); save_config(self.launcher_cfg); set_game_dir(selected)
        self.update_status.config(text="DKB game folder selected.",fg=COLOR_ACCENT_GREEN)
    def _ensure_game(self):
        if GAME_DIR and valid_game_dir(GAME_DIR):return True
        messagebox.showinfo("Select DKB Game Folder","Choose the folder containing your locally built dkb.exe.",parent=self); self.choose_game_directory()
        return bool(GAME_DIR and valid_game_dir(GAME_DIR))
    def _pack_loose_romfs_if_needed(self, romfs_dir: Path, dst_bin: Path) -> bool:
        if not romfs_dir.is_dir():
            return False
        loose_files = [f for f in os.listdir(romfs_dir) if f not in ("romfs.bin", "index.json", ".romfs-verified-v1") and not f.startswith(".")]
        if not loose_files:
            return False
        if not messagebox.askyesno(
            "Pack Loose RomFS Data?",
            f"Loose game files were found in:\n{romfs_dir}\n\n"
            f"Donkey Kong Bananza PC Recomp requires these files to be packed into 'romfs/romfs.bin'.\n\n"
            f"Would you like DKB Launcher to pack 'romfs/romfs.bin' automatically now? (Takes ~1–2 minutes)",
            parent=self,
        ):
            return False
        self.update_status.config(text="Packing romfs/romfs.bin from loose game files…", fg=COLOR_GOLD)
        self.update_idletasks()
        try:
            align = 16
            chunk_sz = 8 << 20
            files_list = []
            for root_dir, _, fnames in os.walk(romfs_dir):
                fnames.sort()
                for fname in fnames:
                    if not fname.startswith(".") and fname not in ("romfs.bin", "index.json", ".romfs-verified-v1"):
                        fp = Path(root_dir) / fname
                        files_list.append((fp.relative_to(romfs_dir).as_posix(), fp))
            files_list.sort(key=lambda e: e[0].encode())
            entries, off = [], 0
            temp_bin = str(dst_bin) + ".tmp"
            sha = hashlib.sha256()
            total_cnt = len(files_list)
            with open(temp_bin, "wb") as dst_f:
                for idx_f, (rel_p, full_p) in enumerate(files_list):
                    pad = -off % align
                    if pad:
                        dst_f.write(b"\0" * pad)
                        sha.update(b"\0" * pad)
                        off += pad
                    sz = 0
                    with open(full_p, "rb") as src_f:
                        while chunk := src_f.read(chunk_sz):
                            dst_f.write(chunk)
                            sha.update(chunk)
                            sz += len(chunk)
                    entries.append({"path": rel_p, "offset": off, "size": sz})
                    off += sz
                    if idx_f % 5000 == 0:
                        self.update_status.config(text=f"Packing romfs.bin: {idx_f}/{total_cnt} files ({off / (1024**3):.1f} GB)…", fg=COLOR_GOLD)
                        self.update_idletasks()
            if dst_bin.exists():
                dst_bin.unlink()
            os.rename(temp_bin, dst_bin)
            manifest = {"status": "pass", "source": "packed from dump", "romfs_size": off, "romfs_sha256": sha.hexdigest(), "files": entries}
            with open(romfs_dir / "index.json", "w", encoding="utf-8") as mf:
                json.dump(manifest, mf)
            self.update_status.config(text="romfs.bin packed and ready!", fg=COLOR_ACCENT_GREEN)
            return True
        except Exception as exc:
            messagebox.showerror("RomFS Pack Failed", f"Could not pack romfs/romfs.bin:\n{exc}", parent=self)
            return False

    def launch_game(self):
        if not self._ensure_game():return
        exefs_main = GAME_DIR / "exefs" / "main"
        exefs_sdk = GAME_DIR / "exefs" / "sdk"
        romfs_bin = GAME_DIR / "romfs" / "romfs.bin"
        if not romfs_bin.is_file() and (GAME_DIR / "romfs.bin").is_file():
            (GAME_DIR / "romfs").mkdir(parents=True, exist_ok=True)
            try:
                os.symlink(GAME_DIR / "romfs.bin", romfs_bin)
            except Exception:
                shutil.move(str(GAME_DIR / "romfs.bin"), str(romfs_bin))
        if not romfs_bin.is_file():
            self._pack_loose_romfs_if_needed(GAME_DIR / "romfs", romfs_bin)
        missing = []
        if not exefs_main.is_file(): missing.append("exefs/main")
        if not exefs_sdk.is_file(): missing.append("exefs/sdk")
        if not romfs_bin.is_file(): missing.append("romfs/romfs.bin")
        if missing:
            messagebox.showerror(
                "Game Data Missing",
                f"Cannot start Donkey Kong Bananza because the following required dump files are missing from {GAME_DIR}:\n\n"
                f"  • " + "\n  • ".join(missing) + "\n\n"
                f"Please run DKB-Setup.exe and select your .nup container or unpacked game dump folder (containing exefs/main, exefs/sdk, and romfs/romfs.bin).",
                parent=self,
            )
            return
        env=os.environ.copy(); _gpu,vendor=detect_gpu()
        if vendor!="Other" and not env.get("DKB_GPU_NAME"):env["DKB_GPU_NAME"]="NVIDIA" if vendor=="NVIDIA" else vendor
        command = [str(GAME_DIR / "dkb.exe")]
        try:
            args=[]
            if len(sys.argv)>1:args=[a for a in sys.argv[1:] if a!="--launcher"]
            command += args
            p=subprocess.Popen(command,cwd=str(GAME_DIR),env=env,creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0) if command[0].lower().endswith("powershell.exe") else 0)
            if not self.keep_launcher_var.get():
                self.withdraw()
                self.after(2500,self.safe_quit)
            if p.pid:
                threading.Thread(target=self._place_game_window,args=(p.pid,self.target_monitor),daemon=True).start()
        except Exception as exc:messagebox.showerror("Launch Error",f"Could not start dkb.exe:\n{exc}",parent=self)
    def _place_game_window(self,pid,monitor):
        if os.name!="nt":return
        try:
            user32=ctypes.windll.user32; found=[]; callback=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
            def visit(hwnd,_):
                proc=wintypes.DWORD(); user32.GetWindowThreadProcessId(hwnd,ctypes.byref(proc))
                if proc.value==pid and user32.IsWindowVisible(hwnd):found.append(hwnd)
                return True
            for _ in range(80):
                found.clear(); user32.EnumWindows(callback(visit),0)
                if found:break
                time.sleep(.25)
            if found:
                hwnd = found[0]
                user32.SetForegroundWindow(hwnd)
                user32.BringWindowToTop(hwnd)
        except Exception:pass
    def check_for_updates(self):
        if getattr(self, "release_check_active", False) or getattr(self, "update_in_progress", False): return
        self.release_check_active = True
        self.update_action_btn.pack_forget()
        self.release_btn.pack_forget()
        self.update_status.config(text="Checking GitHub Releases…", fg=COLOR_TEXT_DIM)
        threading.Thread(target=self._check_release_worker, daemon=True).start()

    def _check_release_worker(self):
        try:
            req = urllib.request.Request(RELEASES_API_URL, headers={"Accept": "application/vnd.github+json", "User-Agent": "DKBLauncher"})
            with urllib.request.urlopen(req, timeout=12) as r:
                data = json.loads(r.read().decode("utf-8"))
            tag = (data.get("tag_name") or "").strip()
            title = (data.get("name") or tag or "Latest DKB release").strip()
            page = data.get("html_url") or RELEASES_URL
            if not tag:
                raise ValueError("The latest release did not include a version tag.")
            assets = data.get("assets", [])
            tag_clean = tag.replace("-", "").replace(".", "")
            candidates = [
                f"SteelTemple-DKB-{tag}-Update.zip",
                f"SteelTemple-DKB-{tag_clean}-Update.zip",
                f"SteelTemple-DKB-Update-{tag}.zip",
                f"dkb-{tag}-update.zip",
            ]
            asset = next((item for item in assets for cand in candidates if item.get("name", "").casefold() == cand.casefold()), None)
            if not asset:
                asset = next((item for item in assets if "update" in item.get("name", "").lower() and item.get("name", "").lower().endswith(".zip")), None)
            asset_name = asset.get("name") if asset else None
            asset_url = asset.get("browser_download_url") if asset else None
            self.after(0, lambda: self._show_release_result(tag, title, page, asset_url, asset_name))
        except Exception as exc:
            detail = str(exc)
            if "404" in detail or "Not Found" in detail:
                self.after(0, lambda: self._show_release_result(CURRENT_VERSION, f"Your DKB is up to date ({CURRENT_VERSION}).", RELEASES_URL, None, None))
            else:
                self.after(0, lambda: self._show_release_error("Update check failed: " + detail))

    def _show_release_error(self, message):
        self.release_check_active = False
        self.update_status.config(text=message, fg="#ff7373")

    def _show_release_result(self, tag, title, page, asset_url=None, asset_name=None):
        self.release_check_active = False
        self.latest_release = {"tag": tag, "title": title, "page": page, "asset_url": asset_url, "asset_name": asset_name}
        clean_title = title.split(":", 1)[0].strip()
        if is_same_version(CURRENT_VERSION, tag) or is_same_version(CURRENT_VERSION, clean_title):
            self.update_status.config(text=f"Your DKB is up to date ({CURRENT_VERSION})", fg=COLOR_ACCENT_GREEN)
            self.update_action_btn.pack_forget()
            self.release_btn.pack_forget()
            return
        if not asset_url:
            self.update_status.config(text=f"Update found ({clean_title}), but update zip is missing.", fg=COLOR_GOLD)
            self.update_action_btn.pack_forget()
            self.release_btn.config(text="VIEW RELEASE NOTES")
            self.release_btn.pack(anchor="e", pady=(5, 0))
            return
        self.update_status.config(text=f"Update available: {clean_title}", fg=COLOR_GOLD)
        self.update_action_btn.config(state=tk.NORMAL, text="⚡ UPDATE NOW (1-CLICK)")
        self.update_action_btn.pack(anchor="e", pady=(5, 0))
        self.release_btn.config(text="VIEW RELEASE NOTES")
        self.release_btn.pack(anchor="e", pady=(3, 0))

    def open_latest_release(self):
        url = self.latest_release["page"] if (self.latest_release and self.latest_release.get("page")) else RELEASES_URL
        webbrowser.open(url)

    def apply_latest_update(self):
        if getattr(self, "update_in_progress", False) or not self.latest_release:
            return
        warning = game_process_warning("applying a launcher/game update")
        if warning:
            messagebox.showwarning("Close the Game First", warning, parent=self)
            return
        title = self.latest_release.get("title", "DKB Update")
        if not messagebox.askyesno("Install DKB Update", f"Download and install {title} automatically now?\n\nThis will update your game executable, launcher, and pre-compiled Vulkan shaders.", parent=self):
            return
        self.update_in_progress = True
        self.update_action_btn.config(state=tk.DISABLED, text="DOWNLOADING UPDATE…")
        self.update_status.config(text="Connecting to GitHub Releases…", fg=COLOR_TEXT_DIM)
        threading.Thread(target=self._download_update_worker, args=(self.latest_release.copy(),), daemon=True).start()

    def _download_update_worker(self, release):
        stage = None
        try:
            if not GAME_DIR:
                raise ValueError("Game folder not selected.")
            GAME_DIR.mkdir(parents=True, exist_ok=True)
            stage = Path(tempfile.mkdtemp(prefix=".dkb-update-", dir=GAME_DIR))
            archive_path = stage / release["asset_name"]
            request = urllib.request.Request(release["asset_url"], headers={"User-Agent": "DKBLauncher", "Accept": "application/octet-stream"})
            with urllib.request.urlopen(request, timeout=60) as response, archive_path.open("wb") as output:
                total = int(response.headers.get("Content-Length", "0") or 0)
                downloaded = 0
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    output.write(block)
                    downloaded += len(block)
                    if total:
                        percent = min(100, downloaded * 100 // total)
                        self._set_update_status(f"Downloading update… {percent}%", COLOR_TEXT_DIM)
            self._set_update_status("Validating update archive…", COLOR_TEXT_DIM)
            files = extract_update_archive(archive_path, stage)
            archive_path.unlink(missing_ok=True)
            if not {"dkb.exe", "DKBLauncher.exe"}.issubset(set(files)):
                raise ValueError("The validated update archive is missing a required executable.")
            self._set_update_status("Verifying updated game executable…", COLOR_TEXT_DIM)
            validate_update_game_executable(stage / "dkb.exe", GAME_DIR)
            self.after(0, lambda: self._update_ready(release, stage, files))
        except Exception as exc:
            if stage:
                shutil.rmtree(stage, ignore_errors=True)
            detail = str(exc)
            self.after(0, lambda: self._update_failed(detail))

    def _set_update_status(self, text, color):
        try:
            self.after(0, lambda: self.update_status.config(text=text, fg=color))
        except tk.TclError:
            pass

    def _update_failed(self, detail):
        self.update_in_progress = False
        self.update_action_btn.config(state=tk.NORMAL, text="⚡ UPDATE NOW (1-CLICK)")
        self.update_status.config(text=f"Update failed:\n{detail[:100]}", fg="#ff7373")

    def _update_ready(self, release, stage, files):
        title = release.get("title", "DKB Update")
        if not messagebox.askyesno("Update Ready", f"{title} is downloaded and verified.\n\nInstall it now? The launcher will close, apply the files, and restart automatically.", parent=self):
            shutil.rmtree(stage, ignore_errors=True)
            self.update_in_progress = False
            self.update_action_btn.config(state=tk.NORMAL, text="⚡ UPDATE NOW (1-CLICK)")
            self.update_status.config(text="Update downloaded; installation cancelled.", fg=COLOR_TEXT_DIM)
            return
        try:
            self._start_update_relauncher(stage, title)
            os._exit(0)
        except Exception as exc:
            shutil.rmtree(stage, ignore_errors=True)
            self._update_failed(str(exc))

    def _start_update_relauncher(self, stage, title):
        def ps_quote(value):
            return "'" + str(value).replace("'", "''") + "'"
        game_path = ps_quote(GAME_DIR.resolve())
        stage_path = ps_quote(Path(stage).resolve())
        title_text = ps_quote(title)
        script = f'''
$ErrorActionPreference = 'Stop'
$game = {game_path}
$stage = {stage_path}
$rollback = Join-Path $stage '_rollback'
New-Item -ItemType Directory -Force -Path $rollback | Out-Null
$changed = [System.Collections.Generic.List[string]]::new()
try {{
    Wait-Process -Id {os.getpid()} -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Get-ChildItem -LiteralPath $stage -File | ForEach-Object {{
        $name = $_.Name
        if ($name.StartsWith('_') -or $name.EndsWith('.tmp') -or $name.EndsWith('.zip')) {{ return }}
        $source = $_.FullName
        $target = Join-Path $game $name
        $backup = Join-Path $rollback $name
        if (Test-Path -LiteralPath $target -PathType Leaf) {{ Copy-Item -LiteralPath $target -Destination $backup -Force }}
        $changed.Add($name)
        $newFile = $target + '.update-new'
        Copy-Item -LiteralPath $source -Destination $newFile -Force
        Move-Item -LiteralPath $newFile -Destination $target -Force
    }}
}} catch {{
    try {{ Add-Type -AssemblyName System.Windows.Forms }} catch {{}}
    for ($i = $changed.Count - 1; $i -ge 0; $i--) {{
        $name = $changed[$i]; $target = Join-Path $game $name; $backup = Join-Path $rollback $name
        if (Test-Path -LiteralPath $backup -PathType Leaf) {{ Copy-Item -LiteralPath $backup -Destination $target -Force }}
        else {{ Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue }}
    }}
    [System.Windows.Forms.MessageBox]::Show(('Update failed and changes were rolled back: ' + $_.Exception.Message), 'Update Failed') | Out-Null
    exit 1
}}
try {{ Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.MessageBox]::Show(({title_text} + ' installed successfully. The launcher will restart.'), 'Update Complete') | Out-Null }} catch {{}}
$targetLauncher = Join-Path $game 'DKBLauncher.exe'
if (Test-Path -LiteralPath $targetLauncher -PathType Leaf) {{
    Start-Process -FilePath $targetLauncher -WorkingDirectory $game
}}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
'''
        encoded = base64.b64encode(script.encode("utf-16le")).decode("ascii")
        flags = getattr(subprocess, "DETACHED_PROCESS", 0)
        subprocess.Popen(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
                          "-EncodedCommand", encoded], cwd=str(GAME_DIR), creationflags=flags,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def open_settings(self):
        if not self._ensure_game():return
        DKBSettingsDialog(self)
    def open_extras(self):
        menu=tk.Menu(self,tearoff=0,bg=COLOR_CARD,fg=COLOR_WHITE,activebackground=COLOR_GOLD,activeforeground="#000",font=("Segoe UI",10,"bold"),bd=1)
        menu.add_command(label="🌐  Visit DKB Recomp Project",command=lambda:webbrowser.open(GITHUB_URL))
        menu.add_command(label="⬆  Check for DKB Releases",command=self.check_for_updates)
        menu.add_command(label="🧩  Open Mod Manager",command=lambda:DKBModsManagerDialog(self))
        menu.add_command(label="🛡  Save Backup Vault",command=self.open_save_vault); menu.add_separator()
        for label,path in (("📁  Open Mods Folder","mods"),("💾  Open Save Data Folder","save"),("🔮  Open Shader Cache","shader-cache"),("📄  Open DKB Logs","logs")):
            menu.add_command(label=label,command=lambda p=path:self.open_game_folder(p))
        menu.add_separator(); menu.add_command(label="🎮  View Controller Guide",command=self.open_controller_guide)
        menu.tk_popup(self.winfo_pointerx(),self.winfo_pointery())
    def open_game_folder(self,relative):
        if not self._ensure_game():return
        path=PROFILE_DIR/"save" if relative=="save" else GAME_DIR/relative; open_folder(path)
    def open_save_vault(self):
        if not self._ensure_game():return
        DKBSaveBackupVaultDialog(self)
    def open_controller_guide(self):
        messagebox.showinfo("Controller Guide","Bananza PC Recomp controller notes\n\nConnect a controller before launching. SDL gamepads are supported; button prompts can be changed in Settings → Controls & Audio.\n\nMotion input, rumble, trigger rumble, and button prompts depend on the controller and DKB build.",parent=self)


def main():
    app=DKBLauncherApp(); app.mainloop()

if __name__=="__main__":main()
