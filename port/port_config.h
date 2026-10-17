/* Settings of the Zero Mission port (port/port_config.c). */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

enum {
    PORT_STYLE_SMOOTH = 0,
    PORT_STYLE_SHARP = 1,
    PORT_STYLE_PIXEL = 2
};

#define PORT_CONFIG_VERSION 2

struct PortConfig {
    int configVersion;
    int viewMode;        /* PORT_VIEW_* */
    int displayStyle;    /* PORT_STYLE_* */
    int frameRate30;     /* render every second frame (game speed unchanged) */
    int missileButton;   /* triangle fires a missile (R + B) */
    int rearTouch;       /* rear touch: left = quick save, right = quick load */
    /* cheats */
    int cheatEnergy;
    int cheatMissiles;
    int cheatSuperMissiles;
    int cheatPowerBombs;
    int cheatAbilities;
};

extern struct PortConfig gPortConfig;

void Port_ConfigDefaults(void);
/* ini text <-> settings */
void Port_ConfigParse(const char *text);
int Port_ConfigWrite(char *buf, int size);
void Port_ConfigMigrate(void);
/* Load / save settings.ini through the platform (host_core.c). */
void Port_ConfigLoad(void);
void Port_ConfigSave(void);

#endif /* PORT_CONFIG_H */
