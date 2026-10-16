/*
 * Host / PS Vita port of Metroid: Zero Mission - shared port interface.
 *
 * The game code is compiled unchanged (apart from a few #ifdef PORT spots);
 * this layer stands in for the GBA hardware: memory regions, BIOS calls, DMA,
 * interrupts, the PPU (software renderer in port/ppu) and the sound hardware.
 * A platform (platform/host, platform/vita) supplies video, audio, input and
 * files through the Platform* functions below.
 */
#ifndef PORT_PORT_H
#define PORT_PORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char gHostEwram[0x40000];
extern unsigned char gHostIwram[0x8000];
extern unsigned char gHostIo[0x400];
extern unsigned char gHostPalram[0x400];
extern unsigned char gHostVram[0x18000];
extern unsigned char gHostOam[0x400];
extern unsigned char gHostSram[0x10000];

#define PORT_GBA_W 240
#define PORT_GBA_H 160

/* ---- game data (port/host_assets.c) ---- */
/* True if the image is the ROM this build was made for. */
int Port_RomMatches(const void *rom, size_t size);
/* Copies the game's data out of the ROM image (call once before Port_Init). */
int Port_LoadAssets(const void *rom, size_t size);
void Port_Sha1(const void *data, size_t len, unsigned char out[20]);

/* ---- core (port/host_core.c) ---- */
void Port_Init(void);
/* Runs the game forever (agbmain, restarted on soft reset). */
void Port_RunGame(void);
/* Called by the core once per frame, after the frame has been rendered. */
uint32_t Port_FrameCounter(void);
void Port_SramDirty(void);
void Port_TraceFrame(void);

/* HBlank DMA (port/host_dma.c) */
void HostDma_HBlank(void);
void HostDma_Reset(void);

/* ---- audio (port/host_audio.c) ---- */
#define PORT_AUDIO_RATE 48000
void HostAudio_Init(void);
/* Consumes one 16-sample block of the DMA sound FIFOs (called 16 samples at a time). */
void HostAudio_PushFifoBlock(const int8_t *fifoA, const int8_t *fifoB);
/* Ends the frame: renders PSG + resamples and hands the samples to the platform. */
void HostAudio_EndFrame(void);

/* ---- profiling (host_core.c) ---- */
enum { PORT_PROF_RENDER, PORT_PROF_COUNT };
void Port_ProfileBegin(int what);
void Port_ProfileEnd(int what);
/* Average time of `what` over the last report period, in microseconds. */
unsigned Port_ProfileAverageUs(int what);
uint64_t Platform_TimeUs(void);

/* ---- platform interface ---- */
/* Shows a finished frame (ABGR8888, `pitch` pixels per row), scaled so that
 * `zoomHeight` lines fill the screen height (smaller frames get a border). */
void Platform_Present(const uint32_t *pixels, int width, int height, int pitch, int zoomHeight);
/* Returns pressed GBA keys (KEY_A ... KEY_L bits), polled once per frame. */
uint16_t Platform_PollKeys(void);
/* Receives interleaved stereo s16 samples at PORT_AUDIO_RATE. */
void Platform_SubmitAudio(const int16_t *samples, int frames);
/* Persistent storage for the 64 KiB SRAM image. Return 0 if nothing saved. */
int Platform_LoadSram(void *data, size_t size);
void Platform_SaveSram(const void *data, size_t size);
/* Optional log output. */
void Platform_Log(const char *fmt, ...);
/* Small files in the port's data folder (settings, quick save).
 * ReadFile returns the number of bytes read or -1; WriteFile returns 0 on
 * success and should replace the file atomically. */
long Platform_ReadFile(const char *name, void *data, size_t size);
int Platform_WriteFile(const char *name, const void *data, size_t size);
/* Frame pacing: called before rendering a frame; returns 0 when the frame
 * should only be computed, not drawn (the game is running late). */
int Platform_PaceAllowsRender(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PORT_H */
