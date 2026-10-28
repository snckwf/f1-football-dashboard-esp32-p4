/* ============================================================================
 *  Ui - framework: header, alert banner, tab bar, pages
 * ========================================================================== */
#include <time.h>

#include "BoardLvgl.h"
#include "Notify.h"
#include "Settings.h"
#include "Ui.h"

static lv_obj_t *g_screen    = nullptr;
static lv_obj_t *g_lblOra    = nullptr;
static lv_obj_t *g_lblWifi   = nullptr;
static lv_obj_t *g_lblAudio  = nullptr;
static lv_obj_t *g_banner    = nullptr;
static lv_obj_t *g_lblBanner = nullptr;
static lv_obj_t *g_btnF1     = nullptr;
static lv_obj_t *g_btnCal    = nullptr;
static lv_obj_t *g_lblF1     = nullptr;
static lv_obj_t *g_lblCal    = nullptr;
static lv_obj_t *g_sub[UI_SUB_MAX]    = { nullptr };
static lv_obj_t *g_subLbl[UI_SUB_MAX] = { nullptr };
static lv_obj_t *g_pagineF1[UI_F1_PAGINE] = { nullptr };
static lv_obj_t *g_paginaCal = nullptr;

static uint8_t g_modo  = 1;      /* 0 = F1, 1 = football */
static uint8_t g_subF1 = 0;

#if SHOW_TOUCH_DEBUG
static lv_obj_t *g_lblTouch = nullptr;
#endif

static const char *const SUB_F1[UI_F1_PAGINE] = { "LIVE", "MAP", "DRIVERS", "CONSTRUCTORS", "LAST RACE" };

/* ---------------------------------------------------------------------------
 *  Shared helpers
 * ------------------------------------------------------------------------- */
lv_obj_t *uiCard(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, C_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *uiLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t *uiCol(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                const lv_font_t *font, lv_color_t color, lv_text_align_t align)
{
    lv_obj_t *l = uiLabel(parent, "", font, color);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

lv_obj_t *uiBox(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

void uiSetText(lv_obj_t *label, const char *txt)
{
    if (label == nullptr) return;
    const char *cur = lv_label_get_text(label);
    if (cur != nullptr && strcmp(cur, txt) == 0) return;
    lv_label_set_text(label, txt);
}

void uiSetColor(lv_obj_t *label, lv_color_t c)
{
    if (label == nullptr) return;
    if (lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), c)) return;
    lv_obj_set_style_text_color(label, c, 0);
}

/* ---------------------------------------------------------------------------
 *  Tabs
 * ------------------------------------------------------------------------- */
static void stileBottone(lv_obj_t *b, lv_obj_t *l, bool attivo, lv_color_t accento)
{
    lv_obj_set_style_bg_color(b, attivo ? accento : C_CARD, 0);
    lv_obj_set_style_border_color(b, attivo ? accento : C_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_text_color(l, attivo ? C_TEXT : C_MUTED, 0);
}

static void aggiornaIconaAudio();

/* football tabs: only the active competitions (web page), in AppConfig order */
static uint8_t g_tabComp[CALCIO_COMP_COUNT];
static uint8_t g_nTab = 0;
static uint8_t g_compAttiveViste = 0xFF;

static void calcolaTabCalcio()
{
    g_nTab = 0;
    for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) if (compAttiva(c)) g_tabComp[g_nTab++] = c;
    if (g_nTab == 0) { g_tabComp[0] = 0; g_nTab = 1; }
    bool ok = false;
    for (uint8_t i = 0; i < g_nTab; ++i) if (g_tabComp[i] == g_compVisibile) ok = true;
    if (!ok) { g_compVisibile = g_tabComp[0]; g_richiestaLista = true; }
    g_compAttiveViste = imp().compAttive;
}

static void aggiornaSchede()
{
    calcolaTabCalcio();
    stileBottone(g_btnF1, g_lblF1, g_modo == 0, C_F1RED);
    stileBottone(g_btnCal, g_lblCal, g_modo == 1, lv_color_hex(0x1F6F3A));

    for (uint8_t i = 0; i < UI_SUB_MAX; ++i) {
        lv_obj_t *b = g_sub[i];
        bool visibile;
        bool attivo;
        if (g_modo == 0) {
            visibile = i < UI_F1_PAGINE;
            attivo = i == g_subF1;
            if (visibile) {
                lv_obj_set_pos(b, UI_SUB_X0 + i * UI_SUB_F1_STEP, UI_TAB_Y);
                lv_obj_set_size(b, UI_SUB_F1_W, UI_TAB_H);
                lv_label_set_text(g_subLbl[i], SUB_F1[i]);
            }
        } else {
            visibile = i < g_nTab;
            attivo = visibile && g_tabComp[i] == g_compVisibile;
            if (visibile) {
                lv_obj_set_pos(b, UI_SUB_X0 + i * UI_SUB_CAL_STEP, UI_TAB_Y);
                lv_obj_set_size(b, UI_SUB_CAL_W, UI_TAB_H);
                lv_label_set_text(g_subLbl[i], CALCIO_TAB[g_tabComp[i]]);
            }
        }
        if (!visibile) { lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(b, attivo ? C_CARD2 : C_CARD, 0);
        lv_obj_set_style_border_color(b, attivo ? C_ORANGE : C_BORDER, 0);
        lv_obj_set_style_border_width(b, attivo ? 2 : 1, 0);
        lv_obj_set_style_text_color(g_subLbl[i], attivo ? C_TEXT : C_MUTED, 0);
    }

    for (uint8_t i = 0; i < UI_F1_PAGINE; ++i) {
        if (g_modo == 0 && i == g_subF1) lv_obj_remove_flag(g_pagineF1[i], LV_OBJ_FLAG_HIDDEN);
        else                             lv_obj_add_flag(g_pagineF1[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (g_modo == 1) lv_obj_remove_flag(g_paginaCal, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_add_flag(g_paginaCal, LV_OBJ_FLAG_HIDDEN);
}

static void aggiornaPagina(bool forza)
{
    if (g_modo == 0) uiF1Update(g_subF1, forza);
    else             uiCalcioUpdate(forza);
}

static void mainEventCb(lv_event_t *e)
{
    const uint8_t m = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (m == g_modo) return;
    uiCalcioChiudiDettaglio();
    g_modo = m;
    aggiornaSchede();
    aggiornaPagina(true);
}

static void subEventCb(lv_event_t *e)
{
    const uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (g_modo == 0) {
        if (i >= UI_F1_PAGINE) return;
        g_subF1 = i;
    } else {
        if (i >= g_nTab) return;
        uiCalcioChiudiDettaglio();
        g_compVisibile = g_tabComp[i];
        g_richiestaLista = true;
    }
    aggiornaSchede();
    aggiornaPagina(true);
}

/* highlights in red the sections with something live */
static void aggiornaIndicatoriLive()
{
    bool f1Live = false;
    uint8_t live[CALCIO_COMP_COUNT] = { 0 };
    if (dataLock(20)) {
        f1Live = strcmp(g_f1live->stato, "Started") == 0 || strcmp(g_f1live->stato, "Aborted") == 0;
        for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) live[c] = g_comp[c].live;
        dataUnlock();
    }
    bool calLive = false;
    for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) if (live[c]) calLive = true;

    uiSetText(g_lblF1, f1Live ? "F1 " LV_SYMBOL_PLAY : "F1");
    uiSetText(g_lblCal, calLive ? "FOOTBALL " LV_SYMBOL_PLAY : "FOOTBALL");
    if (g_modo != 0) lv_obj_set_style_border_color(g_btnF1, f1Live ? C_RED : C_BORDER, 0);
    if (g_modo != 1) lv_obj_set_style_border_color(g_btnCal, calLive ? C_RED : C_BORDER, 0);

    if (g_modo == 1) {
        for (uint8_t i = 0; i < g_nTab; ++i) {
            const uint8_t c = g_tabComp[i];
            const bool attivo = c == g_compVisibile;
            uiSetColor(g_subLbl[i], live[c] ? C_RED : (attivo ? C_TEXT : C_MUTED));
        }
    } else {
        uiSetColor(g_subLbl[0], f1Live ? C_RED : (g_subF1 == 0 ? C_TEXT : C_MUTED));
    }
}

/* ---------------------------------------------------------------------------
 *  Header and banner
 * ------------------------------------------------------------------------- */
static void aggiornaHeader()
{
    char buf[64];
    struct tm t;
    if (getLocalTime(&t, 5)) strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    else                     snprintf(buf, sizeof(buf), "--:--:--");
    uiSetText(g_lblOra, buf);
    uiSetText(g_lblWifi, getWifiStatusText());
}

static void aggiornaBanner()
{
    char testo[128];
    NotificaTipo tipo = NOT_INFO;
    if (notificaCorrente(testo, sizeof(testo), &tipo)) {
        lv_color_t sf = C_BLUE;
        switch (tipo) {
            case NOT_GOL: sf = C_GOLD;  break;
            case NOT_F1:  sf = C_F1RED; break;
            case NOT_OK:  sf = C_GREEN; break;
            default:      sf = C_BLUE;  break;
        }
        uiSetText(g_lblBanner, testo);
        lv_obj_set_style_bg_color(g_banner, sf, 0);
        uiSetColor(g_lblBanner, tipo == NOT_F1 ? C_TEXT : C_DARK);
        lv_obj_set_style_text_font(g_lblBanner, &lv_font_montserrat_20, 0);
        return;
    }
    if (g_modo == 0) uiF1Stato(testo, sizeof(testo));
    else             uiCalcioStato(testo, sizeof(testo));
    uiSetText(g_lblBanner, testo);
    lv_obj_set_style_bg_color(g_banner, C_CARD, 0);
    uiSetColor(g_lblBanner, C_MUTED);
    lv_obj_set_style_text_font(g_lblBanner, &lv_font_montserrat_16, 0);
}

/* goes to a screen: modo 0 = F1 (sub = tab), 1 = football (sub = competition) */
static void vaiA(uint8_t modo, uint8_t sub)
{
    if (modo == 0 && sub < UI_F1_PAGINE) {
        uiCalcioChiudiDettaglio();
        g_modo = 0;
        g_subF1 = sub;
    } else if (modo == 1 && sub < CALCIO_COMP_COUNT && compAttiva(sub)) {
        uiCalcioChiudiDettaglio();
        g_modo = 1;
        if (g_compVisibile != sub) {
            g_compVisibile = sub;
            g_richiestaLista = true;
        }
    } else {
        return;
    }
    aggiornaSchede();
}

static void tickCb(lv_timer_t *t)
{
    (void)t;

    /* standby: an alert turns the screen back on and, if it was off, shows the
     * screen of the event (if it was on I do not change page under your fingers) */
    bool forza = false;
    uint8_t modo, sub;
    if (notifyPrendiSveglia(&modo, &sub)) {
        const bool eraSpento = !boardIsScreenOn();
        boardResetScreenTimeout();
        if (eraSpento) {
            vaiA(modo, sub);
            forza = true;
            Serial.printf("[SCREEN] wake-up from alert -> %s %u\n", modo == 0 ? "F1" : "football", sub);
        }
    }

    /* with the screen off there is no need to redraw anything */
    static bool eraAcceso = true;
    const bool acceso = boardIsScreenOn();
    if (!acceso) {
        if (eraAcceso) Serial.println("[SCREEN] standby");
        eraAcceso = false;
        return;
    }
    if (!eraAcceso) {                                      /* just woken up */
        Serial.println("[SCREEN] on");
        eraAcceso = true;
        forza = true;
    }

    /* settings changed from the web page */
    if (imp().compAttive != g_compAttiveViste) {
        uiCalcioChiudiDettaglio();
        aggiornaSchede();
        forza = true;
    }
    aggiornaIconaAudio();

    aggiornaHeader();
    aggiornaBanner();
    aggiornaIndicatoriLive();
    aggiornaPagina(forza);
}

/* ---------------------------------------------------------------------------
 *  Audio: icon, on/off, sound test
 * ------------------------------------------------------------------------- */
static void aggiornaIconaAudio()
{
    const bool on = notifySuoni();
    uiSetText(g_lblAudio, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    uiSetColor(g_lblAudio, on ? C_TEXT : C_RED);
}

static void audioEventCb(lv_event_t *e)
{
    (void)e;
    notifySetSuoni(!notifySuoni());
    aggiornaIconaAudio();
    notifica(NOT_INFO, SND_COUNT, notifySuoni() ? "Alert sounds ON"
                                                : "Alert sounds OFF (on-screen alerts only)");
}

static void demoEventCb(lv_event_t *e)
{
    (void)e;
    notifica(NOT_INFO, SND_COUNT, "Sound test: F1 start, pass-by, kick-off whistle, goal, half time, full time");
    notifyDemoSuoni();
}

/* ---------------------------------------------------------------------------
 *  Build
 * ------------------------------------------------------------------------- */
static lv_obj_t *fascia(lv_coord_t y, lv_coord_t h, lv_color_t bg)
{
    lv_obj_t *o = lv_obj_create(g_screen);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_size(o, UI_W, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *bottone(lv_obj_t *parent, lv_coord_t x, lv_coord_t w, const char *txt,
                         const lv_font_t *font, lv_event_cb_t cb, uintptr_t dato, lv_obj_t **outLbl)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, UI_TAB_Y);
    lv_obj_set_size(b, w, UI_TAB_H);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_border_color(b, C_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)dato);
    lv_obj_t *l = uiLabel(b, txt, font, C_MUTED);
    lv_obj_center(l);
    if (outLbl != nullptr) *outLbl = l;
    return b;
}

void uiCreate()
{
    g_screen = lv_screen_active();
    lv_obj_set_style_bg_color(g_screen, C_BG, 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(g_screen, 0, 0);

    /* ---- header ---- */
    lv_obj_t *hdr = fascia(0, UI_HEADER_H, C_CARD);
    /* tap on the title: test all the sounds */
    lv_obj_t *tit = uiLabel(hdr, "SPORT  " LV_SYMBOL_PLAY "  F1 + FOOTBALL", &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(tit, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(tit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tit, 12);
    lv_obj_add_event_cb(tit, demoEventCb, LV_EVENT_CLICKED, nullptr);

    g_lblOra = uiLabel(hdr, "--:--:--", &lv_font_montserrat_24, C_ORANGE);
    lv_obj_align(g_lblOra, LV_ALIGN_CENTER, 0, 0);

    /* tap on the speaker: alert sounds on/off */
    g_lblAudio = uiLabel(hdr, "", &lv_font_montserrat_24, C_TEXT);
    lv_obj_align(g_lblAudio, LV_ALIGN_CENTER, 110, 0);
    lv_obj_add_flag(g_lblAudio, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g_lblAudio, 16);
    lv_obj_add_event_cb(g_lblAudio, audioEventCb, LV_EVENT_CLICKED, nullptr);
    aggiornaIconaAudio();
    g_lblWifi = uiLabel(hdr, "starting", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(g_lblWifi, LV_ALIGN_RIGHT_MID, -12, 0);
#if SHOW_TOUCH_DEBUG
    g_lblTouch = uiLabel(hdr, "", &lv_font_montserrat_14, C_PURPLE);
    lv_obj_align(g_lblTouch, LV_ALIGN_RIGHT_MID, -260, 0);
#endif

    /* ---- banner ---- */
    g_banner = fascia(UI_BANNER_Y, UI_BANNER_H, C_CARD);
    g_lblBanner = uiLabel(g_banner, "Starting...", &lv_font_montserrat_16, C_MUTED);
    lv_obj_set_width(g_lblBanner, UI_W - 24);
    lv_label_set_long_mode(g_lblBanner, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(g_lblBanner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_lblBanner);

    /* ---- tab bar ---- */
    lv_obj_t *bar = fascia(UI_TABBAR_Y, UI_TABBAR_H, C_BG);
    g_btnF1  = bottone(bar, UI_MAIN_F1_X,  UI_MAIN_F1_W,  "F1",     &lv_font_montserrat_24, mainEventCb, 0, &g_lblF1);
    g_btnCal = bottone(bar, UI_MAIN_CAL_X, UI_MAIN_CAL_W, "FOOTBALL", &lv_font_montserrat_20, mainEventCb, 1, &g_lblCal);
    lv_obj_t *sep = uiBox(bar, UI_SUB_X0 - 8, 10, 2, UI_TAB_H - 8, C_BORDER);
    (void)sep;
    for (uint8_t i = 0; i < UI_SUB_MAX; ++i) {
        g_sub[i] = bottone(bar, UI_SUB_X0 + i * UI_SUB_CAL_STEP, UI_SUB_CAL_W, "",
                           &lv_font_montserrat_16, subEventCb, i, &g_subLbl[i]);
    }

    /* ---- pages ---- */
    for (uint8_t i = 0; i <= UI_F1_PAGINE; ++i) {
        lv_obj_t *p = fascia(UI_CONTENT_Y, UI_CONTENT_H, C_BG);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        if (i < UI_F1_PAGINE) g_pagineF1[i] = p;
        else                  g_paginaCal = p;
    }
    uiF1Create(g_pagineF1);
    uiCalcioCreate(g_paginaCal);

    aggiornaSchede();
    aggiornaPagina(true);
    lv_timer_create(tickCb, 1000, nullptr);
}

void uiSetTouchDebug(const char *text)
{
#if SHOW_TOUCH_DEBUG
    if (g_lblTouch != nullptr) lv_label_set_text(g_lblTouch, text);
#else
    (void)text;
#endif
}
