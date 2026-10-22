/*
 * ps3_launcher.c -- the game picker shown before the engine starts, and
 * the switch between the two renderers.
 *
 * Copyright (C) 2026 the TyrQuakeCell contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * One package (1.6.0) instead of six: the games are picked here and the
 * renderer in Video Settings. The renderer is compiled in (#ifdef
 * GLQUAKE), so the package carries two programs and runs the one wanted:
 *
 *   EBOOT.BIN          OpenGL, what the XMB starts
 *   RELOAD.SELF        the same program as a plain SELF
 *   TYRQUAKE_SW.SELF   the software renderer
 *
 * sysProcessExitSpawn2 cannot start EBOOT.BIN (an NPDRM SELF), hence the
 * plain copy, as CrispyCell's Quit Game does.
 *
 * Both programs have this launcher. It lists what is installed in USRDIR
 * -- Quake (id1/), Scourge of Armagon (hipnotic/), Dissolution of Eternity
 * (rogue/) and, on a MODS tab, every other folder with a pak0.pak or a
 * progs.dat in it (run with -game) -- and skips the menu when there is
 * only one thing to play. When the chosen renderer is the other program,
 * the game's command line goes to USRDIR/ps3_launch.txt and that program
 * is started: it finds the file, takes the command line and goes straight
 * into the game. Video Settings' Renderer does the same for the game being
 * played (a restart: the renderer can't change under a running engine).
 *
 * Quit from the game's menu comes back here, with a fresh process (the
 * marker USRDIR/ps3_return_to_launcher makes the menu show even with one
 * entry; CIRCLE then goes to the XMB). "Quit Game" from the XMB quits for
 * good.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include <io/pad.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>

#include "ps3_launcher.h"
#include "ps3_platform.h"
#include "ps3_video.h"

#define LAUNCH_FILE   PS3_APP_DIR "/ps3_launch.txt"
#define RETURN_MARKER PS3_APP_DIR "/ps3_return_to_launcher"
#define RENDERER_FILE PS3_APP_DIR "/ps3_renderer.txt"
#define GL_SELF       PS3_APP_DIR "/RELOAD.SELF"
#define SW_SELF       PS3_APP_DIR "/TYRQUAKE_SW.SELF"

volatile int ps3_launcher_active;
volatile int ps3_launcher_quit_requested;
volatile int ps3_quit_to_xmb;

#define LAUNCHER_W   640
#define LAUNCHER_H   400
#define MAX_ENTRIES  48
#define MAX_NAME     48
#define MAX_ARGS     8
#define MAX_ARG_LEN  64
#define VISIBLE_ROWS 9
#define GLYPH_W      8
#define GLYPH_H      8
#define FONT_FIRST   32
#define FONT_LAST    95

#define COL_BG      0xff101018
#define COL_TEXT    0xffc8c8c0
#define COL_DIM     0xff707068
#define COL_SEL_BG  0xff803018
#define COL_SEL_TX  0xffffffff
#define COL_TITLE   0xffd08040

/* 8x8 glyphs for ASCII 32..95 (printable, uppercase only -- lowercase
   is folded to uppercase at draw time), CrispyCell's launcher font */
static const unsigned char launcher_font[FONT_LAST - FONT_FIRST + 1][8] =
{
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // 'space'
    { 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00 },  // '!'
    { 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '"'
    { 0x36, 0x36, 0xfe, 0x36, 0xfe, 0x36, 0x36, 0x00 },  // '#'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '$'
    { 0xe2, 0xd2, 0xe4, 0x08, 0x3b, 0x2b, 0x3b, 0x00 },  // '%'
    { 0x38, 0x6c, 0x38, 0x76, 0xdc, 0xcc, 0x3d, 0x00 },  // '&'
    { 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '''
    { 0x0c, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0c, 0x00 },  // '('
    { 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x18, 0x30, 0x00 },  // ')'
    { 0x00, 0x2a, 0x1c, 0x3e, 0x1c, 0x2a, 0x00, 0x00 },  // '*'
    { 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00 },  // '+'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },  // ','
    { 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00 },  // '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },  // '.'
    { 0x02, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x40, 0x00 },  // '/'
    { 0x3c, 0x66, 0x6e, 0x7e, 0x76, 0x66, 0x3c, 0x00 },  // '0'
    { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // '1'
    { 0x3c, 0x66, 0x06, 0x0c, 0x18, 0x30, 0x7e, 0x00 },  // '2'
    { 0x7e, 0x0c, 0x18, 0x0c, 0x06, 0x66, 0x3c, 0x00 },  // '3'
    { 0x0e, 0x1e, 0x36, 0x66, 0x7f, 0x06, 0x0f, 0x00 },  // '4'
    { 0x7e, 0x60, 0x7c, 0x06, 0x06, 0x66, 0x3c, 0x00 },  // '5'
    { 0x1c, 0x30, 0x60, 0x7c, 0x66, 0x66, 0x3c, 0x00 },  // '6'
    { 0x7e, 0x66, 0x0c, 0x18, 0x18, 0x18, 0x18, 0x00 },  // '7'
    { 0x3c, 0x66, 0x66, 0x3c, 0x66, 0x66, 0x3c, 0x00 },  // '8'
    { 0x3c, 0x66, 0x66, 0x3e, 0x06, 0x0c, 0x38, 0x00 },  // '9'
    { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00 },  // ':'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ';'
    { 0x0e, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0e, 0x00 },  // '<'
    { 0x00, 0x00, 0x7e, 0x00, 0x7e, 0x00, 0x00, 0x00 },  // '='
    { 0x70, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x70, 0x00 },  // '>'
    { 0x3c, 0x66, 0x0c, 0x18, 0x18, 0x00, 0x18, 0x00 },  // '?'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '@'
    { 0x18, 0x3c, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x00 },  // 'A'
    { 0x7c, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x7c, 0x00 },  // 'B'
    { 0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x00 },  // 'C'
    { 0x78, 0x6c, 0x66, 0x66, 0x66, 0x6c, 0x78, 0x00 },  // 'D'
    { 0x7f, 0x63, 0x68, 0x78, 0x68, 0x63, 0x7f, 0x00 },  // 'E'
    { 0x7f, 0x63, 0x68, 0x78, 0x68, 0x60, 0x78, 0x00 },  // 'F'
    { 0x3c, 0x66, 0x60, 0x60, 0x67, 0x66, 0x3c, 0x00 },  // 'G'
    { 0x66, 0x66, 0x66, 0x7e, 0x66, 0x66, 0x66, 0x00 },  // 'H'
    { 0x3c, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // 'I'
    { 0x07, 0x03, 0x03, 0x03, 0x63, 0x63, 0x3e, 0x00 },  // 'J'
    { 0x63, 0x66, 0x6c, 0x78, 0x6c, 0x66, 0x63, 0x00 },  // 'K'
    { 0x78, 0x60, 0x60, 0x60, 0x63, 0x63, 0x7f, 0x00 },  // 'L'
    { 0x63, 0x77, 0x7f, 0x6b, 0x63, 0x63, 0x63, 0x00 },  // 'M'
    { 0x63, 0x73, 0x7b, 0x6f, 0x67, 0x63, 0x63, 0x00 },  // 'N'
    { 0x3e, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00 },  // 'O'
    { 0x7e, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x78, 0x00 },  // 'P'
    { 0x3e, 0x63, 0x63, 0x63, 0x6b, 0x66, 0x3b, 0x00 },  // 'Q'
    { 0x7e, 0x63, 0x63, 0x7e, 0x6c, 0x66, 0x63, 0x00 },  // 'R'
    { 0x3e, 0x63, 0x70, 0x3e, 0x0e, 0x66, 0x3c, 0x00 },  // 'S'
    { 0x7f, 0x5a, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00 },  // 'T'
    { 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3e, 0x00 },  // 'U'
    { 0x63, 0x63, 0x63, 0x63, 0x63, 0x2c, 0x18, 0x00 },  // 'V'
    { 0x63, 0x63, 0x63, 0x6b, 0x7f, 0x77, 0x63, 0x00 },  // 'W'
    { 0x63, 0x63, 0x2c, 0x18, 0x2c, 0x63, 0x63, 0x00 },  // 'X'
    { 0x63, 0x63, 0x63, 0x3e, 0x0c, 0x18, 0x3c, 0x00 },  // 'Y'
    { 0x7f, 0x63, 0x0c, 0x18, 0x30, 0x63, 0x7f, 0x00 },  // 'Z'
    { 0x3c, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3c, 0x00 },  // '['
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '\'
    { 0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c, 0x00 },  // ']'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // '^'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },  // '_'
};

typedef struct {
    char label[MAX_NAME];
    char game[MAX_NAME];        /* -game <folder>; empty for id1 */
    const char *pack;           /* "-hipnotic" / "-rogue" or NULL */
    int is_mod;
} entry_t;

static entry_t entries[MAX_ENTRIES];
static int num_entries;

/* The game's command line: what Host_Init gets, and what a restart in
   the other renderer passes on */
static char arg_buf[MAX_ARGS][MAX_ARG_LEN];
static char *arg_ptrs[MAX_ARGS + 1];
static int arg_count;

static uint32_t *fb;

/*
 * Files
 */

static int
FileExists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int
GameFile(const char *game, const char *file)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%.*s/%.*s", PS3_DATA_DIR,
             MAX_NAME, game, MAX_NAME, file);
    return FileExists(path);
}

/*
 * The command line
 */

static void
ClearArgs(void)
{
    arg_count = 0;
    arg_ptrs[0] = NULL;
}

static void
AddArg(const char *arg)
{
    if (arg_count >= MAX_ARGS)
        return;

    snprintf(arg_buf[arg_count], MAX_ARG_LEN, "%s", arg);
    arg_ptrs[arg_count] = arg_buf[arg_count];
    arg_count++;
    arg_ptrs[arg_count] = NULL;
}

static void
ArgsForEntry(const entry_t *e)
{
    ClearArgs();
    AddArg("TyrQuake");

    /* -hipnotic / -rogue only act together with -game (common.c,
       COM_InitGameDirectoryFromCommandLine) */
    if (e->game[0]) {
        AddArg("-game");
        AddArg(e->game);
    }
    if (e->pack)
        AddArg(e->pack);
}

static int
WriteLaunchFile(void)
{
    FILE *f = fopen(LAUNCH_FILE, "w");
    int i;

    if (!f) {
        PS3_Log("Launcher: cannot write %s", LAUNCH_FILE);
        return -1;
    }

    for (i = 1; i < arg_count; i++)
        fprintf(f, "%s\n", arg_ptrs[i]);
    fclose(f);

    return 0;
}

/* Started by the other program (or a restart): the game is chosen */
static int
ReadLaunchFile(void)
{
    char line[MAX_ARG_LEN];
    FILE *f = fopen(LAUNCH_FILE, "r");

    if (!f)
        return 0;

    ClearArgs();
    AddArg("TyrQuake");

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0])
            AddArg(line);
    }

    fclose(f);
    remove(LAUNCH_FILE);

    return 1;
}

/*
 * The renderer
 */

int
PS3_Renderer_Running(void)
{
#ifdef GLQUAKE
    return 1;
#else
    return 0;
#endif
}

static int renderer_chosen = -1;   /* -1: not read yet */

int
PS3_Renderer_Chosen(void)
{
    char line[32];
    FILE *f;

    if (renderer_chosen >= 0)
        return renderer_chosen;

    renderer_chosen = 1;   /* OpenGL unless the player chose otherwise */

    f = fopen(RENDERER_FILE, "r");
    if (f) {
        if (fgets(line, sizeof(line), f) && !strncasecmp(line, "software", 8))
            renderer_chosen = 0;
        fclose(f);
    }

    return renderer_chosen;
}

void
PS3_Renderer_Choose(int gl)
{
    FILE *f;

    renderer_chosen = gl;

    f = fopen(RENDERER_FILE, "w");

    if (!f) {
        PS3_Log("Launcher: cannot write %s", RENDERER_FILE);
        return;
    }

    fprintf(f, "%s\n", gl ? "opengl" : "software");
    fclose(f);
    PS3_Log("Renderer: %s chosen", gl ? "OpenGL" : "software");
}

static const char *
ProgramFor(int gl)
{
    return gl ? GL_SELF : SW_SELF;
}

/* Replaces this process with the program; returns only if that failed */
static void
Spawn(const char *self)
{
    PS3_Log("Launcher: starting %s", self);
    sysProcessExitSpawn2(self, NULL, NULL, NULL, 0, 1001,
                         SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    PS3_Log("Launcher: sysProcessExitSpawn2 failed");
}

/*
 * Drawing
 */

static void
FillRect(int x, int y, int w, int h, uint32_t colour)
{
    int px, py;

    for (py = y; py < y + h; py++) {
        if (py < 0 || py >= LAUNCHER_H)
            continue;
        for (px = x; px < x + w; px++) {
            if (px >= 0 && px < LAUNCHER_W)
                fb[py * LAUNCHER_W + px] = colour;
        }
    }
}

static void
DrawChar(int x, int y, char c, uint32_t colour, int scale)
{
    const unsigned char *glyph;
    int row, col, sx, sy;

    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';
    if (c < FONT_FIRST || c > FONT_LAST)
        c = '?';

    glyph = launcher_font[c - FONT_FIRST];

    for (row = 0; row < GLYPH_H; row++) {
        for (col = 0; col < GLYPH_W; col++) {
            if (!(glyph[row] & (0x80 >> col)))
                continue;
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;

                    if (px >= 0 && px < LAUNCHER_W && py >= 0 && py < LAUNCHER_H)
                        fb[py * LAUNCHER_W + px] = colour;
                }
            }
        }
    }
}

static void
DrawText(int x, int y, const char *s, uint32_t colour, int scale)
{
    for (; *s; s++, x += GLYPH_W * scale)
        DrawChar(x, y, *s, colour, scale);
}

static int
TextWidth(const char *s, int scale)
{
    return (int)strlen(s) * GLYPH_W * scale;
}

/*
 * What is installed
 */

static entry_t *
NewEntry(const char *label, const char *game, const char *pack, int is_mod)
{
    entry_t *e;

    if (num_entries >= MAX_ENTRIES)
        return NULL;

    e = &entries[num_entries++];
    snprintf(e->label, sizeof(e->label), "%.*s", MAX_NAME - 1, label);
    snprintf(e->game, sizeof(e->game), "%.*s", MAX_NAME - 1, game);
    e->pack = pack;
    e->is_mod = is_mod;

    return e;
}

static char *
Trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';

    return s;
}

/* mods/<folder>/mod.txt is optional: "name = Arcane Dimensions" */
static void
ModLabel(const char *folder, char *label, size_t size)
{
    char path[256], line[128];
    FILE *f;

    snprintf(label, size, "%.*s", MAX_NAME - 1, folder);
    snprintf(path, sizeof(path), "%s/%.*s/mod.txt", PS3_DATA_DIR, MAX_NAME, folder);

    f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');

        if (eq) {
            *eq = '\0';
            if (!strcasecmp(Trim(line), "name") && *Trim(eq + 1)) {
                snprintf(label, size, "%.*s", MAX_NAME - 1, Trim(eq + 1));
                break;
            }
        }
    }

    fclose(f);
}

static int
CompareEntries(const void *a, const void *b)
{
    return strcasecmp(((const entry_t *)a)->label, ((const entry_t *)b)->label);
}

/* A mod is any other folder the engine can run with -game: one with a
   pak0.pak or a progs.dat */
static void
ScanMods(void)
{
    DIR *dir = opendir(PS3_DATA_DIR);
    struct dirent *de;
    int first = num_entries;

    if (!dir)
        return;

    while ((de = readdir(dir)) != NULL) {
        char label[MAX_NAME];
        const char *name = de->d_name;

        if (name[0] == '.' || !strcasecmp(name, "id1") || !strcasecmp(name, "hipnotic")
            || !strcasecmp(name, "rogue") || strlen(name) >= MAX_NAME)
            continue;
        if (!GameFile(name, "pak0.pak") && !GameFile(name, "progs.dat"))
            continue;

        ModLabel(name, label, sizeof(label));
        NewEntry(label, name, NULL, 1);
    }

    closedir(dir);

    qsort(entries + first, num_entries - first, sizeof(entry_t), CompareEntries);
}

static void
ScanGames(void)
{
    num_entries = 0;

    if (!GameFile("id1", "pak0.pak")) {
        PS3_Log("Launcher: no id1/pak0.pak in %s", PS3_DATA_DIR);
        return;
    }

    /* without pak1.pak, the shareware episode */
    NewEntry(GameFile("id1", "pak1.pak") ? "Quake" : "Quake (shareware)", "", NULL, 0);

    if (GameFile("hipnotic", "pak0.pak"))
        NewEntry("Scourge of Armagon", "hipnotic", "-hipnotic", 0);
    if (GameFile("rogue", "pak0.pak"))
        NewEntry("Dissolution of Eternity", "rogue", "-rogue", 0);

    ScanMods();
}

/* The launcher is drawn before the engine reads config.cfg: TV Screen
   Fit is read from it by hand */
static void
LoadScreenFit(void)
{
    char line[128];
    FILE *f = fopen(PS3_DATA_DIR "/id1/config.cfg", "r");
    int fit = 90;

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "vid_ps3_screenfit \"%d\"", &fit) == 1)
                break;
        }
        fclose(f);
    }

    PS3_Video_SetFit(fit < 70 ? 70 : fit > 100 ? 100 : fit);
}

/*
 * Input
 */

#define PAD_LEFT   32768
#define PAD_UP     4096
#define PAD_RIGHT  8192
#define PAD_DOWN   16384
#define PAD_CROSS  64
#define PAD_CIRCLE 32
#define PAD_SQUARE 128
#define PAD_START  2048
#define PAD_L1     4
#define PAD_R1     8

static unsigned
ReadPad(void)
{
    padInfo info;
    padData data;
    int i;

    ioPadGetInfo(&info);

    for (i = 0; i < MAX_PADS; i++) {
        if (info.status[i]) {
            ioPadGetData(i, &data);
            return (data.button[2] << 8) | (data.button[3] & 0xff);
        }
    }

    return 0;
}

/*
 * The menu
 */

#define TAB_GAMES 0
#define TAB_MODS  1

static int tab_items[2][MAX_ENTRIES];   /* indices into entries[] */
static int tab_count[2];

static void
BuildTabs(void)
{
    int i;

    tab_count[TAB_GAMES] = tab_count[TAB_MODS] = 0;

    for (i = 0; i < num_entries; i++) {
        int t = entries[i].is_mod ? TAB_MODS : TAB_GAMES;
        tab_items[t][tab_count[t]++] = i;
    }
}

static void
DrawMenu(int tab, int selected, int top, int gl)
{
    const char *title = "TYRQUAKECELL";
    const int tabs = tab_count[TAB_MODS] > 0;
    const char *hint = tabs ? "D-PAD CHOOSE  L1/R1 GAMES/MODS  CROSS PLAY  CIRCLE EXIT"
                            : "D-PAD TO CHOOSE    CROSS TO PLAY    CIRCLE TO EXIT";
    const int count = tab_count[tab];
    const int row_h = 26;
    char line[64];
    int i;

    fb = PS3_Video_Source();
    if (!fb)
        return;

    FillRect(0, 0, LAUNCHER_W, LAUNCHER_H, COL_BG);
    DrawText((LAUNCHER_W - TextWidth(title, 3)) / 2, 34, title, COL_TITLE, 3);

    if (tabs) {
        static const char *const names[2] = { "GAMES", "MODS" };
        int x = LAUNCHER_W / 2 - TextWidth("GAMES", 2) - 24;
        int t;

        for (t = 0; t < 2; t++) {
            int w = TextWidth(names[t], 2);

            DrawText(x, 70, names[t], t == tab ? COL_TITLE : COL_DIM, 2);
            if (t == tab)
                FillRect(x, 88, w, 2, COL_TITLE);
            x += w + 48;
        }
    }

    for (i = 0; i < VISIBLE_ROWS && top + i < count; i++) {
        const entry_t *e = &entries[tab_items[tab][top + i]];
        int y = 100 + i * row_h;
        uint32_t colour = COL_TEXT;

        if (top + i == selected) {
            FillRect(26, y - 4, LAUNCHER_W - 52, row_h - 4, COL_SEL_BG);
            colour = COL_SEL_TX;
        }

        DrawText(44, y, e->label, colour, 2);

        /* a mod's folder, small and to the right, when its name is not it */
        if (e->is_mod && strcasecmp(e->label, e->game)) {
            int lw = TextWidth(e->label, 2), bw = TextWidth(e->game, 1);

            if (44 + lw + 16 + bw <= LAUNCHER_W - 40)
                DrawText(LAUNCHER_W - 40 - bw, y + 4, e->game,
                         top + i == selected ? COL_SEL_TX : COL_DIM, 1);
        }
    }

    if (count > VISIBLE_ROWS) {
        snprintf(line, sizeof(line), "%d / %d", selected + 1, count);
        DrawText(LAUNCHER_W - TextWidth(line, 1) - 24, LAUNCHER_H - 26, line, COL_DIM, 1);
    }

    snprintf(line, sizeof(line), "RENDERER: %s   (SQUARE TO CHANGE)", gl ? "OPENGL" : "SOFTWARE");
    DrawText(24, LAUNCHER_H - 44, line, COL_DIM, 1);
    DrawText(24, LAUNCHER_H - 26, hint, COL_DIM, 1);

    PS3_Video_Present();
}

/* An index into entries[], or -1 to leave for the XMB (CIRCLE, or "Quit
   Game" from the XMB while the menu is up) */
static int
RunMenu(void)
{
    int tab = tab_count[TAB_GAMES] > 0 ? TAB_GAMES : TAB_MODS;
    int selected[2] = { 0, 0 }, top[2] = { 0, 0 };
    int gl = PS3_Renderer_Chosen();
    /* as if everything were held: the button that started the game from
       the XMB must not pick an entry by itself */
    unsigned last = ~0u;

    for (;;) {
        unsigned now = ReadPad();
        unsigned pressed = now & ~last;
        int count;

        last = now;

        sysUtilCheckCallback();   /* PS button, "Quit Game" from the XMB */

        if (ps3_launcher_quit_requested || (pressed & PAD_CIRCLE))
            return -1;

        if ((pressed & (PAD_L1 | PAD_R1 | PAD_LEFT | PAD_RIGHT))
            && tab_count[TAB_GAMES] > 0 && tab_count[TAB_MODS] > 0)
            tab = (pressed & (PAD_L1 | PAD_LEFT)) ? TAB_GAMES : TAB_MODS;

        if (pressed & PAD_SQUARE) {
            gl = !gl;
            PS3_Renderer_Choose(gl);
        }

        count = tab_count[tab];

        if (pressed & PAD_UP)
            selected[tab] = (selected[tab] + count - 1) % count;
        if (pressed & PAD_DOWN)
            selected[tab] = (selected[tab] + 1) % count;
        if (pressed & (PAD_CROSS | PAD_START))
            return tab_items[tab][selected[tab]];

        if (selected[tab] < top[tab])
            top[tab] = selected[tab];
        else if (selected[tab] >= top[tab] + VISIBLE_ROWS)
            top[tab] = selected[tab] - VISIBLE_ROWS + 1;

        DrawMenu(tab, selected[tab], top[tab], gl);
    }
}

/*
 * Entry points
 */

void
PS3_Launcher_Run(int *argc, char ***argv)
{
    int returning, video = 0, chosen = 0, gl;

    /* started by the other program, or a renderer restart */
    if (ReadLaunchFile()) {
        PS3_Log("Launcher: game chosen before this program started");
        goto done;
    }

    returning = FileExists(RETURN_MARKER);
    if (returning) {
        remove(RETURN_MARKER);
        PS3_Log("Launcher: back from Quit");
    }

    ScanGames();
    BuildTabs();

    if (num_entries == 0) {
        /* the engine's own error says what is missing */
        ClearArgs();
        AddArg("TyrQuake");
        goto done;
    }

    if (num_entries > 1 || returning) {
        if (PS3_Video_Init() != 0 || PS3_Video_SetSource(LAUNCHER_W, LAUNCHER_H) != 0) {
            PS3_Log("Launcher: no video, playing the first entry");
        } else {
            video = 1;
            LoadScreenFit();
            ioPadInit(MAX_PADS);

            ps3_launcher_active = 1;
            chosen = RunMenu();
            ps3_launcher_active = 0;

            if (chosen < 0) {
                /* only the RSX is up: let go of it, or the console goes
                   down with us */
                PS3_Log("Launcher: exit to the XMB");
                PS3_Video_Shutdown();
                exit(0);
            }
        }
    }

    ArgsForEntry(&entries[chosen]);
    PS3_Log("Launcher: '%s'", entries[chosen].label);

    /* the other renderer: its program takes it from here */
    gl = PS3_Renderer_Chosen();
    if (gl != PS3_Renderer_Running()) {
        const char *self = ProgramFor(gl);

        if (!FileExists(self)) {
            PS3_Log("Launcher: %s missing, staying in this renderer", self);
        } else if (WriteLaunchFile() == 0) {
            if (video)
                PS3_Video_Shutdown();
            Spawn(self);
            remove(LAUNCH_FILE);
            if (video && PS3_Video_Init() != 0)
                PS3_Log("Launcher: video did not come back up");
        }
    }

done:
    {
        int i;

        for (i = 1; i < arg_count; i++)
            PS3_Log("Launcher:   %s", arg_ptrs[i]);
    }

    *argc = arg_count;
    *argv = arg_ptrs;
}

void
PS3_Renderer_Restart(void)
{
    extern void Host_Shutdown(void);
    const int gl = PS3_Renderer_Chosen();
    const char *self = ProgramFor(gl);

    if (gl == PS3_Renderer_Running())
        return;

    if (!FileExists(self) || arg_count == 0) {
        PS3_Log("Renderer: %s missing, no restart", self);
        return;
    }

    if (WriteLaunchFile() != 0)
        return;

    PS3_Log("Renderer: restarting in %s", gl ? "OpenGL" : "software");
    Host_Shutdown();   /* config.cfg, audio, RSX */
    Spawn(self);

    /* nothing left to come back to */
    remove(LAUNCH_FILE);
    exit(0);
}

void
PS3_Launcher_Relaunch(void)
{
    const char *self = ProgramFor(PS3_Renderer_Running());
    FILE *marker;

    if (ps3_quit_to_xmb)
        return;

    if (!FileExists(self)) {
        PS3_Log("Launcher: %s missing, exiting instead", self);
        return;
    }

    marker = fopen(RETURN_MARKER, "w");
    if (!marker) {
        PS3_Log("Launcher: cannot write %s, exiting instead", RETURN_MARKER);
        return;
    }
    fclose(marker);

    PS3_Log("Launcher: Quit, back to the launcher");
    Spawn(self);
    remove(RETURN_MARKER);
}
