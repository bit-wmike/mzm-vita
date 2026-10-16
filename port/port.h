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

/* ---- platform interface ---- */
/* Shows a finished frame (ABGR8888, `pitch` pixels per row). */
void Platform_Present(const uint32_t *pixels, int width, int height, int pitch);
/* Returns pressed GBA keys (KEY_A ... KEY_L bits), polled once per frame. */
uint16_t Platform_PollKeys(void);
/* Receives interleaved stereo s16 samples at PORT_AUDIO_RATE. */
void Platform_SubmitAudio(const int16_t *samples, int frames);
/* Persistent storage for the 64 KiB SRAM image. Return 0 if nothing saved. */
int Platform_LoadSram(void *data, size_t size);
void Platform_SaveSram(const void *data, size_t size);
/* Optional log output. */
void Platform_Log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PORT_H */
