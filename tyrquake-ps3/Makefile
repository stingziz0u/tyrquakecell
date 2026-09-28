#
# TyrQuake PS3 Makefile
#
# Standalone build -- does NOT use tyrquake's own top-level Makefile (that
# one is a generic desktop cross-platform build system not worth wedging a
# PS3 branch into).
#
# IMPORTANT: unlike earlier versions of this file, packaging (.elf -> .self
# -> .pkg) is now handled entirely by ppu_rules' own built-in pattern
# rules, not hand-written here. The hand-written version was missing two
# steps that the real rule always does: stripping + sprxlinker before
# signing, and package_finalize after pkg.py -- both of which are likely
# why nothing installed from this project (or even the original Hello
# World) actually ran on real hardware. This mirrors the confirmed-working
# GamePad Tester project's Makefile structure.
#
# Expects to sit alongside a TyrQuake source checkout (adjust TQ_SRC below
# to match your actual layout).
#
# Three independent choices, any combination (6 release PKGs):
#
#   RENDERER=gl (default)  OpenGL: TyrQuake's GL renderer (GLQuake) drawn
#                          by the RSX through ps3gl (common/gl/)
#   RENDERER=soft          the software renderer, drawn by the CPU
#   MISSIONPACK=hipnotic / rogue
#   DEBUG_BUILD=1
#
#   make                                 -> TyrQuake.gnpdrm.pkg
#   make MISSIONPACK=hipnotic            -> TyrQuakeHipnotic.gnpdrm.pkg
#   make MISSIONPACK=rogue               -> TyrQuakeRogue.gnpdrm.pkg
#   make RENDERER=soft                   -> TyrQuakeSW.gnpdrm.pkg
#   make RENDERER=soft MISSIONPACK=...   -> TyrQuakeHipnoticSW / TyrQuakeRogueSW
#
# Both renderers of a game are the SAME game for the PS3 (same APPID,
# title and icon): installing one replaces the other, and they share the
# game data, config.cfg and saves.
#
# Every variant builds in its own folder (obj/<target>, build/<target>):
# switching variants needs no "make clean" any more.
#
include /usr/local/ps3dev/ppu_rules

MISSIONPACK ?=
DEBUG_BUILD ?=
RENDERER ?= gl

ifeq ($(MISSIONPACK),hipnotic)
    BASE_TARGET := TyrQuakeHipnotic
    BASE_TITLE  := TyrQuakeCell (Scourge of Armagon)
    BASE_APPID  := TYRQHIP01
    # Both defines are required together -- confirmed by reading
    # COM_InitGameDirectoryFromCommandLine (common.c): "-hipnotic" alone
    # only sets a local game_type variable, but the function bails out
    # (returns without ever calling COM_InitGameDirectory -- meaning the
    # hipnotic/ search path never gets added AND the QuakeC-visible
    # "hipnotic" global never gets set true, so even the new weapons/
    # monsters wouldn't activate) unless "-game <dir>" is ALSO present.
    # The actual directory name passed to -game is ignored by
    # COM_InitGameDirectory's HIPNOTIC/ROGUE switch cases (they hardcode
    # "hipnotic"/"rogue" regardless), so its value doesn't matter --
    # just that -game is present at all.
    CFLAGS      += -DTYRQUAKE_MISSIONPACK_ARG='"-hipnotic"' -DTYRQUAKE_MISSIONPACK_NAME='"hipnotic"'
    # Mission pack variants don't bundle anything -- they run against
    # the base game's already-installed data (same basedir, hardcoded in
    # sys_ps3.c to TYRQ00001's USRDIR regardless of which .pkg is
    # running), so anything bundled here would install under THIS
    # package's own folder (TYRQHIP01), which the game never reads. The
    # hipnotic/ data goes under the shared TYRQ00001 folder instead (the
    # base PKG creates hipnotic/ and hipnotic/music/ there).
    PKGFILES  :=
    ICON0     := $(CURDIR)/ICON0_Hipnotic.PNG
else ifeq ($(MISSIONPACK),rogue)
    BASE_TARGET := TyrQuakeRogue
    BASE_TITLE  := TyrQuakeCell (Dissolution of Eternity)
    BASE_APPID  := TYRQROG01
    CFLAGS      += -DTYRQUAKE_MISSIONPACK_ARG='"-rogue"' -DTYRQUAKE_MISSIONPACK_NAME='"rogue"'
    # Same reasoning as hipnotic's PKGFILES above.
    PKGFILES  :=
    ICON0     := $(CURDIR)/ICON0_Rogue.PNG
else
    BASE_TARGET := TyrQuake
    BASE_TITLE  := TyrQuakeCell
    BASE_APPID  := TYRQ00001
    PKGFILES  := $(CURDIR)/pkgfiles
    ICON0     := $(CURDIR)/ICON0.PNG
endif

# RENDERER only changes the file names, never what the PS3 sees (APPID,
# title, icon): the two renderers are one game.
ifeq ($(RENDERER),soft)
    RENDERER_SUFFIX := SW
else ifeq ($(RENDERER),gl)
    RENDERER_SUFFIX :=
else
    $(error RENDERER must be gl or soft)
endif

# DEBUG_BUILD is a separate axis on top of the others: a separate .pkg
# (own APPID, so it installs alongside the release build rather than
# overwriting it) with the testing/debugging tools this project
# accumulated during development -- noclip, godmode, the L1+R1 level-skip
# shortcut, verbose diagnostic PS3_Log calls, the r_speeds/r_dspeeds
# render-stage profiling (software) and the fps/timing log every 10 s. None
# of that ships in the default (release) build; see TYRQUAKE_DEBUG_BUILD's
# #ifdef sites in in_ps3.c, vid_ps3_sw.c, vid_ps3_common.c and (shared engine
# code) common/screen.c and common/menu.c for exactly what's gated.
ifeq ($(DEBUG_BUILD),1)
    TARGET := $(BASE_TARGET)$(RENDERER_SUFFIX)Debug
    TITLE  := $(BASE_TITLE) Debug
    APPID  := TYRQDBG01
    CFLAGS += -DTYRQUAKE_DEBUG_BUILD
else
    TARGET := $(BASE_TARGET)$(RENDERER_SUFFIX)
    TITLE  := $(BASE_TITLE)
    APPID  := $(BASE_APPID)
endif

CONTENTID := UP0001-$(APPID)_00-0000000000000000

# ppu_rules packs the PKG in $(BUILDDIR)/pkg and never empties it: one per
# variant, or a mission pack PKG would carry the base game's folders.
BUILDDIR := $(CURDIR)/build/$(TARGET)
OBJDIR   := $(CURDIR)/obj/$(TARGET)

CC := ppu-gcc
LD := ppu-gcc

# NOTE: on Ren's machine the tyrquake-0.71 tarball was extracted with its
# contents placed directly inside tyrquake/ (no nested tyrquake-0.71/
# folder) -- i.e. ../tyrquake/Makefile, ../tyrquake/NQ/, ../tyrquake/common/
# all exist directly. If you extract the tarball preserving its own
# top-level folder instead, this needs to be ../tyrquake/tyrquake-0.71.
TQ_SRC := ../tyrquake

CFLAGS += -DNQ_HACK -DTYRQUAKE_PS3
# buildinfo.c wants these two -- normally auto-generated by tyrquake's own
# Makefile (git describe + build timestamp). We don't need anything fancy,
# just valid values so it compiles.
CFLAGS += -DTYR_VERSION=0.71 -DTYR_VERSION_TIME=0LL
# GLQUAKE is used as `#ifdef GLQUAKE` throughout (presence, not value):
# defined, the GL renderer (GL_OBJS); not defined, the software one.
ifeq ($(RENDERER),gl)
CFLAGS += -DGLQUAKE
endif
CFLAGS += -Icommon/compat -I$(TQ_SRC)/include -I$(TQ_SRC)/NQ -I$(TQ_SRC)/external -Icommon
# PSL1GHT SDK headers (rsx/, sysutil/, lv2/, etc) -- ppu_rules already
# exports this path in LIBPSL1GHT_INC, we just weren't using it yet.
CFLAGS += $(LIBPSL1GHT_INC)
CFLAGS += -Wall -Wno-trigraphs -Wwrite-strings -std=gnu99
CFLAGS += -O3
# NOTE: no -DUSE_X86_ASM -- PPU obviously can't use the x86 .S rasterizer
# files (d_draw.S etc). We're using the plain-C SW_OBJS list only.

# Confirmed working combination on real PS3 hardware, per
# dragonfly-quake-ps3's cmake/ps3/README.md.
LIBS := gcm_sys rsx sysutil io audio rt lv2 m

# --- Source lists (see project notes for how these were derived from
#     tyrquake's own Makefile OBJS groups: COMMON_OBJS, CL_OBJS, SV_OBJS,
#     NQCL_OBJS, SW_OBJS, GL_OBJS) ---

COMMON_SRCS := \
	buildinfo.c cmd.c common.c crc.c cvar.c mathlib.c model.c \
	rb_tree.c shell.c zone.c \
	alias_model.c cd_common.c cl_input.c console.c developer.c \
	keys.c menu.c pcx.c r_lerp.c r_efrag.c r_light.c r_model.c \
	r_part.c screen.c snd_dma.c snd_mem.c snd_mix.c snd_music.c \
	sprite_model.c vid_mode.c wad.c \
	pr_cmds.c pr_edict.c pr_exec.c sv_move.c sv_phys.c world.c \
	cd_null.c

SW_SRCS := \
	d_edge.c d_fill.c d_init.c d_modech.c d_part.c d_polyse.c \
	d_scan.c d_sky.c d_sprite.c d_surf.c d_vars.c draw.c \
	r_aclip.c r_alias.c r_bsp.c r_draw.c r_edge.c r_main.c \
	r_misc.c r_sky.c r_sprite.c r_surf.c r_vars.c

GL_SRCS := \
	drawhulls.c gl_draw.c gl_extensions.c gl_fog.c gl_mesh.c \
	gl_model.c gl_rmain.c gl_rmisc.c gl_rsurf.c gl_sky.c \
	gl_textures.c gl_warp.c qpic.c tga.c

NQ_SRCS := \
	cl_demo.c cl_main.c cl_parse.c cl_tent.c sbar.c view.c \
	sv_main.c sv_user.c chase.c host.c host_cmd.c \
	net_common.c net_loop.c net_main.c net_none.c
# NOTE: net_dgrm.c (real UDP multiplayer) deliberately excluded -- needs
# POSIX socket headers (arpa/inet.h) not present in this toolchain, and we
# only need single-player, which net_none.c + net_loop.c fully cover (see
# NQ/net_none.c: it registers only the Loopback driver, zero real network
# drivers -- that's the standard "no networking" configuration, not a
# workaround).

# ps3_video.c: RSX init, flips and the scaled blit to the TV (from
# Quake2PS3), vid_ps3_common.c: resolutions and Video Settings, shared by
# both renderers' drivers.
PS3_SRCS := in_ps3.c snd_ps3.c sys_ps3.c ps3_video.c vid_ps3_common.c

ifeq ($(RENDERER),gl)
RENDER_SRCS := $(GL_SRCS) vid_ps3_gl.c \
	ps3gl_main.c ps3gl_state.c ps3gl_texture.c ps3gl_draw.c
else
RENDER_SRCS := $(SW_SRCS) vid_ps3_sw.c
endif

vpath %.c $(TQ_SRC)/common $(TQ_SRC)/NQ common common/gl

SRCS := $(COMMON_SRCS) $(NQ_SRCS) $(PS3_SRCS) $(RENDER_SRCS)
OBJS := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

all: $(TARGET).pkg

$(OBJDIR):
	@mkdir -p $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Headers too: a changed .h rebuilds whatever includes it.
-include $(OBJS:.o=.d)

# The file that talks to GCM the most goes at -O1 -fno-inline, like in
# Quake2PS3 (Doom64-PS3's lesson: PSL1GHT's syscall stubs don't survive
# aggressive inlining).
$(OBJDIR)/ps3_video.o: CFLAGS := $(filter-out -O3,$(CFLAGS)) -O1 -fno-inline

$(TARGET).elf: $(OBJS)
	$(LD) $(OBJS) $(LIBPSL1GHT_LIB) -o $(TARGET).elf $(foreach lib,$(LIBS),-l$(lib))

# $(TARGET).self and $(TARGET).pkg are NOT defined here -- they come from
# ppu_rules' own %.self: %.elf and %.pkg: %.self pattern rules (see the
# big comment at the top of this file). Those rules also pick up
# $(PKGFILES) automatically.
#
# The folders the base PKG creates on the PS3 are made here at build time:
# they are empty (the player fills them by FTP), and git doesn't keep empty
# folders. pkg.py packs empty folders too.
PKG_DIRS := USRDIR/id1/music USRDIR/hipnotic/music USRDIR/rogue/music

.PHONY: all clean pkgdirs
pkgdirs:
ifneq ($(PKGFILES),)
	@for d in $(PKG_DIRS); do mkdir -p "$(PKGFILES)/$$d"; done
endif

$(TARGET).pkg: | pkgdirs

clean:
	# Only intermediate build files, every variant's (object files,
	# .elf/.self, the PKG staging folders). Deliberately does NOT touch
	# any .pkg: ppu_rules' own %.pkg rule already regenerates one fresh
	# whenever its .self prerequisite is newer (which removing .self here
	# guarantees), so there's no correctness reason to delete them -- and
	# deleting them here was actively harmful in 1.x ("make clean && make
	# DEBUG_BUILD=1" right after building the release variant deleted the
	# release .pkg).
	rm -rf obj build
	rm -f *.o TyrQuake*.elf TyrQuake*.self TyrQuake*.fake.self
