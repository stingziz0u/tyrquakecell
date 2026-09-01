// vid_ps3.c -- PS3 video driver, RSX-backed.
//
// Milestone 3 implementation. The RSX init/present sequence (double
// buffering, gcmSetFlip -> rsxFlushBuffer -> gcmSetWaitFlip -> flip-status
// wait) and the 8-bit-indexed -> ARGB palette expansion are adapted from
// the dragonfly-quake-ps3 port (src/video/src/vid_ps3.c + vid_buffers.c),
// which documents in its own comments that an earlier version hung after
// 127 frames from missing gcmSetWaitFlip -- so that specific sequence
// below is deliberate, not arbitrary.
//
// TyrQuake's own render backend contract (see vid_null.c) is kept as-is:
// VID_Init allocates vid.buffer (8-bit indexed, BASEWIDTH x BASEHEIGHT),
// the software renderer draws into it, VID_Update presents it.

#include "common.h"
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

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>

/* Defined in sys_ps3.c -- writes to a log file on dev_hdd0 since printf
 * output isn't visible on real hardware without a debug console. */
extern void PS3_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

viddef_t vid; // global video state

// Internal render resolution. Classic Quake software-rendering default
// was 320x200 (VGA-era), but the renderer supports higher resolutions
// fine -- we just never tried until now. Doubling both dimensions (same
// aspect ratio, no new distortion) means each "block" from the final
// upscale-to-720p step is 1/4 the area, much less obviously blocky.
// Trade-off: 4x the pixels for the software rasterizer to fill every
// frame -- expect this to cost some framerate, which is exactly the
// next thing on the list to tune.
// Internal render resolution. Classic Quake software-rendering default
// was 320x200 (VGA-era), but the renderer supports higher resolutions
// fine -- we just never tried until now. 512x384 is a commonly
// recommended sweet spot for Quake's software renderer specifically
// (good detail/performance balance) -- note it's NOT the same aspect
// ratio as 320x200 (512x384 is close to classic 4:3, vs 320x200's
// 8:5), unlike our earlier 640x400 attempt which preserved the exact
// original ratio. Watch for any stretching in the 3D view; if so,
// vid.aspect (currently hardcoded to 1.0 below) is the knob to fix it.
#define BASEWIDTH  512
#define BASEHEIGHT 384

static byte vid_buffer[BASEWIDTH * BASEHEIGHT]; // 8-bit indexed, vid.buffer points here
static short zbuffer[BASEWIDTH * BASEHEIGHT];
// Bumped from 1MB (was already 4x the original 256KB, matching the 4x
// pixel-count jump from 320x200). Now that the Quake heap itself is a
// generous 128MB (see sys_ps3.c) with plenty of headroom to spare, and
// given busy scenes (lots of monsters/effects visible at once, e.g. the
// Shub-Niggurath fight) are exactly the kind that can exhaust a small
// cache and force it to keep re-rendering surfaces it should be able to
// keep around (r_cache_thrash, now logged below) -- 4MB is cheap
// insurance against that, not a response to confirmed thrashing yet.
static byte surfcache[4 * 1024 * 1024];

unsigned short d_8to16table[256];
unsigned d_8to24table[256]; // doubles as our ARGB palette LUT (0xAARRGGBB)

static u32 argb_buffer[BASEWIDTH * BASEHEIGHT]; // 32-bit expansion of vid_buffer

// Precomputed source-pixel index for each destination column/row of the
// upscale blit, built once (not per-pixel, not even per-frame) since the
// mapping never changes unless the display resolution changes. Sized for
// the largest realistic display mode (1080p) -- only entries up to the
// actual out_w/out_h in use get filled in.
static int scale_x_lut[1920];
static int scale_y_lut[1080];
static qboolean scale_lut_built = false;
static qboolean palette_dirty = true;

// --- RSX state ---
// PS3_NUM_BUFFERS was 2, which (per a confirmed hardware diagnosis in a
// sibling project, xash3d-fwgs's PS3 port, encountering this exact
// symptom) quantizes framerate to a hard 30fps: with only 2 buffers, the
// CPU must block until the *previous* flip has fully finished scanning
// out before it can start drawing the next frame, so any frame whose
// work exceeds one 16.6ms vsync interval gets bumped a full extra vsync
// tick -- regardless of how much real GPU/CPU headroom there actually
// is. 3 buffers leaves one buffer always free to render into while a
// previous flip is still displaying, removing that artificial floor.
#define PS3_NUM_BUFFERS 3

static void *rsx_io_buffer = NULL;
static gcmContextData *rsx_context = NULL;
static u32   rsx_offset[PS3_NUM_BUFFERS];
static void *rsx_mem[PS3_NUM_BUFFERS];
static int   rsx_current_buf = 0;
static int   rsx_display_w   = 0;
static int   rsx_display_h   = 0;
static int   rsx_pitch       = 0;
static qboolean rsx_ready    = false;

// Flip completion via gcmSetFlipHandler, not gcmGetFlipStatus() polling --
// the same sibling project's notes report the polling read reads back
// "already completed" instantly on real hardware, making it useless for
// telling whether a buffer is actually still in flight.
static volatile u32 rsx_flip_queued = 0;
static volatile u32 rsx_flip_completed = 0;

// --- GPU-accelerated upscale ---
// Replaces the old CPU nearest-neighbour loop entirely. rsxSetTransferScaleSurface
// is a fixed-function 2D scaled-blit unit -- entirely separate from (and much
// simpler than) the full 3D pipeline, no shaders/vertex programs/render
// targets needed. What used to cost ~900,000 scalar reads/writes on a
// single PPU thread every frame now costs one small memcpy plus one
// hardware blit call.
static void *rsx_src_mem = NULL;
static u32   rsx_src_offset = 0;
static qboolean gpu_scale_ready = false;

static void
PS3_RSX_FlipHandler(const u32 head)
{
    (void)head;
    rsx_flip_completed++;
}

// Block only if there's no free buffer left to render into -- i.e. both
// of the other PS3_NUM_BUFFERS-1 buffers are still in flight. With 3
// buffers this only blocks when 2 flips are outstanding, leaving the
// CPU free to start the next frame immediately otherwise.
static void
PS3_RSX_WaitForFreeBuffer(void)
{
    int waited = 0;
    while ((int)(rsx_flip_queued - rsx_flip_completed) > PS3_NUM_BUFFERS - 2) {
	usleep(100);
	if (++waited > 20000) {
	    // Force-resync, not just break -- otherwise rsx_flip_completed
	    // stays permanently behind rsx_flip_queued and every later
	    // call fires early forever after a single stall.
	    PS3_Log("PS3_RSX_WaitForFreeBuffer: flip fence timed out, force-syncing");
	    rsx_flip_completed = rsx_flip_queued;
	    break;
	}
    }
}

static void
PS3_RSX_Init(void)
{
    PS3_Log("PS3_RSX_Init: start");

    rsx_io_buffer = memalign(1024 * 1024, 1024 * 1024);
    if (!rsx_io_buffer)
	Sys_Error("PS3 video: memalign failed for RSX IO buffer");
    PS3_Log("PS3_RSX_Init: io buffer allocated");

    s32 rc = rsxInit(&rsx_context, 0x10000, 1024 * 1024, rsx_io_buffer);
    if (rc != 0)
	Sys_Error("PS3 video: rsxInit failed: %d", (int)rc);
    PS3_Log("PS3_RSX_Init: rsxInit OK");

    videoState vstate;
    if (videoGetState(0, 0, &vstate) != 0)
	Sys_Error("PS3 video: videoGetState failed");
    PS3_Log("PS3_RSX_Init: videoGetState OK, state=%d resolution=%d",
	    vstate.state, vstate.displayMode.resolution);

    /* Force 720p rather than whatever the TV's current output mode is
     * (which was 1080p on Ren's TV). Two reasons: it's a much smaller
     * buffer to upscale into every frame (less GPU/memory bandwidth for
     * our simple nearest-neighbour blit), and it's the more universally-
     * supported/predictable HD mode across different TVs. If a TV
     * somehow doesn't support 720p, fall back to the display's current
     * mode rather than hard failing. */
    u8 target_resolution = VIDEO_RESOLUTION_720;
    videoResolution res;
    if (videoGetResolution(target_resolution, &res) != 0) {
	PS3_Log("PS3_RSX_Init: 720p not available, falling back to %d",
		vstate.displayMode.resolution);
	target_resolution = vstate.displayMode.resolution;
	if (videoGetResolution(target_resolution, &res) != 0)
	    Sys_Error("PS3 video: videoGetResolution failed");
    }
    PS3_Log("PS3_RSX_Init: using resolution %dx%d", res.width, res.height);

    rsx_display_w = res.width;
    rsx_display_h = res.height;
    rsx_pitch     = rsx_display_w * 4; // XRGB, 4 bytes/pixel

    videoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = target_resolution;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = rsx_pitch;
    if (videoConfigure(0, &vconfig, NULL, 1) != 0)
	Sys_Error("PS3 video: videoConfigure failed");
    PS3_Log("PS3_RSX_Init: videoConfigure OK");

    videoState wait_state;
    do {
	usleep(10000);
	if (videoGetState(0, 0, &wait_state) != 0)
	    break;
    } while (wait_state.state == 3);
    PS3_Log("PS3_RSX_Init: video ready wait done");

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    for (int i = 0; i < PS3_NUM_BUFFERS; i++) {
	rsx_mem[i] = rsxMemalign(64, rsx_pitch * rsx_display_h);
	if (!rsx_mem[i])
	    Sys_Error("PS3 video: rsxMemalign failed for buffer %d", i);
	rsxAddressToOffset(rsx_mem[i], &rsx_offset[i]);
	gcmSetDisplayBuffer(i, rsx_offset[i], rsx_pitch,
			     rsx_display_w, rsx_display_h);
	PS3_Log("PS3_RSX_Init: buffer %d ready", i);
    }

    rsx_flip_queued = 0;
    rsx_flip_completed = 0;
    gcmSetFlipHandler(PS3_RSX_FlipHandler);
    rsx_current_buf = 0;

    // Small RSX-visible source buffer for the hardware scaled blit: we
    // memcpy the internal ARGB render here each frame (source and
    // destination for rsxSetTransferScaleSurface both need to live in
    // RSX-addressable memory), then let the fixed-function transfer unit
    // do the scaling straight into the display buffer.
    u32 src_pitch = BASEWIDTH * 4;
    rsx_src_mem = rsxMemalign(64, src_pitch * BASEHEIGHT);
    if (!rsx_src_mem) {
	PS3_Log("PS3_RSX_Init: rsxMemalign for GPU-scale source buffer FAILED");
    } else {
	rsxAddressToOffset(rsx_src_mem, &rsx_src_offset);
	PS3_Log("PS3_RSX_Init: GPU-scale source buffer allocated (%dx%d)", BASEWIDTH, BASEHEIGHT);
    }

    gpu_scale_ready = (rsx_src_mem != NULL);
    PS3_Log("PS3_RSX_Init: gpu_scale_ready=%d", gpu_scale_ready);

    rsx_ready = true;
    PS3_Log("PS3_RSX_Init: complete, rsx_ready=true");
}

static void
PS3_RSX_Shutdown(void)
{
    if (!rsx_ready)
	return;
    gcmSetFlipHandler(NULL);
    for (int i = 0; i < PS3_NUM_BUFFERS; i++) {
	if (rsx_mem[i]) {
	    rsxFree(rsx_mem[i]);
	    rsx_mem[i] = NULL;
	}
    }
    if (rsx_src_mem) {
	rsxFree(rsx_src_mem);
	rsx_src_mem = NULL;
    }
    if (rsx_io_buffer) {
	free(rsx_io_buffer);
	rsx_io_buffer = NULL;
    }
    rsx_ready = false;
}

// Upscales the small internal ARGB render to display resolution via a
// hardware-accelerated scaled blit (rsxSetTransferScaleSurface -- see
// gpu_scale_ready block below), falling back to a CPU nearest-neighbour
// loop if that failed to set up. Then flips. The gcmSetFlip ->
// rsxFlushBuffer -> gcmSetWaitFlip -> gcmGetFlipStatus sequence is
// load-bearing -- see file header comment.
static void
PS3_RSX_Present(const u32 *src, int src_w, int src_h)
{
    extern double Sys_DoubleTime(void);
    static int frame_count = 0;
    static double fps_window_start = -1.0;
    static int fps_window_frames = 0;
    qboolean verbose = (frame_count < 8); // full step-by-step detail for the first 8 frames
    if (!rsx_ready)
	return;

    // Measure real FPS over a continuous 1-second window, logged every
    // second for the whole session -- for a dedicated performance-
    // testing playthrough like this one, per-second granularity across
    // a full level matters more than avoiding the disk-activity icon
    // (which this will likely bring back for the duration of this test).
    double now = Sys_DoubleTime();
    if (fps_window_start < 0) fps_window_start = now;
    fps_window_frames++;
    if (now - fps_window_start >= 1.0) {
	PS3_Log("PS3_RSX_Present: ~%.1f fps over last %.1fs (%d frames)",
		fps_window_frames / (now - fps_window_start),
		now - fps_window_start, fps_window_frames);
	fps_window_start = now;
	fps_window_frames = 0;

#ifdef TYRQUAKE_DEBUG_BUILD
	// Same 1-second cadence, same log file -- Quake's own built-in
	// r_speeds/r_dspeeds render-stage timing (see r_misc.c's
	// R_PrintTimes/R_PrintDSpeeds, which this mirrors -- reading the
	// same variables directly instead of parsing their Con_Printf
	// text output). Lets us tell, from a single busy-scene test
	// session, whether world geometry, brush entities (doors/plats),
	// edge scanning, alias models (monsters/weapons -- the ones that
	// scale with enemy count), the view model, or particles is
	// actually dominating frame time, instead of guessing.
	PS3_Log("R_Speeds: %3ims poly=%d/%d surf=%d amodels=%d | world=%.1fms bmodels=%.1fms edges=%.1fms entities=%.1fms viewmodel=%.1fms particles=%.1fms cache_thrash=%d",
		(int)((rw_time2 - rw_time1 + db_time2 - db_time1 + se_time2 - se_time1
		       + de_time2 - de_time1 + dv_time2 - dv_time1 + dp_time2 - dp_time1) * 1000),
		r_polycount, r_drawnpolycount, c_surf, r_amodels_drawn,
		(rw_time2 - rw_time1) * 1000, (db_time2 - db_time1) * 1000,
		(se_time2 - se_time1) * 1000, (de_time2 - de_time1) * 1000,
		(dv_time2 - dv_time1) * 1000, (dp_time2 - dp_time1) * 1000,
		r_cache_thrash);
#endif
    }

    // Cheap, always-on heartbeat -- but infrequent, since every write to
    // the log file touches disk, and the PS3 flashes a system activity
    // icon on frequent disk access (this was showing up as an unwanted
    // "RAM" icon flickering onscreen when this ran twice a second).
    if (frame_count % 1800 == 0) // roughly once a minute at 30fps
	PS3_Log("PS3_RSX_Present: still running, frame %d", frame_count);

    if (verbose) PS3_Log("PS3_RSX_Present: frame %d starting", frame_count);

    // Wait here, BEFORE claiming a buffer to render into -- not after
    // flipping, like the old gcmGetFlipStatus() polling did. That old
    // placement is exactly what caused the hard-30fps quantization: with
    // only 2 buffers it amounted to "always wait for the flip we just
    // queued to fully finish before doing anything else", so any frame
    // over one vsync (16.6ms) cost a full extra vsync tick regardless of
    // real GPU/CPU headroom. This only blocks when there's genuinely no
    // free buffer left (both non-current buffers still in flight).
    PS3_RSX_WaitForFreeBuffer();

    int next = rsx_current_buf;
    u32 *dst = (u32 *)rsx_mem[next];

    // Safe-area margin: most TVs (especially over HDMI in certain modes)
    // crop a percentage of the picture at the edges ("overscan"), which
    // otherwise makes a full-bleed image look zoomed in / cropped. Leave
    // a black border and draw into the inset rectangle instead -- the
    // standard fix, independent of whatever resolution we picked above.
    const float safe_area = 0.90f; // use the middle 90% of the screen
    int out_w = (int)(rsx_display_w * safe_area);
    int out_h = (int)(rsx_display_h * safe_area);
    int off_x = (rsx_display_w - out_w) / 2;
    int off_y = (rsx_display_h - out_h) / 2;

    if (frame_count < PS3_NUM_BUFFERS)
	memset(dst, 0, rsx_pitch * rsx_display_h); // black out borders once per buffer (first PS3_NUM_BUFFERS frames cover all of them)

    if (gpu_scale_ready) {
	// Hardware-accelerated scaled blit: this frame's small ARGB
	// render sits in RSX-visible memory, then the fixed-function 2D
	// transfer unit stretches it straight into the display buffer.
	// Replaces the old CPU nearest-neighbour loop (~900,000 scalar
	// reads/writes per frame on a single PPU thread) with one
	// hardware blit call.
	//
	// No copy into rsx_src_mem here anymore: VID_Update now writes
	// the palette-to-ARGB conversion directly into rsx_src_mem in
	// the first place (when gpu_scale_ready), so src already IS
	// rsx_src_mem by the time we get here -- copying it onto itself
	// would just be wasted work. The pointer check exists only so
	// this still works correctly if ever called with some other
	// source buffer (e.g. the CPU fallback path never takes this
	// branch at all, but nothing here assumes it's the only caller).
	if (src != (const u32 *)rsx_src_mem)
	    memcpy(rsx_src_mem, src, src_w * src_h * 4);

	gcmTransferScale scale;
	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format     = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation  = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = off_x;
	scale.clipY = off_y;
	scale.clipW = out_w;
	scale.clipH = out_h;
	scale.outX  = off_x;
	scale.outY  = off_y;
	scale.outW  = out_w;
	scale.outH  = out_h;
	// "Ratio of source rectangle size to destination rectangle size"
	// (per gcm_sys.h's own field doc) -- source is always src_w/src_h
	// (our fixed internal render resolution), destination is the
	// safe-area rectangle we just picked above.
	scale.ratioX = rsxGetFixedSint32((float)src_w / (float)out_w);
	scale.ratioY = rsxGetFixedSint32((float)src_h / (float)out_h);
	scale.inW    = src_w;
	scale.inH    = src_h;
	scale.pitch  = src_w * 4;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST; // matches the CPU path's look; _LINEAR is free to try if a softer look is ever wanted
	scale.offset = rsx_src_offset;
	scale.inX = 0;
	scale.inY = 0;

	gcmTransferSurface surface;
	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch  = rsx_pitch;
	surface.offset = rsx_offset[next];

	rsxSetTransferScaleSurface(rsx_context, &scale, &surface);
	if (verbose) PS3_Log("PS3_RSX_Present: frame %d GPU scale blit done", frame_count);
    } else {
	// Fallback CPU path, only reached if the RSX source buffer failed
	// to allocate at init -- see PS3_RSX_Init.
	if (!scale_lut_built) {
	    for (int y = 0; y < out_h && y < 1080; y++)
		scale_y_lut[y] = (y * src_h) / out_h;
	    for (int x = 0; x < out_w && x < 1920; x++)
		scale_x_lut[x] = (x * src_w) / out_w;
	    scale_lut_built = true;
	}
	for (int y = 0; y < out_h; y++) {
	    int sy = scale_y_lut[y];
	    const u32 *sr = src + (sy * src_w);
	    u32       *dr = dst + ((y + off_y) * rsx_display_w) + off_x;
	    for (int x = 0; x < out_w; x++)
		dr[x] = sr[scale_x_lut[x]];
	}
	if (verbose) PS3_Log("PS3_RSX_Present: frame %d CPU fallback upscale done", frame_count);
    }

    gcmSetFlip(rsx_context, next);
    if (verbose) PS3_Log("PS3_RSX_Present: frame %d gcmSetFlip done", frame_count);
    rsxFlushBuffer(rsx_context);
    if (verbose) PS3_Log("PS3_RSX_Present: frame %d rsxFlushBuffer done", frame_count);
    gcmSetWaitFlip(rsx_context);
    if (verbose) PS3_Log("PS3_RSX_Present: frame %d gcmSetWaitFlip done", frame_count);

    rsx_flip_queued++;
    rsx_current_buf = (rsx_current_buf + 1) % PS3_NUM_BUFFERS;

    if (verbose) PS3_Log("PS3_RSX_Present: frame %d complete, queued=%u completed=%u",
			  frame_count, rsx_flip_queued, rsx_flip_completed);
    frame_count++;
}

// --- TyrQuake vid.h contract ---

void
VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = BASEWIDTH;
    rect->height = BASEHEIGHT;
}

void
VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void
VID_SetPalette(const byte *palette)
{
    static int call_count = 0;
    call_count++;
    // Confirmed safe via earlier logging (ran 50+ times fine, including
    // through a burst at level-load fade-in) -- now only logging rarely,
    // since logging every call during that fade-in burst was itself
    // triggering the disk-activity RAM icon.
    if (call_count == 1 || call_count % 500 == 0)
	PS3_Log("VID_SetPalette: call #%d, palette=%p", call_count, (void *)palette);

    if (!palette) {
	PS3_Log("VID_SetPalette: NULL palette pointer! bailing out");
	return;
    }

    // PS3 is big-endian: 0xAARRGGBB as a u32 literally matches memory
    // byte order [A][R][G][B], which is what VIDEO_BUFFER_FORMAT_XRGB
    // expects.
    for (int i = 0; i < 256; i++) {
	unsigned r = palette[i * 3 + 0];
	unsigned g = palette[i * 3 + 1];
	unsigned b = palette[i * 3 + 2];
	d_8to24table[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    palette_dirty = true;

    if (call_count == 1 || call_count % 500 == 0)
	PS3_Log("VID_SetPalette: call #%d done", call_count);
}

void
VID_Init(const byte *palette)
{
    PS3_Log("VID_Init: start");
    vid.width = vid.conwidth = BASEWIDTH;
    vid.height = vid.conheight = BASEHEIGHT;
    vid.aspect = 1.0;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = vid.conrowbytes = BASEWIDTH;

    d_pzbuffer = zbuffer;
    D_InitCaches(surfcache, sizeof(surfcache));
    PS3_Log("VID_Init: software renderer buffers ready, calling PS3_RSX_Init");

    // Every platform's video driver allocates this (search common/vid_*.c
    // for "r_warpbuffer =") -- we hadn't. It's the scratch buffer the
    // software renderer draws into when the player is submerged
    // (r_dowarp true, see r_misc.c/d_init.c), instead of the normal
    // vid.buffer. We never triggered r_dowarp during testing (walking on
    // ground, ordinary combat) so this went unnoticed until actually
    // standing in lava/water -- at which point d_viewbuffer became NULL
    // and the renderer crashed the instant it tried to draw a pixel.
    r_warpbuffer = Hunk_HighAllocName(BASEWIDTH * BASEHEIGHT, "warpbuf");
    if (!r_warpbuffer)
	Sys_Error("VID_Init: failed to allocate r_warpbuffer");
    PS3_Log("VID_Init: r_warpbuffer allocated (%d bytes)", BASEWIDTH * BASEHEIGHT);

    PS3_RSX_Init();
    VID_SetPalette(palette);

    // Every other driver wires these up (search common/vid_*.c for
    // "vid_menudrawfn ="); we hadn't, so opening the "Video Options"
    // menu called a null function pointer and crashed instantly.
    // VID_MenuDraw/VID_MenuKey are the shared, generic implementations
    // in vid_mode.c -- we don't need our own, just to point at them like
    // everyone else does.
    vid_menudrawfn = VID_MenuDraw;
    vid_menukeyfn = VID_MenuKey;

    // The HUD/console auto-scale calculation (scr_hudscale cvar, default
    // "0" = automatic, based on vid.height) runs once very early during
    // Cvars_Init -- well before VID_Init (here) has actually set
    // vid.width/vid.height to anything real, so it silently computes a
    // degenerate scale and gets stuck at the 1.0 minimum forever. Forcing
    // a recheck now, with the real resolution in place, is what actually
    // lets the HUD scale up to match our (now higher) internal render
    // resolution instead of staying pinned in the corner at its
    // original 320x200-sized footprint.
    // Quake's auto-scale formula for the HUD (SCR_SetHudscale, when the
    // scr_hudscale cvar is left at its default "0" = automatic) treats
    // 600 pixels tall as the "no scaling needed" reference point, and
    // never scales UP below that -- it's designed for resolutions well
    // above classic 320x200-era values, not for our modest internal
    // resolution. So at our resolution the auto-calc correctly computes
    // "don't scale" by its own design, not by any bug. We want the HUD
    // scaled up by the same factor we scaled the internal render
    // resolution by (from the original 320x200), so derive it from
    // BASEWIDTH instead of hardcoding a number that silently goes wrong
    // if BASEWIDTH/BASEHEIGHT ever change without updating it to match
    // (exactly what happened switching between 640x400 and 512x384).
    Cvar_SetValue("scr_hudscale", (float)BASEWIDTH / 320.0f);
    SCR_CheckResize();
    PS3_Log("VID_Init: after SCR_CheckResize, scr_scale=%.3f vid.width=%d vid.height=%d",
	    scr_scale, vid.width, vid.height);

#ifdef TYRQUAKE_DEBUG_BUILD
    // Enables Quake's own built-in render-stage timing (r_dspeeds) and
    // poly/surface counters (r_speeds) -- without this, the timing
    // variables our R_Speeds log above reads (rw_time1/2, de_time1/2,
    // etc.) never get populated at all, staying at whatever stale/zero
    // values they last had. Debug build only: the cost (a handful of
    // extra Sys_DoubleTime() calls per frame) isn't huge, but there's no
    // reason to pay it in the release build where nothing reads the
    // result.
    Cvar_SetValue("r_speeds", 1.0f);
    Cvar_SetValue("r_dspeeds", 1.0f);
#endif

    PS3_Log("VID_Init: complete");
}

void
VID_InitColormap(const byte *palette)
{
}

void
VID_Shutdown(void)
{
    PS3_RSX_Shutdown();
}

void
VID_Update(vrect_t *rects)
{
    static qboolean first_call = true;
    if (first_call) {
	PS3_Log("VID_Update: first call reached");
	first_call = false;
    }

    // Write straight into rsx_src_mem (RSX-visible memory, already
    // CPU-writable -- we were memcpy'ing into it moments later anyway)
    // when the GPU blit path is available, skipping argb_buffer
    // entirely -- one less full-buffer copy every single frame. Falls
    // back to argb_buffer (paired with the CPU nearest-neighbour blit)
    // only if gpu_scale_ready is false, i.e. rsx_src_mem never got
    // allocated in the first place.
    u32 *convert_dst = gpu_scale_ready ? (u32 *)rsx_src_mem : argb_buffer;

    // TODO: honor the dirty-rect list properly instead of re-expanding
    // the whole buffer every frame -- still valid future work, separate
    // from the unrolling below (helps mostly-static scenes/menus; the
    // unroll helps every frame regardless of how much changed).
    //
    // Manually unrolled 4-wide: the PS3's PPU is an in-order core (no
    // out-of-order execution to hide load latency the way a modern x86
    // chip would), so it benefits more than usual from cutting the
    // per-iteration loop overhead (branch/increment/compare) and giving
    // the CPU several independent loads/stores to work through per
    // iteration instead of one fully-dependent chain of them. A
    // remainder loop below covers whatever BASEWIDTH isn't a multiple
    // of 4 (none of our current presets need it, but this doesn't
    // assume that stays true).
    for (int y = 0; y < BASEHEIGHT; y++) {
	const byte *src = vid_buffer + y * BASEWIDTH;
	u32        *dst = convert_dst + y * BASEWIDTH;
	int x = 0;
	for (; x + 4 <= BASEWIDTH; x += 4) {
	    dst[x + 0] = d_8to24table[src[x + 0]];
	    dst[x + 1] = d_8to24table[src[x + 1]];
	    dst[x + 2] = d_8to24table[src[x + 2]];
	    dst[x + 3] = d_8to24table[src[x + 3]];
	}
	for (; x < BASEWIDTH; x++)
	    dst[x] = d_8to24table[src[x]];
    }
    palette_dirty = false;

    PS3_RSX_Present(convert_dst, BASEWIDTH, BASEHEIGHT);
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
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    return false;
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
