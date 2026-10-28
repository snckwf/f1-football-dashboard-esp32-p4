/* ============================================================================
 *  Ui - theme, geometry and screen API
 *
 *  GEOMETRY (1024 x 600 screen):
 *
 *    header      y   0 ..  46    title, clock, WiFi
 *    banner      y  46 ..  76    alerts (goals, flags) or status line
 *    tab bar     y  76 .. 134    [F1][FOOTBALL]  + sub-tabs of the section
 *                                F1:       5 tabs 146 px, step 150 from x=266
 *                                FOOTBALL: 6 tabs 119 px, step 125 from x=266
 *    content     y 134 .. 600    466 high, usable area 1008 x 450 from (8,8)
 * ========================================================================== */
#pragma once

#include <lvgl.h>
#include "AppConfig.h"
#include "DataModel.h"

/* ---- geometry ------------------------------------------------------------- */
#define UI_W            1024
#define UI_H            600
#define UI_HEADER_H     46
#define UI_BANNER_Y     46
#define UI_BANNER_H     30
#define UI_TABBAR_Y     (UI_BANNER_Y + UI_BANNER_H)     /* 76  */
#define UI_TABBAR_H     58
#define UI_CONTENT_Y    (UI_TABBAR_Y + UI_TABBAR_H)     /* 134 */
#define UI_CONTENT_H    (UI_H - UI_CONTENT_Y)           /* 466 */
#define UI_PAD          8
#define UI_INNER_W      (UI_W - 2 * UI_PAD)             /* 1008 */
#define UI_INNER_H      (UI_CONTENT_H - 2 * UI_PAD)     /* 450  */

#define UI_TAB_Y        6
#define UI_TAB_H        46
#define UI_MAIN_F1_X    8
#define UI_MAIN_F1_W    88
#define UI_MAIN_CAL_X   102
#define UI_MAIN_CAL_W   150
#define UI_SUB_X0       266
#define UI_SUB_F1_W     146
#define UI_SUB_F1_STEP  150
#define UI_SUB_CAL_W    119
#define UI_SUB_CAL_STEP 125
#define UI_SUB_MAX      6

#define UI_F1_PAGINE    5

/* ---- palette -------------------------------------------------------------- */
#define C_BG        lv_color_hex(0x0D1117)
#define C_CARD      lv_color_hex(0x161B22)
#define C_CARD2     lv_color_hex(0x21262D)
#define C_BORDER    lv_color_hex(0x30363D)
#define C_ORANGE    lv_color_hex(0xF7931A)
#define C_GREEN     lv_color_hex(0x3FB950)
#define C_RED       lv_color_hex(0xF85149)
#define C_F1RED     lv_color_hex(0xE10600)
#define C_YELLOW    lv_color_hex(0xD29922)
#define C_GOLD      lv_color_hex(0xFFD33D)
#define C_BLUE      lv_color_hex(0x58A6FF)
#define C_CYAN      lv_color_hex(0x39D3E8)
#define C_PURPLE    lv_color_hex(0xBC8CFF)
#define C_TEXT      lv_color_hex(0xE6EDF3)
#define C_MUTED     lv_color_hex(0x8B949E)
#define C_DARK      lv_color_hex(0x0D1117)

/* ---- shared helpers -------------------------------------------------------- */
lv_obj_t *uiCard(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
lv_obj_t *uiLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color);
/* fixed-width label with alignment and "..." if the text does not fit */
lv_obj_t *uiCol(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                const lv_font_t *font, lv_color_t color, lv_text_align_t align);
/* coloured box (team bar, chip) */
lv_obj_t *uiBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t color);
/* changes the text only if different: avoids useless redraws */
void      uiSetText(lv_obj_t *label, const char *txt);
void      uiSetColor(lv_obj_t *label, lv_color_t c);
/* compound colour: S red, M yellow, H white, I green, W blue */
lv_color_t uiColoreGomma(char g);

/* ---- screens --------------------------------------------------------------- */
void uiF1Create(lv_obj_t *pagine[UI_F1_PAGINE]);
/* forza = true on tab change */
void uiF1Update(uint8_t pagina, bool forza);
/* status text for the banner when there are no alerts */
void uiF1Stato(char *out, size_t n);

void uiCalcioCreate(lv_obj_t *pagina);
void uiCalcioUpdate(bool forza);
void uiCalcioStato(char *out, size_t n);
/* closes the match detail card, if open */
void uiCalcioChiudiDettaglio();

/* ---- framework -------------------------------------------------------------- */
void uiCreate();
void uiSetTouchDebug(const char *text);
