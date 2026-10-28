/* ============================================================================
 *  BoardLvgl - MIPI-DSI display, GT911 touch and LVGL task
 *  Based on the sketch already validated on this board.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

bool        initBoardLvgl();
bool        lvglLock(int timeoutMs);
void        lvglUnlock();
bool        isLvglReady();
bool        boardTouchReady();
const char *boardTouchStatus();
void        boardResetScreenTimeout();
bool        boardIsScreenOn();

/* brightness 5..100 % and standby (0 = never), adjustable at runtime */
void        boardSetBrightness(uint8_t percent);
uint8_t     boardGetBrightness();
void        boardSetScreenTimeoutMs(uint32_t ms);
uint32_t    boardGetScreenTimeoutMs();
