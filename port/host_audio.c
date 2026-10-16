/*
 * GBA sound hardware for the host / PS Vita port.
 *
 *  - Direct sound: the game's own mixer fills two 8-bit FIFO streams
 *    (A = right, B = left). host_core.c hands over 16 samples at a time,
 *    exactly as the FIFO DMA would consume them.
 *  - PSG: the four Game Boy channels (two squares, wave, noise) are emulated
 *    from the sound registers the game writes.
 *
 * Once per frame the result is resampled to PORT_AUDIO_RATE and passed to
 * the platform.
 */
#include <string.h>

#include "port/port.h"
#include "types.h"

#define FRAME_OUT (PORT_AUDIO_RATE / 60)
#define MAX_FIFO 1024

#define IO8(off) (*(volatile u8 *)(gHostIo + (off)))
#define IO16(off) (*(volatile u16 *)(gHostIo + (off)))

static int8_t sFifoA[MAX_FIFO], sFifoB[MAX_FIFO];
static int sFifoCount;
static int sLastA, sLastB;
static int16_t sOut[FRAME_OUT * 2];

struct Envelope {
    int volume;
    int dir;
    int period;
    int timer;
};

struct Square {
    int on;
    u32 phase; /* one waveform period = 2^32 */
    int duty;
    struct Envelope env;
    int lengthTimer; /* in 1/256 s ticks remaining, -1 = off */
    int freqReg;
    /* sweep (channel 1) */
    int sweepPeriod, sweepDir, sweepShift, sweepTimer;
};

struct Wave {
    int on;
    u32 phase;
    int lengthTimer;
};

struct Noise {
    int on;
    u64 phase; /* LFSR clocks, 32.32 fixed point */
    u16 lfsr;
    struct Envelope env;
    int lengthTimer;
};

static struct Square sSq[2];
static struct Wave sWave;
static struct Noise sNoise;
static u32 sFrameSeqAcc; /* 512 Hz frame sequencer */
static int sFrameSeqStep;
static int sLastFreq1;

static const u8 sDuty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 1, 1, 1},
    {0, 1, 1, 1, 1, 1, 1, 0},
};

void HostAudio_Init(void)
{
    memset(sSq, 0, sizeof(sSq));
    memset(&sWave, 0, sizeof(sWave));
    memset(&sNoise, 0, sizeof(sNoise));
    sFifoCount = 0;
    sLastA = sLastB = 0;
}

void HostAudio_PushFifoBlock(const int8_t *fifoA, const int8_t *fifoB)
{
    if (sFifoCount + 16 > MAX_FIFO)
        return;
    memcpy(&sFifoA[sFifoCount], fifoA, 16);
    memcpy(&sFifoB[sFifoCount], fifoB, 16);
    sFifoCount += 16;
}

static void EnvTrigger(struct Envelope *e, u16 reg)
{
    e->volume = (reg >> 12) & 0xF;
    e->dir = (reg >> 11) & 1;
    e->period = (reg >> 8) & 7;
    e->timer = e->period;
}

static void EnvTick(struct Envelope *e)
{
    if (e->period == 0)
        return;
    if (--e->timer > 0)
        return;
    e->timer = e->period;
    if (e->dir && e->volume < 15)
        e->volume++;
    else if (!e->dir && e->volume > 0)
        e->volume--;
}

/* Starts channels whose trigger bit the game has set since the last frame. */
static void ProcessTriggers(void)
{
    u16 r;

    /* Square 1 */
    r = IO16(0x64);
    if (r & 0x8000) {
        u16 duty = IO16(0x62);
        u16 sweep = IO16(0x60);
        struct Square *s = &sSq[0];
        s->on = 1;
        EnvTrigger(&s->env, duty);
        s->lengthTimer = (r & 0x4000) ? 64 - (duty & 0x3F) : -1;
        s->sweepPeriod = (sweep >> 4) & 7;
        s->sweepDir = (sweep >> 3) & 1;
        s->sweepShift = sweep & 7;
        s->sweepTimer = s->sweepPeriod;
        s->freqReg = r & 0x7FF;
        sLastFreq1 = s->freqReg;
        IO16(0x64) = r & 0x7FFF;
    }
    sSq[0].duty = (IO16(0x62) >> 6) & 3;
    /* A frequency write without trigger takes effect directly; keep the
     * sweep's own value while the register is unchanged. */
    if ((IO16(0x64) & 0x7FF) != sLastFreq1) {
        sLastFreq1 = IO16(0x64) & 0x7FF;
        sSq[0].freqReg = sLastFreq1;
    }

    /* Square 2 */
    r = IO16(0x6C);
    if (r & 0x8000) {
        u16 duty = IO16(0x68);
        struct Square *s = &sSq[1];
        s->on = 1;
        EnvTrigger(&s->env, duty);
        s->lengthTimer = (r & 0x4000) ? 64 - (duty & 0x3F) : -1;
        s->sweepPeriod = 0;
        IO16(0x6C) = r & 0x7FFF;
    }
    sSq[1].duty = (IO16(0x68) >> 6) & 3;
    sSq[1].freqReg = IO16(0x6C) & 0x7FF;

    /* Wave */
    r = IO16(0x74);
    if (r & 0x8000) {
        sWave.on = 1;
        sWave.phase = 0;
        sWave.lengthTimer = (r & 0x4000) ? 256 - (IO16(0x72) & 0xFF) : -1;
        IO16(0x74) = r & 0x7FFF;
    }
    if (!(IO8(0x70) & 0x80))
        sWave.on = 0;

    /* Noise */
    r = IO16(0x7C);
    if (r & 0x8000) {
        u16 env = IO16(0x78);
        sNoise.on = 1;
        EnvTrigger(&sNoise.env, env);
        sNoise.lengthTimer = (r & 0x4000) ? 64 - (env & 0x3F) : -1;
        sNoise.lfsr = 0x7FFF;
        IO16(0x7C) = r & 0x7FFF;
    }
}

static void FrameSequencerTick(void)
{
    int step = sFrameSeqStep++ & 7;
    int i;

    /* length counters at 256 Hz */
    if ((step & 1) == 0) {
        for (i = 0; i < 2; i++)
            if (sSq[i].lengthTimer > 0 && --sSq[i].lengthTimer == 0)
                sSq[i].on = 0;
        if (sWave.lengthTimer > 0 && --sWave.lengthTimer == 0)
            sWave.on = 0;
        if (sNoise.lengthTimer > 0 && --sNoise.lengthTimer == 0)
            sNoise.on = 0;
    }
    /* sweep at 128 Hz */
    if (step == 2 || step == 6) {
        struct Square *s = &sSq[0];
        if (s->on && s->sweepPeriod && --s->sweepTimer <= 0) {
            int delta = s->freqReg >> s->sweepShift;
            s->sweepTimer = s->sweepPeriod;
            if (s->sweepShift) {
                int f = s->sweepDir ? s->freqReg - delta : s->freqReg + delta;
                if (f > 2047)
                    s->on = 0;
                else if (f >= 0)
                    s->freqReg = f;
            }
        }
    }
    /* envelopes at 64 Hz */
    if (step == 7) {
        EnvTick(&sSq[0].env);
        EnvTick(&sSq[1].env);
        EnvTick(&sNoise.env);
    }
}

/* One output sample of the four PSG channels, as (left, right) in roughly
 * [-480, 480] each before the PSG ratio. */
/* Phase increment per output sample for a tone of numerator/denominator Hz,
 * with 2^32 = one period. Integer only: identical results on every CPU. */
static u32 PhaseInc(u64 numerator, u64 denominator)
{
    return (u32)((numerator << 32) / (denominator * PORT_AUDIO_RATE));
}

/* One output sample of the four PSG channels, as (left, right) in roughly
 * [-480, 480] each before the PSG ratio. */
static void PsgSample(int *left, int *right)
{
    int ch[4] = {0, 0, 0, 0};
    u16 cntL = IO16(0x80);
    int i;

    sFrameSeqAcc += 512;
    while (sFrameSeqAcc >= PORT_AUDIO_RATE) {
        sFrameSeqAcc -= PORT_AUDIO_RATE;
        FrameSequencerTick();
    }

    for (i = 0; i < 2; i++) {
        struct Square *s = &sSq[i];
        if (s->on && s->env.volume) {
            s->phase += PhaseInc(131072, 2048 - s->freqReg);
            ch[i] = sDuty[s->duty][s->phase >> 29] ? s->env.volume : -s->env.volume;
        }
    }

    if (sWave.on) {
        u16 cntH = IO16(0x72);
        int freqReg = IO16(0x74) & 0x7FF;
        int pos, nib, vol;
        /* 32 samples per period, one sample every (2048 - x) / 2097152 s */
        sWave.phase += PhaseInc(2097152, (u64)(2048 - freqReg) * 32);
        pos = (int)(sWave.phase >> 27);
        nib = (gHostIo[0x90 + pos / 2] >> ((pos & 1) ? 0 : 4)) & 0xF;
        vol = (cntH >> 13) & 3;
        nib = nib * 2 - 15;
        if (cntH & 0x8000)
            ch[2] = nib * 3 / 4;
        else if (vol == 0)
            ch[2] = 0;
        else
            ch[2] = nib >> (vol - 1);
    }

    if (sNoise.on && sNoise.env.volume) {
        u16 r = IO16(0x7C);
        int shift = (r >> 4) & 0xF;
        /* LFSR clock = 524288 / r / 2^(s+1) Hz, r = 0 counts as 0.5 */
        u64 num = 1048576;
        u64 den = (u64)((r & 7) ? (r & 7) * 2 : 1) << (shift + 1);
        sNoise.phase += ((num << 32) / (den * PORT_AUDIO_RATE));
        while (sNoise.phase >= ((u64)1 << 32)) {
            int bit = (sNoise.lfsr ^ (sNoise.lfsr >> 1)) & 1;
            sNoise.phase -= (u64)1 << 32;
            sNoise.lfsr = (sNoise.lfsr >> 1) | (bit << 14);
            if (r & 8)
                sNoise.lfsr = (sNoise.lfsr & ~0x40) | (bit << 6);
        }
        ch[3] = (sNoise.lfsr & 1) ? -sNoise.env.volume : sNoise.env.volume;
    }

    *left = *right = 0;
    for (i = 0; i < 4; i++) {
        if (cntL & (0x1000 << i))
            *left += ch[i];
        if (cntL & (0x100 << i))
            *right += ch[i];
    }
    *left *= ((cntL >> 4) & 7) + 1;
    *right *= (cntL & 7) + 1;
}

void HostAudio_EndFrame(void)
{
    u16 cntH = IO16(0x82);
    int psgShift = 2 - (cntH & 3); /* 25 %, 50 %, 100 % */
    int dmaAShift = (cntH & 4) ? 2 : 1;
    int dmaBShift = (cntH & 8) ? 2 : 1;
    int master = IO8(0x84) & 0x80;
    int i;

    ProcessTriggers();

    for (i = 0; i < FRAME_OUT; i++) {
        int a, b, l = 0, r = 0, pl, pr;
        /* linear interpolation through this frame's FIFO samples */
        if (sFifoCount > 0) {
            int fixed = (int)(((long long)i * sFifoCount << 8) / FRAME_OUT);
            int idx = fixed >> 8, frac = fixed & 0xFF;
            int a0 = idx == 0 ? sLastA : sFifoA[idx - 1];
            int b0 = idx == 0 ? sLastB : sFifoB[idx - 1];
            a = (a0 * (256 - frac) + sFifoA[idx] * frac) >> 8;
            b = (b0 * (256 - frac) + sFifoB[idx] * frac) >> 8;
        } else {
            a = b = 0;
        }
        if (cntH & 0x0100) r += a << dmaAShift;
        if (cntH & 0x0200) l += a << dmaAShift;
        if (cntH & 0x1000) r += b << dmaBShift;
        if (cntH & 0x2000) l += b << dmaBShift;

        PsgSample(&pl, &pr);
        if (psgShift >= 0) {
            l += pl >> psgShift;
            r += pr >> psgShift;
        }
        if (!master)
            l = r = 0;

        l *= 28;
        r *= 28;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        sOut[i * 2] = (int16_t)l;
        sOut[i * 2 + 1] = (int16_t)r;
    }

    if (sFifoCount > 0) {
        sLastA = sFifoA[sFifoCount - 1];
        sLastB = sFifoB[sFifoCount - 1];
    }
    sFifoCount = 0;
    Platform_SubmitAudio(sOut, FRAME_OUT);
}
