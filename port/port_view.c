/*
 * Wide view for the Zero Mission port.
 *
 * The game keeps running for its 240x160 GBA screen: camera, scrolling and
 * collision are untouched. During gameplay the port shows a bigger window of
 * the same room around that screen:
 *
 *  - The room's tile layers (BG0-2) are read from the decompressed room maps:
 *    the visible window is written into the VRAM tilemap ring (512x256 px)
 *    right before each frame is rendered; the game itself only streams the
 *    area around its own screen.
 *  - Every layer is shifted by (dx, dy): the window starts dx/dy pixels
 *    left of/above the GBA screen. At the room's edges the window is moved
 *    so it stays inside the room. A room smaller than the window gets a
 *    smaller frame (same zoom, centred, black around it): outside the room
 *    there is nothing valid to show.
 *  - Objects: every OAM entry knows the screen position of the object it
 *    belongs to (Port_OamMark), so coordinates beyond the 9/8-bit OAM range
 *    are recovered. World objects move with the room, the HUD stays at the
 *    screen edges (minimap top right), message boxes are centred.
 *  - Objects are activated and drawn for the whole window
 *    (SpriteCheckOnScreen etc. use gPortViewMargin*).
 *
 * Menus, cutscenes and the map screen are shown at 240x160.
 */
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/port_view.h"
#include "port/ppu/include/cpu/mode1.h"

#include "types.h"
#include "gba.h"
#include "constants/game_state.h"
#include "constants/room.h"
#include "constants/in_game_cutscene.h"
#include "structs/game_state.h"
#include "structs/bg_clip.h"
#include "structs/room.h"
#include "structs/clipdata.h"
#include "structs/in_game_cutscene.h"

int gPortViewMode = PORT_VIEW_FULL;

/* View of the frame being shown (latched at VBlank). */
static int sActive;
static int sWidth = PORT_GBA_W, sHeight = PORT_GBA_H;
static int sDx, sDy;

/* Margins (pixels) of the window around the GBA screen, for the game's
 * on-screen checks during the logic frame in progress. */
int gPortViewMarginLeft, gPortViewMarginRight, gPortViewMarginTop, gPortViewMarginBottom;

/* Per OAM slot: anchor and object position, staged while the game builds its
 * OAM buffer, committed when the buffer is copied to OAM at VBlank. */
struct SlotInfo {
    unsigned char anchor;
    short x, y;
};
static struct SlotInfo sStaged[128], sSlots[128];
static int sStagedInit;

static void ResetStaged(void)
{
    int i;
    for (i = 0; i < 128; i++) {
        sStaged[i].anchor = PORT_ANCHOR_WORLD;
        sStaged[i].x = PORT_GBA_W / 2;
        sStaged[i].y = PORT_GBA_H / 2;
    }
    sStagedInit = 1;
}

int Port_ViewWidth(void)
{
    return sActive ? sWidth : PORT_GBA_W;
}

int Port_ViewHeight(void)
{
    return sActive ? sHeight : PORT_GBA_H;
}

int Port_ViewZoomHeight(void)
{
    /* Constant zoom in Full View: during gameplay every frame is scaled as
     * if it had the Full View height, so nothing jumps when the view turns
     * off for a moment (close-up, etc.). Menus keep their own scale. */
    if (gPortViewMode == PORT_VIEW_FULL && (gMainGameMode == GM_INGAME || gMainGameMode == GM_DEMO))
        return PORT_VIEW_FULL_H;
    return Port_ViewHeight();
}

void Port_OamMark(int start, int end, int anchor, int x, int y)
{
    int i;
    if (!sStagedInit)
        ResetStaged();
    if (start < 0)
        start = 0;
    if (end > 128)
        end = 128;
    for (i = start; i < end; i++) {
        sStaged[i].anchor = (unsigned char)anchor;
        sStaged[i].x = (short)x;
        sStaged[i].y = (short)y;
    }
}

void Port_OamCommit(void)
{
    if (!sStagedInit)
        ResetStaged();
    memcpy(sSlots, sStaged, sizeof(sSlots));
    ResetStaged();
}

static int GameplayVisible(void)
{
    if (gMainGameMode != GM_INGAME && gMainGameMode != GM_DEMO)
        return FALSE;
    if (gSubGameMode1 != SUB_GAME_MODE_PLAYING && gSubGameMode1 != SUB_GAME_MODE_DOOR_TRANSITION &&
        gSubGameMode1 != SUB_GAME_MODE_DYING)
        return FALSE;
    if (gBgPointersAndDimensions.clipdataWidth == 0)
        return FALSE;
    /* Samus' close-up is a full-screen picture made for 240x160. */
    if ((gInGameCutscene.cutsceneNumber & IGC_NO_STARTED_FLAG) == IGC_CLOSE_UP && gInGameCutscene.stage <= 11)
        return FALSE;
    return TRUE;
}

static void SizeForMode(int mode, int *w, int *h)
{
    switch (mode) {
        case PORT_VIEW_WIDE:
            *w = PORT_VIEW_WIDE_W;
            *h = PORT_GBA_H;
            break;
        case PORT_VIEW_FULL:
            *w = PORT_VIEW_FULL_W;
            *h = PORT_VIEW_FULL_H;
            break;
        default:
            *w = PORT_GBA_W;
            *h = PORT_GBA_H;
            break;
    }
}

/* Offset of the window start before the GBA screen along one axis (>= 0).
 * The window is never larger than the room (see Port_ViewOnVBlank), so it
 * always fits: it follows the camera and stops at the room's edges. */
static int WindowOffset(int cam, int screen, int window, int room)
{
    int extra = window - screen;
    int start = cam - extra / 2;
    int off;

    if (start > room - window)
        start = room - window;
    if (start < 0)
        start = 0;
    off = cam - start;
    if (off < 0)
        off = 0;
    if (off > extra)
        off = extra;
    return off;
}

/* The window never shows anything outside the room: the GBA camera never
 * does either, so the VRAM ring holds no valid tiles there (old tiles of
 * other rooms showed up as garbage). A room smaller than the window gets a
 * smaller frame instead; the zoom stays the same and the frame is centred,
 * so the screen around it is black. */
static int ClampToRoom(int size, int room, int gba)
{
    if (size > room)
        size = room;
    if (size < gba)
        size = gba;
    return size;
}

/* Called after the game's VBlank handler: the registers and OAM it has just
 * written are what the next rendered frame shows. */
void Port_ViewOnVBlank(void)
{
    int w, h;

    SizeForMode(gPortViewMode, &w, &h);
    sActive = (w != PORT_GBA_W || h != PORT_GBA_H) && GameplayVisible();
    if (sActive) {
        w = ClampToRoom(w, gBgPointersAndDimensions.clipdataWidth * 16, PORT_GBA_W);
        h = ClampToRoom(h, gBgPointersAndDimensions.clipdataHeight * 16, PORT_GBA_H);
    }
    sWidth = w;
    sHeight = h;
    if (sActive) {
        sDx = WindowOffset(gBg1XPosition / 4, PORT_GBA_W, w, gBgPointersAndDimensions.clipdataWidth * 16);
        sDy = WindowOffset(gBg1YPosition / 4, PORT_GBA_H, h, gBgPointersAndDimensions.clipdataHeight * 16);
    } else {
        sDx = sDy = 0;
    }

    /* The logic frame that starts now checks visibility against this window. */
    gPortViewMarginLeft = sActive ? sDx : 0;
    gPortViewMarginRight = sActive ? w - PORT_GBA_W - sDx : 0;
    gPortViewMarginTop = sActive ? sDy : 0;
    gPortViewMarginBottom = sActive ? h - PORT_GBA_H - sDy : 0;
}

static int FloorDiv16(int v)
{
    return v >= 0 ? v / 16 : -((15 - v) / 16);
}

/* Writes the blocks of the visible window into the VRAM tilemap ring of each
 * RLE room layer, the same way RoomUpdateHorizontalTilemap does. */
static void FillRoomLayers(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        u8 prop = i == 0 ? gCurrentRoomEntry.bg0Prop : i == 1 ? gCurrentRoomEntry.bg1Prop : gCurrentRoomEntry.bg2Prop;
        int posX = (i == 0 ? gBg0XPosition : i == 1 ? gBg1XPosition : gBg2XPosition) / 4;
        int posY = (i == 0 ? gBg0YPosition : i == 1 ? gBg1YPosition : gBg2YPosition) / 4;
        const u16 *decomp = gBgPointersAndDimensions.backgrounds[i].pDecomp;
        int w = gBgPointersAndDimensions.backgrounds[i].width;
        int h = gBgPointersAndDimensions.backgrounds[i].height;
        const u16 *tilemap = gTilemapAndClipPointers.pTilemap;
        int bx0, bx1, by0, by1, bx, by;

        if (!(prop & BG_PROP_RLE_COMPRESSED) || decomp == NULL || tilemap == NULL || w == 0 || h == 0)
            continue;

        /* The frame is rendered with the scroll registers written at the
         * last VBlank; the game may have moved the camera since. Use the
         * register value (9 bits) nearest to the game's position. */
        {
            int hofs = *(u16 *)(gHostIo + 0x10 + i * 4) & 0x1FF;
            int vofs = *(u16 *)(gHostIo + 0x12 + i * 4) & 0x1FF;
            posX += ((hofs - posX + 256) & 511) - 256;
            posY += ((vofs - posY + 256) & 511) - 256;
        }
        /* Exact blocks of the window, then one spare block on each side as
         * long as the ring (32x16 blocks) does not wrap onto itself. */
        bx0 = FloorDiv16(posX - sDx);
        bx1 = FloorDiv16(posX - sDx + sWidth - 1);
        by0 = FloorDiv16(posY - sDy);
        by1 = FloorDiv16(posY - sDy + sHeight - 1);
        if (bx1 - bx0 + 1 <= 30) {
            bx0--;
            bx1++;
        }
        if (by1 - by0 + 1 <= 14) {
            by0--;
            by1++;
        }
        if (bx0 < 0)
            bx0 = 0;
        if (by0 < 0)
            by0 = 0;
        if (bx1 >= w)
            bx1 = w - 1;
        if (by1 >= h)
            by1 = h - 1;

        for (by = by0; by <= by1; by++) {
            u32 *row = (u32 *)(gHostVram + i * 0x1000) + (by & 0xF) * 32;
            const u16 *src = decomp + by * w;
            for (bx = bx0; bx <= bx1; bx++) {
                const u16 *t = &tilemap[src[bx] * 4];
                int slot = (bx & 0xF) + ((bx & 0x10) ? 0x200 : 0);
                row[slot] = t[0] | (u32)t[1] << 16;
                row[slot + 0x10] = t[2] | (u32)t[3] << 16;
            }
        }
    }
}

static void DumpOam(void)
{
#ifndef __vita__
    static int dumpFrame = -2;
    int i;
    if (dumpFrame == -2) {
        const char *e = getenv("MZM_OAM_DUMP");
        dumpFrame = e ? atoi(e) : -1;
    }
    if ((int)Port_FrameCounter() != dumpFrame)
        return;
    for (i = 0; i < 128; i++) {
        u16 a0 = *(u16 *)(gHostOam + i * 8), a1 = *(u16 *)(gHostOam + i * 8 + 2);
        if ((a0 & 0x300) == 0x200)
            continue;
        Platform_Log("oam %3d y=%3d x=%3d anchor=%d base=%d,%d d=%d,%d\n", i, a0 & 0xFF, a1 & 0x1FF, sSlots[i].anchor,
                     sSlots[i].x, sSlots[i].y, virtuappu_mode1_obj_dx[i], virtuappu_mode1_obj_dy[i]);
    }
    Platform_Log("view: dx=%d dy=%d size=%dx%d\n", sDx, sDy, sWidth, sHeight);
#endif
}

/* Called right before a frame is rendered. */
void Port_ViewBeforeRender(int *width, int *height)
{
    int i;

    if (!sActive) {
        virtuappu_mode1_obj_port = 0;
        virtuappu_mode1_win_dx = virtuappu_mode1_win_dy = 0;
        virtuappu_mode1_line_dy = 0;
        for (i = 0; i < 4; i++)
            virtuappu_mode1_bg_dx[i] = virtuappu_mode1_bg_dy[i] = 0;
        *width = PORT_GBA_W;
        *height = PORT_GBA_H;
        return;
    }

    FillRoomLayers();

    for (i = 0; i < 4; i++) {
        virtuappu_mode1_bg_dx[i] = -sDx;
        virtuappu_mode1_bg_dy[i] = -sDy;
    }
    virtuappu_mode1_win_dx = sDx;
    virtuappu_mode1_win_dy = sDy;
    virtuappu_mode1_line_dy = sDy;
    virtuappu_mode1_obj_port = 1;

    for (i = 0; i < 128; i++) {
        int dx = sDx, dy = sDy;
        switch (sSlots[i].anchor) {
            case PORT_ANCHOR_HUD: {
                /* Left HUD stays at the left edge, the minimap goes right. */
                int raw = *(u16 *)(gHostOam + i * 8 + 2) & 0x1FF;
                int x = sSlots[i].x + (((raw - sSlots[i].x + 256) & 511) - 256);
                dx = x >= PORT_GBA_W / 2 ? sWidth - PORT_GBA_W : 0;
                dy = 0;
                break;
            }
            case PORT_ANCHOR_CENTER:
                dx = (sWidth - PORT_GBA_W) / 2;
                dy = (sHeight - PORT_GBA_H) / 2;
                break;
        }
        virtuappu_mode1_obj_dx[i] = (int16_t)dx;
        virtuappu_mode1_obj_dy[i] = (int16_t)dy;
        virtuappu_mode1_obj_base_x[i] = sSlots[i].x;
        virtuappu_mode1_obj_base_y[i] = sSlots[i].y;
    }
    DumpOam();
    *width = sWidth;
    *height = sHeight;
}
