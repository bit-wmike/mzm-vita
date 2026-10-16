/*
 * PS Vita platform layer of the Metroid: Zero Mission port.
 *
 * Video: vita2d, the 240x160 frame scaled to the full screen height
 *        ("Sharp": integer pre-scale, then smooth down-scale).
 * Audio: a thread feeding sceAudioOut from a ring buffer (48 kHz stereo).
 * Input: buttons and left stick mapped to the GBA keys.
 * Files: ux0:data/mzm/ - the ROM (any *.gba of the European version), the
 *        save (mzm_eu.sav, 32 KiB, compatible with GBA emulators) and log.txt.
 */
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include "port/port.h"
#include "gba/keys.h"

#define DATA_DIR "ux0:data/mzm"
#define SAVE_PATH DATA_DIR "/mzm_eu.sav"
#define LOG_PATH DATA_DIR "/log.txt"
#define SAVE_SIZE 0x8000

unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
int _newlib_heap_size_user = 192 * 1024 * 1024;

/* ---------------------------------------------------------------- log */

static FILE *sLog;

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    if (!sLog)
        return;
    va_start(ap, fmt);
    vfprintf(sLog, fmt, ap);
    va_end(ap);
    fflush(sLog);
}

/* -------------------------------------------------------------- video */

static vita2d_texture *sTex;
static vita2d_texture *sSharp;
static vita2d_pgf *sFont;

#define SHARP_K 3

static void VideoInit(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_set_vblank_wait(1);
    sTex = vita2d_create_empty_texture_format(PORT_GBA_W, PORT_GBA_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sSharp = vita2d_create_empty_texture_rendertarget(PORT_GBA_W * SHARP_K, PORT_GBA_H * SHARP_K,
                                                       SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    sFont = vita2d_load_default_pgf();
}

/* Frame timing for the log: work time = game logic + rendering of a frame. */
static SceUInt64 sLastPresentEnd;
static SceUInt64 sWorkSum, sWorkMax;
static int sWorkFrames, sSlowFrames;

static void PerfFrame(SceUInt64 start)
{
    if (sLastPresentEnd) {
        SceUInt64 work = start - sLastPresentEnd;
        sWorkSum += work;
        if (work > sWorkMax)
            sWorkMax = work;
        if (work > 16000)
            sSlowFrames++;
        if (++sWorkFrames == 300) {
            Platform_Log("[perf] frame work avg %.2f ms, max %.2f ms, >16 ms: %d of 300\n",
                         sWorkSum / 300.0 / 1000.0, sWorkMax / 1000.0, sSlowFrames);
            sWorkSum = sWorkMax = 0;
            sWorkFrames = sSlowFrames = 0;
        }
    }
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch)
{
    uint8_t *dst;
    unsigned stride;
    int y;
    float scale = 544.0f / height;
    float dx = (960.0f - width * scale) / 2.0f;

    PerfFrame(sceKernelGetProcessTimeWide());
    vita2d_wait_rendering_done();
    dst = (uint8_t *)vita2d_texture_get_datap(sTex);
    stride = vita2d_texture_get_stride(sTex);
    for (y = 0; y < height; y++)
        memcpy(dst + y * stride, pixels + y * pitch, (size_t)width * 4);

    /* Sharp: nearest-neighbour to 3x, then a smooth scale to the screen. */
    vita2d_pool_reset();
    vita2d_start_drawing_advanced(sSharp, SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY);
    vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    vita2d_draw_texture_scale(sTex, 0, 0, (float)SHARP_K, (float)SHARP_K);
    vita2d_end_drawing();

    vita2d_start_drawing_advanced(NULL, SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY);
    vita2d_clear_screen();
    vita2d_texture_set_filters(sSharp, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_draw_texture_scale(sSharp, dx, 0, scale / SHARP_K, scale / SHARP_K);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    sLastPresentEnd = sceKernelGetProcessTimeWide();
}

static void ShowMessage(const char *title, const char *text)
{
    for (;;) {
        SceCtrlData pad;
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_pgf_draw_text(sFont, 40, 80, RGBA8(255, 120, 60, 255), 1.4f, title);
        vita2d_pgf_draw_text(sFont, 40, 140, RGBA8(230, 230, 230, 255), 1.0f, text);
        vita2d_pgf_draw_text(sFont, 40, 500, RGBA8(150, 150, 150, 255), 0.9f, "START: beenden / quit");
        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START)
            break;
    }
}

/* -------------------------------------------------------------- audio */

#define AUDIO_GRAIN 256
#define RING_FRAMES 8192 /* stereo frames, power of two */

static int16_t sRing[RING_FRAMES * 2];
static volatile uint32_t sRingWrite, sRingRead;
static int sAudioPort = -1;

void Platform_SubmitAudio(const int16_t *samples, int frames)
{
    int i;
    uint32_t w = sRingWrite;
    for (i = 0; i < frames; i++) {
        if (w - sRingRead >= RING_FRAMES - 1)
            break; /* full: drop the rest */
        sRing[(w & (RING_FRAMES - 1)) * 2] = samples[i * 2];
        sRing[(w & (RING_FRAMES - 1)) * 2 + 1] = samples[i * 2 + 1];
        w++;
    }
    sRingWrite = w;
}

static int AudioThread(SceSize args, void *argp)
{
    static int16_t buf[AUDIO_GRAIN * 2];
    (void)args; (void)argp;
    for (;;) {
        uint32_t r = sRingRead;
        int i;
        for (i = 0; i < AUDIO_GRAIN; i++) {
            if (r == sRingWrite) {
                buf[i * 2] = buf[i * 2 + 1] = 0; /* underrun */
            } else {
                buf[i * 2] = sRing[(r & (RING_FRAMES - 1)) * 2];
                buf[i * 2 + 1] = sRing[(r & (RING_FRAMES - 1)) * 2 + 1];
                r++;
            }
        }
        sRingRead = r;
        sceAudioOutOutput(sAudioPort, buf);
    }
    return 0;
}

static void AudioInit(void)
{
    SceUID th;
    sAudioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, PORT_AUDIO_RATE,
                                     SCE_AUDIO_OUT_MODE_STEREO);
    if (sAudioPort < 0) {
        Platform_Log("audio: open port failed 0x%08x\n", sAudioPort);
        return;
    }
    {
        int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
        sceAudioOutSetVolume(sAudioPort, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    }
    th = sceKernelCreateThread("mzm_audio", AudioThread, 0x10000100, 0x10000, 0, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
}

/* -------------------------------------------------------------- input */

uint16_t Platform_PollKeys(void)
{
    SceCtrlData pad;
    uint16_t keys = 0;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_CROSS) keys |= KEY_A;    /* jump */
    if (pad.buttons & SCE_CTRL_SQUARE) keys |= KEY_B;   /* shoot */
    if (pad.buttons & SCE_CTRL_CIRCLE) keys |= KEY_B;
    if (pad.buttons & SCE_CTRL_LTRIGGER) keys |= KEY_L;
    if (pad.buttons & SCE_CTRL_RTRIGGER) keys |= KEY_R;
    if (pad.buttons & SCE_CTRL_START) keys |= KEY_START;
    if (pad.buttons & SCE_CTRL_SELECT) keys |= KEY_SELECT;
    if ((pad.buttons & SCE_CTRL_UP) || pad.ly < 128 - 64) keys |= KEY_UP;
    if ((pad.buttons & SCE_CTRL_DOWN) || pad.ly > 128 + 64) keys |= KEY_DOWN;
    if ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 128 - 64) keys |= KEY_LEFT;
    if ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 128 + 64) keys |= KEY_RIGHT;
    return keys;
}

/* -------------------------------------------------------------- files */

int Platform_LoadSram(void *data, size_t size)
{
    SceUID fd = sceIoOpen(SAVE_PATH, SCE_O_RDONLY, 0);
    int n;
    if (fd < 0)
        return 0;
    memset(data, 0xFF, size);
    n = sceIoRead(fd, data, size < SAVE_SIZE ? size : SAVE_SIZE);
    sceIoClose(fd);
    return n > 0;
}

void Platform_SaveSram(const void *data, size_t size)
{
    SceUID fd = sceIoOpen(DATA_DIR "/mzm_eu.sav.tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        Platform_Log("save: cannot write (0x%08x)\n", fd);
        return;
    }
    sceIoWrite(fd, data, size < SAVE_SIZE ? size : SAVE_SIZE);
    sceIoClose(fd);
    sceIoRemove(SAVE_PATH);
    sceIoRename(DATA_DIR "/mzm_eu.sav.tmp", SAVE_PATH);
    Platform_Log("save: written\n");
}

/* Finds the European ROM among the .gba files in ux0:data/mzm/. */
static unsigned char *LoadRom(size_t *outSize, int *foundAny)
{
    SceUID dir = sceIoDopen(DATA_DIR);
    SceIoDirent ent;
    unsigned char *rom = NULL;

    *foundAny = 0;
    if (dir < 0)
        return NULL;
    while (sceIoDread(dir, &ent) > 0) {
        size_t len = strlen(ent.d_name);
        char path[512];
        SceUID fd;
        if (len < 4 || strcasecmp(ent.d_name + len - 4, ".gba") != 0)
            continue;
        *foundAny = 1;
        snprintf(path, sizeof(path), DATA_DIR "/%s", ent.d_name);
        if (ent.d_stat.st_size != 8 * 1024 * 1024) {
            Platform_Log("rom: %s has the wrong size\n", ent.d_name);
            continue;
        }
        rom = (unsigned char *)malloc(8 * 1024 * 1024);
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd >= 0 && rom) {
            int n = sceIoRead(fd, rom, 8 * 1024 * 1024);
            sceIoClose(fd);
            if (n == 8 * 1024 * 1024 && Port_RomMatches(rom, (size_t)n)) {
                Platform_Log("rom: using %s\n", ent.d_name);
                *outSize = (size_t)n;
                break;
            }
            Platform_Log("rom: %s is not Metroid - Zero Mission (Europe)\n", ent.d_name);
        }
        free(rom);
        rom = NULL;
    }
    sceIoDclose(dir);
    return rom;
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    unsigned char *rom;
    size_t romSize = 0;
    int foundAny;

    (void)argc; (void)argv;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceIoMkdir(DATA_DIR, 0777);
    sceIoRemove(DATA_DIR "/log_prev.txt");
    sceIoRename(LOG_PATH, DATA_DIR "/log_prev.txt");
    sLog = fopen(LOG_PATH, "w");
    Platform_Log("Metroid Zero Mission Vita %s\n", MZM_PORT_VERSION);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    VideoInit();

    rom = LoadRom(&romSize, &foundAny);
    if (!rom) {
        ShowMessage("ROM nicht gefunden / ROM not found",
                    foundAny ? "Die .gba-Datei in ux0:data/mzm/ ist nicht Metroid - Zero Mission (Europa).\n"
                               "The .gba file in ux0:data/mzm/ is not Metroid - Zero Mission (Europe)."
                             : "Kopiere deine ROM (Metroid - Zero Mission, Europa) nach ux0:data/mzm/\n"
                               "Copy your ROM (Metroid - Zero Mission, Europe) to ux0:data/mzm/");
        sceKernelExitProcess(0);
        return 0;
    }
    if (Port_LoadAssets(rom, romSize) != 0) {
        ShowMessage("Fehler / Error", "Die Spieldaten konnten nicht geladen werden.\nGame data could not be loaded.");
        sceKernelExitProcess(0);
        return 0;
    }
    free(rom);

    AudioInit();
    Port_Init();
    Port_RunGame();

    sceKernelExitProcess(0);
    return 0;
}
