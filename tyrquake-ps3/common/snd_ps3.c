// snd_ps3.c -- PS3 native audio backend using PSL1GHT libaudio.
//
// The libaudio API calls and the dedicated-audio-thread pattern (wait on
// an event queue for each DMA block the hardware finishes consuming,
// convert Quake's int16 ring buffer to float32, write the next block)
// are adapted from dragonfly-quake-ps3's snd_ps3.c, a confirmed-working
// PS3 Quake port. That engine's dma_t has an extra "signed8" field and a
// differently-shaped SNDDMA_Init(dma_t*) -- ours doesn't, so the struct
// setup below follows common/snd_sdl.c instead (this engine's own
// cleanest cross-platform reference) for exactly what fields TyrQuake's
// dma_t/SNDDMA_* contract actually expects.

#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "sound.h"
#include "sys.h"

#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/thread.h>
#include <string.h>
#include <stdlib.h>

extern void PS3_Log(const char *fmt, ...);

#define PS3_AUDIO_RATE 48000

static u32               audio_port = (u32)-1;
static audioPortConfig   audio_config;
static sys_event_queue_t audio_queue;
static sys_ipc_key_t     audio_queue_key;
static int               last_filled_buf;
static volatile int      audio_running = 0;
static sys_ppu_thread_t  audio_thread_id;
static qboolean          audio_initialized = false;
static dma_t             the_shm;

// Runs on its own thread (like the main game thread, this needs a real
// stack -- see sys_ps3.c's PS3_QuakeThread for why the default PS3
// thread stack is too small for anything non-trivial).
static void
PS3_AudioThread(void *arg)
{
    int num_blocks = (int)audio_config.numBlocks;
    int ch = (int)audio_config.channelCount;
    int samples_per_block = AUDIO_BLOCK_SAMPLES * ch;
    size_t block_bytes = samples_per_block * sizeof(float);
    const float scale = 1.0f / 32768.0f;

    while (audio_running) {
	// Block until the hardware has consumed a DMA block and wants
	// the next one -- this is what actually paces the whole thing to
	// real playback speed, not a fixed sleep/timer.
	sys_event_t event;
	s32 ret = sysEventQueueReceive(audio_queue, &event, 20 * 1000);
	if (ret != 0)
	    continue; // timeout or spurious wake -- just retry

	int filling = (last_filled_buf + 1) % num_blocks;
	last_filled_buf = filling;

	float *dst = (float *)(uintptr_t)audio_config.audioDataStart;
	dst += filling * AUDIO_BLOCK_SAMPLES * ch;

	if (shm && shm->buffer) {
	    int pos = shm->samplepos;
	    // shm->buffer is written by the main game thread (S_Update's
	    // mixer) and read here on the audio thread -- this barrier
	    // makes sure we see its latest writes before copying out.
	    __asm__ volatile("lwsync" ::: "memory");
	    int16_t *src = (int16_t *)shm->buffer;
	    for (int i = 0; i < samples_per_block; i++) {
		int idx = (pos + i) % shm->samples;
		dst[i] = (float)src[idx] * scale;
	    }
	    shm->samplepos = (pos + samples_per_block) % shm->samples;
	} else {
	    memset(dst, 0, block_bytes);
	}
    }

    sysThreadExit(0);
}

qboolean
SNDDMA_Init(void)
{
    s32 ret = audioInit();
    if (ret != 0) {
	Con_Printf("PS3 audio: audioInit failed (%d)\n", (int)ret);
	return false;
    }

    audioPortParam params;
    memset(&params, 0, sizeof(params));
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks   = AUDIO_BLOCK_8;
    params.attrib      = 0;
    params.level       = 1;

    ret = audioPortOpen(&params, &audio_port);
    if (ret != 0) {
	Con_Printf("PS3 audio: audioPortOpen failed (%d)\n", (int)ret);
	audioQuit();
	return false;
    }

    ret = audioGetPortConfig(audio_port, &audio_config);
    if (ret != 0) {
	Con_Printf("PS3 audio: audioGetPortConfig failed (%d)\n", (int)ret);
	audioPortClose(audio_port);
	audioQuit();
	return false;
    }

    ret = audioCreateNotifyEventQueue(&audio_queue, &audio_queue_key);
    if (ret != 0) {
	Con_Printf("PS3 audio: audioCreateNotifyEventQueue failed (%d)\n", (int)ret);
	audioPortClose(audio_port);
	audioQuit();
	return false;
    }

    ret = audioSetNotifyEventQueue(audio_queue_key);
    if (ret != 0) {
	Con_Printf("PS3 audio: audioSetNotifyEventQueue failed (%d)\n", (int)ret);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
	return false;
    }

    sysEventQueueDrain(audio_queue);

    ret = audioPortStart(audio_port);
    if (ret != 0) {
	Con_Printf("PS3 audio: audioPortStart failed (%d)\n", (int)ret);
	audioRemoveNotifyEventQueue(audio_queue_key);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
	return false;
    }

    last_filled_buf = (int)(audio_config.numBlocks - 1);

    // TyrQuake's own dma_t contract (see common/snd_sdl.c) -- no
    // "signed8" field, SNDDMA_Init takes no parameter, shm is a bare
    // global pointer we assign ourselves.
    shm = &the_shm;
    memset(shm, 0, sizeof(*shm));
    shm->samplebits = 16;
    shm->speed = PS3_AUDIO_RATE;
    shm->channels = (int)audio_config.channelCount;
    shm->samplepos = 0;
    shm->submission_chunk = AUDIO_BLOCK_SAMPLES;
    shm->samples = AUDIO_BLOCK_SAMPLES * (int)audio_config.numBlocks * 8;

    int buffer_size = shm->samples * (shm->samplebits / 8);
    shm->buffer = Hunk_AllocName(buffer_size, "snd_ps3");
    if (!shm->buffer) {
	Con_Printf("PS3 audio: failed to allocate %d byte ring buffer\n", buffer_size);
	audioPortStop(audio_port);
	audioRemoveNotifyEventQueue(audio_queue_key);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
	shm = NULL;
	return false;
    }
    memset(shm->buffer, 0, buffer_size);

    audio_running = 1;
    ret = sysThreadCreate(&audio_thread_id, PS3_AudioThread, NULL,
			  1001, 64 * 1024, THREAD_JOINABLE, (char *)"ps3_audio");
    if (ret != 0) {
	Con_Printf("PS3 audio: sysThreadCreate failed (%d)\n", (int)ret);
	audio_running = 0;
	shm = NULL;
	audioPortStop(audio_port);
	audioRemoveNotifyEventQueue(audio_queue_key);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
	return false;
    }

    audio_initialized = true;
    PS3_Log("SNDDMA_Init: OK, %d Hz, %d ch, %d blocks, %d samples/block",
	    PS3_AUDIO_RATE, shm->channels, (int)audio_config.numBlocks, AUDIO_BLOCK_SAMPLES);
    Con_Printf("PS3 audio: %d Hz, 16-bit, %dch\n", PS3_AUDIO_RATE, shm->channels);
    return true;
}

int
SNDDMA_GetDMAPos(void)
{
    return shm ? shm->samplepos : 0;
}

int
SNDDMA_LockBuffer(void)
{
    return 0;
}

void
SNDDMA_UnlockBuffer(void)
{
}

void
SNDDMA_Submit(void)
{
    // The audio thread pulls from shm->buffer on its own schedule
    // (paced by the hardware's DMA-block-consumed events) -- nothing
    // for the main thread to push here.
}

void
SNDDMA_Shutdown(void)
{
    if (!shm)
	return;
    if (audio_initialized) {
	audio_running = 0;
	u64 exit_code = 0;
	sysThreadJoin(audio_thread_id, &exit_code);

	audioPortStop(audio_port);
	audioRemoveNotifyEventQueue(audio_queue_key);
	audioPortClose(audio_port);
	sysEventQueueDestroy(audio_queue, 0);
	audioQuit();
	audio_initialized = false;
    }
    shm = NULL;
}

void
S_BlockSound(void)
{
}

void
S_UnblockSound(void)
{
}
