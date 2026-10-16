/*
 * Game data from the user's ROM.
 *
 * The port's binary contains the game's code but none of its graphics, text,
 * level data or sound samples. Every data array is registered by the build
 * (tools/port/pp_assets.py, tools/port/gen_sound.py) with its location in the
 * ROM and filled from the ROM file when the game starts.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"

struct PortAsset {
    unsigned char *dst;
    uint32_t capacity;
    uint32_t prefix;
    uint32_t romOffset;
    uint32_t size;
};

static struct PortAsset *sAssets;
static size_t sAssetCount, sAssetCap;

void PortAsset_Add(void *dst, unsigned capacity, unsigned prefixBytes, unsigned romOffset, unsigned size)
{
    if (sAssetCount == sAssetCap) {
        sAssetCap = sAssetCap ? sAssetCap * 2 : 4096;
        sAssets = (struct PortAsset *)realloc(sAssets, sAssetCap * sizeof(*sAssets));
    }
    sAssets[sAssetCount].dst = (unsigned char *)dst;
    sAssets[sAssetCount].capacity = capacity;
    sAssets[sAssetCount].prefix = prefixBytes;
    sAssets[sAssetCount].romOffset = romOffset;
    sAssets[sAssetCount].size = size;
    sAssetCount++;
}

/* ---- SHA-1 (to recognise the supported ROM) ---- */

struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t used;
};

static uint32_t Rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void Sha1Block(struct Sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void Port_Sha1(const void *data, size_t len, unsigned char out[20])
{
    struct Sha1 s = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
    const unsigned char *p = (const unsigned char *)data;
    uint64_t bits = (uint64_t)len * 8;
    size_t i;
    while (len >= 64) {
        Sha1Block(&s, p);
        p += 64;
        len -= 64;
    }
    memcpy(s.buf, p, len);
    s.buf[len++] = 0x80;
    if (len > 56) {
        memset(s.buf + len, 0, 64 - len);
        Sha1Block(&s, s.buf);
        len = 0;
    }
    memset(s.buf + len, 0, 56 - len);
    for (i = 0; i < 8; i++)
        s.buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    Sha1Block(&s, s.buf);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(s.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s.h[i];
    }
}

/* Metroid: Zero Mission (Europe) - the version this build is made for. */
static const unsigned char sExpectedSha1[20] = {
    0x0f, 0xd1, 0x07, 0x44, 0x5a, 0x42, 0xe6, 0xf3, 0xa3, 0xe5,
    0xce, 0x8c, 0x86, 0x5f, 0x41, 0x25, 0x83, 0x17, 0x99, 0x03,
};

int Port_RomMatches(const void *rom, size_t size)
{
    unsigned char sha[20];
    if (size != 8 * 1024 * 1024)
        return 0;
    Port_Sha1(rom, size, sha);
    return memcmp(sha, sExpectedSha1, 20) == 0;
}

void PortAssets_RegisterAll(void);

/* Copies all game data out of the ROM image. Returns 0 on success. */
int Port_LoadAssets(const void *rom, size_t size)
{
    const unsigned char *r = (const unsigned char *)rom;
    size_t i;

    PortAssets_RegisterAll();
    for (i = 0; i < sAssetCount; i++) {
        struct PortAsset *a = &sAssets[i];
        uint32_t n = a->size;
        if (a->romOffset + (uint64_t)n > size)
            return -1;
        if (a->prefix + n > a->capacity)
            n = a->capacity > a->prefix ? a->capacity - a->prefix : 0;
        memcpy(a->dst + a->prefix, r + a->romOffset, n);
    }
    Platform_Log("port: %u data blocks loaded from the ROM\n", (unsigned)sAssetCount);
    free(sAssets);
    sAssets = NULL;
    sAssetCount = sAssetCap = 0;
    return 0;
}
