// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_present.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
//
// The look (colours, rounding, the tab bar with L / R and Q / E, the window over a dimmed game) follows
// SavageWindRecomp's runtime/ui/menu.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai); the pages are rows of option_rows.cpp.
#include <algorithm>
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, NavCursorVisible
#include "backends/imgui_impl_vulkan.h"
// A private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system font before the
// atlas gets it (font_usable). Its asserts only mark the font bad here.
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop
#include "ui/binds_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "ui_text.h"
// host.h and vk.h pull in cpu.h, which is C only: the functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
void res_sizes(unsigned out[2], unsigned scene[2]);
void vk_dlss_meter(char *b, int n);
// Vulkan & GPU capabilities
int vk_gpu_is_nvidia(void);
int vk_gpu_is_rtx40_plus(void);
int vk_gpu_is_rtx50(void);
int vk_gpu_is_rx9000(void);
int vk_gpu_is_intel(void);
int vk_gpu_is_intel_arc(void);
int vk_has_amd_antilag(void);
int vk_fg_active_multiplier(void);

double frame_step_limit_hz(void);
extern const int rt_game_version;
// fs.h (romfs.c): romfs-patches-xbox was next to romfs-index.bin at start, and was loaded.
int romfs_xbox_prompts_ready(void);
int romfs_xbox_prompts_active(void);
extern const char rt_port_version[];
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input

// g_open, the input block and the log line change together under open_lock, so they cannot disagree when the window thread
// (F1, Esc), the polling thread (combo) and the present path (Continue, B) race.
static PlatMutex open_lock = PLAT_MUTEX_INIT;

// want 0 or 1 sets the state; -1 flips it. Returns the new state.
static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        g_reset.store(1);
        input_menu(open);
        rt_log("menu: %s", open ? "open" : "closed");
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

static int g_failed_flag();
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (!g_failed_flag()) change_open(-1);
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;   // buttons at the last frame; a button held when the menu opened counts once released
static int stick_dir = -1;  // the D-pad button the left stick holds, -1 none

// The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
// it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one.
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;   // y up
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (reset) {
        pad_prev = b;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok;              // context made
static std::atomic<int> g_failed; // set-up failed for good (the menu stays closed)
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;               // back end initialised for g_vk_fmt
static VkFormat g_vk_fmt;
static float g_font_scale;        // the scale of the uploaded font atlas; 0 when none is uploaded
static float g_style_scale;       // the scale the style was last sized for
static ImGuiStyle g_base_style;   // the menu's style at 1080 lines
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

// Sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display (sizes are physical
// pixels) still gets text as large as the display scale asks for.
static float menu_scale(uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

// SavageWindRecomp's colours: dark green panels, gold accents.
static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "dkb_menu";
    style_init(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_present.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

// Roboto Medium (Latin, Cyrillic, dashes and quotes) at 26 px per 1080 lines, with the glyphs of the Japanese, Korean
// and Chinese language names merged in from a system font when there is one (each language is named in its own script).
// Made again when the window height changes; the old texture goes after the queue is idle (CreateFontsTexture).
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 1 when Dear ImGui can take this font file: more than the 100 bytes AddFontFromMemoryTTF asserts on, every table of its
// directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the glyphs
// are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process with
// 0xc0000409 and no line in the log.
static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof roboto_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, roboto_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);   // no character map
    int ok = font_usable(roboto_ttf, n) && !font_usable(roboto_ttf, 4096) && !font_usable(roboto_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);             // an unknown font tag
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    uint8_t *data[N] = {};
    ImVector<ImWchar> ranges[N];
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t size = 0;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 26.0f * scale, &mc, ranges[i].Data);
    }
    vk_queue_lock();   // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();   // the glyphs are in the texture: the font files and the pixels are not needed again
    fa->ClearTexData();
    for (uint8_t *d : data) free(d);
    rt_log("menu: font atlas at %.1f px, language names from %d of %d system fonts", 26.0f * scale, merged, (int)N);
    g_font_scale = ok ? scale : 0;
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- test hooks
// DKB_MENU_TEST=<frame>[:<frames per page>]: at that presented frame the menu opens on its first tab; each tab stays
// <frames per page> frames (default 30) and its last frame's swapchain image goes to DKB_MENU_TEST_DIR (default: the
// working directory) as menu-test-<frame>-<page>.png; after the last tab the menu closes.
// DKB_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]: cfg_set of each key (TOTK_ prefix
// optional) at that presented frame; changes live settings with no input, menu open or not. dkb.ini is written only by
// the pseudo-key SAVE=1 (cfg_save, as a row change does; a failed write shows the menu's save note).
enum { TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_MENU, TAB_ADVANCED, TAB_COUNT };
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at;          // DKB_MENU_TEST frame; 0 none
    uint64_t per_page;
    uint64_t opened;      // the frame the hook opened the menu; 0 not yet
    int page;             // the tab being shot; -1 none
    TestSet sets[TEST_SETS];
    int n_sets, next_set; // DKB_MENU_TEST_SET, in frame order
} g_test = {0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("DKB_MENU_TEST")) {
        long long at = atoll(e), per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        g_test.at = at > 0 ? (uint64_t)at : 0;
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("DKB_MENU_TEST_SET");
    while (s && *s) {   // groups "<frame>:<key>=<value>,..." separated by ';'
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "DKB_", 5) ? "DKB_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu, %llu frames per page, %d settings changes",
               (unsigned long long)g_test.at, (unsigned long long)g_test.per_page, g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "DKB_SAVE")) rt_log("menu: test hook: cfg_save %s", cfg_save() ? "wrote" : "failed");
        else cfg_set(t.key, t.value);
    }
}

// ---- the lists the Screen and Game tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet

// The Vulkan devices in vkEnumeratePhysicalDevices order, which TOTK_GPU_INDEX counts in (vk_device.c pick_device).
static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    if (cfg_bool("DKB_FULLSCREEN")) opt_disable_next(UI_MENU_WINDOWED_ONLY);
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("DKB_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("DKB_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

// The game resolution (resolution.c): 16:9 sizes from its smallest, 1280x720 (for weak GPUs), up, the common 21:9 and
// 32:9 monitor sizes and the 16:10 screens of the Steam Deck, handhelds and laptops (the camera and the HUD follow
// another shape, resolution.c res_init; the installer lays the HUD out for each, installer/steps.py ASPECT_SIZES), and
// the current value when it is another size.
static void row_output_res(void) {
    static const char *const vals[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                       "2560x1080", "3440x1440", "3840x1600", "5120x2160", "3840x1080", "5120x1440",
                                       "1280x800", "1680x1050", "1920x1200", "2560x1600"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    for (int i = 0; i < N; i++) {
        int w = 0, h = 0;
        sscanf(vals[i], "%dx%d", &w, &h);
        snprintf(shown[i], sizeof shown[i], "%d × %d", w, h);   // U+00D7, the multiplication sign
        names[i] = shown[i];
    }
    opt_choice_list("DKB_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, N, UI_MENU_OUTPUT_RES_DESC);
}

static void row_render_scale(void) {
    static const char *const vals[] = {"game", "33", "50", "58", "67", "75", "100", "125", "150", "200"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    names[0] = menu_text(UI_MENU_RENDER_SCALE_GAME);
    for (int i = 1; i < N; i++) {
        snprintf(shown[i], sizeof shown[i], "%s %%", vals[i]);
        names[i] = shown[i];
    }
    if (opt_choice_list("DKB_RENDER_SCALE", UI_MENU_RENDER_SCALE, vals, names, N, UI_MENU_RENDER_SCALE_DESC)) {
        const char *rs = cfg_value("DKB_RENDER_SCALE");
        if (!strcmp(rs, "100") || !strcmp(rs, "125") || !strcmp(rs, "150") || !strcmp(rs, "200") || !strcmp(rs, "game")) {
            cfg_set("DKB_OPERATION_MODE", "docked");
            cfg_set("DKB_UPSCALER_PRESET", "native");
        } else if (!strcmp(rs, "75") || !strcmp(rs, "67")) {
            cfg_set("DKB_OPERATION_MODE", "docked");
            cfg_set("DKB_UPSCALER_PRESET", "quality");
        } else if (!strcmp(rs, "58")) {
            cfg_set("DKB_OPERATION_MODE", "handheld");
            cfg_set("DKB_UPSCALER_PRESET", "balanced");
        } else if (!strcmp(rs, "50")) {
            cfg_set("DKB_OPERATION_MODE", "handheld");
            cfg_set("DKB_UPSCALER_PRESET", "performance");
        } else if (!strcmp(rs, "33")) {
            cfg_set("DKB_OPERATION_MODE", "handheld");
            cfg_set("DKB_UPSCALER_PRESET", "ultra_performance");
        }
        cfg_save();
    }
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("DKB_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per language TotK ships (a Switch code that names it), each under its own name. The
// Switch's Portuguese is not among them.
static void row_game_lang(void) {
    static const struct { const char *code; UiTextId name; } langs[] = {
        {"en-US", UI_MENU_LANGNAME_EN}, {"ja", UI_MENU_LANGNAME_JA}, {"fr", UI_MENU_LANGNAME_FR},
        {"de", UI_MENU_LANGNAME_DE}, {"it", UI_MENU_LANGNAME_IT}, {"es", UI_MENU_LANGNAME_ES},
        {"nl", UI_MENU_LANGNAME_NL}, {"ru", UI_MENU_LANGNAME_RU},
        {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS}, {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT}, {"ko", UI_MENU_LANGNAME_KO},
    };
    enum { N = sizeof langs / sizeof *langs + 1 };
    const char *vals[N], *names[N];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("DKB_LANG");
    int cur = *now ? -1 : 0;
    for (int i = 1; i < N; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (*now && !strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;   // en-GB is English too
    }
    opt_choice_list("DKB_LANG", UI_MENU_GAME_LANG, vals, names, N, UI_MENU_GAME_LANG_DESC, cur);
}

// The frame limit (frame_rate.c): unset, the older fps60 = 0 still means 30. The monitor choice shows the rate it gives.
static void row_fps_limit() {
    static const char *const vals[] = {"30", "60"};
    static const UiTextId ids[] = {UI_MENU_FPS_30, UI_MENU_FPS_60};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    for (int i = 0; i < N; i++) names[i] = menu_text(ids[i]);
    const char *set = cfg_get("DKB_FPS_LIMIT"), *old = cfg_get("DKB_FPS60");
    int cur = (!set || !*set) ? (old && !strcmp(old, "0") ? 0 : 1) : (!strcmp(set, "30") ? 0 : 1);
    opt_choice_list("DKB_FPS_LIMIT", UI_MENU_FPS_LIMIT, vals, names, N, UI_MENU_FPS_LIMIT_DESC, cur);
}

// The menu's name of a TOTK_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
static const char *present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = present_name(got);
    if (!*got || !strcmp(got, cfg_value("DKB_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// ---- the tabs
// Colour adjustment (vk_color.c): a profile writes the four values (STS78721's presets, issue #136); moving one of them
// makes the profile Custom.
static void rows_color(void) {
    static const struct { const char *name, *b, *c, *s, *g; } presets[] = {
        {"default", "0", "100", "100", "1"}, {"vivid", "0", "115", "135", "0.95"}, {"clarity", "2", "110", "115", "1"}};
    static const UiTextId names[] = {UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
                                     UI_MENU_COLOR_CUSTOM};
    opt_group(UI_MENU_COLOR);
    if (opt_choice("DKB_COLOR_PRESET", UI_MENU_COLOR_PRESET, names, 4, UI_MENU_COLOR_PRESET_DESC)) {
        const char *p = cfg_value("DKB_COLOR_PRESET");
        for (const auto &e : presets)
            if (!strcmp(p, e.name)) {
                cfg_set("DKB_COLOR_BRIGHTNESS", e.b), cfg_set("DKB_COLOR_CONTRAST", e.c);
                cfg_set("DKB_COLOR_SATURATION", e.s), cfg_set("DKB_COLOR_GAMMA", e.g);
                cfg_save();
            }
    }
    bool moved = opt_slider("DKB_COLOR_BRIGHTNESS", UI_MENU_COLOR_BRIGHTNESS, -30, 30, UI_MENU_COLOR_BRIGHTNESS_DESC, 1,
                            "%+.0f %%");
    moved |= opt_slider("DKB_COLOR_CONTRAST", UI_MENU_COLOR_CONTRAST, 70, 150, UI_MENU_COLOR_CONTRAST_DESC, 5, "%.0f %%");
    moved |= opt_slider("DKB_COLOR_SATURATION", UI_MENU_COLOR_SATURATION, 50, 200, UI_MENU_COLOR_SATURATION_DESC, 5,
                        "%.0f %%");
    moved |= opt_slider("DKB_COLOR_GAMMA", UI_MENU_COLOR_GAMMA, 0.8f, 1.2f, UI_MENU_COLOR_GAMMA_DESC, 0.02f, "%.2f");
    if (moved && strcmp(cfg_value("DKB_COLOR_PRESET"), "custom")) {
        cfg_set("DKB_COLOR_PRESET", "custom");
        cfg_save();
    }
}

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("DKB_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    row_output_res();
    row_window_size();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("DKB_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("DKB_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group_text("Nodix Super Motion & Super Resolution");
    row_render_scale();
    static const char *const up_vals[] = {"smrs", "fsr", "dlss", "bilinear", "off"};
    static const char *const up_names[] = {
        "Nodix Super Motion SMRS",
        "AMD FSR 1.0 (Native)",
        "NVIDIA DLSS",
        "Bilinear (Clean Scaler)",
        "Off (Raw 1:1)"
    };
    opt_choice_list("DKB_UPSCALER", UI_MENU_UPSCALER, up_vals, up_names, 5, UI_MENU_UPSCALER_DESC);
    const char *up = cfg_value("DKB_UPSCALER");
    if (!strcmp(up, "dlss"))
        opt_group_text("DLSS follows the game's scale: 100% DLAA K, 67% Quality K, 58% Balanced K, 50% Performance M, 33% Ultra Performance L. Motion vectors are dilated camera reprojection, matched to the previous frame where something moves on its own.");
    bool is_smrs = !strcmp(up, "smrs");
    bool is_fsr = !strcmp(up, "fsr");
    if (is_smrs) {
        static const struct { const char *name, *label, *scale; } p[] = {
            {"native", "Native / DLAA (Docked 1620p)", "100"},
            {"quality", "Quality (Docked 1620p)", "67"},
            {"balanced", "Balanced (Handheld 1080p)", "58"},
            {"performance", "Performance (Handheld 1080p)", "50"},
            {"ultra_performance", "Ultra Performance (Handheld 1080p)", "33"}
        };
        static const char *names[5], *vals[5];
        for (int i = 0; i < 5; i++) { vals[i] = p[i].name; names[i] = p[i].label; }
        const char *cur_op = cfg_value("DKB_OPERATION_MODE");
        const char *p_note = (!cur_op || !strcmp(cur_op, "docked")) ? "Docked Profile Active (1620p base)" : "Handheld Profile Active (1080p base, 56% lighter)";
        if (opt_choice_list("DKB_UPSCALER_PRESET", UI_MENU_UPSCALE_PRESET, vals, names, 5, UI_MENU_UPSCALE_PRESET_DESC, -1, p_note)) {
            const char *preset = cfg_value("DKB_UPSCALER_PRESET");
            if (!strcmp(preset, "native") || !strcmp(preset, "quality")) {
                cfg_set("DKB_OPERATION_MODE", "docked");
            } else {
                cfg_set("DKB_OPERATION_MODE", "handheld");
            }
            for (int i = 0; i < 5; i++) {
                if (!strcmp(preset, p[i].name)) {
                    cfg_set("DKB_RENDER_SCALE", p[i].scale);
                    break;
                }
            }
            cfg_save();
        }
        opt_slider("DKB_SHARPNESS", UI_MENU_SMRS_SHARPNESS, 0, 100, UI_MENU_SMRS_SHARPNESS_DESC, 5, "%.0f %%");
    } else if (is_fsr) {
        opt_slider("DKB_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    }
    opt_toggle("DKB_FXAA", UI_MENU_FXAA, UI_MENU_FXAA_DESC);

    // Frame Generation / Super Motion (Nodix Super Motion and DLSS MFG)
    opt_group_text("Frame Generation (Nodix Super Motion & MFG)");
    static const char *const fg_gen_vals[] = {"off", "msm_fg", "dlss_fg", "fsr3_fg", "xefg"};
    static const char *const fg_gen_names[] = {
        "Off",
        "Nodix Super Motion (SMFG 1.3)",
        "DLSS MFG (NVIDIA RTX)",
        "AMD FSR 3.1 FG (Universal)",
        "XeSS MFG (Intel Universal)"
    };
    const char *fg_note = nullptr;
    const char *cur_fg = cfg_value("DKB_FG_MODE");
    if (!strcmp(cur_fg, "msm_fg")) {
        fg_note = "Nodix Super Motion DirectML / Compute Active";
    } else if (!strcmp(cur_fg, "dlss_fg")) {
        fg_note = vk_gpu_is_nvidia() ? "NVIDIA Hardware Optical Flow Active" : "Requires NVIDIA RTX GPU";
    } else if (!strcmp(cur_fg, "fsr3_fg")) {
        fg_note = "AMD FidelityFX FSR 3.1 Frame Generation Active";
    } else if (!strcmp(cur_fg, "xefg")) {
        fg_note = "Intel XeSS Frame Generation Active";
    }
    opt_choice_list("DKB_FG_MODE", UI_MENU_FG_MODE, fg_gen_vals, fg_gen_names, 5, UI_MENU_FG_MODE_DESC, -1, fg_note);

    if (strcmp(cur_fg, "off") != 0) {
        static const char *const mode_vals[] = {"fixed", "adaptive"};
        static const char *const mode_names[] = {"Fixed Multiplier", "Adaptive Frame Pacing"};
        opt_choice_list("DKB_FG_TYPE", UI_MENU_PRESENT, mode_vals, mode_names, 2, UI_MENU_PRESENT_DESC);

        static const char *const mult_vals[] = {"2x", "3x", "4x", "6x"};
        static const char *const mult_names[] = {
            "x2 (Double FPS)",
            "x3 (Triple FPS)",
            "x4 (Quadruple FPS)",
            "x6 (6x Multiplier)"
        };
        opt_choice_list("DKB_FG_MULT", UI_MENU_FG_MULT, mult_vals, mult_names, 4, UI_MENU_FG_MULT_DESC);

        if (!strcmp(cur_fg, "dlss_fg")) {
            static const char *const dpreset_vals[] = {"drivers_choice", "preset_c", "preset_b", "preset_a"};
            static const char *const dpreset_names[] = {
                "Driver's choice",
                "Performance · Preset C",
                "Quality · Preset B (Optical Flow)",
                "Balanced · Preset A (Transformer)"
            };
            opt_choice_list("DKB_FG_PRESET", UI_MENU_FG_PRESET, dpreset_vals, dpreset_names, 4, UI_MENU_FG_PRESET_DESC);
        }

        opt_toggle("DKB_FG_STILL", UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC); // Still Pixels / HUD Guard
        opt_slider("DKB_FG_FLOW", UI_MENU_FG_FLOW, 10, 100, UI_MENU_FG_FLOW_DESC, 5, "%.0f %%");
    }

    // Low Latency (Strictly mutually exclusive per GPU vendor to prevent conflicts)
    opt_group_text("Input Latency & Responsiveness");
    static const char *const reflex_vals[] = {"off", "on", "boost"};
    static const char *const reflex_names[] = {"Off", "On", "On + Boost"};
    static const char *const al_vals[] = {"off", "on"};
    static const char *const al_names[] = {"Off", "On"};
    static const char *const xell_vals[] = {"off", "on", "boost"};
    static const char *const xell_names[] = {"Off", "On", "On + Boost"};

    if (vk_gpu_is_nvidia()) {
        const char *ref_note = "NVIDIA Reflex (VK_NV_low_latency2 Active)";
        if (opt_choice_list("DKB_REFLEX", UI_MENU_REFLEX, reflex_vals, reflex_names, 3, UI_MENU_REFLEX_DESC, -1, ref_note)) {
            // Guarantee no conflict: disable AMD/Intel low-latency
            cfg_set("DKB_ANTILAG2", "off");
            cfg_set("DKB_XELL", "off");
            if (!strcmp(cfg_value("DKB_REFLEX"), "off") && !strcmp(cfg_value("DKB_FG_MODE"), "dlss_fg")) {
                cfg_set("DKB_FG_MODE", "off");
            }
            cfg_save();
        }
    } else if (vk_has_amd_antilag()) {
        const char *al_note = "AMD Anti-Lag 2 (VK_AMD_anti_lag Active)";
        if (opt_choice_list("DKB_ANTILAG2", UI_MENU_ANTILAG2, al_vals, al_names, 2, UI_MENU_ANTILAG2_DESC, -1, al_note)) {
            cfg_set("DKB_REFLEX", "off");
            cfg_set("DKB_XELL", "off");
            cfg_save();
        }
    } else if (vk_gpu_is_intel()) {
        const char *xell_note = "Intel Xe Low Latency (XeLL Active)";
        if (opt_choice_list("DKB_XELL", UI_MENU_XELL, xell_vals, xell_names, 3, UI_MENU_XELL_DESC, -1, xell_note)) {
            cfg_set("DKB_REFLEX", "off");
            cfg_set("DKB_ANTILAG2", "off");
            cfg_save();
        }
    } else {
        const char *ref_note = "Universal GPU Frame Pacing";
        if (opt_choice_list("DKB_REFLEX", UI_MENU_REFLEX, reflex_vals, reflex_names, 3, UI_MENU_REFLEX_DESC, -1, ref_note)) {
            cfg_set("DKB_ANTILAG2", "off");
            cfg_set("DKB_XELL", "off");
            cfg_save();
        }
    }

    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    opt_choice("DKB_ASPECT", UI_MENU_ASPECT, aspect, 2, UI_MENU_ASPECT_DESC);
    opt_slider("DKB_FOV", UI_MENU_FOV, 70, 140, UI_MENU_FOV_DESC, 5, "%.0f %%");
    rows_color();
    opt_end();
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    row_fps_limit();
    opt_toggle("DKB_REALTIME", UI_MENU_REALTIME, UI_MENU_REALTIME_DESC);
    opt_toggle("DKB_CLOTH_REALTIME", UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC);
    opt_toggle("DKB_HIGHFPS_FIX", UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC);
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("DKB_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("DKB_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("DKB_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    static const UiTextId channels[] = {UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51};
    opt_choice("DKB_AUDIO_CHANNELS", UI_MENU_AUDIO_CHANNELS, channels, 3, UI_MENU_AUDIO_CHANNELS_DESC);
    opt_end();
}

// The note under the layout. Xbox prompts (the Nintendo layout with the A/B and X/Y prompts swapped, romfs.c
// overlay_init) change the buttons at once but the prompts only at the next start: "files missing" without
// romfs-patches-xbox, "prompts change after restart" while the prompts in use differ from the choice. Automatic with a
// gamepad connected: "in use: Xbox". NULL otherwise.
static const char *pad_layout_note(void) {
    bool want = !strcasecmp(cfg_value("DKB_PAD_LAYOUT"), "xbox_prompts");
    if (want && !romfs_xbox_prompts_ready()) return menu_text(UI_MENU_PAD_LAYOUT_PROMPTS_MISSING);
    if (want != (romfs_xbox_prompts_active() != 0)) return menu_text(UI_MENU_PAD_LAYOUT_PROMPTS_RESTART);
    int got = input_pad_layout();
    if (got < 0 || strcmp(cfg_value("DKB_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE),
             menu_text(got == PAD_LAYOUT_XBOX ? UI_MENU_PAD_LAYOUT_XBOX : UI_MENU_PAD_LAYOUT_NINTENDO));
    return note;
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX,
                                      UI_MENU_PAD_LAYOUT_XBOX_PROMPTS};
    opt_choice("DKB_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 4, UI_MENU_PAD_LAYOUT_DESC, pad_layout_note());
    opt_toggle("DKB_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    opt_slider("DKB_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("DKB_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("DKB_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);

    opt_group(UI_MENU_GROUP_DUALSENSE);
    opt_toggle("DKB_DUALSENSE", UI_MENU_DUALSENSE, UI_MENU_DUALSENSE_DESC);
    opt_toggle("DKB_DUALSENSE_TRIGGERS", UI_MENU_DUALSENSE_TRIGGERS, UI_MENU_DUALSENSE_TRIGGERS_DESC);
    opt_slider("DKB_DUALSENSE_TRIGGER_STRENGTH", UI_MENU_DUALSENSE_TRIGGER_STRENGTH, 0, 100, UI_MENU_DUALSENSE_TRIGGER_STRENGTH_DESC, 5, "%.0f %%");
    opt_toggle("DKB_DUALSENSE_AUDIO", UI_MENU_DUALSENSE_AUDIO, UI_MENU_DUALSENSE_AUDIO_DESC);
    opt_slider("DKB_DUALSENSE_VOL", UI_MENU_DUALSENSE_VOL, 0, 150, UI_MENU_DUALSENSE_VOL_DESC, 5, "%.0f %%");
    opt_toggle("DKB_DUALSENSE_HAPTICS", UI_MENU_DUALSENSE_HAPTICS, UI_MENU_DUALSENSE_HAPTICS_DESC);

    opt_slider("DKB_STICK_DEADZONE", UI_MENU_STICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%");
    opt_slider("DKB_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    binds_rows(scale);
    opt_end();
    binds_after_page();
}

static void tab_menu(float scale) {
    opt_begin("##menu", scale);
    static const UiTextId langs[] = {UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_RU};
    opt_choice("DKB_MENU_LANG", UI_MENU_MENU_LANG, langs, 3, UI_MENU_MENU_LANG_DESC);
    opt_end();
}

// ---- Advanced: the testing and troubleshooting keys, the bug report helpers and the reset of every setting
static bool g_modal;   // the reset confirmation is open: B and Esc close it, not the menu; L / R stay on the page

// A thread count with no fixed default: Automatic (unset), then the common counts.
static void row_threads(const char *key, UiTextId label, UiTextId desc) {
    static const char *const vals[] = {"", "1", "2", "4", "6", "8", "12", "16", "24", "32", "48", "64"};
    enum { N = sizeof vals / sizeof *vals };
    const char *names[N];
    names[0] = menu_text(UI_MENU_AUTO);
    for (int i = 1; i < N; i++) names[i] = vals[i];
    opt_choice_list(key, label, vals, names, N, desc);
}

// A free-text key shown as Off (unset) / On (value set): On writes on_value, any set value reads as On.
static void row_set_or_not(const char *key, UiTextId label, UiTextId desc, const char *on_value) {
    const char *vals[] = {"", on_value}, *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON)};
    opt_choice_list(key, label, vals, names, 2, desc, *cfg_value(key) ? 1 : 0);
}

static bool validation_layer_present(void) {
    static int have = -1;
    if (have < 0) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        VkLayerProperties *l = (VkLayerProperties *)calloc(n ? n : 1, sizeof *l);
        if (l && vkEnumerateInstanceLayerProperties(&n, l) >= 0)
            for (uint32_t i = 0; i < n && have <= 0; i++) have = !strcmp(l[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(l);
        if (have < 0) have = 0;
    }
    return have != 0;
}

// Every setting the menu shows back to its default (the environment's keys stay), written to dkb.ini.
static void reset_all(void) {
    const char *keys[128];
    int n = 0;
    for (const CfgMeta *m; n < 128 && (m = cfg_meta_at(n)); n++) keys[n] = m->key;
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note;
    opt_begin("##advanced", scale);
    opt_text(UI_MENU_ADV_WARNING, true);

    opt_group(UI_MENU_GROUP_SCHED);
    static const UiTextId preempt[] = {UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON, UI_MENU_PREEMPT_INHERIT};   // 0|1|2
    opt_choice("DKB_ASYNC_PREEMPT", UI_MENU_ASYNC_PREEMPT, preempt, 3, UI_MENU_ASYNC_PREEMPT_DESC);
    opt_toggle("DKB_REPLAY_ASYNC", UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC);
    opt_toggle("DKB_TIMER_1MS", UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC);

    opt_group(UI_MENU_GROUP_SHADERS);
    static const UiTextId wait[] = {UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW};
    opt_choice("DKB_PIPE_WAIT", UI_MENU_PIPE_WAIT, wait, 2, UI_MENU_PIPE_WAIT_DESC);
    static const UiTextId skip[] = {UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY, UI_MENU_PIPE_SKIP_FULLSCREEN};
    opt_choice("DKB_PIPE_SKIP", UI_MENU_PIPE_SKIP, skip, 3, UI_MENU_PIPE_SKIP_DESC);
    opt_toggle("DKB_LIB_PREBUILD", UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC);
    row_threads("DKB_LIB_PREBUILD_THREADS", UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC);
    opt_toggle("DKB_PIPE_PREBUILD", UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC);
    row_threads("DKB_PREWARM_XLATE", UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC);
    opt_toggle("DKB_GPL", UI_MENU_GPL, UI_MENU_GPL_DESC);

    opt_group(UI_MENU_GROUP_TEXMEM);
    static const UiTextId astc[] = {UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU};
    opt_choice("DKB_ASTC", UI_MENU_ASTC, astc, 4, UI_MENU_ASTC_DESC);
    static const UiTextId mem[] = {UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
                                   UI_MENU_MEMORY_VERIFY};
    opt_choice("DKB_MEMORY", UI_MENU_MEMORY, mem, 4, UI_MENU_MEMORY_DESC);
    opt_toggle("DKB_TEX_ALIAS", UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC);
    opt_slider("DKB_TEX_VRAM_PRESSURE", UI_MENU_TEX_VRAM_PRESSURE, 0, 100, UI_MENU_TEX_VRAM_PRESSURE_DESC, 1, "%.0f%%");
    opt_toggle("DKB_ASTC_BC", UI_MENU_ASTC_BC, UI_MENU_ASTC_BC_DESC);

    opt_group(UI_MENU_GROUP_FIXES);
    opt_toggle("DKB_LOD_GUARD", UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC);
    opt_toggle("DKB_FIX_C0", UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC);
    opt_toggle("DKB_FIX_ZERO", UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC);
    opt_toggle("DKB_BIND_SKIP", UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC);
    opt_toggle("DKB_SHADER_MEMO", UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC);
    opt_toggle("DKB_PIPE_MEMO", UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC);
    opt_toggle("DKB_TEX_MEMO", UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC);
    opt_toggle("DKB_DESC_TEMPLATE", UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC);
    opt_toggle("DKB_PUSH_SKIP", UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC);
    opt_slider("DKB_SPLIT_DRAWS", UI_MENU_SPLIT_DRAWS, 0, 1000, UI_MENU_SPLIT_DRAWS_DESC, 8, "%.0f");

    opt_group(UI_MENU_GROUP_VULKAN);
    if (!validation_layer_present() && !cfg_bool("DKB_VK_VALIDATION")) opt_disable_next(UI_MENU_VK_VALIDATION_MISSING);
    opt_toggle("DKB_VK_VALIDATION", UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC);
    static const UiTextId dyn[] = {UI_MENU_AUTO, UI_MENU_OFF, UI_MENU_ON};   // unset, 0, 1
    opt_choice("DKB_VK_DYN_VERTEX", UI_MENU_VK_DYN_VERTEX, dyn, 3, UI_MENU_VK_DYN_VERTEX_DESC);
    opt_choice("DKB_VK_DYN_A2C", UI_MENU_VK_DYN_A2C, dyn, 3, UI_MENU_VK_DYN_A2C_DESC);
    opt_toggle("DKB_VK_SUBGROUP_PIN", UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC);

    opt_group(UI_MENU_GROUP_DIAG);
    row_set_or_not("DKB_TRACE", UI_MENU_TRACE, UI_MENU_TRACE_DESC, "perf");
    opt_toggle("DKB_FLIGHT", UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC);
    opt_slider("DKB_FLIGHT_SPIKE_MS", UI_MENU_FLIGHT_SPIKE_MS, 20, 1000, UI_MENU_FLIGHT_SPIKE_MS_DESC, 10, "%.0f");
    static char rec_path[600];   // the recording goes beside the log
    if (!rec_path[0]) {
        char dir[512];
        snprintf(rec_path, sizeof rec_path, "%s/input-record.txt", sysinfo_log_folder(dir, sizeof dir) ? dir : ".");
    }
    row_set_or_not("DKB_INPUT_RECORD", UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC, rec_path);
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[512];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (ImGui::Button(no, ImVec2(bw, 0))) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(yes, ImVec2(bw, 0))) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_MENU, UI_MENU_TAB_ADVANCED};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none

// ---- the window: title, tabs (L / R or Q / E switch), the page, and the bottom row with "Continue" and "Quit game".
// B on the gamepad and Esc close it.
static void layout_menu(uint32_t w, uint32_t h, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, 120));
    float ww = fminf(w * 0.88f, 1400 * scale), wh = fminf(h * 0.86f, 820 * scale);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", menu_text(UI_MENU_TITLE));
    // the port version and the game update, right-aligned on the title line: what a bug report needs first
    char game[16], version[96];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
    snprintf(version, sizeof version, menu_text(UI_MENU_VERSION), rt_port_version, game);
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(version).x);
    ImGui::TextDisabled("%s", version);

    int step = g_modal || binds_modal()                                                                  ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    if (step) g_tab_request = (g_tab + step + TAB_COUNT) % TAB_COUNT;
    if (g_tab_request >= 0) g_focus_page = g_tab_request;   // the new page's first row takes the cursor

    const ImGuiStyle &st = ImGui::GetStyle();
    float bottom = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < TAB_COUNT; k++) {
            char label[128];
            snprintf(label, sizeof label, "  %s  ###tab%d", menu_text(kTabNames[k]), k);   // the id stays per language
            if (!ImGui::BeginTabItem(label, nullptr, g_tab_request == k ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            g_tab = k;
            if (g_focus_page == k) {
                opt_focus_first();
                g_focus_page = -1;
            }
            ImGui::Spacing();
            // the page's rows are a NavFlattened child of this window itself: ImGui clips the items of a direct child
            // only, so behind a second child level the rows scrolled out above the tabs won D-pad moves from the
            // buttons and the tabs (up from "Continue" went to the tabs, down from the tabs to "Continue")
            opt_reserve_bottom(bottom);
            switch (k) {
            case TAB_SCREEN: tab_screen(scale); break;
            case TAB_GAME: tab_game(scale); break;
            case TAB_SOUND: tab_sound(scale); break;
            case TAB_CONTROLS: tab_controls(scale); break;
            case TAB_MENU: tab_menu(scale); break;
            case TAB_ADVANCED: tab_advanced(scale); break;
            }
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }

    // bottom row: the save status on the left, the two buttons on the right
    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    if (!cfg_last_save_ok()) {   // stays until a save succeeds; the settings hold for the session
        ImGui::SetCursorPosY(y + st.FramePadding.y);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    const char *cont = menu_text(UI_MENU_CONTINUE), *quit = menu_text(UI_MENU_QUIT);
    float bw = fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(quit).x) + st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - st.WindowPadding.x - bw * 2 - st.ItemSpacing.x, y));
    if (ImGui::Button(cont, ImVec2(bw, 0))) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button(quit, ImVec2(bw, 0))) gfx_window_request_close();
    bool was_modal = g_modal || binds_modal();
    if (g_tab != TAB_ADVANCED) g_modal = false;
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        set_open(0);
    ImGui::End();
}

static int g_save_pending;   // a changed setting still waits for its write to dkb.ini (opt_save_tick)

// ---- the performance overlay (TOTK_PERF_OVERLAY), in the top right corner. "fps": the frame rate over the presents of
// the last half second. "full": a panel of perfmon.c's numbers, in the style of SavageWindRecomp's perf_overlay_draw
// (menu.mm): the machine's load and temperatures, the frame rate and frame times with a graph, the render thread's waits.
// Off, it costs a cfg_version load per present.
static unsigned g_perf_ver = ~0u;
static int g_perf_mode;                  // 0 off, 1 fps, 2 full
static std::atomic<int> g_perf_save;     // F10 changed the mode: dkb.ini is written on the present thread
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];
static char g_perf_fps_num[32];
static char g_perf_ms_num[32];
static char g_perf_upscaler[80];
static char g_perf_line2[160];
static char g_perf_battery[24];
static char g_perf_fg_badge[32];
static int  g_perf_theme;                // 0: NVIDIA green, 1: AMD red, 2: Cyber cyan, 3: Intel blue
static uint32_t g_dt_ring[1000];
static int g_dt_count = 0;
static int g_dt_head = 0;
static char g_perf_1pct_num[32];
static char g_perf_01pct_num[32];

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    cfg_set("DKB_PERF_OVERLAY", modes[(perf_mode_of(cfg_value("DKB_PERF_OVERLAY")) + 1) % 3]);
    g_perf_save.store(1);
}

// The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else
// name); once, before full mode.
static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("DKB_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            g_perf_fps_num[0] = 0;
            g_perf_1pct_num[0] = 0;
            g_perf_01pct_num[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;

        // Record into rolling 1000-frame ring buffer for 1% and 0.1% low calculation
        g_dt_ring[g_dt_head] = (uint32_t)(d > 2000000000ull ? 2000000000ull : d);
        g_dt_head = (g_dt_head + 1) % 1000;
        if (g_dt_count < 1000) g_dt_count++;

        if (g_perf_win_ns >= 500000000ull) {
            double base_fps = (g_perf_frames * 1e9) / (double)g_perf_win_ns;
            double base_ms = ((double)g_perf_win_ns / (double)g_perf_frames) / 1e6;

            int fgm = vk_fg_active_multiplier();
            double eff_fps = (fgm > 1) ? (base_fps * fgm) : base_fps;
            double eff_ms = (eff_fps > 0) ? (1000.0 / eff_fps) : base_ms;

            if (fgm > 1) {
                const char *fgm_mode = cfg_value("DKB_FG_MODE");
                const char *fgt = !strcmp(fgm_mode, "msm_fg") ? "MSM-FG"
                                : (!strcmp(fgm_mode, "dlss_fg") && vk_gpu_is_nvidia()) ? "DLSS FG"
                                : !strcmp(fgm_mode, "xefg") ? "XeFG"
                                : "FSR3 FG";
                snprintf(g_perf_fg_badge, sizeof g_perf_fg_badge, "%s %dx", fgt, fgm);
                snprintf(g_perf_fps_num, sizeof g_perf_fps_num, "%.0f FPS (Base %.0f)", eff_fps, base_fps);
            } else {
                g_perf_fg_badge[0] = 0;
                if (eff_fps >= 100.0) snprintf(g_perf_fps_num, sizeof g_perf_fps_num, "%.0f FPS", eff_fps);
                else snprintf(g_perf_fps_num, sizeof g_perf_fps_num, "%.1f FPS", eff_fps);
            }
            snprintf(g_perf_ms_num, sizeof g_perf_ms_num, "%.1f ms", eff_ms);

            int battery_percent = -1, battery_charging = 0;
            if (input_pad_power_info(&battery_percent, &battery_charging)) {
                if (battery_charging) snprintf(g_perf_battery, sizeof g_perf_battery, "[CHG]");
                else snprintf(g_perf_battery, sizeof g_perf_battery, "[BAT %d%%]", battery_percent);
            } else {
                g_perf_battery[0] = 0;
            }

            if (g_dt_count >= 10) {
                uint32_t sorted_dt[1000];
                int n_samples = g_dt_count;
                memcpy(sorted_dt, g_dt_ring, n_samples * sizeof(uint32_t));
                std::sort(sorted_dt, sorted_dt + n_samples);

                int idx_1p = (int)(n_samples * 0.99f);
                if (idx_1p >= n_samples) idx_1p = n_samples - 1;
                int idx_01p = (int)(n_samples * 0.999f);
                if (idx_01p >= n_samples) idx_01p = n_samples - 1;

                uint32_t dt_1p = sorted_dt[idx_1p];
                uint32_t dt_01p = sorted_dt[idx_01p];

                double low_1p = (dt_1p > 0 ? (1e9 / (double)dt_1p) : base_fps) * (fgm > 1 ? fgm : 1.0);
                double low_01p = (dt_01p > 0 ? (1e9 / (double)dt_01p) : base_fps) * (fgm > 1 ? fgm : 1.0);
                snprintf(g_perf_1pct_num, sizeof g_perf_1pct_num, "1%% %.0f", low_1p);
                snprintf(g_perf_01pct_num, sizeof g_perf_01pct_num, "0.1%% %.0f", low_01p);
            } else {
                snprintf(g_perf_1pct_num, sizeof g_perf_1pct_num, "1%% %.0f", eff_fps);
                snprintf(g_perf_01pct_num, sizeof g_perf_01pct_num, "0.1%% %.0f", eff_fps);
            }

            const char *up = cfg_value("DKB_UPSCALER");
            bool is_smrs = !strcmp(up, "smrs");
            bool is_fsr = !strcmp(up, "fsr");
            if (is_smrs) {
                g_perf_theme = 2;
                const char *p = cfg_value("DKB_UPSCALER_PRESET");
                const char *op = cfg_value("DKB_OPERATION_MODE");
                bool is_hh = (op && !strcmp(op, "handheld"));
                const char *plabel = !strcmp(p, "native") ? "Native DLAA (1620p)"
                                   : !strcmp(p, "quality") ? "Quality (1620p)"
                                   : !strcmp(p, "balanced") ? "Balanced (1080p)"
                                   : !strcmp(p, "performance") ? "Perf (1080p)"
                                   : !strcmp(p, "ultra_performance") ? "UltraP (1080p)"
                                   : is_hh ? "Handheld 1080p" : "Docked 1620p";
                snprintf(g_perf_upscaler, sizeof g_perf_upscaler, "Nodix SMRS [%s]", plabel);
            } else if (!strcmp(up, "dlss")) {
                g_perf_theme = 0;
                vk_dlss_meter(g_perf_upscaler, (int)sizeof g_perf_upscaler);
            } else if (is_fsr) {
                g_perf_theme = 1;
                snprintf(g_perf_upscaler, sizeof g_perf_upscaler, "FSR 1.0 (EASU)");
            } else if (!strcmp(up, "bilinear")) {
                g_perf_theme = 2;
                snprintf(g_perf_upscaler, sizeof g_perf_upscaler, "Bilinear");
            } else {
                g_perf_theme = 2;
                snprintf(g_perf_upscaler, sizeof g_perf_upscaler, "Native 1:1");
            }

            unsigned out[2], scene[2];
            res_sizes(out, scene);

            char line2[64] = {0};
            if (scene[0] > 0 && out[0] > 0 && scene[0] < out[0]) {
                char res_buf[32];
                if (out[1] == 2160 && scene[1] == 1080) snprintf(res_buf, sizeof res_buf, "1080p->4K");
                else if (out[1] == 1440 && scene[1] == 720) snprintf(res_buf, sizeof res_buf, "720p->1440p");
                else snprintf(res_buf, sizeof res_buf, "%up->%up", scene[1], out[1]);
                strncat(line2, res_buf, sizeof(line2) - strlen(line2) - 1);
            }
            const char *ref = cfg_value("DKB_REFLEX");
            if (ref && !strcmp(ref, "boost")) {
                if (line2[0]) strncat(line2, " · Reflex Boost", sizeof(line2) - strlen(line2) - 1);
                else strncat(line2, "Reflex Boost", sizeof(line2) - strlen(line2) - 1);
            } else if (ref && !strcmp(ref, "on")) {
                if (line2[0]) strncat(line2, " · Reflex/Anti-Lag", sizeof(line2) - strlen(line2) - 1);
                else strncat(line2, "Reflex/Anti-Lag", sizeof(line2) - strlen(line2) - 1);
            } else if (cfg_bool("DKB_XELL")) {
                if (line2[0]) strncat(line2, " · XeLL", sizeof(line2) - strlen(line2) - 1);
                else strncat(line2, "XeLL", sizeof(line2) - strlen(line2) - 1);
            }
            snprintf(g_perf_line2, sizeof g_perf_line2, "%s", line2);
            snprintf(g_perf_text, sizeof g_perf_text, "%s", g_perf_fps_num);
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
// on samples of 8s (Roboto's digits share one width), and the numbers are right-aligned in it.
struct PerfCol { UiTextId fmt; float sample; };

static float perf_text_w(UiTextId fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    return ImGui::CalcTextSize(b).x;
}

// A fixed column as wide as the widest of the formats (with their samples) and labels given.
static void perf_column(const char *id, const PerfCol *cols, int n, const UiTextId *labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(menu_text(UI_MENU_OV_NONE)).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, perf_text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(menu_text(labels[i])).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

static void perf_right(const char *s, bool dim) {
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

// One cell of the full panel's table: fmt (a catalog format, one number) with v, or "-" when the machine gives none.
static void perf_cell(UiTextId fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return perf_right(menu_text(UI_MENU_OV_NONE), true);
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    perf_right(b, false);
}

static void perf_label(UiTextId label, const ImVec4 &c) {
    ImGui::TableNextColumn();
    ImGui::TextColored(c, "%s", menu_text(label));
}

static float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// The picture lines: the window, the game image the present shows and the 3D scene it holds (resolution.c), how the
// present scales the image to the window (gfx_present.c, the last present), the frame limit and the presentation mode
// in use.
static void perf_picture(const MenuTarget *t, const ImVec4 &c) {
    unsigned out[2], scene[2];
    res_sizes(out, scene);
    char v[8][64];
    snprintf(v[0], sizeof v[0], menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], sizeof v[1], menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    snprintf(v[2], sizeof v[2], menu_text(UI_MENU_OV_FMT_SCENE), scene[0], scene[1], scene[0] * 100.0 / out[0]);
    if (!strcmp(cfg_value("DKB_UPSCALER"), "smrs")) {
        int s = cfg_int("DKB_SHARPNESS");
        const char *p = cfg_value("DKB_UPSCALER_PRESET");
        const char *op = cfg_value("DKB_OPERATION_MODE");
        bool is_hh = (op && !strcmp(op, "handheld"));
        snprintf(v[3], sizeof v[3], "Nodix SMRS (%s, %s, %d%% Sharp)",
                 is_hh ? "Handheld 1080p" : "Docked 1620p",
                 p ? p : "Quality", s < 0 ? 0 : s > 100 ? 100 : s);
    } else if (!strcmp(cfg_value("DKB_UPSCALER"), "dlss")) {
        vk_dlss_meter(v[3], (int)sizeof v[3]);
    } else if (t->scaler == 2) {
        int s = cfg_int("DKB_SHARPNESS");
        snprintf(v[3], sizeof v[3], menu_text(UI_MENU_OV_FMT_FSR), s < 0 ? 0 : s > 100 ? 100 : s);
    } else {
        snprintf(v[3], sizeof v[3], "%s", menu_text(t->scaler ? UI_MENU_OV_SCALE_BILINEAR : UI_MENU_OV_SCALE_NONE));
    }
    const char *ref = cfg_value("DKB_REFLEX");
    const char *al = cfg_value("DKB_ANTILAG2");
    const char *xl = cfg_value("DKB_XELL");
    if (vk_gpu_is_nvidia()) {
        snprintf(v[4], sizeof v[4], !strcmp(ref, "boost") ? "Reflex (On + Boost)" : !strcmp(ref, "on") ? "Reflex (On)" : "Off");
    } else if (vk_has_amd_antilag()) {
        snprintf(v[4], sizeof v[4], !strcmp(al, "on") ? "Anti-Lag 2 (On)" : "Off");
    } else if (vk_gpu_is_intel()) {
        snprintf(v[4], sizeof v[4], !strcmp(xl, "boost") ? "XeLL (On + Boost)" : !strcmp(xl, "on") ? "XeLL (On)" : "Off");
    } else {
        snprintf(v[4], sizeof v[4], !strcmp(ref, "boost") ? "Low Latency (Boost)" : !strcmp(ref, "on") ? "Low Latency (On)" : "Off");
    }
    int fg_mult = vk_fg_active_multiplier();
    const char *fg_m = cfg_value("DKB_FG_MODE");
    if (fg_mult > 1 && fg_m && strcmp(fg_m, "off") != 0) {
        const char *gen = !strcmp(fg_m, "msm_fg") ? "Nodix Super Motion"
                        : !strcmp(fg_m, "dlss_fg") ? "DLSS MFG"
                        : !strcmp(fg_m, "fsr3_fg") ? "FSR 3.1 FG"
                        : !strcmp(fg_m, "xefg") ? "XeSS MFG" : "Super Motion";
        snprintf(v[5], sizeof v[5], "%s (%dx Active)", gen, fg_mult);
    } else {
        snprintf(v[5], sizeof v[5], "Off");
    }
    double hz = frame_step_limit_hz();
    snprintf(v[6], sizeof v[6], "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[6], sizeof v[6], menu_text(UI_MENU_OV_FPS), hz);
    const char *pm = present_name(gfx_window_present_mode());
    snprintf(v[7], sizeof v[7], "%s", pm ? pm : menu_text(UI_MENU_OV_NONE));
    static const char *const custom_labels[8] = {
        "Window", "Output", "Scene", "Scaling", "Latency", "Frame Gen", "Limit", "Present"
    };
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (auto &s : v) vw = fmaxf(vw, ImGui::CalcTextSize(s).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < 8; i++) {
        ImGui::TableNextColumn();
        ImGui::TextColored(c, "%s", custom_labels[i]);
        ImGui::TableNextColumn();
        perf_right(v[i], false);
    }
    ImGui::EndTable();
}

static void perf_full_draw(const MenuTarget *t, float scale) {
    float w = (float)t->width;
    PerfStats s;
    perfmon_snapshot(&s);
    ImGui::SetNextWindowPos(ImVec2(w - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    // under the menu while it is open (the settings stay readable), else over everything
    if (g_open.load(std::memory_order_relaxed)) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c(0.55f, 0.83f, 0.31f, 1), cpu_c(0.18f, 0.59f, 0.80f, 1), ram_c(0.80f, 0.45f, 0.70f, 1),
        fps_c(0.95f, 0.95f, 0.95f, 1), wait_c(0.93f, 0.76f, 0.33f, 1), pic_c(0.62f, 0.70f, 0.95f, 1);
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        static const UiTextId labels[] = {UI_MENU_OV_GPU, UI_MENU_OV_VRAM, UI_MENU_OV_CPU, UI_MENU_OV_RAM};
        static const PerfCol c0[] = {{UI_MENU_OV_FPS, 888}};
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_PCT, 100}, {UI_MENU_OV_FMT_GB, 888.8f}, {UI_MENU_OV_FRAME_MS, 888.8f}};
        static const PerfCol c2[] = {{UI_MENU_OV_FMT_TEMP, 188}, {UI_MENU_OV_FMT_GB_OF, 888.8f}, {UI_MENU_OV_MAX_MS, 8888.8f}};
        static const PerfCol c3[] = {{UI_MENU_OV_FMT_POWER, 888}, {UI_MENU_OV_FMT_MHZ, 8888}, {UI_MENU_OV_GAME_PCT, 100},
                                     {UI_MENU_OV_GAME_GB, 888.8f}};
        perf_column("##c0", c0, 1, labels, 4);
        perf_column("##c1", c1, 3);
        perf_column("##c2", c2, 3);
        perf_column("##c3", c3, 4);
        perf_label(UI_MENU_OV_GPU, gpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.gpu_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
        perf_cell(UI_MENU_OV_FMT_POWER, s.gpu_power_w);
        perf_label(UI_MENU_OV_VRAM, gpu_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.vram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.vram_total_mb));
        perf_cell(UI_MENU_OV_FMT_MHZ, s.gpu_clock_mhz);
        perf_label(UI_MENU_OV_CPU, cpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.cpu_temp_c);
        perf_cell(UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
        perf_label(UI_MENU_OV_RAM, ram_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.ram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.ram_total_mb));
        perf_cell(UI_MENU_OV_GAME_GB, perf_gb(s.ram_game_mb));
        ImGui::TableNextColumn();   // the frame row: no label, the rate in its place
        char b[64];
        snprintf(b, sizeof b, menu_text(UI_MENU_OV_FPS), s.fps);
        ImGui::TextColored(fps_c, "%s", b);
        perf_cell(UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
        perf_cell(UI_MENU_OV_MAX_MS, s.frame_ms_max);
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    // as wide as the panel the tables size; the plot does not count toward that width, so the panel can still shrink
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float max_x = win->DC.CursorMaxPos.x;
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    win->DC.CursorMaxPos.x = max_x;
    // per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's
    // right edge, and the table still asks for no more than its widest label and number
    if (ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_MS, 888.88f}, {UI_MENU_OV_PER_FRAME, 888.8f}};
        ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
        perf_column("##w1", c1, 2);
        perf_label(UI_MENU_OV_REPLAY, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.replay_ms);
        perf_label(UI_MENU_OV_GPU_TIME, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.gpu_ms);
        perf_label(UI_MENU_OV_PIPE_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.pipe_wait_ms);
        perf_label(UI_MENU_OV_SHADER_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.xlate_ms);
        perf_label(UI_MENU_OV_TEX_UPLOADS, wait_c);
        perf_cell(UI_MENU_OV_PER_FRAME, s.tex_uploads);
        ImGui::EndTable();
    }
    perf_picture(t, pic_c);
    const char *pname = input_gamepad_name();
    if (pname) {
        int pct = -1, chg = 0;
        int hp = input_pad_power_info(&pct, &chg);
        if (hp && pct >= 0) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "[Controller] %s [%s %d%%]", pname, chg ? "Charging" : "Battery", pct);
        } else {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "[Controller] %s", pname);
        }
    }
    if (s.gpu_name[0]) ImGui::TextDisabled("%s", s.gpu_name);
    if (s.cpu_name[0]) ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
}

static void perf_draw(const MenuTarget *t, float scale) {
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_fps_num[0]) return;
    // with the menu open: under its window, over the dimming layout_menu drew before
    ImDrawList *dl = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();

    ImFont *font = ImGui::GetFont();
    // Sleek, compact HUD font size: ~14.5px at 1080p, ~29px at 4K (fits unobtrusively without taking up half the screen)
    float font_sz = 14.5f * scale;

    auto text_sz = [&](const char *s) -> ImVec2 {
        if (!s || !s[0]) return ImVec2(0, 0);
        return font->CalcTextSizeA(font_sz, FLT_MAX, -1.0f, s);
    };

    float pad_x = 9.0f * scale;
    float pad_y = 5.0f * scale;
    float spacing_x = 6.0f * scale;
    float line_gap = 3.0f * scale;

    // Line 1: Pure performance metrics [LED Dot] [FPS]  1% [1% Low]  0.1% [0.1% Low]  [ms]
    float dot_rad = 3.0f * scale;
    float dot_area_w = dot_rad * 2.0f + 5.0f * scale;
    ImVec2 sz_fps = text_sz(g_perf_fps_num);
    ImVec2 sz_1p = g_perf_1pct_num[0] ? text_sz(g_perf_1pct_num) : ImVec2(0, 0);
    ImVec2 sz_01p = g_perf_01pct_num[0] ? text_sz(g_perf_01pct_num) : ImVec2(0, 0);
    ImVec2 sz_ms = text_sz(g_perf_ms_num);

    float line1_w = dot_area_w + sz_fps.x;
    if (sz_1p.x > 0) line1_w += spacing_x + sz_1p.x;
    if (sz_01p.x > 0) line1_w += spacing_x + sz_01p.x;
    line1_w += spacing_x + sz_ms.x;
    float line1_h = font_sz;

    // Line 2: Grouped technologies [Upscaler] -> [FG Badge] -> [Tail Info] -> [Battery]
    ImVec2 sz_up = text_sz(g_perf_upscaler);
    ImVec2 sz_fg = g_perf_fg_badge[0] ? text_sz(g_perf_fg_badge) : ImVec2(0, 0);
    float fg_pill_w = g_perf_fg_badge[0] ? (sz_fg.x + 6.0f * scale) : 0.0f;

    ImVec2 sz_l2_tail = g_perf_line2[0] ? text_sz(g_perf_line2) : ImVec2(0, 0);
    ImVec2 sz_battery = g_perf_battery[0] ? text_sz(g_perf_battery) : ImVec2(0, 0);

    float line2_w = sz_up.x;
    if (fg_pill_w > 0) line2_w += spacing_x + fg_pill_w;
    if (sz_l2_tail.x > 0) line2_w += spacing_x + sz_l2_tail.x;
    if (sz_battery.x > 0) line2_w += spacing_x + sz_battery.x;
    float line2_h = font_sz;

    float card_w = fmaxf(line1_w, line2_w) + pad_x * 2.0f;
    float card_h = line1_h + line2_h + line_gap + pad_y * 2.0f;

    ImVec2 p_min(w - 12.0f * scale - card_w, 10.0f * scale);
    ImVec2 p_max(p_min.x + card_w, p_min.y + card_h);

    // Color theme
    ImU32 border_col = (g_perf_theme == 0) ? IM_COL32(118, 185, 0, 180)   // NVIDIA RTX Green
                     : (g_perf_theme == 1) ? IM_COL32(235, 60, 60, 180)   // AMD Crimson
                     : (g_perf_theme == 3) ? IM_COL32(0, 140, 255, 180)   // Intel Electric Blue
                     : IM_COL32(40, 160, 230, 160);                        // Cyber Cyan

    ImU32 dot_core_col = (g_perf_theme == 0)  ? IM_COL32(118, 185, 0, 255)
                       : (g_perf_theme == 1)  ? IM_COL32(245, 65, 65, 255)
                       : (g_perf_theme == 3)  ? IM_COL32(0, 160, 255, 255)
                       : IM_COL32(50, 185, 255, 255);

    ImU32 dot_glow_col = (g_perf_theme == 0)  ? IM_COL32(118, 185, 0, 65)
                       : (g_perf_theme == 1)  ? IM_COL32(245, 65, 65, 65)
                       : (g_perf_theme == 3)  ? IM_COL32(0, 140, 255, 75)
                       : IM_COL32(50, 185, 255, 65);

    // Card background & sleek rounded border
    dl->AddRectFilled(p_min, p_max, IM_COL32(12, 16, 24, 225), 4.0f * scale);
    dl->AddRect(p_min, p_max, border_col, 4.0f * scale, 0, 1.0f * scale);

    // Line 1 drawing:
    float cur_x = p_min.x + pad_x;
    float line1_y = p_min.y + pad_y;

    // LED Dot
    ImVec2 dot_c(cur_x + dot_rad, line1_y + line1_h * 0.5f);
    dl->AddCircleFilled(dot_c, dot_rad + 1.5f * scale, dot_glow_col);
    dl->AddCircleFilled(dot_c, dot_rad, dot_core_col);
    cur_x += dot_area_w;

    // Numeric FPS
    dl->AddText(font, font_sz, ImVec2(cur_x, line1_y), IM_COL32(255, 255, 255, 255), g_perf_fps_num);
    cur_x += sz_fps.x + spacing_x;

    // 1% Low
    if (sz_1p.x > 0) {
        dl->AddText(font, font_sz, ImVec2(cur_x, line1_y), IM_COL32(210, 225, 245, 225), g_perf_1pct_num);
        cur_x += sz_1p.x + spacing_x;
    }

    // 0.1% Low
    if (sz_01p.x > 0) {
        dl->AddText(font, font_sz, ImVec2(cur_x, line1_y), IM_COL32(180, 205, 235, 200), g_perf_01pct_num);
        cur_x += sz_01p.x + spacing_x;
    }

    // Frame Time
    dl->AddText(font, font_sz, ImVec2(cur_x, line1_y), IM_COL32(160, 185, 215, 230), g_perf_ms_num);

    // Line 2 drawing: [Upscaler] -> [FG Badge] -> [Tail Info] -> [Battery]
    float line2_y = line1_y + line1_h + line_gap;
    float l2_x = p_min.x + pad_x;

    // 1) Upscaler name & preset in theme accent color
    ImU32 up_col = (g_perf_theme == 0) ? IM_COL32(118, 195, 20, 255)
                 : (g_perf_theme == 1) ? IM_COL32(255, 95, 95, 255)
                 : (g_perf_theme == 3) ? IM_COL32(30, 175, 255, 255)
                 : IM_COL32(95, 205, 250, 255);
    dl->AddText(font, font_sz, ImVec2(l2_x, line2_y), up_col, g_perf_upscaler);
    l2_x += sz_up.x;

    // 2) Frame Generation pill badge directly adjacent to upscaler
    if (fg_pill_w > 0) {
        l2_x += spacing_x;
        ImVec2 fg_min(l2_x, line2_y + 0.5f * scale);
        ImVec2 fg_max(l2_x + fg_pill_w, line2_y + line2_h - 0.5f * scale);
        dl->AddRectFilled(fg_min, fg_max, IM_COL32(105, 40, 175, 190), 2.5f * scale);
        dl->AddRect(fg_min, fg_max, IM_COL32(175, 100, 250, 220), 2.5f * scale, 0, 0.8f * scale);
        dl->AddText(font, font_sz, ImVec2(l2_x + 3.0f * scale, line2_y), IM_COL32(245, 215, 255, 255), g_perf_fg_badge);
        l2_x += fg_pill_w;
    }

    // 3) Remaining tail info: resolution, Reflex, XeLL
    if (sz_l2_tail.x > 0) {
        l2_x += spacing_x;
        dl->AddText(font, font_sz, ImVec2(l2_x, line2_y), IM_COL32(175, 190, 205, 225), g_perf_line2);
        l2_x += sz_l2_tail.x;
    }
    if (sz_battery.x > 0) {
        l2_x += spacing_x;
        dl->AddText(font, font_sz, ImVec2(l2_x, line2_y), IM_COL32(135, 225, 180, 240), g_perf_battery);
    }
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    if (!g_test.parsed) test_parse();
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    if (g_perf_save.exchange(0)) cfg_save();
    perf_tick(plat_now_ns(), t);
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_save_pending) g_save_pending = opt_save_tick(1);   // closed: the last change goes to dkb.ini now
        if (g_test.at && !g_test.opened && frame >= g_test.at) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = 0;
            set_open(1);
            open = true;
        } else if (!g_perf_mode) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->height);
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // opening: the first row has the cursor
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // a confirmation left open on closing
        g_modal = false;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {   // the overlay alone: no input reaches ImGui
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset);
    } else {   // the test hook's pages: the user's input in a background test window must not change settings
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open) layout_menu(t->width, t->height, scale);
    perf_draw(t, scale);
    ImGui::Render();
    if (open) g_save_pending = opt_save_tick(0);
    g_cost_ns += plat_now_ns() - t0;
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];   // this frame's image goes to a file; then the next tab, or the menu closes
        const char *dir = cfg_get("DKB_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == TAB_COUNT) {
            g_test.page = -1;
            set_open(0);   // the next frame lays out nothing
        } else {
            g_tab_request = g_test.page;
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
