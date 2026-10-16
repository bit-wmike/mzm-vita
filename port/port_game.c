/*
 * Gameplay additions of the Zero Mission port: control macros, cheats and
 * quick save / quick load (a snapshot of the whole game state).
 */
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/port_config.h"
#include "port/port_game.h"
#include "port/port_view.h"

#include "types.h"
#include "gba.h"
#include "constants/game_state.h"
#include "constants/samus.h"
#include "structs/game_state.h"
#include "structs/samus.h"

/* ---------------------------------------------------------------- input */

/* The missile button: arm missiles (R) for one frame, then keep R held and
 * press B, like doing it by hand. */
static int sMissileFrames;

uint16_t Port_TranslateKeys(uint16_t keys)
{
    uint16_t out = keys & 0x3FF;

    if (gPortConfig.missileButton && (keys & PORT_KEY_MISSILE)) {
        out |= KEY_R;
        if (sMissileFrames > 0)
            out |= KEY_B;
        sMissileFrames++;
    } else {
        sMissileFrames = 0;
    }
    return out;
}

/* --------------------------------------------------------------- cheats */

static int InGameplay(void)
{
    return (gMainGameMode == GM_INGAME) && gSubGameMode1 == SUB_GAME_MODE_PLAYING;
}

static void ApplyCheats(void)
{
    struct Equipment *e = &gEquipment;

    if (!InGameplay())
        return;

    if (gPortConfig.cheatAbilities) {
        /* Newly given items are switched on once; the player can still switch
         * them off on the status screen. Plasma, gravity and space jump are
         * left out: the game hands them out only after Mother Brain. */
        unsigned beams = (BBF_LONG_BEAM | BBF_ICE_BEAM | BBF_WAVE_BEAM | BBF_CHARGE_BEAM | BBF_BOMBS) & ~e->beamBombs;
        unsigned misc = (SMF_HIGH_JUMP | SMF_SPEEDBOOSTER | SMF_SCREW_ATTACK | SMF_VARIA_SUIT | SMF_MORPH_BALL |
                         SMF_POWER_GRIP) & ~e->suitMisc;
        e->beamBombs |= beams;
        e->beamBombsActivation |= beams;
        e->suitMisc |= misc;
        e->suitMiscActivation |= misc;
        if (e->maxMissiles < 50)
            e->maxMissiles = 50;
        if (e->maxSuperMissiles < 10)
            e->maxSuperMissiles = 10;
        if (e->maxPowerBombs < 5)
            e->maxPowerBombs = 5;
    }
    if (gPortConfig.cheatEnergy)
        e->currentEnergy = e->maxEnergy;
    if (gPortConfig.cheatMissiles)
        e->currentMissiles = e->maxMissiles;
    if (gPortConfig.cheatSuperMissiles)
        e->currentSuperMissiles = e->maxSuperMissiles;
    if (gPortConfig.cheatPowerBombs)
        e->currentPowerBombs = e->maxPowerBombs;
}

/* ------------------------------------------------------ quick save/load */

extern unsigned char __start_mzm_iwram[], __stop_mzm_iwram[];

#define STATE_MAGIC 0x5A4D5153u /* "SQMZ" */

struct StateHeader {
    uint32_t magic;
    char build[48];
    uint32_t ramSize;
};

static int sRequest; /* 1 = save, 2 = load */
static char sStateMessage[96];
static int sStateMessageFrames;

static void BuildId(char out[48])
{
    memset(out, 0, 48);
    strncpy(out, MZM_PORT_VERSION " " __DATE__ " " __TIME__, 47);
}

/* The regions written to / read from a state file, in order. */
static size_t Regions(unsigned char **ptr, size_t *size)
{
    size_t n = 0;
    ptr[n] = gHostEwram; size[n++] = sizeof(gHostEwram);
    ptr[n] = gHostIwram; size[n++] = sizeof(gHostIwram);
    ptr[n] = gHostIo; size[n++] = sizeof(gHostIo);
    ptr[n] = gHostPalram; size[n++] = sizeof(gHostPalram);
    ptr[n] = gHostVram; size[n++] = sizeof(gHostVram);
    ptr[n] = gHostOam; size[n++] = sizeof(gHostOam);
    ptr[n] = __start_mzm_iwram; size[n++] = (size_t)(__stop_mzm_iwram - __start_mzm_iwram);
    return n;
}

static void DoSave(void)
{
    unsigned char *ptr[8];
    size_t size[8], total = sizeof(struct StateHeader), n, i, off;
    unsigned char *buf;
    struct StateHeader *h;

    n = Regions(ptr, size);
    for (i = 0; i < n; i++)
        total += size[i];
    buf = (unsigned char *)malloc(total);
    if (!buf)
        return;
    h = (struct StateHeader *)buf;
    h->magic = STATE_MAGIC;
    BuildId(h->build);
    h->ramSize = (uint32_t)(total - sizeof(*h));
    off = sizeof(*h);
    for (i = 0; i < n; i++) {
        memcpy(buf + off, ptr[i], size[i]);
        off += size[i];
    }
    if (Platform_WriteFile("quicksave.bin", buf, total) == 0)
        Port_ShowMessage("Schnellspeicherung gesichert");
    else
        Port_ShowMessage("Schnellspeicherung fehlgeschlagen");
    free(buf);
}

static void DoLoad(void)
{
    unsigned char *ptr[8];
    size_t size[8], total = sizeof(struct StateHeader), n, i, off;
    unsigned char *buf;
    struct StateHeader *h;
    char build[48];

    n = Regions(ptr, size);
    for (i = 0; i < n; i++)
        total += size[i];
    buf = (unsigned char *)malloc(total);
    if (!buf)
        return;
    if (Platform_ReadFile("quicksave.bin", buf, total) != (long)total) {
        Port_ShowMessage("Keine Schnellspeicherung vorhanden");
        free(buf);
        return;
    }
    h = (struct StateHeader *)buf;
    BuildId(build);
    if (h->magic != STATE_MAGIC || memcmp(h->build, build, sizeof(build)) != 0 || h->ramSize != total - sizeof(*h)) {
        Port_ShowMessage("Schnellspeicherung stammt von einer anderen Version");
        free(buf);
        return;
    }
    off = sizeof(*h);
    for (i = 0; i < n; i++) {
        memcpy(ptr[i], buf + off, size[i]);
        off += size[i];
    }
    free(buf);
    HostDma_Reset();
    Port_ShowMessage("Schnellspeicherung geladen");
}

void Port_RequestQuickSave(void)
{
    sRequest = 1;
}

void Port_RequestQuickLoad(void)
{
    sRequest = 2;
}

void Port_ShowMessage(const char *text)
{
    strncpy(sStateMessage, text, sizeof(sStateMessage) - 1);
    sStateMessage[sizeof(sStateMessage) - 1] = 0;
    sStateMessageFrames = 120;
    Platform_Log("message: %s\n", text);
}

const char *Port_CurrentMessage(void)
{
    if (sStateMessageFrames <= 0)
        return NULL;
    sStateMessageFrames--;
    return sStateMessage;
}

/* Called once per frame at the frame boundary (the game is halted). */
void Port_GameFrame(int mainLoop)
{
    ApplyCheats();

    if (sRequest) {
        /* Only during gameplay and at the end of the game's main loop: the
         * whole game state is then in memory, nothing on the C stack. */
        if (!mainLoop)
            return; /* try again next frame */
        if (InGameplay()) {
            if (sRequest == 1)
                DoSave();
            else
                DoLoad();
        } else {
            Port_ShowMessage("Schnellspeichern geht nur im laufenden Spiel");
        }
        sRequest = 0;
    }
}
