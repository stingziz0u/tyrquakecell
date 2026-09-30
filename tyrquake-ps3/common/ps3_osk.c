// ps3_osk.c -- the console with the pad: SELECT + TRIANGLE and the XMB
// on-screen keyboard (OSK).
//
// Quake's console only ever had a keyboard. With the pad:
//   SELECT + TRIANGLE  opens / closes the console (anywhere)
//   CROSS              opens the XMB keyboard with the current line; what
//                      is accepted there runs as a command
//   TRIANGLE           runs the current line as it is
//   SQUARE             command completion (Tab)
//   CIRCLE             closes the console
//   D-pad up / down    command history, left: erase a character
//   L1 / R1            scroll the console up / down
//   START              the menu, as always
//
// SELECT is a modifier now, the same as in ioRTWC-PS3: on its own it goes
// to the game on release (a tap) or after being held for SELECT_HOLD_MS
// (so +showscores still works while held), which means SELECT + TRIANGLE
// never triggers either button's binding.
//
// The OSK recipe is the one from CrispyCell / ioRTWC-PS3, proven on the
// console (itself from Apollo Save Tool, bucanero/apollo-ps3): a memory
// container, a sysutil callback on slot 1 (slot 0 is the game's exit
// callback, sys_ps3.c), oskLoadAsync, and a state machine driven by that
// callback -- sysUtilCheckCallback() already runs every frame in
// Sys_SendKeyEvents() -- while the game keeps drawing underneath. While
// the OSK is up the pad is not read for the game, and the buttons still
// held when it closes (the X that accepted) are ignored until released.

#include <string.h>

#include <ppu-types.h>
#include <sys/memory.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>

#include "quakedef.h"
#include "common.h"
#include "console.h"
#include "keys.h"
#include "sys.h"

#include "ps3_osk.h"

extern void PS3_Log(const char *fmt, ...);

// Same bit layout as in_ps3.c (paddata.button[2..3] as one mask)
#define PAD_LEFT     32768
#define PAD_DOWN     16384
#define PAD_RIGHT    8192
#define PAD_UP       4096
#define PAD_START    2048
#define PAD_SELECT   256
#define PAD_SQUARE   128
#define PAD_CROSS    64
#define PAD_CIRCLE   32
#define PAD_TRIANGLE 16
#define PAD_R1       8
#define PAD_L1       4

#define SELECT_HOLD_MS 250

// ---------------------------------------------------------------------
// The XMB keyboard
// ---------------------------------------------------------------------

#define OSK_MAX_CHARS       (MAXCMDLINE - 2)    // ']' and the terminator
#define OSK_CONTAINER_BIG   (8 * 1024 * 1024)   // CrispyCell, proven
#define OSK_CONTAINER_SMALL (4 * 1024 * 1024)   // "typical" per osk.h

enum { OSK_IDLE, OSK_LOADING, OSK_OPEN, OSK_UNLOADING };

static int osk_state = OSK_IDLE;
static int osk_accepted;
static qboolean osk_just_closed;
static qboolean osk_result_ready;
static char osk_result[OSK_MAX_CHARS + 1];
static sys_mem_container_t osk_container;
static u16 osk_title[32];
static u16 osk_start[OSK_MAX_CHARS + 1];
static u16 osk_text[OSK_MAX_CHARS + 1];
static oskCallbackReturnParam osk_ret;

qboolean
PS3_OSK_Active(void)
{
    return osk_state != OSK_IDLE;
}

static void
osk_ascii_to_utf16(const char *in, u16 *out, int max)
{
    int i;

    for (i = 0; in[i] && i < max; i++)
	out[i] = (u16)(unsigned char)in[i];
    out[i] = 0;
}

// The full-width panel writes U+FF01..U+FF5E; U+3000 is its space.
// Unlike the save names of the other ports, quotes and semicolons are
// kept: the console needs them (bind x "a; b").
static void
osk_utf16_to_ascii(const u16 *in, int len, char *out, int size)
{
    int i, n = 0;

    for (i = 0; i < len && in[i] && n < size - 1; i++) {
	u16 c = in[i];

	if (c >= 0xFF01 && c <= 0xFF5E)
	    c -= 0xFEE0;
	else if (c == 0x3000)
	    c = ' ';
	if (c >= 32 && c < 127)
	    out[n++] = (char)c;
    }
    out[n] = 0;
}

static void
osk_finish(void)
{
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    if (osk_container) {
	sysMemContainerDestroy(osk_container);
	osk_container = 0;
    }
    osk_state = OSK_IDLE;
    osk_just_closed = true;
}

static void
osk_callback(u64 status, u64 param, void *userdata)
{
    (void)param;
    (void)userdata;

    switch (status) {
    case SYSUTIL_OSK_LOADED:
	osk_state = OSK_OPEN;
	break;
    case SYSUTIL_OSK_INPUT_ENTERED:
	osk_accepted = 1;
	oskGetInputText(&osk_ret);
	break;
    case SYSUTIL_OSK_INPUT_CANCELED:
	osk_accepted = 0;
	oskAbort();
	osk_state = OSK_UNLOADING;
	oskUnloadAsync(&osk_ret);
	break;
    case SYSUTIL_OSK_DONE:
	if (osk_state != OSK_UNLOADING) {
	    osk_state = OSK_UNLOADING;
	    oskUnloadAsync(&osk_ret);
	}
	break;
    case SYSUTIL_OSK_UNLOADED:
	if (osk_accepted || osk_ret.res == OSK_OK || osk_ret.res == OSK_NO_TEXT) {
	    osk_utf16_to_ascii(osk_text, OSK_MAX_CHARS, osk_result, sizeof(osk_result));
	    osk_result_ready = true;
	    PS3_Log("[osk] accepted \"%s\"", osk_result);
	} else {
	    PS3_Log("[osk] canceled");
	}
	osk_finish();
	break;
    default:
	break;
    }
}

// Opens the keyboard with the console's current line. Returns false if it
// could not open (no memory for the dialog): the console stays as it was.
static qboolean
osk_open(void)
{
    oskParam param;
    oskInputFieldInfo info;
    char line[OSK_MAX_CHARS + 1];
    int line_len;

    if (osk_state != OSK_IDLE)
	return true;

    if (sysMemContainerCreate(&osk_container, OSK_CONTAINER_BIG) != 0 &&
	sysMemContainerCreate(&osk_container, OSK_CONTAINER_SMALL) != 0) {
	osk_container = 0;
	PS3_Log("[osk] no memory for the keyboard");
	return false;
    }

    osk_ascii_to_utf16("Console", osk_title, 31);
    // key_lines[edit_line] starts with the ']' prompt, and the line ends at
    // the cursor (backspace only moves it back)
    line_len = key_linepos - 1;
    if (line_len < 0)
	line_len = 0;
    if (line_len > OSK_MAX_CHARS)
	line_len = OSK_MAX_CHARS;
    memcpy(line, key_lines[edit_line] + 1, line_len);
    line[line_len] = 0;
    osk_ascii_to_utf16(line, osk_start, OSK_MAX_CHARS);
    memset(osk_text, 0, sizeof(osk_text));
    memset(&osk_ret, 0, sizeof(osk_ret));
    osk_ret.len = OSK_MAX_CHARS;
    osk_ret.str = osk_text;
    osk_accepted = 0;
    osk_result_ready = false;

    memset(&param, 0, sizeof(param));
    param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH;
    param.allowedPanels = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL |
			  OSK_PANEL_TYPE_ENGLISH | OSK_PANEL_TYPE_URL;
    param.prohibitFlags = OSK_PROHIBIT_RETURN;
    param.controlPoint.x = 0;
    param.controlPoint.y = 0;

    memset(&info, 0, sizeof(info));
    info.message = osk_title;
    info.startText = osk_start;
    info.maxLength = OSK_MAX_CHARS;

    oskSetKeyLayoutOption(OSK_10KEY_PANEL | OSK_FULLKEY_PANEL);
    oskAddSupportLanguage(OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_ENGLISH);
    oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER |
		     OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);
    oskSetInitialInputDevice(OSK_DEVICE_PAD);

    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT1, osk_callback, NULL);
    if (oskLoadAsync(osk_container, &param, &info) != 0) {
	PS3_Log("[osk] oskLoadAsync failed");
	osk_finish();
	return false;
    }
    osk_state = OSK_LOADING;
    PS3_Log("[osk] open for the console");
    return true;
}

// Leaving the game with the keyboard still up (Quit Game from the XMB):
// nothing of ours may stay registered with the system (3.64).
void
PS3_OSK_Shutdown(void)
{
    if (osk_state == OSK_IDLE)
	return;
    oskAbort();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT1);
    osk_state = OSK_IDLE;
}

// ---------------------------------------------------------------------
// The pad while the console is open
// ---------------------------------------------------------------------

static void
console_key(knum_t key)
{
    Key_Event(key, true);
    Key_Event(key, false);
}

// The accepted text becomes the console line and runs, the same as typing
// it and pressing Enter.
static void
console_run_osk_result(void)
{
    osk_result_ready = false;
    if (key_dest != key_console)
	return;

    key_lines[edit_line][0] = ']';
    qstrncpy(key_lines[edit_line] + 1, osk_result, MAXCMDLINE - 1);
    key_linepos = strlen(key_lines[edit_line]);
    if (osk_result[0])
	console_key(K_ENTER);
}

unsigned
PS3_ConsolePad_Filter(unsigned raw)
{
    // select_state: SELECT not held / held, not given to the game yet /
    // held and given to the game / used for SELECT + TRIANGLE
    enum { SEL_UP, SEL_PENDING, SEL_SENT, SEL_COMBO };
    static unsigned prev, swallow;
    static int select_state = SEL_UP;
    static double select_at;
    unsigned pressed, out;
    qboolean select_out = false;

    pressed = raw & ~prev;
    prev = raw;
    swallow &= raw;		// released buttons count again

    // The keyboard has the pad; what is still held when it closes is not
    // a new press.
    if (osk_state != OSK_IDLE || osk_just_closed) {
	osk_just_closed = false;
	swallow |= raw;
	select_state = (raw & PAD_SELECT) ? SEL_COMBO : SEL_UP;
	return 0;
    }
    if (osk_result_ready)
	console_run_osk_result();

    // SELECT + TRIANGLE: the console
    if (raw & PAD_SELECT) {
	if (pressed & PAD_SELECT) {
	    select_state = SEL_PENDING;
	    select_at = Sys_DoubleTime();
	}
	if ((pressed & PAD_TRIANGLE) && select_state != SEL_UP) {
	    Con_ToggleConsole_f();
	    swallow |= PAD_TRIANGLE;
	    if (select_state == SEL_PENDING)
		select_state = SEL_COMBO;
	    pressed &= ~PAD_TRIANGLE;
	}
	if (select_state == SEL_PENDING
	    && Sys_DoubleTime() - select_at >= SELECT_HOLD_MS / 1000.0)
	    select_state = SEL_SENT;
	select_out = (select_state == SEL_SENT);
    } else {
	// a tap: pressed now, released on the next frame
	select_out = (select_state == SEL_PENDING);
	select_state = SEL_UP;
    }

    if (key_dest == key_console) {
	pressed &= ~swallow;
	if (pressed & PAD_CROSS) {
	    if (!osk_open())
		console_key(K_ENTER);
	}
	if (pressed & PAD_TRIANGLE)
	    console_key(K_ENTER);
	if (pressed & PAD_SQUARE)
	    console_key(K_TAB);
	if (pressed & PAD_CIRCLE)
	    Con_ToggleConsole_f();
	if (pressed & PAD_UP)
	    console_key(K_UPARROW);
	if (pressed & PAD_DOWN)
	    console_key(K_DOWNARROW);
	if (pressed & PAD_LEFT)
	    console_key(K_BACKSPACE);
	if (pressed & PAD_L1)
	    console_key(K_PGUP);
	if (pressed & PAD_R1)
	    console_key(K_PGDN);

	// Nothing else reaches the game while the console is open (its
	// bindings would run: Key_Event sends non-console keys to the
	// command interpreter even here), and nothing held now counts as a
	// press when it closes. START still opens the menu.
	swallow |= raw & ~PAD_START;
	return raw & PAD_START;
    }

    out = raw & ~swallow & ~PAD_SELECT;
    if (select_out)
	out |= PAD_SELECT;
    return out;
}
