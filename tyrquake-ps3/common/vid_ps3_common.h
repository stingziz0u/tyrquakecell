/*
 * vid_ps3_common.h -- what both PS3 video drivers share: the internal
 * resolutions, the Video Settings cvars and applying them to the display
 * (ps3_video.c).
 *
 * Copyright (C) 2026 the TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef VID_PS3_COMMON_H
#define VID_PS3_COMMON_H

#include "cvar.h"

/* Video Settings (menu.c), saved in config.cfg */
extern cvar_t vid_ps3_brightness;   /* gain, 0.5 - 2.0 */
extern cvar_t vid_ps3_screenfit;    /* % of the TV used, 70 - 100 */
extern cvar_t vid_ps3_filter;       /* 1 smooth, 0 sharp */
extern cvar_t vid_ps3_fps30;        /* 1 locked at 30 */
extern cvar_t vid_ps3_hudscale;     /* 0 automatic, else 1 - 4 */

/* Called by the drivers' VID_RegisterVariables. */
void VID_PS3_RegisterVariables(void);

/* The internal resolutions of this renderer (the software one and the
   OpenGL one have their own lists). */
int         VID_PS3_NumModes(void);
int         VID_PS3_ModeWidth(int mode);
int         VID_PS3_ModeHeight(int mode);
const char *VID_PS3_ModeAspect(int mode);   /* "4:3" / "16:9" */

/* The one chosen (its cvar: vid_ps3_swmode / vid_ps3_glmode, "WxH"); the
   renderer's default if the cvar holds anything else. */
int  VID_PS3_CurrentMode(void);
/* Chooses one: the driver switches to it at the end of the frame. */
void VID_PS3_SelectMode(int mode);
/* The driver: the mode it should be in, after the config is read or the
   menu changed it. Sets *w, *h; nonzero if different from w_now x h_now. */
int  VID_PS3_WantedMode(int w_now, int h_now, int *w, int *h);

/* HUD / menu scale for a resolution: 1 at 320x200 (vid_ps3_hudscale,
   or as big as fits). ApplyHudScale sets it for the current one. */
float VID_PS3_HudScale(int width, int height);
void  VID_PS3_ApplyHudScale(void);

/* Screen fit, filter, frame rate, brightness and (OpenGL) gamma to the
   display. Once per frame, before presenting (the HUD scale after it,
   between frames). */
void VID_PS3_ApplySettings(void);

/* Back to the defaults (menu). */
void VID_PS3_ResetSettings(void);

#endif
