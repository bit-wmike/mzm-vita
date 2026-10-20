/* Wide view of the Zero Mission port (port/port_view.c). */
#ifndef PORT_VIEW_H
#define PORT_VIEW_H

enum {
    PORT_VIEW_GBA = 0,  /* 240x160 */
    PORT_VIEW_WIDE = 1, /* 16:9 at the GBA's 160 lines, same zoom */
    PORT_VIEW_FULL = 2  /* "Weitsicht": 16:9, more of the room in both directions */
};

#define PORT_VIEW_WIDE_W 282
#define PORT_VIEW_FULL_W 400
#define PORT_VIEW_FULL_H 240
#define PORT_VIEW_MAX_W 400
#define PORT_VIEW_MAX_H 240

enum {
    PORT_ANCHOR_WORLD = 0,
    PORT_ANCHOR_HUD = 1,
    PORT_ANCHOR_CENTER = 2
};

extern int gPortViewMode;
extern int gPortViewMarginLeft, gPortViewMarginRight, gPortViewMarginTop, gPortViewMarginBottom;

/* Marks OAM slots [start, end) as belonging to one object at GBA-screen
 * position (x, y), drawn with the given anchor. */
void Port_OamMark(int start, int end, int anchor, int x, int y);
void Port_OamCommit(void);
void Port_ViewOnVBlank(void);
void Port_ViewBeforeRender(int *width, int *height);
int Port_ViewWidth(void);
int Port_ViewHeight(void);
int Port_ViewZoomHeight(void);

/* Helpers for the game code: remember the first slot, mark after drawing. */
#define PORT_OAM_BEGIN() u8 portOamStart_ = gNextOamSlot
#define PORT_OAM_END(anchor, x, y) Port_OamMark(portOamStart_, gNextOamSlot, (anchor), (x), (y))

#endif /* PORT_VIEW_H */
