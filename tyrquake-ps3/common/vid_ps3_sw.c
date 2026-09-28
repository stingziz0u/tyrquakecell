// vid_ps3_sw.c -- PS3 video driver for the software renderer (1.x: vid_ps3.c).
//
// The RSX side (init, triple buffered flips, the scaled blit to the TV,
// brightness) is ps3_video.c, shared with the OpenGL build. What stays
// here is TyrQuake's software renderer contract (see vid_null.c): the
// renderer draws 8 bit indexed pixels into vid.buffer, VID_Update pushes
// them through the palette into the source image in RSX memory and
// presents it.
//
// 1.x history, still true: the RSX init/present sequence (gcmSetFlip ->
// rsxFlushBuffer -> gcmSetWaitFlip) and the 8-bit -> ARGB palette
// expansion come from the dragonfly-quake-ps3 port; 3 buffers, not 2,
// or any frame over 16.6 ms costs a whole extra vsync (xash3d-fwgs's PS3
// port hit the same); r_warpbuffer must exist or standing in water or
// lava crashes.
//
// The internal resolution is chosen in Video Settings (vid_ps3_common.c):
// every buffer is sized for the largest one, so switching is only a
// matter of new sizes and a flushed surface cache.

#include "common.h"
#include "console.h"
#include "d_iface.h"
#include "d_local.h"
#include "quakedef.h"
#include "screen.h"
#include "r_local.h"
#include "r_shared.h"
#include "sys.h"
#include "zone.h"

#ifdef NQ_HACK
#include "host.h"
#endif

#include <string.h>
#include <ppu-types.h>
#include <lv2/systime.h>

#include "ps3_video.h"
#include "vid_ps3_common.h"

/* Defined in sys_ps3.c -- writes to a log file on dev_hdd0 since printf
 * output isn't visible on real hardware without a debug console. */
extern void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

viddef_t vid; // global video state

// The largest internal resolution in vid_ps3_common.c's list.
#define MAX_WIDTH  960
#define MAX_HEIGHT 540

static byte vid_buffer[MAX_WIDTH * MAX_HEIGHT];   // 8-bit indexed, vid.buffer
static short zbuffer[MAX_WIDTH * MAX_HEIGHT];
static byte warpbuffer[MAX_WIDTH * MAX_HEIGHT];   // r_warpbuffer (underwater view)

// D_SurfaceCacheForRes(960, 540) is about 3.6 MB; 1.x gave 512x384 4 MB
// as insurance against r_cache_thrash in busy scenes. Every resolution
// gets whatever fits here.
static byte surfcache[6 * 1024 * 1024];

unsigned short d_8to16table[256];
unsigned d_8to24table[256]; // doubles as our ARGB palette LUT (0xAARRGGBB)

static qvidmode_t ps3_mode;  // the one mode, so VID_IsFullScreen() is true
static qboolean caches_ready;

static void
PS3_SW_SetMode(int width, int height)
{
    int size;

    if (width > MAX_WIDTH || height > MAX_HEIGHT || PS3_Video_SetSource(width, height) != 0) {
        PS3_Log("VID: %dx%d not possible, using 512x384", width, height);
        width = 512;
        height = 384;

        if (PS3_Video_SetSource(width, height) != 0)
            Sys_Error("PS3 video: no source image for %dx%d", width, height);
    }

    vid.width = vid.conwidth = width;
    vid.height = vid.conheight = height;
    vid.aspect = 1.0;
    vid.numpages = 1;
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = vid.conrowbytes = width;
    vid.output.width = width;
    vid.output.height = height;
    vid.output.scale = 1;

    d_pzbuffer = zbuffer;
    r_warpbuffer = warpbuffer;

    size = D_SurfaceCacheForRes(width, height);
    if (size > (int)sizeof(surfcache))
        size = sizeof(surfcache);
    if (caches_ready)
        D_FlushCaches();
    D_InitCaches(surfcache, size);
    caches_ready = true;

    ps3_mode.width = width;
    ps3_mode.height = height;
    ps3_mode.bpp = 8;
    ps3_mode.refresh = 60;
    vid_currentmode = &ps3_mode;

    vid.recalc_refdef = 1;

    // The HUD, console and menus: Video Settings > HUD Scale, by default
    // the classic 320x200 picture as big as it fits (1.x's 512x384 gave
    // 1.6; TyrQuake's own automatic scale never scales up below 600 rows).
    VID_PS3_ApplyHudScale();
    SCR_CheckResize();
    Con_CheckResize();

    PS3_Log("VID: software renderer at %dx%d (surface cache %d KB, HUD scale %.2f)",
            width, height, size / 1024, scr_scale);
}

// --- TyrQuake vid.h contract ---

void
VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = vid.width;
    rect->height = vid.height;
}

void
VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void
VID_SetPalette(const byte *palette)
{
    if (!palette)
        return;

    // PS3 is big-endian: 0xAARRGGBB as a u32 literally matches memory
    // byte order [A][R][G][B], which is what the A8R8G8B8 source expects.
    for (int i = 0; i < 256; i++) {
        unsigned r = palette[i * 3 + 0];
        unsigned g = palette[i * 3 + 1];
        unsigned b = palette[i * 3 + 2];
        d_8to24table[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

void
VID_Init(const byte *palette)
{
    int w, h;

    PS3_Log("VID_Init: start (software renderer)");

    if (PS3_Video_Init() != 0)
        Sys_Error("PS3 video: RSX init failed (see the log)");

    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    // config.cfg isn't read yet: this is the default (or the command
    // line); VID_Update switches to the saved one after the first frame.
    VID_PS3_WantedMode(0, 0, &w, &h);
    PS3_SW_SetMode(w, h);

    VID_SetPalette(palette);

    // Every other driver wires these up; opening the old "Video Options"
    // menu with them NULL crashed. Video Settings (menu.c) doesn't use
    // them any more, but anything else that might still reaches a
    // harmless menu.
    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;

#ifdef TYRQUAKE_DEBUG_BUILD
    // Quake's own render-stage timing (r_dspeeds) and poly/surface
    // counters (r_speeds), read by the R_Speeds log line in VID_Update.
    Cvar_SetValue("r_speeds", 1.0f);
    Cvar_SetValue("r_dspeeds", 1.0f);
#endif

    PS3_Log("VID_Init: complete");
}

void
VID_InitColormap(const byte *palette)
{
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

void
VID_Shutdown(void)
{
    PS3_Video_Shutdown();
}

#ifdef TYRQUAKE_DEBUG_BUILD
// 1.x's per second render-stage log (R_PrintTimes / R_PrintDSpeeds's
// variables, read directly): tells whether world geometry, brush
// entities, edge scanning, alias models, the view model or particles
// dominate a busy scene.
static void
PS3_LogRSpeeds(void)
{
    static double window_start = -1.0;
    double now = Sys_DoubleTime();

    if (window_start < 0)
        window_start = now;
    if (now - window_start < 1.0)
        return;
    window_start = now;

    PS3_Log("R_Speeds: %3ims poly=%d/%d surf=%d amodels=%d | world=%.1fms bmodels=%.1fms edges=%.1fms entities=%.1fms viewmodel=%.1fms particles=%.1fms cache_thrash=%d",
            (int)((rw_time2 - rw_time1 + db_time2 - db_time1 + se_time2 - se_time1
                   + de_time2 - de_time1 + dv_time2 - dv_time1 + dp_time2 - dp_time1) * 1000),
            r_polycount, r_drawnpolycount, c_surf, r_amodels_drawn,
            (rw_time2 - rw_time1) * 1000, (db_time2 - db_time1) * 1000,
            (se_time2 - se_time1) * 1000, (de_time2 - de_time1) * 1000,
            (dv_time2 - dv_time1) * 1000, (dp_time2 - dp_time1) * 1000,
            r_cache_thrash);
}
#endif

void
VID_Update(vrect_t *rects)
{
    static qboolean first_call = true;
    const int width = vid.width, height = vid.height;
    u64 t0;
    u32 *dst;
    int w, h;

    (void)rects;

    if (first_call) {
        PS3_Log("VID_Update: first call reached");
        first_call = false;
    }

    // Straight into this frame's source image in RSX memory (it waits
    // until the RSX is done with it). The whole image every frame: the
    // dirty rectangles would need the other two images kept in step.
    dst = PS3_Video_Source();
    if (!dst)
        return;

    t0 = sysGetSystemTime();

    // Manually unrolled 4-wide: the PPU is an in-order core, so it gains
    // more than usual from fewer loop branches and several independent
    // loads/stores per iteration.
    for (int y = 0; y < height; y++) {
        const byte *src = vid_buffer + y * width;
        u32 *out = dst + y * width;
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            out[x + 0] = d_8to24table[src[x + 0]];
            out[x + 1] = d_8to24table[src[x + 1]];
            out[x + 2] = d_8to24table[src[x + 2]];
            out[x + 3] = d_8to24table[src[x + 3]];
        }
        for (; x < width; x++)
            out[x] = d_8to24table[src[x]];
    }

    PS3_Video_ProfileCopy((long long)(sysGetSystemTime() - t0));

    VID_PS3_ApplySettings();
    PS3_Video_Present();

#ifdef TYRQUAKE_DEBUG_BUILD
    PS3_LogRSpeeds();
#endif

    // Between frames: the resolution and HUD scale from config.cfg or
    // Video Settings.
    if (VID_PS3_WantedMode(vid.width, vid.height, &w, &h))
        PS3_SW_SetMode(w, h);
    else
        VID_PS3_ApplyHudScale();
}

void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
}

void
D_EndDirectRect(int x, int y, int width, int height)
{
}

qboolean
VID_CheckAdequateMem(int width, int height)
{
    return true;
}

void
VID_ProcessEvents(void)
{
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}

void
VID_AddCommands(void)
{
}

void
VID_RegisterVariables(void)
{
    VID_PS3_RegisterVariables();
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    // Only reached through the "game" command: same size again.
    PS3_SW_SetMode(vid.width, vid.height);
    VID_SetPalette(palette);
    return true;
}

void
VID_SetDefaultMode(void)
{
}

qboolean
window_visible(void)
{
    return true;
}
