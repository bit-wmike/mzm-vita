#ifndef GBA_MEMORY_H
#define GBA_MEMORY_H

#ifdef PORT
/* Host build: the GBA memory regions are ordinary arrays (port/host_mem.c). */
extern unsigned char gHostEwram[], gHostIwram[], gHostIo[], gHostPalram[], gHostVram[], gHostOam[], gHostSram[];
#endif

#ifdef PORT
#define EWRAM_BASE ((void *)gHostEwram)
#else
#define EWRAM_BASE (void *)0x02000000
#endif
#define EWRAM_SIZE (256 * 0x400)

#ifdef PORT
#define IWRAM_BASE ((void *)gHostIwram)
#else
#define IWRAM_BASE (void *)0x03000000
#endif
#define IWRAM_SIZE (32 * 0x400)

#ifdef PORT
#define REG_BASE ((void *)gHostIo)
#else
#define REG_BASE (void *)0x04000000
#endif

#ifdef PORT
#define PALRAM_BASE ((void *)gHostPalram)
#else
#define PALRAM_BASE (void *)0x05000000
#endif
#define PALRAM_OBJ (PALRAM_BASE + 0x200)
#define PAL_ROW (16)
#define PAL_ROW_SIZE (PAL_ROW * 2)
#define COLORS_IN_PAL (16 * PAL_ROW)
#define PAL_SIZE (COLORS_IN_PAL * 2)
#define PALRAM_SIZE (PAL_SIZE * 2)

#ifdef PORT
#define VRAM_BASE ((void *)gHostVram)
#else
#define VRAM_BASE (void *)0x06000000
#endif
#define VRAM_OBJ (VRAM_BASE + 0x10000)
#define VRAM_SIZE (96 * 0x400)
#define VRAM_BG_SIZE (0x10000)
#define VRAM_OBJ_SIZE (0x8000)

#ifdef PORT
#define OAM_BASE ((void *)gHostOam)
#else
#define OAM_BASE (void *)0x07000000
#endif
#define OAM_SIZE 0x400

#define ROM_BASE (void *)0x08000000
#define ROM_SIZE (32 * 0x100000)

#ifdef PORT
#define SRAM_BASE ((void *)gHostSram)
#else
#define SRAM_BASE (void *)0x0e000000
#endif
#define SRAM_SIZE (32 * 0x400)

#endif /* GBA_MEMORY_H */
