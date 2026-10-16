/*
 * Link cable / multiboot pieces of the GBA game that have no counterpart on
 * the Vita. They report "no connection", so the game's link menus simply fail
 * gracefully.
 */
#include "types.h"
#include "multiboot.h"

void MultiBootInit(struct MultiBootData* pMultiBoot)
{
    (void)pMultiBoot;
}

u32 MultiBootMain(struct MultiBootData* pMultiBoot)
{
    (void)pMultiBoot;
    return 0;
}

void MultiBootStartParent(struct MultiBootData* pMultiBoot, const u8* src, s32 length, u8 palette_color, s8 palette_speed)
{
    (void)pMultiBoot; (void)src; (void)length; (void)palette_color; (void)palette_speed;
}

u32 MultiBootCheckComplete(struct MultiBootData* pMultiBoot)
{
    (void)pMultiBoot;
    return 0;
}

/* End marker of the link-transfer payload (a linker symbol on the GBA). */
const u8 sTransferRom_After[1];
