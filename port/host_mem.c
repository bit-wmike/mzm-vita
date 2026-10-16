/*
 * GBA memory regions for the host / PS Vita port.
 *
 * The game addresses Ewram, Iwram, I/O registers, palette RAM, VRAM, OAM and
 * SRAM through include/gba/memory.h; in the port those bases point here.
 */
#include "port/port.h"

#define ALIGN64 __attribute__((aligned(64)))

unsigned char gHostEwram[0x40000] ALIGN64;
unsigned char gHostIwram[0x8000] ALIGN64;
unsigned char gHostIo[0x400] ALIGN64;
unsigned char gHostPalram[0x400] ALIGN64;
unsigned char gHostVram[0x18000] ALIGN64;
unsigned char gHostOam[0x400] ALIGN64;
unsigned char gHostSram[0x10000] ALIGN64;

/* Game buffers that the GBA linker script (linker.ld) places at fixed Ewram /
 * Iwram addresses: the same offsets inside the host arrays. */
__asm__(
    ".globl gClipdataCollisionTypes\n.set gClipdataCollisionTypes, gHostEwram + 0x00002000\n"
    ".globl gClipdataCollisionTypes_Tilemap\n.set gClipdataCollisionTypes_Tilemap, gHostEwram + 0x00002400\n"
    ".globl gClipdataBehaviorTypes\n.set gClipdataBehaviorTypes, gHostEwram + 0x00003000\n"
    ".globl gClipdataBehaviorTypes_Tilemap\n.set gClipdataBehaviorTypes_Tilemap, gHostEwram + 0x00003800\n"
    ".globl gTilemap\n.set gTilemap, gHostEwram + 0x00004000\n"
    ".globl gCommonTilemap\n.set gCommonTilemap, gHostEwram + 0x00006000\n"
    ".globl gDecompBg3Map\n.set gDecompBg3Map, gHostEwram + 0x00007000\n"
    ".globl gHazeValues\n.set gHazeValues, gHostEwram + 0x00026300\n"
    ".globl gPreviousHazeValues\n.set gPreviousHazeValues, gHostEwram + 0x00026d00\n"
    ".globl gCurrentCharacterGfx\n.set gCurrentCharacterGfx, gHostEwram + 0x00027700\n"
    ".globl gMakeSolidBlocks\n.set gMakeSolidBlocks, gHostEwram + 0x00027780\n"
    ".globl gDecompClipdataMap\n.set gDecompClipdataMap, gHostEwram + 0x00027800\n"
    ".globl gDecompBg0Map\n.set gDecompBg0Map, gHostEwram + 0x0002a800\n"
    ".globl gDecompBg1Map\n.set gDecompBg1Map, gHostEwram + 0x0002d800\n"
    ".globl gDecompBg2Map\n.set gDecompBg2Map, gHostEwram + 0x00030800\n"
    ".globl gMinimapTilesWithObtainedItems\n.set gMinimapTilesWithObtainedItems, gHostEwram + 0x00033800\n"
    ".globl gDecompressedMinimapVisitedTiles\n.set gDecompressedMinimapVisitedTiles, gHostEwram + 0x00034000\n"
    ".globl gDecompressedMinimapData\n.set gDecompressedMinimapData, gHostEwram + 0x00034800\n"
    ".globl gUnk_02035400\n.set gUnk_02035400, gHostEwram + 0x00035400\n"
    ".globl gNeverReformBlocks\n.set gNeverReformBlocks, gHostEwram + 0x00035c00\n"
    ".globl gItemsCollected\n.set gItemsCollected, gHostEwram + 0x00036c00\n"
    ".globl gVisitedMinimapTiles\n.set gVisitedMinimapTiles, gHostEwram + 0x00037400\n"
    ".globl gEventsTriggered\n.set gEventsTriggered, gHostEwram + 0x00037e00\n"
    ".globl gMinimapTilesGfx\n.set gMinimapTilesGfx, gHostEwram + 0x00037e20\n"
    ".globl gSram\n.set gSram, gHostEwram + 0x00038000\n"
    ".globl gSramDemoInputData\n.set gSramDemoInputData, gHostEwram + 0x0003f080\n"
    ".globl gSramDemoInputDuration\n.set gSramDemoInputDuration, gHostEwram + 0x0003f280\n"
    ".globl gInterruptCheckFlag\n.set gInterruptCheckFlag, gHostIwram + 0x00007ff8\n"
    ".globl gIntrCodePointer\n.set gIntrCodePointer, gHostIwram + 0x00007ffc\n"
    ".globl gIntrVector\n.set gIntrVector, gHostIwram + 0x00007ffc\n"
);
