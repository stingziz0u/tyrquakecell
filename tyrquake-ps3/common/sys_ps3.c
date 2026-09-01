/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
// sys_ps3.c -- PS3 system driver, based on sys_null.c

#include "quakedef.h"
#include "input.h"
#include "keys.h"
#include "common.h"
#include "console.h"
#include "host.h"
#include "errno.h"
#include <unistd.h> /* usleep */

/* PSL1GHT headers. sysGetSystemTime() from <lv2/systime.h> is confirmed
 * against a working PS3 homebrew port (dragonfly-quake-ps3's Sys_FloatTime
 * uses exactly this call). Plain exit() is used for process termination,
 * also matching that reference -- no sysProcessExit() needed. */
#include <lv2/systime.h>

/* Reserve a real PRX heap size for Quake's zone allocator.
 * PS3 has plenty of RAM (256MB system) compared to the 8MB sys_null used;
 * start conservative and raise once we know the actual game's needs. */
// Quake's own "hunk" memory pool -- everything the game loads
// dynamically (level data, models, sounds, etc.) comes out of this.
// Was 32MB (comfortable for the shareware episode, which is all we'd
// tested against at the time), bumped to 128MB as a preventive margin
// now that the full game (pak0+pak1, ~60MB on disk) is in play --
// Episode 4 in particular has noticeably heavier levels than anything
// tested so far. Plenty of room even at 128MB: the PS3 has 256MB of
// system RAM total, and this is a plain malloc() from general memory,
// not some constrained hardware-mapped region.
#define PS3_QUAKE_MEMSIZE (128 * 1024 * 1024)

/* Every platform's sys_*.c defines this -- false since we're not a
 * dedicated server (see sys_unix.c for the pattern). */
qboolean isDedicated;

/* Called from Host_Init. sys_unix.c only registers cvars here under
 * #ifdef SERVERONLY, which we don't build, so nothing to do. */
void
Sys_RegisterVariables(void)
{
}

/* Normally lives in net_dgrm.c (real UDP networking), which we exclude --
 * see the Makefile comment next to NQ_SRCS. It's wired into sv_user.c's
 * console command table ("ban"), so it needs *a* definition to link, even
 * though banning by IP is meaningless without real network clients. */
void
NET_Ban_f(client_t *client)
{
    Con_Printf("Banning is not supported on this platform.\n");
}

/*
===============================================================================

FILE IO

===============================================================================
*/

int
Sys_FileTime(char *path)
{
    FILE *f;

    f = fopen(path, "rb");
    if (f) {
	fclose(f);
	return 1;
    }

    return -1;
}

void
Sys_mkdir(char *path)
{
    /* TODO: cellFs / sysFs mkdir call once we need to write configs/saves.
     * Not needed for the first bring-up milestone (read-only pak access). */
}

/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void
Sys_MakeCodeWriteable(void *start_addr, void *end_addr)
{
    /* No-op: PPU homebrew doesn't need runtime code-page protection changes
     * the way old x86 self-modifying-code tricks did. */
}

void
Sys_MakeCodeUnwriteable(void *start_addr, void *end_addr)
{
}

/* Writes to a log file on dev_hdd0 so we have visibility into crashes on
 * real hardware, where printf output goes nowhere visible. Opens/closes
 * and flushes on every call (deliberately not buffered/kept open) so that
 * if the game crashes hard right after a call, the line we just wrote is
 * still on disk -- that's the whole point of this function existing. */
#define PS3_LOG_PATH "/dev_hdd0/game/TYRQ00001/USRDIR/tyrquake_log.txt"

void
PS3_Log(const char *fmt, ...)
{
    va_list argptr;
    FILE *f = fopen(PS3_LOG_PATH, "a");
    if (!f)
	return;
    va_start(argptr, fmt);
    vfprintf(f, fmt, argptr);
    va_end(argptr);
    fprintf(f, "\n");
    fclose(f);
}

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    FILE *f = fopen(PS3_LOG_PATH, "a");
    if (!f)
	return;
    va_start(argptr, fmt);
    vfprintf(f, fmt, argptr);
    va_end(argptr);
    fclose(f);
}

void
Sys_Error(const char *error, ...)
{
    va_list argptr;

    PS3_Log("=== Sys_Error ===");
    {
	FILE *f = fopen(PS3_LOG_PATH, "a");
	if (f) {
	    va_start(argptr, error);
	    vfprintf(f, error, argptr);
	    va_end(argptr);
	    fprintf(f, "\n");
	    fclose(f);
	}
    }

    printf("Sys_Error: ");
    va_start(argptr, error);
    vprintf(error, argptr);
    va_end(argptr);
    printf("\n");

    /* TODO: once vid_ps3.c can be reached safely from an error path,
     * consider flashing an error color to screen too, since printf output
     * alone is invisible on a real console with no debug TTY attached. */

    exit(1);
}

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;

    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
}

void
Sys_Quit(void)
{
    exit(0);
}

double
Sys_DoubleTime(void)
{
    /* sysGetSystemTime() returns microseconds since some epoch as a
     * uint64_t -- confirmed against dragonfly-quake-ps3's Sys_FloatTime. */
    static int first = 1;
    static uint64_t start;
    uint64_t now;

    now = sysGetSystemTime();

    if (first) {
	start = now;
	first = 0;
    }

    return (double)(now - start) / 1000000.0;
}

char *
Sys_ConsoleInput(void)
{
    return NULL;
}

void
Sys_Sleep(void)
{
    usleep(1000);
}

#include <sysutil/sysutil.h>

void
Sys_SendKeyEvents(void)
{
    /* Pumps the sysutil callback queue -- this is what actually fires
     * PS3_SysutilCallback() below (registered in PS3_QuakeThread) when
     * the PS button is pressed or "Quit Game" is picked from the XMB.
     * Without pumping this every frame, that event is never delivered
     * and the game (and RSX context) never gets a chance to shut down
     * cleanly -- which is what was causing the console to hard-reboot
     * on exit before this fix. */
    sysUtilCheckCallback();

    /* Also pump our own gamepad/keyboard/mouse polling here, but ONLY
     * during SCR_ModalMessage's blocking Y/N wait (screen.c) -- gated by
     * key_count <= 0, which that function sets right before its loop
     * starts (see keys.h's own comment: "just catching keys for
     * Con_NotifyBox"). Normal per-frame play already calls IN_Commands()
     * separately, right after Sys_SendKeyEvents() (see NQ/host.c) -- so
     * calling it here unconditionally would just double the work every
     * single frame for no benefit. But SCR_ModalMessage calls ONLY
     * Sys_SendKeyEvents() + Sys_Sleep() in its own tight loop, completely
     * outside that normal frame flow -- without this, nothing ever pumps
     * the controller during it, so a modal confirmation (e.g. "Are you
     * sure you want to start a new game?") looked and behaved exactly
     * like a hard freeze: the game was still running, just permanently
     * deaf to the controller until forced off. */
    if (key_count <= 0)
	IN_Commands();
}

void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}

//=============================================================================

// The PS3 OS gives the default EBOOT main thread a small fixed stack
// (~128 KB), but several Quake renderer functions allocate large arrays
// directly on the stack (well over 100 KB in some cases) -- they'd
// silently blow that stack and crash before doing anything visible.
// This exact problem, with this exact fix, is documented by
// dragonfly-quake-ps3 (see src/main.c in that project): spawn a worker
// PPU thread with a real stack size and run the whole game there instead.
#include <sys/thread.h>

#define PS3_GAME_STACK (2 * 1024 * 1024)
#define PS3_GAME_PRIO  1000

static int    g_argc;
static char **g_argv;

/* VID_Shutdown() is called by Host_Shutdown() itself (see NQ/host.c) --
 * this extern is no longer needed for the exit path below, but other
 * code in this file may still reference it, so leave the declaration. */
extern void VID_Shutdown(void);

/* Fires when the PS button opens the XMB overlay, or when the user picks
 * "Quit Game" from it. Registered via sysUtilRegisterCallback below, but
 * only actually delivered when something calls sysUtilCheckCallback() --
 * see Sys_SendKeyEvents() above, called once per frame from the main
 * loop. */
static void
PS3_SysutilCallback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;
    switch (status) {
    case SYSUTIL_EXIT_GAME:
	PS3_Log("PS3_SysutilCallback: SYSUTIL_EXIT_GAME -- shutting down cleanly");
	// Host_Shutdown() (NQ/host.c) does everything a clean exit needs --
	// writes config.cfg (every CVAR_CONFIG cvar: sensitivity,
	// joy_sensitivity, sound/music volume, key bindings, etc.), shuts
	// down audio/input/network, and calls VID_Shutdown() itself at the
	// end (releasing RSX before exit(), which is what actually fixes
	// the console hard-rebooting on exit -- an abrupt process kill
	// while RSX still holds the display buffers crashes the whole
	// system, not just our game).
	//
	// We used to call VID_Shutdown() directly and skip straight to
	// exit() -- which handled the RSX/hard-reboot half fine, but
	// silently skipped everything else Host_Shutdown does, config.cfg
	// included: any change made in Options this session was applied in
	// memory but never written to disk, so the next launch loaded the
	// previous run's config.cfg (or the compiled-in defaults) instead.
	Host_Shutdown();
	exit(0);
	break;
    default:
	// Log every other sysutil event too (menu opened via PS button,
	// menu closed, etc.) -- previously only SYSUTIL_EXIT_GAME was
	// logged, so there was no way to tell from a log alone whether a
	// given stutter actually coincided with the PS button being
	// pressed, or just happened to land near it in time.
	PS3_Log("PS3_SysutilCallback: status=0x%llx (not EXIT_GAME)", (unsigned long long)status);
	break;
    }
}

static void
PS3_QuakeThread(void *arg)
{
    quakeparms_t parms;
    double oldtime, newtime, dt;

    remove(PS3_LOG_PATH); /* fresh log each run */
    PS3_Log("=== TyrQuake PS3 boot (worker thread) ===");

    if (sysUtilRegisterCallback(0, PS3_SysutilCallback, NULL) != 0)
	PS3_Log("WARNING: sysUtilRegisterCallback failed -- PS button exit won't be clean");
    else
	PS3_Log("sysUtilRegisterCallback OK");

    parms.memsize = PS3_QUAKE_MEMSIZE;
    parms.membase = malloc(parms.memsize);
    if (!parms.membase) {
	PS3_Log("FATAL: failed to allocate %d bytes for Quake heap",
		parms.memsize);
	exit(1);
    }
    PS3_Log("Heap allocated OK: %d bytes", parms.memsize);

    /* Matches APPID=TYRQ00001 from the Makefile (9 chars, correct PS3
     * TITLE_ID format). This is where the PS3 installs the .pkg's USRDIR
     * contents. Quake itself then looks for pak0.pak inside a "id1"
     * subfolder of this path (standard Quake convention), i.e.
     * .../USRDIR/id1/pak0.pak */
    parms.basedir = "/dev_hdd0/game/TYRQ00001/USRDIR";
    PS3_Log("basedir = %s", parms.basedir);

#ifdef TYRQUAKE_MISSIONPACK_ARG
    // Mission pack variants (Hipnotic/Rogue) are separate .pkg builds
    // from the same source -- see Makefile's MISSIONPACK variable. This
    // synthesizes the equivalent of running "quake -game hipnotic
    // -hipnotic" from a command line -- BOTH arguments are required
    // together (see the Makefile's comment on this, from actually
    // reading COM_InitGameDirectoryFromCommandLine in common.c: passing
    // only "-hipnotic" alone silently does nothing at all, since that
    // function bails out before ever loading the hipnotic/ directory or
    // setting the QuakeC-visible "hipnotic" flag unless "-game <dir>"
    // is also present). There's no menu option for this, only launch
    // arguments, and the PS3 never gives us real argv, so we build our
    // own.
    static char arg0[] = "TyrQuake";
    static char arg1[] = "-game";
    static char arg2[] = TYRQUAKE_MISSIONPACK_NAME;
    static char arg3[] = TYRQUAKE_MISSIONPACK_ARG;
    static char *mp_argv[] = { arg0, arg1, arg2, arg3 };
    g_argc = 4;
    g_argv = mp_argv;
    PS3_Log("Mission pack build: injecting argv = { %s %s %s %s }", arg0, arg1, arg2, arg3);
#endif

    COM_InitArgv(g_argc, (const char **)g_argv);
    PS3_Log("COM_InitArgv OK");

    parms.argc = com_argc;
    parms.argv = com_argv;

    PS3_Log("Calling Host_Init...");
    Host_Init(&parms, NULL);
    PS3_Log("Host_Init returned OK -- entering main loop");

    oldtime = Sys_DoubleTime();
    while (1) {
	newtime = Sys_DoubleTime();
	dt = newtime - oldtime;
	oldtime = newtime;
	Host_Frame(dt);
    }
}

int
main(int argc, char **argv)
{
    sys_ppu_thread_t thread_id;
    u64 exit_code = 0;
    s32 rc;

    /* Absolute earliest possible checkpoint -- if this line doesn't show
     * up in the log, the crash is happening before main() even runs
     * (process/ELF loading), not in anything we control here. */
    PS3_Log("=== main() entered ===");

    g_argc = argc;
    g_argv = argv;

    rc = sysThreadCreate(&thread_id, PS3_QuakeThread, NULL,
			  PS3_GAME_PRIO, PS3_GAME_STACK,
			  THREAD_JOINABLE, (char *)"quake_game");
    if (rc != 0) {
	/* Can't use PS3_Log here in principle it would still work (fopen
	 * doesn't need the big stack), but keep this path minimal since
	 * if thread creation itself fails, something is fundamentally
	 * wrong and we want the simplest possible code here. */
	PS3_Log("FATAL: sysThreadCreate failed: %d", (int)rc);
	return 1;
    }

    sysThreadJoin(thread_id, &exit_code);
    return (int)exit_code;
}
