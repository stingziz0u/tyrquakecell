/*
 * ps3_launcher.h -- the game picker shown before the engine starts, and
 * the switch between the two renderers (ps3_launcher.c).
 *
 * Copyright (C) 2026 the TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef PS3_LAUNCHER_H
#define PS3_LAUNCHER_H

/* Where the game data is (id1/, hipnotic/, rogue/, mods): the base
   package's USRDIR, for every build */
#ifndef PS3_DATA_DIR
#define PS3_DATA_DIR "/dev_hdd0/game/TYRQ00001/USRDIR"
#endif

/* This package's own USRDIR (the debug build has its own APPID): the
   programs, the log and the launcher's files */
#ifndef PS3_APP_DIR
#define PS3_APP_DIR PS3_DATA_DIR
#endif

/* Before Host_Init: picks the game (the menu, or straight through when
   there is one thing to play) and returns its command line. If the game
   wants the other renderer, starts that program instead and never
   returns. */
void PS3_Launcher_Run(int *argc, char ***argv);

/* While the menu is up the XMB's "Quit Game" only asks it to leave
   (nothing of the engine exists yet to shut down). */
extern volatile int ps3_launcher_active;
extern volatile int ps3_launcher_quit_requested;
/* Set when the XMB asked to quit: Quit then exits for good. */
extern volatile int ps3_quit_to_xmb;

/* The renderer of this program (1 OpenGL, 0 software), and the one the
   player chose in Video Settings (saved in USRDIR/ps3_renderer.txt). */
int  PS3_Renderer_Running(void);
int  PS3_Renderer_Chosen(void);
void PS3_Renderer_Choose(int gl);

/* Video Settings: restarts the game in the chosen renderer, same game,
   from the start (Host_Shutdown saves the config first). Returns only if
   it could not. */
void PS3_Renderer_Restart(void);

/* Quit from the game's menu, after Host_Shutdown: back to the launcher
   (a fresh copy of this program). Returns only when the game must really
   exit. */
void PS3_Launcher_Relaunch(void);

#endif
