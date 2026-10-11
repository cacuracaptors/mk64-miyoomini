/*
 * Miyoo Mini (Plus) audio output, through the SoC's own audio output (MI_AO),
 * as in the Hexen II and OpenLara ports (SDL/ALSA on this device accept rates
 * they do not honour). Keeps the DC symbol name audio_dc so src/main.c needs no
 * change.
 *
 * The game mixes one audio frame per 60 Hz tick (port_audio_pump in src/main.c):
 * 448 stereo samples, i.e. 26880 samples a second, into play() below. A thread of
 * our own takes them from a ring, raises the rate to 44100 Hz with linear
 * interpolation and sends blocks to MI_AO. The tick comes from the panel, whose
 * rate is not exactly the sound chip's, so the step is nudged (at most 0.5%,
 * inaudible) to keep the ring near its target fill instead of running dry or
 * overflowing now and then.
 *
 * -mudo: no sound (the engine still runs).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include "dcaudio/audio_api.h"
#include "port_miyoo.h"

#if __has_include(<mi_ao.h>)
#include <mi_sys.h>
#include <mi_ao.h>
#define HAVE_MI_AO 1
#endif

#define SRC_RATE 26880       /* 448 samples per 60 Hz tick */
#define OUT_RATE 44100
#define OUT_FRAMES 512       /* frames per block sent to MI_AO (11.6 ms) */
#define OUT_RING 8           /* blocks, so a block is never rewritten while MI_AO may read it */
#define RING_FRAMES 8192     /* source frames (0.3 s) */
#define TARGET_FILL 1344     /* 3 game frames (50 ms) kept in the ring */

typedef struct { int16_t l, r; } frame_t;

static frame_t ring[RING_FRAMES];
static volatile uint32_t ring_w, ring_r;   /* frame counters (index = counter % RING_FRAMES) */
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int muted;
static int started, running;

#ifdef HAVE_MI_AO
static struct {
    MI_S32 (*SysInit)(void);
    MI_S32 (*SetDepth)(MI_SYS_ChnPort_t *, MI_U32, MI_U32);
    MI_S32 (*SetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *);
    MI_S32 (*GetPubAttr)(MI_AUDIO_DEV, MI_AUDIO_Attr_t *);
    MI_S32 (*Enable)(MI_AUDIO_DEV);
    MI_S32 (*Disable)(MI_AUDIO_DEV);
    MI_S32 (*EnableChn)(MI_AUDIO_DEV, MI_AO_CHN);
    MI_S32 (*DisableChn)(MI_AUDIO_DEV, MI_AO_CHN);
    MI_S32 (*SendFrame)(MI_AUDIO_DEV, MI_AO_CHN, MI_AUDIO_Frame_t *, MI_S32);
    MI_S32 (*QueryChnStat)(MI_AUDIO_DEV, MI_AO_CHN, MI_AO_ChnState_t *);
    MI_S32 (*SetVolume)(MI_AUDIO_DEV, MI_S32);
} ao;

static MI_AUDIO_Attr_t ao_attr;
static pthread_t ao_thread;
static frame_t ao_blocks[OUT_RING][OUT_FRAMES];
static uint32_t phase;   /* position between the current source frame and the next, 16.16 */

static void *open_lib(const char *name) {
    static const char *dirs[] = { "", "/customer/lib/", "/config/lib/", "/lib/", "/usr/lib/" };
    char path[96];
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (h)
            return h;
    }
    return NULL;
}

/* one output block from the ring (silence until the ring first reaches its target, and
   when it runs dry) */
static void fill_block(frame_t *out) {
    static int primed;
    pthread_mutex_lock(&ring_mutex);
    uint32_t r = ring_r;
    const uint32_t w = ring_w;
    int32_t fill = (int32_t) (w - r);
    if (!primed && fill >= TARGET_FILL)
        primed = 1;
    if (!primed || muted || fill < 2) {
        if (fill < 2)
            primed = 0;
        pthread_mutex_unlock(&ring_mutex);
        memset(out, 0, OUT_FRAMES * sizeof(frame_t));
        return;
    }
    /* step: SRC_RATE / OUT_RATE, nudged by how far the ring is from its target */
    double adj = (double) (fill - TARGET_FILL) / TARGET_FILL * 0.005;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    const uint32_t step = (uint32_t) ((double) SRC_RATE / OUT_RATE * (1.0 + adj) * 65536.0);
    for (int i = 0; i < OUT_FRAMES; i++) {
        if ((int32_t) (w - r) < 2) {   /* ran dry inside the block: hold the last frame */
            out[i] = i ? out[i - 1] : ring[r % RING_FRAMES];
            continue;
        }
        const frame_t a = ring[r % RING_FRAMES], b = ring[(r + 1) % RING_FRAMES];
        const int32_t f = (int32_t) (phase & 0xFFFF);
        out[i].l = (int16_t) (a.l + (((int32_t) b.l - a.l) * f >> 16));
        out[i].r = (int16_t) (a.r + (((int32_t) b.r - a.r) * f >> 16));
        phase += step;
        r += phase >> 16;
        phase &= 0xFFFF;
    }
    ring_r = r;
    pthread_mutex_unlock(&ring_mutex);
}

static void *ao_loop(void *arg) {
    (void) arg;
    int slot = 0, logs = 0;
    while (running) {
        frame_t *buf = ao_blocks[slot];
        slot = (slot + 1) % OUT_RING;
        fill_block(buf);

        MI_AUDIO_Frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.eBitwidth = ao_attr.eBitwidth;
        frame.eSoundmode = ao_attr.eSoundmode;
        frame.u32Len = OUT_FRAMES * sizeof(frame_t);
        frame.apVirAddr[0] = buf;
        frame.apVirAddr[1] = NULL;

        /* wait for room: keep only a few blocks queued (filling MI_AO's queue to the
           brim made it overwrite audio not yet played, in the Hexen II port) */
        while (running) {
            MI_AO_ChnState_t st;
            memset(&st, 0, sizeof(st));
            if (ao.QueryChnStat(0, 0, &st) != MI_SUCCESS)
                break;
            const MI_U32 total = st.u32ChnFreeNum + st.u32ChnBusyNum;
            const MI_U32 target = total > 64 ? frame.u32Len * 3 : 3;   /* bytes or blocks */
            if (st.u32ChnBusyNum <= target)
                break;
            usleep(2000);
        }
        if (ao.SendFrame(0, 0, &frame, 20) != MI_SUCCESS && logs < 5) {
            fprintf(stderr, "som: o MI_AO recusou um bloco\n");
            logs++;
        }
    }
    return NULL;
}

static bool ao_start(void) {
    void *hs = open_lib("libmi_sys.so"), *ha = open_lib("libmi_ao.so");
    if (!hs || !ha) {
        printf("som: sem libmi_sys/libmi_ao, sem som\n");
        return false;
    }
    ao.SysInit = dlsym(hs, "MI_SYS_Init");
    ao.SetDepth = dlsym(hs, "MI_SYS_SetChnOutputPortDepth");
    ao.SetPubAttr = dlsym(ha, "MI_AO_SetPubAttr");
    ao.GetPubAttr = dlsym(ha, "MI_AO_GetPubAttr");
    ao.Enable = dlsym(ha, "MI_AO_Enable");
    ao.Disable = dlsym(ha, "MI_AO_Disable");
    ao.EnableChn = dlsym(ha, "MI_AO_EnableChn");
    ao.DisableChn = dlsym(ha, "MI_AO_DisableChn");
    ao.SendFrame = dlsym(ha, "MI_AO_SendFrame");
    ao.QueryChnStat = dlsym(ha, "MI_AO_QueryChnStat");
    ao.SetVolume = dlsym(ha, "MI_AO_SetVolume");
    if (!ao.SetPubAttr || !ao.GetPubAttr || !ao.Enable || !ao.Disable || !ao.EnableChn || !ao.DisableChn ||
        !ao.SendFrame || !ao.QueryChnStat) {
        printf("som: MI_AO incompleto, sem som\n");
        return false;
    }
    if (ao.SysInit)
        ao.SysInit();

    MI_AUDIO_Attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.u32FrmNum = 6;
    attr.u32PtNumPerFrm = OUT_FRAMES;
    attr.u32ChnCnt = 2;
    attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
    attr.eSamplerate = (MI_AUDIO_SampleRate_e) OUT_RATE;
    if (ao.SetPubAttr(0, &attr) != MI_SUCCESS || ao.GetPubAttr(0, &ao_attr) != MI_SUCCESS) {
        printf("som: MI_AO nao aceitou o formato, sem som\n");
        return false;
    }
    if (ao.Enable(0) != MI_SUCCESS) {
        printf("som: MI_AO nao ligou, sem som\n");
        return false;
    }
    if (ao.EnableChn(0, 0) != MI_SUCCESS) {
        printf("som: canal do MI_AO nao ligou, sem som\n");
        ao.Disable(0);
        return false;
    }
    if (ao.SetVolume)
        ao.SetVolume(0, 0);
    if (ao.SetDepth) {
        MI_SYS_ChnPort_t port;
        memset(&port, 0, sizeof(port));
        port.eModId = E_MI_MODULE_ID_AO;
        port.u32DevId = 0;
        port.u32ChnId = 0;
        port.u32PortId = 0;
        ao.SetDepth(&port, 12, 13);
    }
    running = 1;
    if (pthread_create(&ao_thread, NULL, ao_loop, NULL) != 0) {
        running = 0;
        ao.DisableChn(0, 0);
        ao.Disable(0);
        printf("som: sem thread de som, sem som\n");
        return false;
    }
    printf("som: MI_AO %d Hz estereo\n", OUT_RATE);
    return true;
}
#endif

static bool am_init(void) {
    if (port_opt_mute) {
        printf("som: desligado (-mudo)\n");
        return true;
    }
#ifdef HAVE_MI_AO
    started = ao_start();
#else
    printf("som: compilado sem mi_ao.h, sem som\n");
#endif
    return true;
}

static int am_buffered(void) {
    return (int) (ring_w - ring_r);
}

static int am_get_desired_buffered(void) {
    return TARGET_FILL;
}

/* 16-bit stereo frames from the game; when the ring is full the oldest are dropped */
static void am_play(uint8_t *buf, size_t len) {
    if (!started)
        return;
    const frame_t *in = (const frame_t *) buf;
    const size_t n = len / sizeof(frame_t);
    pthread_mutex_lock(&ring_mutex);
    uint32_t w = ring_w;
    for (size_t i = 0; i < n; i++)
        ring[(w + i) % RING_FRAMES] = in[i];
    w += (uint32_t) n;
    if ((int32_t) (w - ring_r) > RING_FRAMES - 64)
        ring_r = w - (RING_FRAMES - 64);
    ring_w = w;
    pthread_mutex_unlock(&ring_mutex);
}

struct AudioAPI audio_dc = { am_init, am_buffered, am_get_desired_buffered, am_play };

void mute_stream(void) {
    muted = 1;
}

void unmute_stream(void) {
    muted = 0;
}
