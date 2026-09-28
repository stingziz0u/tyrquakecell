/*
 * vid_ps3_common.c -- the part of the PS3 video drivers both renderers
 * share: internal resolutions, the Video Settings cvars (menu.c's Video
 * Settings menu) and applying them to the display every frame.
 *
 * The software and OpenGL builds install as the same game and share
 * config.cfg, so each renderer keeps its own resolution cvar
 * (vid_ps3_swmode, vid_ps3_glmode) and both are registered in both
 * builds: switching PKGs doesn't lose the other one's choice.
 *
 * Copyright (C) 2026 the TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <stdio.h>
#include <string.h>

#include <math.h>

#include "common.h"
#include "cvar.h"
#include "quakedef.h"
#include "vid.h"
#include "view.h"

#include "ps3_video.h"
#include "vid_ps3_common.h"

typedef struct {
    int width, height;
    const char *aspect;
} ps3_mode_t;

/*
 * Software: the CPU draws every pixel, so up to 960x540 (the default
 * stays 1.x's 512x384). OpenGL: the RSX draws, 640x480 and up (lower
 * would only look worse; the water warp textures also need 256 rows).
 * Widths are multiples of 8 (the software renderer's spans).
 */
#ifdef GLQUAKE
static const ps3_mode_t modes[] = {
    { 640, 480, "4:3" },
    { 800, 600, "4:3" },
    { 960, 720, "4:3" },
    { 848, 480, "16:9" },
    { 960, 540, "16:9" },
    { 1280, 720, "16:9" },
};
#define DEFAULT_MODE 5
#else
static const ps3_mode_t modes[] = {
    { 320, 240, "4:3" },
    { 400, 300, "4:3" },
    { 512, 384, "4:3" },
    { 640, 480, "4:3" },
    { 640, 360, "16:9" },
    { 768, 432, "16:9" },
    { 848, 480, "16:9" },
    { 960, 540, "16:9" },
};
#define DEFAULT_MODE 2
#endif

#define NUM_MODES ((int)(sizeof(modes) / sizeof(modes[0])))

static cvar_t vid_ps3_swmode = { "vid_ps3_swmode", "512x384", CVAR_CONFIG };
static cvar_t vid_ps3_glmode = { "vid_ps3_glmode", "1280x720", CVAR_CONFIG };

cvar_t vid_ps3_brightness = { "vid_ps3_brightness", "1", CVAR_CONFIG };
cvar_t vid_ps3_screenfit = { "vid_ps3_screenfit", "90", CVAR_CONFIG };
cvar_t vid_ps3_filter = { "vid_ps3_filter", "1", CVAR_CONFIG };
cvar_t vid_ps3_fps30 = { "vid_ps3_fps30", "0", CVAR_CONFIG };
/* 0 automatic (as big as fits), otherwise the HUD's scale (1 = 320x200) */
cvar_t vid_ps3_hudscale = { "vid_ps3_hudscale", "0", CVAR_CONFIG };

/* fps and timings in the log every 10 seconds (not saved) */
#ifdef TYRQUAKE_DEBUG_BUILD
static cvar_t vid_ps3_perflog = { "vid_ps3_perflog", "1" };
#else
static cvar_t vid_ps3_perflog = { "vid_ps3_perflog", "0" };
#endif

static cvar_t *
ModeCvar(void)
{
#ifdef GLQUAKE
    return &vid_ps3_glmode;
#else
    return &vid_ps3_swmode;
#endif
}

void
VID_PS3_RegisterVariables(void)
{
    Cvar_RegisterVariable(&vid_ps3_swmode);
    Cvar_RegisterVariable(&vid_ps3_glmode);
    Cvar_RegisterVariable(&vid_ps3_brightness);
    Cvar_RegisterVariable(&vid_ps3_screenfit);
    Cvar_RegisterVariable(&vid_ps3_filter);
    Cvar_RegisterVariable(&vid_ps3_fps30);
    Cvar_RegisterVariable(&vid_ps3_hudscale);
    Cvar_RegisterVariable(&vid_ps3_perflog);
}

int
VID_PS3_NumModes(void)
{
    return NUM_MODES;
}

int
VID_PS3_ModeWidth(int mode)
{
    return modes[(mode >= 0 && mode < NUM_MODES) ? mode : DEFAULT_MODE].width;
}

int
VID_PS3_ModeHeight(int mode)
{
    return modes[(mode >= 0 && mode < NUM_MODES) ? mode : DEFAULT_MODE].height;
}

const char *
VID_PS3_ModeAspect(int mode)
{
    return modes[(mode >= 0 && mode < NUM_MODES) ? mode : DEFAULT_MODE].aspect;
}

int
VID_PS3_CurrentMode(void)
{
    const char *s = ModeCvar()->string;
    int w, h, i;

    if (s && sscanf(s, "%dx%d", &w, &h) == 2) {
        for (i = 0; i < NUM_MODES; i++) {
            if (modes[i].width == w && modes[i].height == h)
                return i;
        }
    }

    return DEFAULT_MODE;
}

void
VID_PS3_SelectMode(int mode)
{
    char buf[32];

    if (mode < 0 || mode >= NUM_MODES)
        mode = DEFAULT_MODE;

    snprintf(buf, sizeof(buf), "%dx%d", modes[mode].width, modes[mode].height);
    Cvar_Set(ModeCvar()->name, buf);
}

int
VID_PS3_WantedMode(int w_now, int h_now, int *w, int *h)
{
    int mode = VID_PS3_CurrentMode();

    *w = modes[mode].width;
    *h = modes[mode].height;

    return *w != w_now || *h != h_now;
}

float
VID_PS3_HudScale(int width, int height)
{
    /* the classic 320x200 picture, as big as fits (1.x: 512 / 320 = 1.6) */
    float sx = (float)width / 320.0f;
    float sy = (float)height / 200.0f;
    float fit = sx < sy ? sx : sy;
    float s = vid_ps3_hudscale.value;

    if (fit < 1.0f)
        fit = 1.0f;

    /* Video Settings > HUD Scale; never bigger than what fits, or the
       menus and the status bar leave the screen */
    if (s <= 0.0f || s > fit)
        s = fit;

    return s;
}

void
VID_PS3_ApplyHudScale(void)
{
    float s = VID_PS3_HudScale(vid.width, vid.height);

    /* Also undoes an scr_hudscale from config.cfg (1.x saved 1.6 there,
       for 512x384). The cvar's callback rescales the HUD and console. */
    if (fabsf(Cvar_VariableValue("scr_hudscale") - s) > 0.001f)
        Cvar_SetValue("scr_hudscale", s);
}

void
VID_PS3_ApplySettings(void)
{
    float exponent = 1.0f;

#ifdef GLQUAKE
    /* Quake's "gamma" (Video Settings > Gamma). The software renderer
       puts it in the palette; here the final pass applies it. */
    exponent = v_gamma.value;
    if (exponent < 0.25f || exponent > 4.0f)
        exponent = 1.0f;
#endif

    PS3_Video_SetFit((int)vid_ps3_screenfit.value);
    PS3_Video_SetFilter((int)vid_ps3_filter.value);
    PS3_Video_SetLock30((int)vid_ps3_fps30.value);
    PS3_Video_SetPerfLog((int)vid_ps3_perflog.value);
    PS3_Video_SetColor(vid_ps3_brightness.value > 0.0f ? vid_ps3_brightness.value : 1.0f,
                       exponent);
}

void
VID_PS3_ResetSettings(void)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%dx%d", modes[DEFAULT_MODE].width, modes[DEFAULT_MODE].height);
    Cvar_Set(ModeCvar()->name, buf);
    Cvar_Set("vid_ps3_brightness", "1");
    Cvar_Set("gamma", "1");
    Cvar_Set("vid_ps3_screenfit", "90");
    Cvar_Set("vid_ps3_filter", "1");
    Cvar_Set("vid_ps3_fps30", "0");
    Cvar_Set("vid_ps3_hudscale", "0");
    Cvar_Set("show_fps", "0");
#ifdef GLQUAKE
    Cvar_Set("gl_texturemode", "gl_nearest");
    Cvar_Set("r_shadows", "0");
#endif
}
