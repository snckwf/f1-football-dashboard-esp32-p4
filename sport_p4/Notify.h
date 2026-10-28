/* ============================================================================
 *  Notify - queue of alerts shown in the banner below the header
 *  (goals, F1 flags, leader change...). Callable from any task.
 * ========================================================================== */
#pragma once

#include <Arduino.h>
#include "Audio.h"

enum NotificaTipo : uint8_t {
    NOT_INFO = 0,     /* blue   */
    NOT_GOL,          /* yellow */
    NOT_F1,           /* red    */
    NOT_OK,           /* green  */
};

void notifyInit();
/* suono = SND_COUNT for no sound */
void notifica(NotificaTipo tipo, SoundEvent suono, const char *fmt, ...);

/* For the banner: true if there is an active alert to show now */
bool notificaCorrente(char *testo, size_t n, NotificaTipo *tipo);

/* Alert sounds on/off (stored in NVS, survives reboots).
 * The touch beep does not depend on this setting. */
void notifySetSuoni(bool on);
bool notifySuoni();

/* Plays all the alert sounds in a row */
void notifyDemoSuoni();

/* Screen the next alert leads to if the screen is in standby.
 * modo 0 = F1 (sub = tab), modo 1 = football (sub = competition).
 * Call it right before notifica(). */
void notifySchermata(uint8_t modo, uint8_t sub);

/* For the UI: true if an alert arrived since the last call.
 * modo = 255 if the alert does not point to a screen. */
bool notifyPrendiSveglia(uint8_t *modo, uint8_t *sub);
