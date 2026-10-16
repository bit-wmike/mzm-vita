/* Gameplay additions of the Zero Mission port (port/port_game.c). */
#ifndef PORT_GAME_H
#define PORT_GAME_H

#include <stdint.h>

/* Extra virtual key above the ten GBA keys: the missile button. */
#define PORT_KEY_MISSILE 0x8000

uint16_t Port_TranslateKeys(uint16_t keys);
/* Once per frame; `mainLoop` = the game is halted at the end of its main loop. */
void Port_GameFrame(int mainLoop);
void Port_RequestQuickSave(void);
void Port_RequestQuickLoad(void);
void Port_ShowMessage(const char *text);
/* Message to show on screen this frame, or NULL. */
const char *Port_CurrentMessage(void);

#endif /* PORT_GAME_H */
