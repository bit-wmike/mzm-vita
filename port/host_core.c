/*
 * Frame loop of the host / PS Vita port: stands in for the GBA's display
 * timing, interrupts and sound FIFO DMA.
 *
 * The game runs its logic until it halts the CPU waiting for VBlank
 * (SYSCALL Halt / VBlankIntrWait). At that point the port
 *   1. plays the sound blocks of one frame (sound FIFO DMA + DMA2 interrupt),
 *   2. renders the 160 visible scanlines with the software PPU, running
 *      HBlank DMA and the HBlank / VCount interrupts between scanlines,
 *   3. shows the frame, raises the VBlank interrupt and reads the controls.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "port/port.h"
#include "port/ppu/include/virtuappu.h"
#include "port/ppu/include/cpu/mode1.h"
#include "port/port_view.h"
#include "port/port_config.h"
#include "port/port_game.h"

#include "types.h"
#include "gba.h"
#include "callbacks.h"
#include "music_wrappers.h"
#include "structs/audio.h"
#include "data/generic_data.h"

void agbmain(void);
void PortSoundRelocate(void);

static int sRendering;
static uint32_t sFrameCounter;
static uint32_t sFrameBuffer[PORT_VIEW_MAX_W * PORT_VIEW_MAX_H];
static unsigned char sSramShadow[sizeof(gHostSram)];
static int sSramCheckTimer;
static int sMainLoopHalt; /* HostRunFrame was entered from agbmain's Halt */

#define IO16(off) (*(volatile u16 *)(gHostIo + (off)))

/* ---------------------------------------------------------------------- */

void *HostVcountPtr(void)
{
    if (!sRendering) {
        /* Outside of rendering, every read lets the beam move on one line. */
        u16 vc = IO16(0x006);
        IO16(0x006) = (u16)((vc + 1) % 228);
    }
    return gHostIo + 0x006;
}

/* GBA interrupt dispatch (asm/intr_main.s): the handler table order. */
static int IrqIndex(u16 flag)
{
    switch (flag) {
        case IF_DMA2: return 0;
        case IF_SERIAL: return 1;
        case IF_TIMER3: return 2;
        case IF_VBLANK: return 3;
        case IF_HBLANK: return 4;
        case IF_VCOUNT: return 5;
        default: return -1;
    }
}

static void RaiseIrq(u16 flag)
{
    int index;

    if (!(IO16(0x208) & 1) || !(IO16(0x200) & flag))
        return;
    index = IrqIndex(flag);
    if (index < 0 || sIntrTable[index] == NULL)
        return;
    sIntrTable[index]();
}

/* Software interrupts the game issues inline. */
void HostSyscall(int num)
{
    void HostRunFrame(void);

    switch (num) {
        case 2: /* Halt: wait for the next interrupt, which is VBlank */
            sMainLoopHalt = 1;
            HostRunFrame();
            sMainLoopHalt = 0;
            break;
        default: /* Stop (sleep) and others: return immediately */
            break;
    }
}

void VBlankIntrWait(void)
{
    void HostRunFrame(void);
    HostRunFrame();
}

/* The GBA copies its interrupt handler to Iwram; nothing to copy here. */
void IntrMain(void)
{
}

/* ---- sound FIFO DMA ---------------------------------------------------- */

static void PlaySoundBlocks(void)
{
    u32 blocks = gMusicInfo.unk_C;
    u32 i;

    if (gMusicInfo.unk_E == 0 || blocks == 0) {
        static const int8_t silence[16];
        for (i = 0; i < 14; i++)
            HostAudio_PushFifoBlock(silence, silence);
        return;
    }
    for (i = 0; i < blocks; i++) {
        u32 block = (gMusicInfo.unk_10 + 1u) % gMusicInfo.unk_E;
        const int8_t *a = (const int8_t *)&gMusicInfo.soundRawData[block * 16];
        const int8_t *b = (const int8_t *)&gMusicInfo.soundRawData[sizeof(gMusicInfo.soundRawData) / 2 + block * 16];
        HostAudio_PushFifoBlock(a, b);
        RaiseIrq(IF_DMA2);
    }
}

/* ---- display ---------------------------------------------------------- */

static void PreLine(int line)
{
    u16 dispstat = IO16(0x004);
    int prev = line == 0 ? 227 : line - 1;

    /* HBlank of the previous scanline */
    IO16(0x006) = (u16)prev;
    if (line != 0)
        HostDma_HBlank();
    if (dispstat & DSTAT_IF_HBLANK)
        RaiseIrq(IF_HBLANK);

    /* Start of this scanline */
    IO16(0x006) = (u16)line;
    if ((dispstat & DSTAT_IF_VCOUNT) && (dispstat >> 8) == line)
        RaiseIrq(IF_VCOUNT);
}

/* A frame that is computed but not drawn: the scanline events (HBlank DMA,
 * HBlank / VCount interrupts) still happen, as the game may depend on them. */
static void SkipFrame(void)
{
    int line;
    sRendering = 1;
    for (line = 0; line < 160; line++)
        PreLine(line);
    sRendering = 0;
}

static void RenderFrame(void)
{
    static int bound;
    PPUMemory ppu;
    u16 dispcnt = IO16(0x000);
    int width, height;

    if (!bound) {
        VirtuaPPUMode1GbaMemory mem = {
            gHostIo, gHostVram, (uint16_t *)gHostPalram, (uint16_t *)(gHostPalram + 0x200), (uint16_t *)gHostOam,
        };
        virtuappu_mode1_bind_gba_memory(&mem);
        /* Exact fast paths of the renderer (single-core friendly). */
        virtuappu_mode1_set_old3ds_profile(true);
        virtuappu_mode1_set_output_buffer(sFrameBuffer, PORT_VIEW_MAX_W);
        bound = 1;
    }

    Port_ViewBeforeRender(&width, &height);

    memset(&ppu, 0, sizeof(ppu));
    ppu.frame_width = (uint16_t)width;
    ppu.frame_height = (uint16_t)height;
    ppu.frame_pitch = PORT_VIEW_MAX_W;
    ppu.mode = ((dispcnt & 7) == 0) ? 1 : 2;

    virtuappu_mode1_pre_line_callback = PreLine;
    sRendering = 1;
    Port_ProfileBegin(PORT_PROF_RENDER);
    virtuappu_mode1_render_frame(&ppu);
    Port_ProfileEnd(PORT_PROF_RENDER);
    sRendering = 0;

    Platform_Present(sFrameBuffer, width, height, PORT_VIEW_MAX_W, Port_ViewZoomHeight());
}

/* ---- frame ------------------------------------------------------------ */

static void CheckSram(void)
{
    if (++sSramCheckTimer < 30)
        return;
    sSramCheckTimer = 0;
    /* The cartridge has 32 KiB of SRAM; only that part is saved. */
    if (memcmp(sSramShadow, gHostSram, 0x8000) != 0) {
        memcpy(sSramShadow, gHostSram, 0x8000);
        Platform_SaveSram(gHostSram, 0x8000);
    }
}

void HostRunFrame(void)
{
    u16 keys;

    PlaySoundBlocks();
    if ((gPortConfig.frameRate30 && (sFrameCounter & 1)) || !Platform_PaceAllowsRender())
        SkipFrame();
    else
        RenderFrame();

    /* VBlank */
    IO16(0x006) = 160;
    IO16(0x004) |= DSTAT_IN_VBLANK;
    RaiseIrq(IF_VBLANK);
    Port_ViewOnVBlank();
    virtuappu_mode1_commit_obj_metadata();
    IO16(0x004) &= ~DSTAT_IN_VBLANK;

    HostAudio_EndFrame();

    keys = Port_TranslateKeys(Platform_PollKeys());
    IO16(0x130) = (u16)(~keys & 0x3FF);

    Port_GameFrame(sMainLoopHalt);

    CheckSram();
    Port_TraceFrame();
    sFrameCounter++;
}

/* Debug aid: per-frame checksums of the game state (MZM_TRACE=file). */
extern unsigned char __start_mzm_iwram[], __stop_mzm_iwram[];
static uint32_t Crc(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

void Port_TraceFrame(void)
{
#ifndef __vita__
    static FILE *f;
    static int init;
    if (!init) {
        const char *path = getenv("MZM_TRACE");
        init = 1;
        if (path)
            f = fopen(path, "w");
    }
    if (f && getenv("MZM_TRACE_AUDIO"))
        fprintf(f, "%u psg=%08x raw=%08x ch0=%02x %02x %02x %02x %02x %02x %02x\n", sFrameCounter,
                Crc(gHostIo + 0x60, 0x40), Crc(gMusicInfo.soundRawData, sizeof(gMusicInfo.soundRawData)),
                gMusicInfo.soundChannels[0].unk_0, gMusicInfo.soundChannels[1].unk_0, gMusicInfo.soundChannels[2].unk_0,
                gMusicInfo.soundChannels[3].unk_0, gMusicInfo.soundChannels[4].unk_0, gMusicInfo.soundChannels[5].unk_0,
                gMusicInfo.soundChannels[6].unk_0);
    else if (f)
        fprintf(f, "%u iw=%08x ew=%08x io=%08x vram=%08x\n", sFrameCounter,
                Crc(__start_mzm_iwram, __stop_mzm_iwram - __start_mzm_iwram), Crc(gHostEwram, sizeof(gHostEwram)),
                Crc(gHostIo, 0x400), Crc(gHostVram, sizeof(gHostVram)));
#endif
}

static uint64_t sProfStart[PORT_PROF_COUNT], sProfSum[PORT_PROF_COUNT], sProfMax[PORT_PROF_COUNT];
static unsigned sProfCount[PORT_PROF_COUNT];

void Port_ProfileBegin(int what)
{
    sProfStart[what] = Platform_TimeUs();
}

void Port_ProfileEnd(int what)
{
    uint64_t t = Platform_TimeUs() - sProfStart[what];
    sProfSum[what] += t;
    if (t > sProfMax[what])
        sProfMax[what] = t;
    sProfCount[what]++;
}

unsigned Port_ProfileAverageUs(int what)
{
    unsigned avg = sProfCount[what] ? (unsigned)(sProfSum[what] / sProfCount[what]) : 0;
    sProfSum[what] = sProfMax[what] = 0;
    sProfCount[what] = 0;
    return avg;
}

uint32_t Port_FrameCounter(void)
{
    return sFrameCounter;
}

/* Settings (settings.ini in the data folder). */
void Port_ConfigLoad(void)
{
    static char text[4096];
    long n;
    Port_ConfigDefaults();
    n = Platform_ReadFile("settings.ini", text, sizeof(text) - 1);
    if (n > 0) {
        text[n] = 0;
        Port_ConfigParse(text);
    }
    gPortViewMode = gPortConfig.viewMode;
}

void Port_ConfigSave(void)
{
    static char text[4096];
    int n = Port_ConfigWrite(text, sizeof(text));
    gPortViewMode = gPortConfig.viewMode;
    if (Platform_WriteFile("settings.ini", text, (size_t)n) != 0)
        Platform_Log("config: cannot write settings.ini\n");
}

void Port_Init(void)
{
    /* The sound engine indexes gUnk_300376C[1..4] to reach gPsgSounds, as on
     * the GBA where the two arrays are adjacent in Iwram. */
    if ((void *)&gUnk_300376C[1] != (void *)&gPsgSounds[0])
        Platform_Log("port: WARNING gPsgSounds is not adjacent to gUnk_300376C\n");
    Port_ConfigLoad();
    PortSoundRelocate();
    HostAudio_Init();
    IO16(0x130) = 0x3FF;
    if (Platform_LoadSram(gHostSram, sizeof(gHostSram)) == 0)
        memset(gHostSram, 0xFF, sizeof(gHostSram));
    memcpy(sSramShadow, gHostSram, sizeof(gHostSram));
}

void Port_RunGame(void)
{
    for (;;) {
        /* agbmain returns when the game asks for a full reset (crt0 restarts it). */
        agbmain();
    }
}
