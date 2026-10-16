/*
 * PS Vita platform layer of the Metroid: Zero Mission port.
 *
 * Video: vita2d, the frame scaled to the full screen height; styles smooth,
 *        sharp (integer pre-scale, then smooth down-scale) and pixel (integer).
 * Menu:  START + SELECT (view, display, frame rate, controls, quick save,
 *        cheats), saved to settings.ini.
 * Audio: a thread feeding sceAudioOut from a ring buffer (48 kHz stereo).
 * Input: buttons and left stick mapped to the GBA keys.
 * Files: ux0:data/mzm/ - the ROM (any *.gba of the European version), the
 *        save (mzm_eu.sav, 32 KiB, compatible with GBA emulators) and log.txt.
 */
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/touch.h>
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
#include "port/port_view.h"
#include "port/port_config.h"
#include "port/port_game.h"

#define DATA_DIR "ux0:data/mzm"
#define SAVE_PATH DATA_DIR "/mzm_eu.sav"
#define LOG_PATH DATA_DIR "/log.txt"
#define SAVE_SIZE 0x8000

unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
int _newlib_heap_size_user = 192 * 1024 * 1024;

/* ---------------------------------------------------------------- log */

static FILE *sLog;

uint64_t Platform_TimeUs(void)
{
    return sceKernelGetProcessTimeWide();
}

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
    vita2d_set_vblank_wait(0); /* paced by WaitDue() */
    sTex = vita2d_create_empty_texture_format(PORT_VIEW_MAX_W, PORT_VIEW_MAX_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    sSharp = vita2d_create_empty_texture_rendertarget(PORT_VIEW_MAX_W * SHARP_K, PORT_VIEW_MAX_H * SHARP_K,
                                                       SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    sFont = vita2d_load_default_pgf();
}

static int sCatchup; /* frames computed but not drawn (pacing) */

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
            Platform_Log("[perf] frame work avg %.2f ms (render %.2f ms), max %.2f ms, >16 ms: %d of 300, "
                         "view %dx%d, catchup %d\n",
                         sWorkSum / 300.0 / 1000.0, Port_ProfileAverageUs(PORT_PROF_RENDER) / 1000.0,
                         sWorkMax / 1000.0, sSlowFrames, Port_ViewWidth(), Port_ViewHeight(), sCatchup);
            sCatchup = 0;
            sWorkSum = sWorkMax = 0;
            sWorkFrames = sSlowFrames = 0;
        }
    }
}

/* Frame pacing: logic frame n is due at display vblank sAnchorVcount + n.
 * When the game runs late, frames are computed without drawing (at most two
 * in a row) so the game keeps its speed; after a long stall (loading, menu)
 * the schedule is re-anchored instead of catching up. */
static unsigned sAnchorVcount, sAnchorFrame;
static int sAnchored, sSkipRun;

static int FrameLate(void)
{
    unsigned due = sAnchorVcount + (Port_FrameCounter() - sAnchorFrame);
    return (int)(sceDisplayGetVcount() - due);
}

static void Reanchor(void)
{
    sAnchorVcount = sceDisplayGetVcount();
    sAnchorFrame = Port_FrameCounter();
    sAnchored = 1;
}

int Platform_PaceAllowsRender(void)
{
    int late;
    if (!sAnchored)
        Reanchor();
    late = FrameLate();
    if (late > 8) {
        Reanchor();
        late = 0;
    }
    if (late >= (gPortConfig.frameRate30 ? 3 : 2) && sSkipRun < 2) {
        sSkipRun++;
        sCatchup++;
        return 0;
    }
    sSkipRun = 0;
    return 1;
}

static void WaitDue(void)
{
    while (FrameLate() < 0)
        sceDisplayWaitVblankStart();
}

static int sLastW = 240, sLastH = 160, sLastZoom = 160;

static void DrawGame(void)
{
    int width = sLastW, height = sLastH;
    float scale = 544.0f / sLastZoom;
    float dx, dy;

    if (gPortConfig.displayStyle == PORT_STYLE_PIXEL) {
        int k = 544 / sLastZoom;
        scale = (float)(k < 1 ? 1 : k);
    }
    dx = (float)(int)((960.0f - width * scale) / 2.0f);
    dy = (float)(int)((544.0f - height * scale) / 2.0f);

    if (gPortConfig.displayStyle == PORT_STYLE_SHARP) {
        /* nearest-neighbour to 3x, then a smooth scale to the screen */
        vita2d_start_drawing_advanced(sSharp, SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY);
        vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        vita2d_draw_texture_part_scale(sTex, 0, 0, 0, 0, (float)width, (float)height, (float)SHARP_K, (float)SHARP_K);
        vita2d_end_drawing();
        vita2d_start_drawing_advanced(NULL, SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY);
        vita2d_clear_screen();
        vita2d_texture_set_filters(sSharp, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        vita2d_draw_texture_part_scale(sSharp, dx, dy, 0, 0, (float)(width * SHARP_K), (float)(height * SHARP_K),
                                       scale / SHARP_K, scale / SHARP_K);
    } else {
        SceGxmTextureFilter f = gPortConfig.displayStyle == PORT_STYLE_SMOOTH ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                                                               : SCE_GXM_TEXTURE_FILTER_POINT;
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_texture_set_filters(sTex, f, f);
        vita2d_draw_texture_part_scale(sTex, dx, dy, 0, 0, (float)width, (float)height, scale, scale);
    }
}

static void DrawToast(void)
{
    const char *msg = Port_CurrentMessage();
    if (!msg)
        return;
    vita2d_draw_rectangle(0, 500, 960, 44, RGBA8(0, 0, 0, 170));
    vita2d_pgf_draw_text(sFont, 20, 530, RGBA8(255, 255, 255, 255), 1.0f, msg);
}

void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight)
{
    uint8_t *dst;
    unsigned stride;
    int y;

    PerfFrame(sceKernelGetProcessTimeWide());
    vita2d_wait_rendering_done();
    dst = (uint8_t *)vita2d_texture_get_datap(sTex);
    stride = vita2d_texture_get_stride(sTex);
    for (y = 0; y < height; y++)
        memcpy(dst + y * stride, pixels + y * pitch, (size_t)width * 4);
    sLastW = width;
    sLastH = height;
    sLastZoom = zoomHeight;

    vita2d_pool_reset();
    DrawGame();
    DrawToast();
    vita2d_end_drawing();
    WaitDue();
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
static int16_t sHoldL, sHoldR;

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
                /* underrun: hold the last sample (no click) */
                buf[i * 2] = i ? buf[i * 2 - 2] : sHoldL;
                buf[i * 2 + 1] = i ? buf[i * 2 - 1] : sHoldR;
            } else {
                buf[i * 2] = sRing[(r & (RING_FRAMES - 1)) * 2];
                buf[i * 2 + 1] = sRing[(r & (RING_FRAMES - 1)) * 2 + 1];
                r++;
            }
        }
        sRingRead = r;
        sHoldL = buf[AUDIO_GRAIN * 2 - 2];
        sHoldR = buf[AUDIO_GRAIN * 2 - 1];
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

/* ---- menu (START + SELECT) ---- */

enum { MI_VIEW, MI_STYLE, MI_FPS, MI_MISSILE, MI_REAR, MI_QSAVE, MI_QLOAD, MI_CH_ENERGY, MI_CH_MISSILES,
       MI_CH_SUPER, MI_CH_PB, MI_CH_ABIL, MI_BACK, MI_COUNT };

static const char *OnOff(int v)
{
    return v ? "an" : "aus";
}

static void MenuLine(int item, char *buf, size_t size)
{
    static const char *views[] = {"GBA (240x160)", "Breitbild (282x160)", "Weitsicht (368x208)"};
    static const char *styles[] = {"Weich", "Scharf", "Pixelgenau"};
    switch (item) {
        case MI_VIEW: snprintf(buf, size, "Ansicht: %s", views[gPortConfig.viewMode]); break;
        case MI_STYLE: snprintf(buf, size, "Bilddarstellung: %s", styles[gPortConfig.displayStyle]); break;
        case MI_FPS: snprintf(buf, size, "Bildrate: %s", gPortConfig.frameRate30 ? "30 (Spieltempo bleibt)" : "60"); break;
        case MI_MISSILE: snprintf(buf, size, "Dreieck = Rakete abfeuern: %s", OnOff(gPortConfig.missileButton)); break;
        case MI_REAR: snprintf(buf, size, "Ruecktouch halten: links speichern, rechts laden: %s", OnOff(gPortConfig.rearTouch)); break;
        case MI_QSAVE: snprintf(buf, size, "Schnellspeichern"); break;
        case MI_QLOAD: snprintf(buf, size, "Schnellladen"); break;
        case MI_CH_ENERGY: snprintf(buf, size, "Cheat: Energie unendlich: %s", OnOff(gPortConfig.cheatEnergy)); break;
        case MI_CH_MISSILES: snprintf(buf, size, "Cheat: Raketen unendlich: %s", OnOff(gPortConfig.cheatMissiles)); break;
        case MI_CH_SUPER: snprintf(buf, size, "Cheat: Super-Raketen unendlich: %s", OnOff(gPortConfig.cheatSuperMissiles)); break;
        case MI_CH_PB: snprintf(buf, size, "Cheat: Power-Bomben unendlich: %s", OnOff(gPortConfig.cheatPowerBombs)); break;
        case MI_CH_ABIL: snprintf(buf, size, "Cheat: alle Faehigkeiten: %s", OnOff(gPortConfig.cheatAbilities)); break;
        default: snprintf(buf, size, "Zurueck zum Spiel"); break;
    }
}

static int Wrap(int v, int d, int n)
{
    return (v + d + n) % n;
}

/* Changes an item; returns 1 when the menu should close. */
static int MenuChange(int item, int d)
{
    switch (item) {
        case MI_VIEW: gPortConfig.viewMode = Wrap(gPortConfig.viewMode, d, 3); break;
        case MI_STYLE: gPortConfig.displayStyle = Wrap(gPortConfig.displayStyle, d, 3); break;
        case MI_FPS: gPortConfig.frameRate30 ^= 1; break;
        case MI_MISSILE: gPortConfig.missileButton ^= 1; break;
        case MI_REAR: gPortConfig.rearTouch ^= 1; break;
        case MI_CH_ENERGY: gPortConfig.cheatEnergy ^= 1; break;
        case MI_CH_MISSILES: gPortConfig.cheatMissiles ^= 1; break;
        case MI_CH_SUPER: gPortConfig.cheatSuperMissiles ^= 1; break;
        case MI_CH_PB: gPortConfig.cheatPowerBombs ^= 1; break;
        case MI_CH_ABIL: gPortConfig.cheatAbilities ^= 1; break;
        case MI_QSAVE:
            if (d == 0) { Port_RequestQuickSave(); return 1; }
            break;
        case MI_QLOAD:
            if (d == 0) { Port_RequestQuickLoad(); return 1; }
            break;
        case MI_BACK:
            if (d == 0) return 1;
            break;
    }
    return 0;
}

static void RunMenu(void)
{
    static int sel;
    unsigned prev = ~0u;
    char line[160];

    Platform_Log("[ui] menu open\n");
    for (;;) {
        SceCtrlData pad;
        unsigned pressed;
        int i, close = 0;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        pressed = pad.buttons & ~prev;
        prev = pad.buttons;
        if (pressed & SCE_CTRL_UP) sel = Wrap(sel, -1, MI_COUNT);
        if (pressed & SCE_CTRL_DOWN) sel = Wrap(sel, 1, MI_COUNT);
        if (pressed & SCE_CTRL_LEFT) close = MenuChange(sel, -1);
        if (pressed & SCE_CTRL_RIGHT) close = MenuChange(sel, 1);
        if (pressed & SCE_CTRL_CROSS) close = MenuChange(sel, 0) || close;
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            close = 1;
        if (pressed)
            Platform_Log("[ui] item %d buttons %08x\n", sel, pressed);

        vita2d_pool_reset();
        DrawGame();
        vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 190));
        vita2d_pgf_draw_text(sFont, 60, 50, RGBA8(255, 140, 60, 255), 1.3f, "Metroid Zero Mission Vita " MZM_PORT_VERSION);
        for (i = 0; i < MI_COUNT; i++) {
            MenuLine(i, line, sizeof(line));
            vita2d_pgf_draw_text(sFont, 80, 100 + i * 30, i == sel ? RGBA8(255, 230, 80, 255) : RGBA8(220, 220, 220, 255),
                                 1.0f, line);
        }
        vita2d_pgf_draw_text(sFont, 60, 100 + sel * 30, RGBA8(255, 230, 80, 255), 1.0f, ">");
        vita2d_pgf_draw_text(sFont, 60, 525, RGBA8(150, 150, 150, 255), 0.85f,
                             "Steuerkreuz: waehlen/aendern   X: ausfuehren   O/START: zurueck");
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
        if (close)
            break;
    }
    Port_ConfigSave();
    Platform_Log("[ui] menu closed: view=%d style=%d fps30=%d missile=%d rear=%d cheats=%d%d%d%d%d\n",
                 gPortConfig.viewMode, gPortConfig.displayStyle, gPortConfig.frameRate30, gPortConfig.missileButton,
                 gPortConfig.rearTouch, gPortConfig.cheatEnergy, gPortConfig.cheatMissiles,
                 gPortConfig.cheatSuperMissiles, gPortConfig.cheatPowerBombs, gPortConfig.cheatAbilities);
    sAnchored = 0; /* the menu stalled the game: re-anchor the pacing */
}

/* ---- input ---- */

static int sWaitRelease; /* after the menu: ignore buttons until released */
static int sRearHold, sRearSide, sRearDone;

static void RearTouch(void)
{
    SceTouchData t;
    if (!gPortConfig.rearTouch)
        return;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &t, 1) < 1 || t.reportNum == 0) {
        sRearHold = sRearDone = 0;
        return;
    }
    {
        int side = t.report[0].x < 960 ? 1 : 2; /* rear panel is 1920 wide */
        if (side != sRearSide)
            sRearHold = sRearDone = 0;
        sRearSide = side;
    }
    /* hold for about 0.75 s, so a stray touch does nothing */
    if (!sRearDone && ++sRearHold >= 45) {
        sRearDone = 1;
        if (sRearSide == 1)
            Port_RequestQuickSave();
        else
            Port_RequestQuickLoad();
    }
}

uint16_t Platform_PollKeys(void)
{
    SceCtrlData pad;
    uint16_t keys = 0;

    sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT)) {
        RunMenu();
        sWaitRelease = 1;
        return 0;
    }
    if (sWaitRelease) {
        if (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))
            return 0;
        sWaitRelease = 0;
    }
    RearTouch();

    if (pad.buttons & SCE_CTRL_CROSS) keys |= KEY_A;    /* jump */
    if (pad.buttons & SCE_CTRL_SQUARE) keys |= KEY_B;   /* shoot */
    if (pad.buttons & SCE_CTRL_CIRCLE) keys |= KEY_B;
    if (pad.buttons & SCE_CTRL_TRIANGLE) keys |= PORT_KEY_MISSILE;
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

long Platform_ReadFile(const char *name, void *data, size_t size)
{
    char path[256];
    SceUID fd;
    int n;
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, data, size);
    sceIoClose(fd);
    return n < 0 ? -1 : n;
}

int Platform_WriteFile(const char *name, const void *data, size_t size)
{
    char path[256], tmp[260];
    SceUID fd;
    int n;
    snprintf(path, sizeof(path), DATA_DIR "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return -1;
    n = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    if (n != (int)size) {
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(path);
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
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
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
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
    Port_Init(); /* loads settings.ini */
    Platform_Log("settings: view=%d style=%d fps30=%d missile=%d rear=%d\n", gPortConfig.viewMode,
                 gPortConfig.displayStyle, gPortConfig.frameRate30, gPortConfig.missileButton, gPortConfig.rearTouch);
    Port_RunGame();

    sceKernelExitProcess(0);
    return 0;
}
