/*
 * DMA for the host / PS Vita port.
 *
 * Immediate transfers are plain copies. HBlank-timed transfers (the game uses
 * them on channel 0 for the heat haze / water ripple effects) are remembered
 * and replayed by the renderer once per visible scanline, like the hardware.
 * Sound FIFO DMA (channels 1/2) is emulated by port/host_core.c directly from
 * the mixing buffer, so writes to those registers are ignored here.
 */
#include <string.h>

#include "port/port.h"
#include "types.h"
#include "gba/dma.h"
#include "dma.h"
#include "port/port_view.h"

struct HostHdma {
    int active;
    const u8 *src;
    u8 *dst;
    u32 units;
    u32 unitSize;
    u32 control;
};

static struct HostHdma sHdma[4];

/* The game's RAM variables (IWRAM_DATA, see include/macros.h). */
extern unsigned char __start_mzm_iwram[], __stop_mzm_iwram[];

__attribute__((no_sanitize_address))
static void Transfer(const u8 *src, u8 *dst, u32 units, u32 unitSize, u32 control)
{
    int srcStep = unitSize, dstStep = unitSize;
    u32 i;

    switch ((control >> 7) & 3) { /* source control */
        case 1: srcStep = -(int)unitSize; break;
        case 2: srcStep = 0; break;
    }
    switch ((control >> 5) & 3) { /* destination control */
        case 1: dstStep = -(int)unitSize; break;
        case 2: dstStep = 0; break;
        /* 3 = increment + reload: increments during the transfer */
    }

    if (srcStep == (int)unitSize && dstStep == (int)unitSize && (dst < src || dst >= src + units * unitSize)) {
        u32 n = units * unitSize;
        while (n--)
            *dst++ = *src++;
        return;
    }
    for (i = 0; i < units; i++) {
        if (unitSize == 4)
            *(u32 *)dst = *(const u32 *)src;
        else
            *(u16 *)dst = *(const u16 *)src;
        src += srcStep;
        dst += dstStep;
    }
}

void HostDmaSet(int channel, const void *src, void *dst, unsigned int cnt)
{
    u32 control = cnt >> 16;
    u32 units = cnt & 0xFFFF;
    u32 unitSize = (control & DMA_32BIT) ? 4 : 2;

    if (channel < 0 || channel > 3)
        return;

    if (!(control & DMA_ENABLE)) {
        sHdma[channel].active = 0;
        return;
    }
    if (units == 0)
        units = (channel == 3) ? 0x10000 : 0x4000;

    switch (control & DMA_START_MASK) {
        case DMA_START_NOW:
            Transfer((const u8 *)src, (u8 *)dst, units, unitSize, control);
            /* Clearing all of Iwram (boot / soft reset) also clears the
             * game's variables, which the port keeps outside gHostIwram. */
            if (dst == (void *)gHostIwram && ((control >> 7) & 3) == 2 && units * unitSize >= 0x7000)
                memset(__start_mzm_iwram, 0, __stop_mzm_iwram - __start_mzm_iwram);
            sHdma[channel].active = 0;
            /* The OAM buffer goes to OAM at VBlank: latch the slot anchors. */
            if (dst == (void *)gHostOam)
                Port_OamCommit();
            break;

        case DMA_START_HBLANK:
            sHdma[channel].active = 1;
            sHdma[channel].src = (const u8 *)src;
            sHdma[channel].dst = (u8 *)dst;
            sHdma[channel].units = units;
            sHdma[channel].unitSize = unitSize;
            sHdma[channel].control = control;
            break;

        default:
            /* VBlank / sound FIFO DMA: not used for graphics by this game. */
            break;
    }
}

/* One HBlank: every armed HBlank DMA moves one block. */
void HostDma_HBlank(void)
{
    int ch;
    for (ch = 0; ch < 4; ch++) {
        struct HostHdma *d = &sHdma[ch];
        if (!d->active)
            continue;
        Transfer(d->src, d->dst, d->units, d->unitSize, d->control);
        d->src += d->units * d->unitSize; /* source keeps incrementing */
        if (((d->control >> 5) & 3) != 3)
            d->dst += d->units * d->unitSize;
        if (!(d->control & DMA_REPEAT))
            d->active = 0;
    }
}

void HostDma_Reset(void)
{
    memset(sHdma, 0, sizeof(sHdma));
}

/* src/dma.c replacements: chunked DMA copies / fills, done directly.
 * The game sometimes copies a little past the end of a ROM table (harmless:
 * it only reads), so this copy is exempt from the address sanitizer. */
__attribute__((no_sanitize_address))
void DmaTransfer(u8 channel, const void *src, void *dst, u32 len, u8 bitSize)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    (void)channel;
    (void)bitSize;
    if (d < s || d >= s + len) {
        while (len--)
            *d++ = *s++;
    } else {
        d += len;
        s += len;
        while (len--)
            *--d = *--s;
    }
}

void BitFill(u8 channel, u32 value, void *dst, u32 len, u8 bitSize)
{
    (void)channel;
    if (bitSize == 32) {
        u32 *d = (u32 *)dst;
        len /= 4;
        while (len--)
            *d++ = value;
    } else {
        u16 *d = (u16 *)dst;
        len /= 2;
        while (len--)
            *d++ = (u16)value;
    }
}
