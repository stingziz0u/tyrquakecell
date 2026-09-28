/*
 * vid_ps3_gl.c -- PS3 video driver for TyrQuake's OpenGL renderer
 * (GLQuake), replacing vid_sgl.c.
 *
 * There is no OpenGL on the PS3: ps3gl (gl/, from Quake2PS3) is a small
 * fixed function OpenGL on top of the RSX. It draws into a render target
 * of the internal resolution chosen in Video Settings, and every frame
 * ps3_video.c scales that picture to the TV like the software renderer's
 * (same screen fit, filter, brightness and frame rate settings).
 *
 * Gamma: there is no hardware gamma ramp, but VID_SetGammaRamp is set, so
 * TyrQuake doesn't fall back to darkening with a blended polygon; Quake's
 * "gamma" goes to the final pass instead (vid_ps3_common.c).
 *
 * Copyright (C) 1996-1997 Id Software, Inc.
 * Copyright (C) 2026 the TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <GL/gl.h>

#include "cmd.h"
#include "console.h"
#include "glquake.h"
#include "quakedef.h"
#include "sbar.h"
#include "screen.h"
#include "sys.h"
#include "vid.h"

#ifdef NQ_HACK
#include "host.h"
#endif

#include "ps3_video.h"
#include "vid_ps3_common.h"
#include "gl/ps3gl.h"

extern void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

viddef_t vid;
float gldepthmin, gldepthmax;

static qvidmode_t ps3_mode;  /* the one mode, so VID_IsFullScreen() is true */

/* Only there to be non-NULL (see the top of the file) */
static void
PS3_SetGammaRamp(unsigned short ramp[3][256])
{
    (void)ramp;
}

void (*VID_SetGammaRamp)(unsigned short ramp[3][256]) = PS3_SetGammaRamp;

/* Culling direction check, in case the RSX ever sees it the other way
   round (Quake2PS3's: it stayed 0 on hardware). */
static cvar_t gl_ps3_cullflip = { "gl_ps3_cullflip", "0" };

void *
GL_GetProcAddress(const char *name)
{
    return PS3GL_GetProcAddress(name);
}

static void
PS3_GL_SetMode(int width, int height)
{
    if (PS3_Video_SetRenderTarget(width, height) != 0) {
        PS3_Log("VID: %dx%d render target not possible, using 640x480", width, height);
        width = 640;
        height = 480;

        if (PS3_Video_SetRenderTarget(width, height) != 0)
            Sys_Error("PS3 video: no render target for %dx%d", width, height);
    }

    /* the first time it starts ps3gl; later only the size changes, the
       GL state TyrQuake set up stays */
    if (PS3GL_Init(width, height) != 0)
        Sys_Error("PS3 video: ps3gl failed to start (see the log)");

    vid.width = vid.conwidth = width;
    vid.height = vid.conheight = height;
    vid.aspect = 1;
    vid.numpages = 0; /* the back buffer is undefined after a swap */
    vid.output.width = width;
    vid.output.height = height;
    vid.output.scale = 1;

    ps3_mode.width = width;
    ps3_mode.height = height;
    ps3_mode.bpp = 32;
    ps3_mode.refresh = 60;
    vid_currentmode = &ps3_mode;

    vid.recalc_refdef = 1;

    /* the HUD, console and menus: Video Settings > HUD Scale */
    VID_PS3_ApplyHudScale();
    SCR_CheckResize();
    Con_CheckResize();

    PS3_Log("VID: OpenGL (ps3gl) at %dx%d, HUD scale %.2f", width, height, scr_scale);
}

qboolean VID_CheckAdequateMem(int width, int height) { return true; }
void VID_LockBuffer(void) {}
void VID_UnlockBuffer(void) {}
void VID_Update(vrect_t *rects) {}
void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}
void VID_ProcessEvents(void) {}
void VID_SetDefaultMode(void) {}
void VID_AddCommands(void) {}

qboolean
window_visible(void)
{
    return true;
}

void
VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = PS3_Video_Width();
    rect->height = PS3_Video_Height();
}

void
VID_InitColormap(const byte *palette)
{
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

void
VID_SetPalette(const byte *palette)
{
    QPic32_InitPalettes(palette);
}

void
VID_ShiftPalette(const byte *palette)
{
    /* Done via gl_polyblend instead */
}

void
VID_RegisterVariables(void)
{
    VID_PS3_RegisterVariables();
    Cvar_RegisterVariable(&gl_ps3_cullflip);
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    /* Only reached through the "game" command: same size again. */
    PS3_GL_SetMode(vid.width, vid.height);
    return true;
}

void
VID_Init(const byte *palette)
{
    int w, h;

    PS3_Log("VID_Init: start (OpenGL renderer)");

    if (PS3_Video_Init() != 0)
        Sys_Error("PS3 video: RSX init failed (see the log)");

    VID_SetPalette(palette);
    VID_InitColormap(palette);

    /* config.cfg isn't read yet: this is the default; GL_EndRendering
       switches to the saved one after the first frame. */
    VID_PS3_WantedMode(0, 0, &w, &h);
    PS3_GL_SetMode(w, h);

    PS3GL_SetCullFlip((int)gl_ps3_cullflip.value);

    GL_Init();

    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;

    PS3_Log("VID_Init: complete");
}

void
VID_Shutdown(void)
{
    PS3_Video_Shutdown();
}

void
GL_BeginRendering(int *x, int *y, int *width, int *height)
{
    /* waits for a free render target and makes it the RSX's surface */
    PS3GL_BeginFrame();

    *x = *y = 0;
    *width = vid.width;
    *height = vid.height;
}

void
GL_EndRendering(void)
{
    static int frames;
    int w, h;

    PS3GL_SetCullFlip((int)gl_ps3_cullflip.value);
    PS3GL_EndFrame();
    VID_PS3_ApplySettings();
    PS3_Video_Present();

    if (frames < 3)
        PS3_Log("GL_EndRendering: frame %d presented", frames);
    frames++;

    /* Between frames: the resolution and HUD scale from config.cfg or
       Video Settings. */
    if (VID_PS3_WantedMode(vid.width, vid.height, &w, &h))
        PS3_GL_SetMode(w, h);
    else
        VID_PS3_ApplyHudScale();
}
