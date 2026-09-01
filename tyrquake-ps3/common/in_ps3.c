// in_ps3.c -- PS3 input driver.
//
// The ioPad API usage (ioPadInit, ioPadGetInfo/.status[], ioPadGetData,
// paddata.button[] index layout) is copied from a confirmed-working
// reference (GamePad Tester), not guessed -- see button[] index comments
// below for what each one means.

#include "quakedef.h"
#include "keys.h"
#include "menu.h"

#ifdef NQ_HACK
#include "client.h"
#include "host.h"
#include "server.h"
#endif
#ifdef QW_HACK
#include "protocol.h"
#endif

#include <io/pad.h>
#include <io/kb.h>
#include <io/mouse.h>

cvar_t _windowed_mouse = { "_windowed_mouse", "0", CVAR_CONFIG };
// m_filter is declared locally by every platform driver (not a shared
// engine cvar) -- following that same pattern.
static cvar_t m_filter = { "m_filter", "0" };
static float old_mouse_dx, old_mouse_dy; // for m_filter smoothing

// Joystick look/turn sensitivity -- our own addition (no other TyrQuake
// platform has native joystick support to share a cvar with). 1.0 =
// PS3_TURN_SPEED/PS3_PITCH_SPEED unchanged; adjustable from the Options
// menu ("Joystick Sensitivity" -- see menu.c).
cvar_t joy_sensitivity = { "joy_sensitivity", "1.0", CVAR_CONFIG };

// io/pad.h already defines MAX_PADS (127) -- no need to redefine it
// ourselves (that was causing a harmless but noisy redefinition warning).

// paddata.button[] index layout (confirmed via GamePad Tester):
//   [2],[3]  -- digital buttons, 16-bit bitmask split across two bytes
//   [4],[5]  -- right stick X, Y  (0-255, 128 = centered)
//   [6],[7]  -- left stick X, Y   (0-255, 128 = centered)
#define BUTTON_LEFT       32768
#define BUTTON_DOWN       16384
#define BUTTON_RIGHT      8192
#define BUTTON_UP         4096
#define BUTTON_START      2048
#define BUTTON_R3         1024
#define BUTTON_L3         512
#define BUTTON_SELECT     256
#define BUTTON_SQUARE     128
#define BUTTON_CROSS      64
#define BUTTON_CIRCLE     32
#define BUTTON_TRIANGLE   16
#define BUTTON_R1         8
#define BUTTON_L1         4
#define BUTTON_R2         2
#define BUTTON_L2         1

// PS3 pad buttons -> Quake's own bindable key system (K_AUX1..K_AUX20,
// reserved in keys.h for exactly this purpose -- auxiliary/joystick
// buttons). Feeding these through Key_Event instead of hardcoding
// Cbuf_AddText calls means the existing "Customize Controls" menu can
// rebind them like any keyboard key -- no custom rebind UI needed.
// D-pad and START stay hardcoded (arrows for menu nav, START always
// opens the menu) since those need to always work regardless of what
// the player has bound.
#define PAD_KEY_CROSS    K_AUX1
#define PAD_KEY_CIRCLE   K_AUX2
#define PAD_KEY_SQUARE   K_AUX3
#define PAD_KEY_TRIANGLE K_AUX4
#define PAD_KEY_L1       K_AUX5
#define PAD_KEY_R1       K_AUX6
#define PAD_KEY_L2       K_AUX7
#define PAD_KEY_R2       K_AUX8
#define PAD_KEY_L3       K_AUX10
#define PAD_KEY_R3       K_AUX11
#define PAD_KEY_SELECT   K_AUX12

// Analog deadzone -- sticks rarely rest at exactly 128.
#define STICK_DEADZONE 24

#ifdef TYRQUAKE_DEBUG_BUILD
// L1+R1 held together triggers "next level" (see IN_Commands) -- a
// dedicated single button would've been nicer, but every face/shoulder
// button already has a default action (see PS3_SetDefaultBindings
// below), so this uses a two-button chord instead of taking one over.
// L1+R1 specifically because they're weapon prev/next -- not something
// a player normally presses at the exact same instant, unlike e.g.
// L2+R2 (speed+attack), which legitimately do get held together while
// running and shooting.
//
// This calls "changelevel <mapname>" directly rather than trying to
// simulate walking onto the level's actual exit trigger -- the trigger
// entity that does that is QuakeC (progs.dat, game data, not our engine
// source), and for the two "boss" levels (E1M7's Chthon, END's
// Shub-Niggurath) reaching it isn't a matter of finding a walkable
// trigger at all -- it's the reward for solving a specific in-level
// puzzle. changelevel sidesteps needing to solve (or even implement)
// that puzzle entirely, which is exactly what a "skip this level"
// button should do.
//
// Table follows the verified, official Episode structure (each map's
// single MAIN-path exit -- not secret-level branches, since skipping
// straight past secret content is the intended behavior of a "next
// level" button, not an accident to route around).
//
// Testing tool, debug build only -- not something a normal player
// should have easy access to.
struct next_level_entry { const char *from; const char *to; };
static const struct next_level_entry ps3_next_level_table[] = {
    { "e1m1", "e1m2" }, { "e1m2", "e1m3" }, { "e1m3", "e1m4" },
    { "e1m4", "e1m5" }, { "e1m5", "e1m6" }, { "e1m6", "e1m7" },
    { "e1m7", "start" }, { "e1m8", "e1m5" },

    { "e2m1", "e2m2" }, { "e2m2", "e2m3" }, { "e2m3", "e2m4" },
    { "e2m4", "e2m5" }, { "e2m5", "e2m6" }, { "e2m6", "start" },
    { "e2m7", "e2m4" },

    { "e3m1", "e3m2" }, { "e3m2", "e3m3" }, { "e3m3", "e3m4" },
    { "e3m4", "e3m5" }, { "e3m5", "e3m6" }, { "e3m6", "start" },
    { "e3m7", "e3m5" },

    { "e4m1", "e4m2" }, { "e4m2", "e4m3" }, { "e4m3", "e4m4" },
    { "e4m4", "e4m5" }, { "e4m5", "e4m6" }, { "e4m6", "e4m7" },
    { "e4m7", "start" }, { "e4m8", "e4m6" },

    // Reaching Shub-Niggurath's level ("end") normally requires beating
    // all 4 episodes and returning to the hub, where a 5th teleporter
    // appears. Shortcut for testing: from "start" itself, L1+R1 jumps
    // straight there instead.
    { "start", "end" },
};

static void
PS3_NextLevel(void)
{
    extern void PS3_Log(const char *fmt, ...);
    for (int i = 0; i < sizeof(ps3_next_level_table) / sizeof(ps3_next_level_table[0]); i++) {
	if (!strcmp(sv.name, ps3_next_level_table[i].from)) {
	    PS3_Log("PS3_NextLevel: %s -> %s", sv.name, ps3_next_level_table[i].to);
	    Cbuf_AddText(va("changelevel %s\n", ps3_next_level_table[i].to));
	    return;
	}
    }
    PS3_Log("PS3_NextLevel: no entry for current map '%s' -- doing nothing", sv.name);
}
#endif // TYRQUAKE_DEBUG_BUILD

// Default bindings, applied once (only for keys the player hasn't
// already bound -- e.g. via a saved config.cfg from a previous run, or
// by rebinding in the "Customize Controls" menu this session).
static void
PS3_SetDefaultBindings(void)
{
    struct { knum_t key; const char *cmd; } defaults[] = {
	{ PAD_KEY_CROSS,    "+jump" },
	{ PAD_KEY_R2,       "+attack" },
	{ PAD_KEY_L2,       "+speed" },
	{ PAD_KEY_L1,       "impulse 12" }, // prev weapon
	{ PAD_KEY_R1,       "impulse 10" }, // next weapon
	{ K_MWHEELUP,       "impulse 10" }, // mouse wheel: next weapon
	{ K_MWHEELDOWN,     "impulse 12" }, // mouse wheel: prev weapon
	{ PAD_KEY_TRIANGLE, "save quick" },
	{ PAD_KEY_CIRCLE,   "load quick" },
	// Vertical movement -- needed for swimming up/down in water levels
	// (not just noclip flying). Moved here from TRIANGLE/CIRCLE, which
	// are now quicksave/quickload.
	{ PAD_KEY_L3,       "+moveup" },
	{ PAD_KEY_R3,       "+movedown" },
    };
    for (int i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
	if (!keybindings[defaults[i].key])
	    Key_SetBinding(defaults[i].key, defaults[i].cmd);
    }

    // SQUARE and SELECT are deliberately left unbound by default in
    // EVERY build, debug included -- noclip/god mode are available to
    // bind from Customize Controls in a debug build (see menu.c), but
    // never pre-bound automatically, so there's no scenario where a
    // player launches a debug build fresh and finds cheats already
    // active without asking for them. SELECT's in-menu role is
    // separate and unconditional either way -- a fixed, non-rebindable
    // "clear a binding in Customize Controls" (sent as K_DEL, not
    // through this table at all), same as START; see IN_Commands.
}

static padInfo pad_info;
static padData pad_data;
static qboolean pad_available = false;
static unsigned pad_buttons = 0;
static unsigned pad_buttons_prev = 0;

// Turn/look speed, in degrees per second at full stick deflection.
// Not exposed as cvars yet -- simplest thing that works first.
#define PS3_TURN_SPEED   180.0f
#define PS3_PITCH_SPEED  120.0f

static float
PS3_StickAxis(int raw)
{
    int centered = raw - 128;
    if (centered > -STICK_DEADZONE && centered < STICK_DEADZONE)
	return 0.0f;
    // Normalize to roughly -1..1, keeping the deadzone subtracted out so
    // there's no jump at the edge of the deadzone.
    float v = (float)centered / 128.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return v;
}

// --- USB Keyboard ---
// ioKbRead returns a full snapshot of currently-held raw keycodes each
// call (like USB HID boot protocol), not press/release events -- so we
// diff against the previous snapshot ourselves, same pattern as the pad
// buttons above.

#define KB_MAX_PORTS 1 // only read port 0; MAX_KB_PORT_NUM allows more

static qboolean kb_available = false;
static qboolean kb_key_down[256]; // indexed by raw keycode (all raw codes fit in a byte)

// Raw keycode -> Quake key, for keys that need a specific non-ASCII
// K_* code (arrows, function keys, escape, etc). Anything not in this
// table falls back to ioKbCnvRawCode() for a properly shifted/mapped
// ASCII character instead.
static knum_t
PS3_RawKeyToQuakeKey(u16 rawcode)
{
    switch (rawcode) {
    case KB_RAWKEY_ESCAPE:     return K_ESCAPE;
    case KB_RAWKEY_ENTER:      return K_ENTER;
    case KB_RAWKEY_BS:         return K_BACKSPACE;
    case KB_RAWKEY_TAB:        return K_TAB;
    case KB_RAWKEY_SPACE:      return (knum_t)' ';
    case KB_RAWKEY_UP_ARROW:   return K_UPARROW;
    case KB_RAWKEY_DOWN_ARROW: return K_DOWNARROW;
    case KB_RAWKEY_LEFT_ARROW: return K_LEFTARROW;
    case KB_RAWKEY_RIGHT_ARROW:return K_RIGHTARROW;
    case KB_RAWKEY_DELETE:     return K_DEL;
    case KB_RAWKEY_INSERT:     return K_INS;
    case KB_RAWKEY_HOME:       return K_HOME;
    case KB_RAWKEY_END:        return K_END;
    case KB_RAWKEY_PAGE_UP:    return K_PGUP;
    case KB_RAWKEY_PAGE_DOWN:  return K_PGDN;
    // USB HID usage 0x35 is the backtick/tilde key on a US keyboard --
    // this header labels it "106_KANJI" (its role on JIS keyboards) but
    // the raw scancode is the same physical key. Map it to whatever
    // Quake normally uses for the console-toggle key.
    case 0x35:                 return (knum_t)'`';
    case KB_RAWKEY_F1: case KB_RAWKEY_F1+1: case KB_RAWKEY_F1+2:
    case KB_RAWKEY_F1+3: case KB_RAWKEY_F1+4: case KB_RAWKEY_F1+5:
    case KB_RAWKEY_F1+6: case KB_RAWKEY_F1+7: case KB_RAWKEY_F1+8:
    case KB_RAWKEY_F1+9: case KB_RAWKEY_F1+10: case KB_RAWKEY_F1+11:
	return (knum_t)(K_F1 + (rawcode - KB_RAWKEY_F1));
    default:
	return (knum_t)0; // not special -- caller falls back to ASCII conversion
    }
}

static void
PS3_Keyboard_Update(void)
{
    extern void PS3_Log(const char *fmt, ...);
    static qboolean logged_connect = false;
    static qboolean logged_getinfo_fail = false;

    KbInfo info;
    s32 rc = ioKbGetInfo(&info);
    if (rc != 0) {
	if (!logged_getinfo_fail) {
	    PS3_Log("PS3_Keyboard_Update: ioKbGetInfo failed, rc=%d", (int)rc);
	    logged_getinfo_fail = true;
	}
	kb_available = false;
	return;
    }
    if (info.connected == 0) {
	if (kb_available) // was connected, now isn't
	    PS3_Log("PS3_Keyboard_Update: keyboard disconnected");
	kb_available = false;
	return;
    }
    if (!kb_available) {
	PS3_Log("PS3_Keyboard_Update: keyboard connected (connected=%u, max=%u, info=0x%x -- bit0=%d)",
		info.connected, info.max, info.info, info.info & 1);
	// Re-arm read mode on (re)connect, in case the console resets it
	// per-connection rather than remembering it globally.
	s32 mode_rc = ioKbSetReadMode(0, KB_RMODE_PACKET);
	s32 codetype_rc = ioKbSetCodeType(0, KB_CODETYPE_RAW);
	PS3_Log("PS3_Keyboard_Update: re-armed ioKbSetReadMode=%d ioKbSetCodeType=%d",
		(int)mode_rc, (int)codetype_rc);
    }
    kb_available = true;

    KbData data;
    s32 read_rc = ioKbRead(0, &data);
    if (read_rc != 0) {
	static int fail_count = 0;
	if (fail_count < 5) {
	    PS3_Log("PS3_Keyboard_Update: ioKbRead failed, rc=%d", (int)read_rc);
	    fail_count++;
	}
	return;
    }

    // Periodic heartbeat showing nb_keycode even when it's 0 -- tells us
    // whether ioKbRead is succeeding but genuinely seeing no presses
    // (nb_keycode always 0) versus something else being wrong.
    static int read_count = 0;
    read_count++;
    if (read_count <= 10 || read_count % 300 == 0)
	PS3_Log("PS3_Keyboard_Update: ioKbRead OK, nb_keycode=%d", data.nb_keycode);

    qboolean now_down[256];
    memset(now_down, 0, sizeof(now_down));
    for (int i = 0; i < data.nb_keycode && i < MAX_KEYCODES; i++) {
	u16 raw = data.keycode[i];
	if (raw < 256)
	    now_down[raw] = true;
    }

    for (int raw = 0; raw < 256; raw++) {
	if (now_down[raw] == kb_key_down[raw])
	    continue;

	knum_t key = PS3_RawKeyToQuakeKey((u16)raw);
	if (!key) {
	    // Not a "special" key -- convert to a real (possibly shifted)
	    // ASCII character using the keyboard's own layout mapping.
	    u16 converted = ioKbCnvRawCode(KB_MAPPING_101, data.mkey, data.led, (u16)raw);
	    if (converted & (KB_RAWDAT | KB_KEYPAD)) {
		PS3_Log("PS3_Keyboard_Update: raw=0x%02x down=%d -> non-ASCII converted=0x%04x, skipped",
			raw, now_down[raw], converted);
		continue; // non-ASCII result, nothing sensible to map to
	    }
	    if (converted >= 32 && converted < 127)
		key = (knum_t)converted;
	}

	PS3_Log("PS3_Keyboard_Update: raw=0x%02x down=%d -> quake key=%d",
		raw, now_down[raw], (int)key);

	if (key)
	    Key_Event(key, now_down[raw]);
	kb_key_down[raw] = now_down[raw];
    }
}

// --- USB Mouse ---
// mouseData gives relative deltas (x_axis/y_axis, signed 8-bit) per
// read, not absolute position -- exactly what IN_Move wants for looking
// around, no extra math needed.

static qboolean mouse_available = false;
static s8 mouse_dx, mouse_dy;
static u8 mouse_buttons_prev = 0;

static void
PS3_Mouse_Update(void)
{
    extern void PS3_Log(const char *fmt, ...);
    static qboolean logged_getinfo_fail = false;

    mouseInfo info;
    s32 rc = ioMouseGetInfo(&info);
    if (rc != 0) {
	if (!logged_getinfo_fail) {
	    PS3_Log("PS3_Mouse_Update: ioMouseGetInfo failed, rc=%d", (int)rc);
	    logged_getinfo_fail = true;
	}
	mouse_available = false;
	mouse_dx = mouse_dy = 0;
	return;
    }
    if (info.connected == 0) {
	if (mouse_available)
	    PS3_Log("PS3_Mouse_Update: mouse disconnected");
	mouse_available = false;
	mouse_dx = mouse_dy = 0;
	return;
    }
    if (!mouse_available) {
	PS3_Log("PS3_Mouse_Update: mouse connected (connected=%u, max=%u)",
		info.connected, info.max);
    }
    mouse_available = true;

    // ioMouseGetData only ever returns the single most recent sample --
    // if the mouse reports movement faster than we poll (very likely,
    // since USB mice typically report at 125Hz+ but we only poll once
    // per game frame), earlier deltas within the same frame get
    // silently dropped rather than accumulated. That under-reports
    // total movement and reads as input lag/sluggishness. GetDataList
    // drains every buffered sample since our last read instead, so we
    // sum ALL of them -- nothing gets lost between polls.
    mouseDataList dataList;
    s32 read_rc = ioMouseGetDataList(0, &dataList);
    if (read_rc != 0) {
	static int fail_count = 0;
	if (fail_count < 5) {
	    PS3_Log("PS3_Mouse_Update: ioMouseGetDataList failed, rc=%d", (int)read_rc);
	    fail_count++;
	}
	mouse_dx = mouse_dy = 0;
	return;
    }

    int total_dx = 0, total_dy = 0;
    int wheel_delta = 0;
    u8 latest_buttons = mouse_buttons_prev;
    qboolean any_update = false;

    for (u32 i = 0; i < dataList.count && i < MOUSE_MAX_DATA_LIST; i++) {
	mouseData *d = &dataList.list[i];
	if (!d->update)
	    continue;
	any_update = true;
	total_dx += d->x_axis;
	total_dy += d->y_axis;
	wheel_delta += d->wheel;
	latest_buttons = d->buttons; // most recent sample wins for button state
    }

    if (!any_update) {
	mouse_dx = mouse_dy = 0;
	return;
    }

    // Clamp back into s8 range for storage -- IN_Move only ever sees
    // these as floats afterward anyway, so this just guards against a
    // pathological single-frame sum overflowing the type.
    if (total_dx > 127) total_dx = 127;
    if (total_dx < -127) total_dx = -127;
    if (total_dy > 127) total_dy = 127;
    if (total_dy < -127) total_dy = -127;
    mouse_dx = (s8)total_dx;
    mouse_dy = (s8)total_dy;

    static int move_log_count = 0;
    if ((mouse_dx != 0 || mouse_dy != 0) && move_log_count < 20) {
	PS3_Log("PS3_Mouse_Update: %u buffered samples, summed dx=%d dy=%d buttons=0x%02x",
		dataList.count, (int)mouse_dx, (int)mouse_dy, latest_buttons);
	move_log_count++;
    }

    // Standard HID button bit order: bit0=left, bit1=right, bit2=middle.
    unsigned pressed = latest_buttons & ~mouse_buttons_prev;
    unsigned released = mouse_buttons_prev & ~latest_buttons;
    if (pressed & 1)   Key_Event(K_MOUSE1, true);
    if (released & 1)  Key_Event(K_MOUSE1, false);
    if (pressed & 2)   Key_Event(K_MOUSE2, true);
    if (released & 2)  Key_Event(K_MOUSE2, false);
    if (pressed & 4)   Key_Event(K_MOUSE3, true);
    if (released & 4)  Key_Event(K_MOUSE3, false);
    mouse_buttons_prev = latest_buttons;

    // Mouse wheel -- summed delta across all buffered samples this poll.
    if (wheel_delta > 0) {
	Key_Event(K_MWHEELUP, true);
	Key_Event(K_MWHEELUP, false);
    } else if (wheel_delta < 0) {
	Key_Event(K_MWHEELDOWN, true);
	Key_Event(K_MWHEELDOWN, false);
    }
}

void
IN_Init(void)
{
    extern void PS3_Log(const char *fmt, ...);
    ioPadInit(MAX_PADS);
    s32 kb_rc = ioKbInit(1);
    s32 mouse_rc = ioMouseInit(1);
    PS3_Log("IN_Init: ioKbInit=%d ioMouseInit=%d", (int)kb_rc, (int)mouse_rc);

    // Explicitly set read mode + code type instead of relying on
    // whatever the console defaults to -- ioKbRead never produced any
    // key data at all without this, even though ioKbGetInfo correctly
    // reported the keyboard as connected.
    s32 mode_rc = ioKbSetReadMode(0, KB_RMODE_PACKET);
    s32 codetype_rc = ioKbSetCodeType(0, KB_CODETYPE_RAW);
    PS3_Log("IN_Init: ioKbSetReadMode=%d ioKbSetCodeType=%d",
	    (int)mode_rc, (int)codetype_rc);

    // Quake's compiled-in default is sensitivity=3, which felt too high
    // on a PS3 mouse. Set our own default here -- runs before config.cfg
    // loads (later, via "exec quake.rc"), so a player's own saved value
    // still takes over on subsequent launches; this only affects the
    // very first run / a config that never touched sensitivity.
    Cvar_SetValue("sensitivity", 2.5);
}

void
IN_Shutdown(void)
{
    ioKbEnd();
    ioMouseEnd();
}

void
IN_Commands(void)
{
    static qboolean defaults_applied = false;
    if (!defaults_applied) {
	PS3_SetDefaultBindings();
	defaults_applied = true;
    }

    // Captured HERE, before anything this frame has a chance to change
    // it (in particular, before the unconditional PAD_KEY_CIRCLE/
    // PAD_KEY_CROSS dispatch further down, which -- if this press is
    // exactly what Customize Controls is waiting to capture -- clears
    // m_keys_bind_grab synchronously as a side effect of Key_Event
    // itself, within this same call). The CIRCLE/CROSS logic further
    // down needs to know whether we were STILL in bind-grab mode at the
    // moment this press actually arrived, not whichever way that flag
    // happens to have flipped to by the time we get around to checking
    // it later in the same function.
    qboolean was_bind_grab = m_keys_bind_grab;

    PS3_Keyboard_Update();
    PS3_Mouse_Update();

    ioPadGetInfo(&pad_info);
    pad_available = false;

    for (int i = 0; i < MAX_PADS; i++) {
	if (pad_info.status[i]) {
	    ioPadGetData(i, &pad_data);
	    pad_available = true;
	    break;
	}
    }

    if (!pad_available) {
	pad_buttons = 0;
	return;
    }

    pad_buttons_prev = pad_buttons;
    pad_buttons = (pad_data.button[2] << 8) | (pad_data.button[3] & 0xff);

    // Edge-triggered actions (fire on press, not held).
    unsigned pressed = pad_buttons & ~pad_buttons_prev;
    unsigned released = pad_buttons_prev & ~pad_buttons;

    // Face/shoulder buttons -> bindable keys (see PS3_SetDefaultBindings
    // for what they do by default; the player can rebind any of these
    // from the "Customize Controls" menu).
    if (pressed & BUTTON_CROSS)    Key_Event(PAD_KEY_CROSS, true);
    if (released & BUTTON_CROSS)   Key_Event(PAD_KEY_CROSS, false);
    if (pressed & BUTTON_CIRCLE)   Key_Event(PAD_KEY_CIRCLE, true);
    if (released & BUTTON_CIRCLE)  Key_Event(PAD_KEY_CIRCLE, false);
    if (pressed & BUTTON_SQUARE)   Key_Event(PAD_KEY_SQUARE, true);
    if (released & BUTTON_SQUARE)  Key_Event(PAD_KEY_SQUARE, false);
    if (pressed & BUTTON_TRIANGLE) Key_Event(PAD_KEY_TRIANGLE, true);
    if (released & BUTTON_TRIANGLE) Key_Event(PAD_KEY_TRIANGLE, false);
    if (pressed & BUTTON_L1)       Key_Event(PAD_KEY_L1, true);
    if (released & BUTTON_L1)      Key_Event(PAD_KEY_L1, false);
    if (pressed & BUTTON_R1)       Key_Event(PAD_KEY_R1, true);
    if (released & BUTTON_R1)      Key_Event(PAD_KEY_R1, false);

#ifdef TYRQUAKE_DEBUG_BUILD
    // L1+R1 chord -> next level (see PS3_NextLevel above). Edge-triggered
    // on the transition into "both held" (fires once, not every frame
    // while both stay held), and only during actual gameplay -- not in
    // a menu, where this combo has no meaningful equivalent anyway.
    {
	unsigned l1r1 = BUTTON_L1 | BUTTON_R1;
	if ((pad_buttons & l1r1) == l1r1 && (pad_buttons_prev & l1r1) != l1r1
	    && key_dest == key_game) {
	    PS3_NextLevel();
	}
    }
#endif
    if (pressed & BUTTON_L2)       Key_Event(PAD_KEY_L2, true);
    if (released & BUTTON_L2)      Key_Event(PAD_KEY_L2, false);
    if (pressed & BUTTON_R2)       Key_Event(PAD_KEY_R2, true);
    if (released & BUTTON_R2)      Key_Event(PAD_KEY_R2, false);
    if (pressed & BUTTON_L3)       Key_Event(PAD_KEY_L3, true);
    if (released & BUTTON_L3)      Key_Event(PAD_KEY_L3, false);
    if (pressed & BUTTON_R3)       Key_Event(PAD_KEY_R3, true);
    if (released & BUTTON_R3)      Key_Event(PAD_KEY_R3, false);
    // SELECT is dual-purpose, split cleanly by context (the two never
    // overlap, since the player is either in the menu or in the game,
    // never both):
    // - In the menu: sends K_DEL, a fixed, non-rebindable role (not
    //   routed through the normal PAD_KEY_* bindable-key system) used to
    //   clear a binding on Customize Controls. Gated to key_menu
    //   specifically because K_DEL already has its own default binding
    //   in normal gameplay ("+lookdown", from Quake's classic keyboard
    //   controls) -- without this check, SELECT would make the player
    //   look down during gameplay instead of doing nothing, since
    //   Key_Event dispatches K_DEL exactly like a real keyboard press
    //   regardless of where it came from.
    // - In-game: goes through the normal bindable-key system like every
    //   other face button, defaulting to noclip (handy for testing
    //   things like L3/R3 without fighting through a level first), and
    //   rebindable from Customize Controls like anything else.
    if (key_dest == key_menu) {
	if (pressed & BUTTON_SELECT)   Key_Event(K_DEL, true);
	if (released & BUTTON_SELECT)  Key_Event(K_DEL, false);
    } else {
	if (pressed & BUTTON_SELECT) {
#ifdef TYRQUAKE_DEBUG_BUILD
	    extern void PS3_Log(const char *fmt, ...);
	    static int select_press_count = 0;
	    select_press_count++;
	    // Not a substitute for reading the actual "noclip ON/OFF"
	    // console print -- just a count of presses, since SELECT's
	    // in-game default is a toggle command (noclip), so parity
	    // (odd/even count since boot) hints at the likely current
	    // state without needing to track the real cvar/flag.
	    PS3_Log("in_ps3: SELECT pressed in-game (press #%d, %s if still default noclip binding)",
		    select_press_count, (select_press_count % 2) ? "now ON" : "now OFF");
#endif
	    Key_Event(PAD_KEY_SELECT, true);
	}
	if (released & BUTTON_SELECT)  Key_Event(PAD_KEY_SELECT, false);
    }

    // START always opens/closes the menu -- hardcoded, not rebindable,
    // so the player can never lock themselves out of the menu.
    if (pressed & BUTTON_START) {
	extern void PS3_Log(const char *fmt, ...);
	PS3_Log("in_ps3: START pressed, sending K_ESCAPE down");
	Key_Event(K_ESCAPE, true);
    }
    if (released & BUTTON_START) {
	extern void PS3_Log(const char *fmt, ...);
	PS3_Log("in_ps3: START released, sending K_ESCAPE up");
	Key_Event(K_ESCAPE, false);
    }

    // D-pad -> arrow keys. In the menu these navigate; in-game Quake
    // ignores arrow keys by default unless bound, so this is safe either
    // way. CROSS above already does double duty as "select" in menus
    // (Quake's menu code accepts K_ENTER, but also treats some confirm
    // keys the same -- if CROSS doesn't confirm menu items, we'll bind
    // K_ENTER to it specifically once confirmed in testing).
    if (pressed & BUTTON_UP)
	Key_Event(K_UPARROW, true);
    if (released & BUTTON_UP)
	Key_Event(K_UPARROW, false);
    if (pressed & BUTTON_DOWN)
	Key_Event(K_DOWNARROW, true);
    if (released & BUTTON_DOWN)
	Key_Event(K_DOWNARROW, false);
    if (pressed & BUTTON_LEFT)
	Key_Event(K_LEFTARROW, true);
    if (released & BUTTON_LEFT)
	Key_Event(K_LEFTARROW, false);
    if (pressed & BUTTON_RIGHT)
	Key_Event(K_RIGHTARROW, true);
    if (released & BUTTON_RIGHT)
	Key_Event(K_RIGHTARROW, false);

    // CROSS sends K_ENTER while a menu is open, so it can both jump
    // in-game AND confirm menu selections. Two menu screens work
    // differently and need 'y' sent instead of K_ENTER:
    // - The "Quit" confirmation (checked via m_state == m_quit): sending
    //   both K_ENTER and K_y unconditionally on every press used to mean
    //   the same press that opened the Quit screen (K_ENTER, from the
    //   main menu) also immediately confirmed it (K_y, landing on the
    //   freshly-opened Quit screen within that same frame) -- checking
    //   m_state avoids that.
    // - SCR_ModalMessage's blocking Y/N wait (screen.c) -- e.g. "Are you
    //   sure you want to start a new game?". A completely separate
    //   mechanism (a tight do-while loop, not the normal menu state
    //   machine at all), detected via key_count, which SCR_ModalMessage
    //   sets to -1 right before that loop starts specifically so normal
    //   key dispatch doesn't interfere while its modal box is up.
    //   Without this case, Cross would keep sending K_ENTER here, which
    //   that loop never checks for (only 'y'/'n'/Escape) -- it would
    //   look and behave exactly like a hard freeze, deaf to every
    //   button except Circle (K_ESCAPE, handled separately below,
    //   already one of the keys that loop does accept).
    // Same m_keys_bind_grab hazard as CIRCLE above applies here too --
    // if an action ever gets rebound to CROSS itself, skip our
    // supplementary confirm-key send so it doesn't kick the player back
    // out of Customize Controls immediately after the capture succeeds.
    //
    // confirm_key_press_pending (not just re-checking key_dest/
    // m_keys_bind_grab again at release time) matters because either can
    // change in between: e.g. a press sent while bind_grab was true (so
    // skipped entirely, correctly) could see bind_grab already false by
    // the time of the matching release (the capture having completed in
    // between) -- re-checking then would wrongly send a release for a
    // press we never actually sent, using a stale remembered key from
    // some earlier, unrelated interaction.
    static knum_t last_sent_confirm_key = K_ENTER;
    static qboolean confirm_key_press_pending = false;
    if (pressed & BUTTON_CROSS) {
	if (key_dest == key_menu && !was_bind_grab) {
	    // Remembers which key was actually sent on the press, so the
	    // matching release always sends that SAME key back -- not a
	    // freshly re-decided one. Without this, a press that opens
	    // SCR_ModalMessage's Y/N wait (sent as K_ENTER, since
	    // key_count was still > 0 at press time) would, on release,
	    // get re-evaluated with key_count now <= 0 (the wait loop
	    // having started in between) and send K_y instead --
	    // confirming the dialog the instant the player's original
	    // press was released, even though they never made a second,
	    // distinct press at all.
	    knum_t confirm_key;
	    if (key_count <= 0)
		confirm_key = K_y;
	    else if (m_state == m_quit)
		confirm_key = K_y;
	    else
		confirm_key = K_ENTER;
	    last_sent_confirm_key = confirm_key;
	    confirm_key_press_pending = true;
#ifdef TYRQUAKE_DEBUG_BUILD
	    extern void PS3_Log(const char *fmt, ...);
	    PS3_Log("in_ps3: CROSS pressed in menu, key_count=%d m_state=%d -> confirm_key=%d",
		    key_count, (int)m_state, (int)confirm_key);
#endif
	    Key_Event(confirm_key, true);
	}
    }
    if (released & BUTTON_CROSS) {
	if (confirm_key_press_pending) {
	    Key_Event(last_sent_confirm_key, false);
	    confirm_key_press_pending = false;
	}
    }

    // CIRCLE also sends K_ESCAPE while a menu is open, matching the
    // PlayStation UI convention (Circle = back/cancel) -- in addition to
    // its normal PAD_KEY_CIRCLE binding (quickload by default), which
    // still applies during actual gameplay. The two never conflict:
    // Key_Event only ever routes a bound command through PAD_KEY_CIRCLE
    // when key_dest == key_game, so sending both here is harmless -- the
    // "load quick" binding simply has no effect while in a menu. This
    // also already works to cancel the Quit confirmation (its K_ESCAPE
    // case handles that identically to K_n), no extra key needed there.
    //
    // EXCEPT while Customize Controls is actively capturing a key
    // (m_keys_bind_grab): if the player rebinds some action to CIRCLE
    // itself, the normal PAD_KEY_CIRCLE send just below correctly
    // captures it as the new binding (and, as a side effect of that
    // capture succeeding, immediately clears m_keys_bind_grab). Our
    // supplementary K_ESCAPE send would then fire into a menu that's no
    // longer in bind-grab mode, so instead of being harmlessly swallowed
    // (bind-grab's own K_ESCAPE case, which cancels the capture rather
    // than binding it), it falls through to the menu's normal "go back"
    // handling and kicks the player out of Customize Controls entirely
    // -- immediately after the very key that was just supposed to be
    // getting bound.
    //
    // Tracked via a pending flag (not re-checking key_dest/
    // m_keys_bind_grab again at release time) for the same reason as
    // CROSS above: either can change between the press and the matching
    // release, and a stray, unpaired K_ESCAPE release could wrongly
    // satisfy some other unrelated Escape-sensitive wait (e.g.
    // SCR_ModalMessage's Y/N loop) that happens to be active by then.
    static qboolean circle_escape_press_pending = false;
    if (pressed & BUTTON_CIRCLE) {
	if (key_dest == key_menu && !was_bind_grab) {
	    circle_escape_press_pending = true;
	    Key_Event(K_ESCAPE, true);
	}
    }
    if (released & BUTTON_CIRCLE) {
	if (circle_escape_press_pending) {
	    Key_Event(K_ESCAPE, false);
	    circle_escape_press_pending = false;
	}
    }
}

void
IN_Move(usercmd_t *cmd)
{
    qboolean did_look = false;

    if (pad_available) {
	// Left stick: move forward/back and strafe.
	float lx = PS3_StickAxis(pad_data.button[6]);
	float ly = PS3_StickAxis(pad_data.button[7]);
	// L2 is bound to +speed/-speed by default (see PS3_SetDefaultBindings
	// above, rebindable from the menu), which sets in_speed's state --
	// but that only auto-applies the run multiplier to *keyboard*-driven
	// movement (CL_BaseMove, in cl_input.c). Our stick axes bypass that
	// entirely, so we replicate the same XOR check here: cl_run.value
	// flips the meaning (hold to run vs. hold to walk, matching the
	// "Always Run" menu option).
	float speed_mult = 1.0f;
	if ((in_speed.state & 1) ^ (int)cl_run.value)
	    speed_mult = cl_movespeedkey.value;

	cmd->forwardmove -= ly * cl_forwardspeed.value * speed_mult;
	cmd->sidemove += lx * cl_sidespeed.value * speed_mult;

	// Right stick: turn and look, scaled by frame time so it's speed-
	// independent of framerate.
	float rx = PS3_StickAxis(pad_data.button[4]);
	float ry = PS3_StickAxis(pad_data.button[5]);
	if (rx != 0.0f || ry != 0.0f) {
	    cl.viewangles[YAW] -= rx * PS3_TURN_SPEED * joy_sensitivity.value * host_frametime;
	    cl.viewangles[PITCH] += ry * PS3_PITCH_SPEED * joy_sensitivity.value * host_frametime;
	    did_look = true;
	}
    }

    // USB mouse: turn and look, using Quake's own sensitivity/m_yaw/
    // m_pitch/m_filter cvars -- same system the "Mouse Options" menu
    // already controls, mirroring the reference mouse drivers exactly
    // (see in_x11.c's IN_MouseMove) instead of our own hardcoded
    // constant. This also gets us m_filter's smoothing (averaging with
    // last frame's delta), which is what was making raw movement feel
    // jerky at high sensitivity.
    if (mouse_available) {
	float mx = (float)mouse_dx;
	float my = (float)mouse_dy;

	if (m_filter.value) {
	    mx = (mx + old_mouse_dx) * 0.5f;
	    my = (my + old_mouse_dy) * 0.5f;
	}
	old_mouse_dx = (float)mouse_dx;
	old_mouse_dy = (float)mouse_dy;

	mx *= sensitivity.value;
	my *= sensitivity.value;

	if (mx != 0.0f || my != 0.0f) {
	    cl.viewangles[YAW] -= m_yaw.value * mx;
	    cl.viewangles[PITCH] += m_pitch.value * my;
	    did_look = true;
	}
    }

    if (did_look) {
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
	    cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
	    cl.viewangles[PITCH] = cl_minpitch.value;

	// Quake auto-levels the view pitch toward cl.idealpitch (a value
	// computed server-side for walking up/down stairs) via V_DriftPitch.
	// On flat ground the server sends idealpitch=0 on every single
	// network update (even in single-player -- it's still client/server
	// under the hood), which stomps any one-time sync we do here right
	// back to 0 before the next frame. The actual mechanism every other
	// driver relies on to suppress this is cl.nodrift -- forcing it true
	// every frame makes V_DriftPitch's centering branch never run at
	// all, regardless of what idealpitch the network layer sends.
	cl.nodrift = true;
    }
}

void
IN_Accumulate(void)
{
}

void
IN_ModeChanged(void)
{
}

void
IN_AddCommands(void)
{
}

void
IN_RegisterVariables(void)
{
    Cvar_RegisterVariable(&_windowed_mouse);
    Cvar_RegisterVariable(&m_filter);
    Cvar_RegisterVariable(&joy_sensitivity);
}
