/*
 * GBA BIOS functions used by the game (asm/syscalls.s), implemented in C.
 */
#include <math.h>
#include <string.h>

#include "port/port.h"
#include "types.h"
#include "syscalls.h"

/* SWI 0x0C: copy/fill words in blocks of 8 (count = number of words). */
void CpuFastSet(void *src, void *dst, u32 control)
{
    u32 count = (control & 0x1FFFFF);
    u32 *d = (u32 *)dst;
    const u32 *s = (const u32 *)src;
    count = (count + 7) & ~7u;
    if (control & (1 << 24)) {
        u32 v = *s;
        while (count--)
            *d++ = v;
    } else {
        while (count--)
            *d++ = *s++;
    }
}

/* SWI 0x0B */
void CpuSet(void *src, void *dst, u32 control)
{
    u32 count = control & 0x1FFFFF;
    int fill = (control >> 24) & 1;
    if (control & (1 << 26)) {
        u32 *d = (u32 *)dst;
        const u32 *s = (const u32 *)src;
        u32 v = *s;
        while (count--)
            *d++ = fill ? v : *s++;
    } else {
        u16 *d = (u16 *)dst;
        const u16 *s = (const u16 *)src;
        u16 v = *s;
        while (count--)
            *d++ = fill ? v : *s++;
    }
}

/* SWI 0x07 (DivArm): r0 = denominator, r1 = numerator. */
s32 DivarmDiv(s32 denom, s32 number)
{
    return denom ? number / denom : 0;
}

s32 DivarmMod(s32 denom, s32 number)
{
    return denom ? number % denom : 0;
}

static void Lz77(const void *src, void *dst, int vram)
{
    const u8 *s = (const u8 *)src;
    u8 *d = (u8 *)dst;
    u32 size = s[1] | (s[2] << 8) | (s[3] << 16);
    u32 out = 0;
    s += 4;
    (void)vram; /* byte writes are fine on the host */
    while (out < size) {
        u8 flags = *s++;
        int i;
        for (i = 0; i < 8 && out < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3;
                u32 disp = (((s[0] & 0xF) << 8) | s[1]) + 1;
                s += 2;
                while (len-- && out < size) {
                    d[out] = d[out - disp];
                    out++;
                }
            } else {
                d[out++] = *s++;
            }
        }
    }
}

void LZ77UncompVram(const void *src, void *dst)
{
    Lz77(src, dst, 1);
}

void LZ77UncompWram(const void *src, void *dst)
{
    Lz77(src, dst, 0);
}

/* SWI 0x1F: wd points to a WaveData header (u16 type, u16 stat, u32 freq...). */
u32 MidiKey2Freq(u32 *wd, u8 mk, u8 fp)
{
    double freq = (double)wd[1];
    return (u32)(freq / pow(2.0, (180.0 - mk - fp / 256.0) / 12.0));
}

int Multiboot(void *mbp)
{
    (void)mbp;
    return 1; /* no link cable */
}

void SoundBias0(void)
{
    *(u16 *)(gHostIo + 0x88) &= 0xFC00;
}

void SoundBias200(void)
{
    *(u16 *)(gHostIo + 0x88) = (*(u16 *)(gHostIo + 0x88) & 0xFC00) | 0x200;
}

u16 Sqrt(u32 number)
{
    u32 r = 0, b = 1u << 30;
    while (b > number)
        b >>= 2;
    while (b) {
        if (number >= r + b) {
            number -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (u16)r;
}
