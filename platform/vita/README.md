# Metroid: Zero Mission für die PS Vita

## Installation

1. `ZeroMission_Vita_<version>.vpk` mit VitaShell installieren.
2. Deine ROM von *Metroid: Zero Mission* (Europa, fünf Sprachen) nach
   `ux0:data/mzm/` kopieren. Der Dateiname ist egal, Endung `.gba`.
   Erkannt wird sie an der Prüfsumme
   (SHA-1 `0fd107445a42e6f3a3e5ce8c865f412583179903`).
3. Starten. Die Sprache wählst du wie auf dem GBA beim ersten Start.

Im Paket sind keine Spieldaten. Grafik, Musik und Texte kommen beim Start
aus deiner ROM.

## Steuerung

| Vita | Spiel |
|------|-------|
| Kreuz | Springen (A) |
| Viereck / Kreis | Schießen (B) |
| Dreieck | Rakete abfeuern (R + B) |
| L / R | L / R (zielen, Raketen bereit) |
| Steuerkreuz / linker Stick | Bewegen |
| START / SELECT | Pause / Waffe wechseln |
| START + SELECT | Port-Menü |
| Rücktouch halten (wenn an) | links: schnellspeichern, rechts: schnellladen |

## Menü (START + SELECT)

Ansicht (GBA, Breitbild, Weitsicht), Bilddarstellung (Weich, Scharf,
Pixelgenau), Bildrate 60/30, Raketen-Taste, Rücktouch, Schnellspeichern /
-laden und Cheats (Energie, Raketen, Super-Raketen, Power-Bomben, alle
Fähigkeiten). Die Einstellungen landen in `settings.ini`.

Schnellspeichern geht nur im laufenden Spiel. Eine Schnellspeicherung
gehört zu genau einer Version des Ports; nach einem Update ist sie
ungültig. Die normalen Spielstände bleiben davon unberührt.

## Dateien in `ux0:data/mzm/`

| Datei | Inhalt |
|-------|--------|
| `*.gba` | deine ROM |
| `mzm_eu.sav` | Spielstände (32 KiB, mit GBA-Emulatoren kompatibel) |
| `settings.ini` | Einstellungen |
| `quicksave.bin` | Schnellspeicherung |
| `log.txt`, `log_prev.txt` | Protokoll des letzten / vorletzten Starts |

Bei Problemen bitte `log.txt` mitschicken. Die `[perf]`-Zeilen zeigen die
Rechenzeit pro Bild und wie viele Bilder ausgelassen wurden (`catchup`).
