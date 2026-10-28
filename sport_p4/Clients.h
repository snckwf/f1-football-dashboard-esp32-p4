/* ============================================================================
 *  Clients - network tasks
 *
 *    calcioTask   ESPN site.api.espn.com   (results, live, standings)
 *    f1Task       api.jolpi.ca             (standings, last race, calendar)
 *    f1LiveTask   livetiming.formula1.com  (SignalR Core over WebSocket, real time)
 * ========================================================================== */
#pragma once

#include <Arduino.h>

/* HTTPS GET, body in out. Returns the HTTP code (or negative on error). */
int  httpGet(const String &url, String &out);

void calcioTask(void *arg);
void f1Task(void *arg);
void f1LiveTask(void *arg);

/* true if the WiFi is connected and the clock is synchronised */
bool reteOk();

/* only one HTTPS/TLS connection opened at a time */
bool reteLock(uint32_t timeoutMs);
void reteUnlock();
