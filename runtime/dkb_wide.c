// Bananza's virtual display is two instructions in the game, 2880 by 1620. The generated dkbwide
// sites read the width, height and render-target pool from this page instead. The window size is
// separate: this is the size the game draws.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

static unsigned wide_output[2] = {2880, 1620};

// The game's render-target pool is a fixed 200 MiB sized for 2880x1620 (main+0x458adc).
// Grow it with the output pixel count, rounded up to 2 MiB, never below the default.
// 7680x4320 needs 1424 MiB, which fits in a 32-bit size.
static uint32_t dkb_wide_pool_bytes(unsigned w, unsigned h) {
    const uint64_t base = 0xc800000, align = 2u << 20;
    uint64_t bytes = (base * w * h + 2880ull * 1620 - 1) / (2880ull * 1620);
    bytes = (bytes + align - 1) / align * align;
    return (uint32_t)(bytes > base ? bytes : base);
}

int dkb_wide_output(unsigned out[2]) {
    if (out) memcpy(out, wide_output, sizeof wide_output);
    return wide_output[0] * 9 > wide_output[1] * 16;
}

void dkb_wide_init(void) {
    wide_output[0] = 2880;
    wide_output[1] = 1620;
    int set = rt_patch_set("dkbwide");
    if (set < 0) {
        rt_log("Bananza resolution: this build has no resolution patch; the game keeps 2880x1620");
        return;
    }
    rt_patch_switch(set, 0);
    unsigned width = 0, height = 0;
    char tail;
    const char *value = cfg_value("TOTK_OUTPUT_RES");
    // 16:9 only. The camera and HUD sites that a wider picture needs are not in this build.
    if (sscanf(value, "%ux%u%c", &width, &height, &tail) != 2 ||
        width < 1280 || width > 7680 || height < 720 || height > 4320 ||
        width % 2 || height % 2 || width * 9 != height * 16) {
        rt_log("Bananza resolution: %s is not an even 16:9 size in 1280x720..7680x4320; the game keeps 2880x1620",
               value && *value ? value : "(empty)");
        return;
    }
    uint8_t *page = rt_patch_page();
    float aspect = (float)width / (float)height;
    float factor = aspect / (16.0f / 9.0f);
    uint32_t pool = dkb_wide_pool_bytes(width, height);
    memcpy(page + 0x100, &width, 4);
    memcpy(page + 0x104, &height, 4);
    memcpy(page + 0x108, &aspect, 4);
    memcpy(page + 0x10c, &factor, 4);
    memcpy(page + 0x110, &pool, 4);
    wide_output[0] = width;
    wide_output[1] = height;
    rt_patch_switch(set, 1);
    rt_log("Bananza resolution: virtual display %ux%u, render-target pool %u MiB",
           width, height, pool >> 20);
}
