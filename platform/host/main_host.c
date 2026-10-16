/*
 * Headless test runner for the port (Linux, 32-bit).
 *
 * Runs the game for a number of frames with scripted input and writes
 * screenshots and the audio output, for automated checks without a Vita.
 *
 * Environment:
 *   MZM_FRAMES=n         frames to run (default 600)
 *   MZM_OUT=dir          output directory (default ./host_out)
 *   MZM_SHOT=a,b,c       frames to save as dir/frame_<n>.ppm
 *   MZM_SHOT_EVERY=k     additionally save every k-th frame
 *   MZM_INPUT=script     "frame:keys:len,..." keys as GBA bits (hex), held len frames
 *   MZM_SRAM=file        save file (default dir/mzm.sav)
 *   MZM_VIEW=0|1|2       view mode (GBA / wide / full)
 *   MZM_QSAVE_AT=n       quick save at frame n, MZM_QLOAD_AT=n quick load
 *   MZM_CHEATS=1         all cheats on;  MZM_30FPS=1  render every 2nd frame
 *   settings.ini and quicksave.bin live in the output directory.
 *   Key bit 0x8000 in MZM_INPUT is the missile button.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>

#include "port/port.h"
#include "port/port_view.h"
#include "port/port_config.h"
#include "port/port_game.h"

static const char *sOutDir = "host_out";
static int sMaxFrames = 600;
static int sShotEvery;
static char sShotList[4096];
static FILE *sWav;
static long sWavFrames;
static char sSramPath[1024];
static int sQSaveAt = -1, sQLoadAt = -1;

struct InputEvent { int frame, len; unsigned keys; };
static struct InputEvent sInput[512];
static int sInputCount;

static int ShotWanted(int frame)
{
    char buf[32];
    char *p;
    if (sShotEvery && frame % sShotEvery == 0)
        return 1;
    snprintf(buf, sizeof(buf), ",%d,", frame);
    p = strstr(sShotList, buf);
    return p != NULL;
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight)
{
    int frame = (int)Port_FrameCounter();
    (void)zoomHeight;
    if (getenv("MZM_TIMING") && frame % 300 == 299)
        printf("host: frame %d render avg %u us\n", frame, Port_ProfileAverageUs(PORT_PROF_RENDER));
    if (ShotWanted(frame)) {
        char path[1100];
        FILE *f;
        int x, y;
        snprintf(path, sizeof(path), "%s/frame_%05d.ppm", sOutDir, frame);
        f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++) {
                    uint32_t p = pixels[y * pitch + x];
                    unsigned char rgb[3] = {(unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16)};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
        }
    }
}

uint16_t Platform_PollKeys(void)
{
    int frame = (int)Port_FrameCounter() + 1;
    uint16_t keys = 0;
    int i;
    if (frame >= sMaxFrames) {
        if (sWav) {
            long bytes = sWavFrames * 4;
            fseek(sWav, 4, SEEK_SET);
            { unsigned v = (unsigned)(36 + bytes); fwrite(&v, 4, 1, sWav); }
            fseek(sWav, 40, SEEK_SET);
            { unsigned v = (unsigned)bytes; fwrite(&v, 4, 1, sWav); }
            fclose(sWav);
        }
        printf("host: done after %d frames\n", frame);
        exit(0);
    }
    if (frame == sQSaveAt)
        Port_RequestQuickSave();
    if (frame == sQLoadAt)
        Port_RequestQuickLoad();
    for (i = 0; i < sInputCount; i++)
        if (frame >= sInput[i].frame && frame < sInput[i].frame + sInput[i].len)
            keys |= (uint16_t)sInput[i].keys;
    return keys;
}

void Platform_SubmitAudio(const int16_t *samples, int frames)
{
    if (sWav) {
        fwrite(samples, 4, (size_t)frames, sWav);
        sWavFrames += frames;
    }
}

int Platform_LoadSram(void *data, size_t size)
{
    FILE *f = fopen(sSramPath, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(data, 1, size, f);
    fclose(f);
    return n == size;
}

void Platform_SaveSram(const void *data, size_t size)
{
    FILE *f = fopen(sSramPath, "wb");
    if (!f)
        return;
    fwrite(data, 1, size, f);
    fclose(f);
}

long Platform_ReadFile(const char *name, void *data, size_t size)
{
    char path[1100];
    FILE *f;
    long n;
    snprintf(path, sizeof(path), "%s/%s", sOutDir, name);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    n = (long)fread(data, 1, size, f);
    fclose(f);
    return n;
}

int Platform_WriteFile(const char *name, const void *data, size_t size)
{
    char path[1100], tmp[1110];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", sOutDir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return rename(tmp, path) == 0 ? 0 : -1;
}

int Platform_PaceAllowsRender(void)
{
    return 1;
}

uint64_t Platform_TimeUs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void OpenWav(void)
{
    char path[1100];
    unsigned char hdr[44] = "RIFF\0\0\0\0WAVEfmt ";
    unsigned v;
    snprintf(path, sizeof(path), "%s/audio.wav", sOutDir);
    sWav = fopen(path, "wb");
    if (!sWav)
        return;
    v = 16; memcpy(hdr + 16, &v, 4);
    hdr[20] = 1; hdr[22] = 2;
    v = PORT_AUDIO_RATE; memcpy(hdr + 24, &v, 4);
    v = PORT_AUDIO_RATE * 4; memcpy(hdr + 28, &v, 4);
    hdr[32] = 4; hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, sWav);
}

static void OnCrash(int sig)
{
    void *bt[64];
    int n = backtrace(bt, 64);
    fprintf(stderr, "host: signal %d at frame %u\n", sig, (unsigned)Port_FrameCounter());
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

int main(void)
{
    signal(SIGSEGV, OnCrash);
    signal(SIGBUS, OnCrash);
    signal(SIGFPE, OnCrash);
    const char *s;
    if ((s = getenv("MZM_OUT")) != NULL)
        sOutDir = s;
    if ((s = getenv("MZM_FRAMES")) != NULL)
        sMaxFrames = atoi(s);
    if ((s = getenv("MZM_SHOT_EVERY")) != NULL)
        sShotEvery = atoi(s);
    if ((s = getenv("MZM_SHOT")) != NULL)
        snprintf(sShotList, sizeof(sShotList), ",%s,", s);
    if ((s = getenv("MZM_SRAM")) != NULL)
        snprintf(sSramPath, sizeof(sSramPath), "%s", s);
    else
        snprintf(sSramPath, sizeof(sSramPath), "%s/mzm.sav", sOutDir);
    if ((s = getenv("MZM_INPUT")) != NULL) {
        const char *p = s;
        while (*p && sInputCount < 512) {
            struct InputEvent *e = &sInput[sInputCount];
            if (sscanf(p, "%d:%x:%d", &e->frame, &e->keys, &e->len) == 3)
                sInputCount++;
            p = strchr(p, ',');
            if (!p)
                break;
            p++;
        }
    }
    {
        const char *romPath = getenv("MZM_ROM") ? getenv("MZM_ROM") : "mzm_eu_baserom.gba";
        FILE *f = fopen(romPath, "rb");
        static unsigned char rom[8 * 1024 * 1024 + 1];
        size_t n;
        if (!f) {
            fprintf(stderr, "host: cannot open ROM %s\n", romPath);
            return 1;
        }
        n = fread(rom, 1, sizeof(rom), f);
        fclose(f);
        if (!Port_RomMatches(rom, n)) {
            fprintf(stderr, "host: %s is not Metroid - Zero Mission (Europe)\n", romPath);
            return 1;
        }
        if (Port_LoadAssets(rom, n) != 0) {
            fprintf(stderr, "host: loading game data failed\n");
            return 1;
        }
    }
    if ((s = getenv("MZM_QSAVE_AT")) != NULL)
        sQSaveAt = atoi(s);
    if ((s = getenv("MZM_QLOAD_AT")) != NULL)
        sQLoadAt = atoi(s);
    OpenWav();
    Port_Init(); /* loads settings.ini */
    if ((s = getenv("MZM_VIEW")) != NULL)
        gPortViewMode = gPortConfig.viewMode = atoi(s);
    if (getenv("MZM_CHEATS"))
        gPortConfig.cheatEnergy = gPortConfig.cheatMissiles = gPortConfig.cheatSuperMissiles =
            gPortConfig.cheatPowerBombs = gPortConfig.cheatAbilities = 1;
    if (getenv("MZM_30FPS"))
        gPortConfig.frameRate30 = 1;
    Port_RunGame();
    return 0;
}
