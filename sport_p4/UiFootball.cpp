/* ============================================================================
 *  UiFootball - one tab per competition
 *
 *  Match list grouped by day (scrollable, opens on today).
 *  Serie A: list on the left, standings on the right.
 *  Live: row with a red border and the minute; after a goal the score
 *  stays highlighted in yellow for two minutes.
 * ========================================================================== */
#include "Ui.h"
#include "esp_heap_caps.h"

#define LISTA_Y        40
#define LISTA_H        410
#define LISTA_W_SERIEA 620
#define CLASS_X        (UI_PAD + LISTA_W_SERIEA + 8)
#define CLASS_W        (UI_INNER_W - LISTA_W_SERIEA - 8)
#define RIGA_H         46
#define RIGA_PASSO     50
#define GIORNO_H       30

static lv_obj_t *s_titolo = nullptr;
static lv_obj_t *s_info   = nullptr;
static lv_obj_t *s_lista  = nullptr;
static lv_obj_t *s_class  = nullptr;

struct RigaClass { lv_obj_t *zona, *pos, *squadra, *g, *dr, *pt; };
static RigaClass s_cr[20];

static Competizione *s_copia = nullptr;     /* local copy, PSRAM */
static Classifica    s_cl;
static uint8_t  s_compMostrata = 255;
static uint32_t s_ver = 0xFFFFFFFF;
static uint8_t  s_flash = 0xFF;
static uint32_t s_classVer = 0xFFFFFFFF;

/* ---------------------------------------------------------------------------
 *  Standings
 * ------------------------------------------------------------------------- */
static void creaClassifica(lv_obj_t *pg)
{
    s_class = uiCard(pg, CLASS_X, LISTA_Y, CLASS_W, LISTA_H);
    const lv_coord_t W = CLASS_W - 16;
    lv_obj_t *h;
    h = uiCol(s_class, 6, 0, 150, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(h, "STANDINGS");
    h = uiCol(s_class, W - 138, 0, 30, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "P");
    h = uiCol(s_class, W - 102, 0, 42, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "GD");
    h = uiCol(s_class, W - 52, 0, 52, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(h, "PTS");

    for (uint8_t r = 0; r < 20; ++r) {
        RigaClass &R = s_cr[r];
        const lv_coord_t y = 22 + r * 18;
        R.zona    = uiBox(s_class, 0, y + 1, 3, 15, C_BORDER);
        R.pos     = uiCol(s_class, 6, y, 22, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        R.squadra = uiCol(s_class, 34, y, W - 34 - 142, &lv_font_montserrat_14, C_TEXT, LV_TEXT_ALIGN_LEFT);
        R.g       = uiCol(s_class, W - 138, y, 30, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        R.dr      = uiCol(s_class, W - 102, y, 42, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        R.pt      = uiCol(s_class, W - 52,  y - 1, 52, &lv_font_montserrat_16, C_ORANGE, LV_TEXT_ALIGN_RIGHT);
    }
}

static lv_color_t coloreZona(uint8_t pos, uint8_t n)
{
    if (pos >= 1 && pos <= 4) return C_BLUE;     /* Champions League  */
    if (pos == 5)             return C_ORANGE;   /* Europa League     */
    if (pos == 6)             return C_GREEN;    /* Conference League */
    if (n >= 18 && pos > n - 3) return C_RED;    /* relegation        */
    return C_BORDER;
}

static void aggiornaClassifica(bool forza)
{
    if (!dataLock(30)) return;
    if (!forza && g_classSerieA.versione == s_classVer) { dataUnlock(); return; }
    s_cl = g_classSerieA;
    s_classVer = g_classSerieA.versione;
    dataUnlock();

    char buf[16];
    for (uint8_t r = 0; r < 20; ++r) {
        RigaClass &R = s_cr[r];
        if (r >= s_cl.n) {
            uiSetText(R.pos, ""); uiSetText(R.squadra, r == 0 ? "waiting for data..." : "");
            uiSetText(R.g, ""); uiSetText(R.dr, ""); uiSetText(R.pt, "");
            lv_obj_set_style_bg_color(R.zona, C_CARD, 0);
            continue;
        }
        const RigaClassifica &x = s_cl.r[r];
        snprintf(buf, sizeof(buf), "%u", x.pos);
        uiSetText(R.pos, buf);
        uiSetText(R.squadra, x.squadra);
        snprintf(buf, sizeof(buf), "%u", x.g);
        uiSetText(R.g, buf);
        snprintf(buf, sizeof(buf), "%+d", x.dr);
        uiSetText(R.dr, buf);
        snprintf(buf, sizeof(buf), "%d", x.pt);
        uiSetText(R.pt, buf);
        lv_obj_set_style_bg_color(R.zona, coloreZona(x.pos, s_cl.n), 0);
    }
}

/* ---------------------------------------------------------------------------
 *  Match list
 * ------------------------------------------------------------------------- */
static uint8_t contaFlash(const Competizione &cp)
{
    uint8_t n = 0;
    for (uint16_t i = 0; i < cp.n; ++i) {
        if (cp.p[i].golMs != 0 && millis() - cp.p[i].golMs < CALCIO_GOL_EVIDENZA_MS) ++n;
    }
    return n;
}

/* ESPN id of the rows on screen: the index travels as the event user_data */
static char s_idRighe[CALCIO_MAX_PARTITE][16];
static void apriDettaglio(const char *id);

static void rigaClickCb(lv_event_t *e)
{
    const uint32_t i = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (i < CALCIO_MAX_PARTITE && s_idRighe[i][0]) apriDettaglio(s_idRighe[i]);
}

/* small red square with the number of players sent off */
static void badgeRosso(lv_obj_t *r, lv_coord_t x, uint8_t n)
{
    if (n == 0) return;
    lv_obj_t *b = uiBox(r, x, 13, 14, 20, C_RED);
    lv_obj_set_style_radius(b, 2, 0);
    if (n > 1) {
        char s[4];
        snprintf(s, sizeof(s), "%u", n);
        lv_obj_t *l = uiLabel(b, s, &lv_font_montserrat_12, C_TEXT);
        lv_obj_center(l);
    }
}

static lv_obj_t *creaRiga(lv_obj_t *lista, lv_coord_t y, lv_coord_t W, const Partita &m, uint16_t indice)
{
    lv_obj_t *r = lv_obj_create(lista);
    lv_obj_set_pos(r, 0, y);
    lv_obj_set_size(r, W, RIGA_H);
    lv_obj_set_style_bg_color(r, C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    const bool live = m.stato == 'i';
    lv_obj_set_style_border_color(r, live ? C_RED : C_BORDER, 0);
    lv_obj_set_style_border_width(r, live ? 2 : 1, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    /* tap on the row = detail card (scrolling the list cancels the click) */
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, rigaClickCb, LV_EVENT_CLICKED, (void *)(uintptr_t)indice);

    /* ---- chip: time / minute / status ---- */
    char buf[48];
    lv_color_t chipBg = C_CARD2, chipFg = C_TEXT;
    switch (m.stato) {
        case 'i': snprintf(buf, sizeof(buf), "%s", m.minuto[0] ? m.minuto : "LIVE"); chipBg = C_RED; chipFg = C_TEXT; break;
        case 'f': snprintf(buf, sizeof(buf), "%s", m.dettaglio); chipFg = C_MUTED; break;
        case 'x': snprintf(buf, sizeof(buf), "%s", m.dettaglio); chipBg = C_YELLOW; chipFg = C_DARK; break;
        default:  fmtOra(m.inizio, buf, sizeof(buf)); break;
    }
    lv_obj_t *chip = uiBox(r, 5, 5, 80, RIGA_H - 10, chipBg);
    lv_obj_t *lc = uiLabel(chip, buf, m.stato == 'p' ? &lv_font_montserrat_18 : &lv_font_montserrat_14, chipFg);
    lv_obj_center(lc);

    /* ---- teams and score ---- */
    const lv_coord_t SC = 104;
    const lv_coord_t notaW = W > 800 ? 220 : 0;
    /* 22 px per side are left free for the sent-off square */
    const lv_coord_t nomeW = (W - 92 - SC - 52 - notaW - (notaW ? 8 : 0) - 8) / 2;
    const lv_coord_t xCasa = 92;
    const lv_coord_t xSc   = xCasa + nomeW + 26;
    const lv_coord_t xOsp  = xSc + SC + 26;

    const bool vintaCasa = m.stato == 'f' && m.golCasa >= 0 &&
                           (m.golCasa > m.golOspite || (m.golCasa == m.golOspite && m.rigCasa > m.rigOspite));
    const bool vintaOsp  = m.stato == 'f' && m.golOspite >= 0 &&
                           (m.golOspite > m.golCasa || (m.golCasa == m.golOspite && m.rigOspite > m.rigCasa));
    const bool finita = m.stato == 'f';

    lv_obj_t *casa = uiCol(r, xCasa, 11, nomeW, &lv_font_montserrat_20,
                           finita && !vintaCasa ? C_MUTED : C_TEXT, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(casa, m.casa);
    lv_obj_t *osp = uiCol(r, xOsp, 11, nomeW, &lv_font_montserrat_20,
                          finita && !vintaOsp ? C_MUTED : C_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(osp, m.ospite);

    const bool flash = m.golMs != 0 && millis() - m.golMs < CALCIO_GOL_EVIDENZA_MS;
    lv_obj_t *box = uiBox(r, xSc, 5, SC, RIGA_H - 10, flash ? C_GOLD : C_BG);
    const lv_font_t *fnt = &lv_font_montserrat_22;
    if (m.golCasa >= 0 && m.golOspite >= 0) {
        if (m.rigCasa >= 0 && m.rigOspite >= 0 && m.golCasa == m.golOspite) {
            snprintf(buf, sizeof(buf), "%d-%d (%d-%d)", m.golCasa, m.golOspite, m.rigCasa, m.rigOspite);
            fnt = &lv_font_montserrat_14;
        } else {
            snprintf(buf, sizeof(buf), "%d - %d", m.golCasa, m.golOspite);
        }
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    lv_obj_t *ls = uiLabel(box, buf, fnt, flash ? C_DARK : (live ? C_GOLD : C_TEXT));
    lv_obj_center(ls);
    badgeRosso(r, xSc - 20, m.rossiCasa);
    badgeRosso(r, xSc + SC + 6, m.rossiOspite);

    if (notaW > 0 && m.nota[0]) {
        lv_obj_t *nt = uiCol(r, W - notaW - 8, 14, notaW, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        lv_label_set_text(nt, m.nota);
    }
    return r;
}

static void costruisciLista(const Competizione &cp, bool vaiAOggi)
{
    const lv_coord_t scrollPrima = lv_obj_get_scroll_y(s_lista);
    lv_obj_clean(s_lista);

    const lv_coord_t W = lv_obj_get_width(s_lista) - 14;   /* room for the scrollbar */

    if (cp.n == 0) {
        lv_obj_t *l = uiCol(s_lista, 0, 120, W, &lv_font_montserrat_22, C_MUTED, LV_TEXT_ALIGN_CENTER);
        if (!cp.caricata && cp.errore[0] == 0) lv_label_set_text(l, "Loading...");
        else if (cp.errore[0])                 lv_label_set_text_fmt(l, "Data not available (%s)", cp.errore);
        else                                   lv_label_set_text(l, "No matches scheduled");
        return;
    }

    const uint32_t oggi = chiaveGiorno(time(nullptr));
    uint32_t giornoPrec = 0;
    lv_coord_t y = 0;
    lv_coord_t yTarget = -1;

    for (uint16_t i = 0; i < cp.n; ++i) {
        const Partita &m = cp.p[i];
        const uint32_t g = chiaveGiorno(m.inizio);
        if (g != giornoPrec) {
            char data[24], t[48];
            fmtGiorno(m.inizio, data, sizeof(data));
            if (g == oggi)          snprintf(t, sizeof(t), "TODAY  -  %s", data);
            else if (g == oggi + 1) snprintf(t, sizeof(t), "Tomorrow  -  %s", data);
            else                    snprintf(t, sizeof(t), "%s", data);
            lv_obj_t *h = uiCol(s_lista, 4, y + 6, W - 8, &lv_font_montserrat_16,
                                g == oggi ? C_ORANGE : C_MUTED, LV_TEXT_ALIGN_LEFT);
            lv_label_set_text(h, t);
            if (yTarget < 0 && g >= oggi) yTarget = y;
            y += GIORNO_H;
            giornoPrec = g;
        }
        if (yTarget < 0 && m.stato == 'i') yTarget = y;
        if (i < CALCIO_MAX_PARTITE) snprintf(s_idRighe[i], sizeof(s_idRighe[i]), "%s", m.id);
        creaRiga(s_lista, y, W, m, i);
        y += RIGA_PASSO;
    }
    /* room at the bottom so the last day can also be brought to the top */
    lv_obj_t *fondo = uiBox(s_lista, 0, y, 10, 10, C_BG);
    (void)fondo;

    lv_obj_update_layout(s_lista);
    if (vaiAOggi) lv_obj_scroll_to_y(s_lista, yTarget >= 0 ? yTarget : y, LV_ANIM_OFF);
    else          lv_obj_scroll_to_y(s_lista, scrollPrima, LV_ANIM_OFF);
}

/* ---------------------------------------------------------------------------
 *  Detail card of a match
 *
 *    [ Home      2 - 1      Away   ]  [CLOSE]
 *    [ minute / status  -  stadium  -  date   ]
 *    TIMELINE (goals, cards, subs)      STATISTICS + LINE-UPS
 * ------------------------------------------------------------------------- */
#define DET_TOP_H   92
#define DET_COL_Y   (UI_PAD + DET_TOP_H + 8)
#define DET_COL_H   (UI_CONTENT_H - DET_COL_Y - UI_PAD)
#define DET_SX_W    480
#define DET_DX_X    (UI_PAD + DET_SX_W + 8)
#define DET_DX_W    (UI_INNER_W - DET_SX_W - 8)
#define C_CASA      lv_color_hex(0x58A6FF)
#define C_OSPITE    lv_color_hex(0xF7931A)

static lv_obj_t *s_detPag = nullptr;
static lv_obj_t *s_detCasa = nullptr, *s_detOsp = nullptr, *s_detRis = nullptr, *s_detInfo = nullptr;
static lv_obj_t *s_detCron = nullptr, *s_detStat = nullptr;
static DettaglioPartita *s_detCopia = nullptr;
static uint32_t s_detVer = 0xFFFFFFFF;
static bool     s_detVisibile = false;

static lv_obj_t *colonnaScorrevole(lv_obj_t *parent, lv_coord_t x, lv_coord_t w)
{
    lv_obj_t *c = uiCard(parent, x, DET_COL_Y, w, DET_COL_H);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(c, C_MUTED, LV_PART_SCROLLBAR);
    return c;
}

static void chiudiCb(lv_event_t *e)
{
    (void)e;
    uiCalcioChiudiDettaglio();
}

static void creaDettaglio(lv_obj_t *pg)
{
    s_detCopia = static_cast<DettaglioPartita *>(heap_caps_calloc(1, sizeof(DettaglioPartita), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_detPag = lv_obj_create(pg);
    lv_obj_set_pos(s_detPag, 0, 0);
    lv_obj_set_size(s_detPag, UI_W, UI_CONTENT_H);
    lv_obj_set_style_bg_color(s_detPag, C_BG, 0);
    lv_obj_set_style_bg_opa(s_detPag, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_detPag, 0, 0);
    lv_obj_set_style_radius(s_detPag, 0, 0);
    lv_obj_set_style_pad_all(s_detPag, 0, 0);
    lv_obj_remove_flag(s_detPag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detPag, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *top = uiCard(s_detPag, UI_PAD, UI_PAD, UI_INNER_W, DET_TOP_H);
    s_detCasa = uiCol(top, 0, 6, 370, &lv_font_montserrat_28, C_TEXT, LV_TEXT_ALIGN_RIGHT);
    s_detRis  = uiCol(top, 380, 0, 210, &lv_font_montserrat_36, C_GOLD, LV_TEXT_ALIGN_CENTER);
    s_detOsp  = uiCol(top, 600, 6, 260, &lv_font_montserrat_28, C_TEXT, LV_TEXT_ALIGN_LEFT);
    s_detInfo = uiCol(top, 0, 52, 860, &lv_font_montserrat_16, C_MUTED, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *b = lv_button_create(top);
    lv_obj_set_size(b, 120, 56);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(b, C_CARD2, 0);
    lv_obj_set_style_border_color(b, C_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, chiudiCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = uiLabel(b, LV_SYMBOL_CLOSE "  CLOSE", &lv_font_montserrat_16, C_TEXT);
    lv_obj_center(l);

    s_detCron = colonnaScorrevole(s_detPag, UI_PAD, DET_SX_W);
    s_detStat = colonnaScorrevole(s_detPag, DET_DX_X, DET_DX_W);
}

static lv_obj_t *etichetta(lv_obj_t *p, lv_coord_t x, lv_coord_t y, lv_coord_t w, const char *txt,
                           const lv_font_t *f, lv_color_t c, lv_text_align_t a)
{
    lv_obj_t *l = uiCol(p, x, y, w, f, c, a);
    lv_label_set_text(l, txt);
    return l;
}

static void costruisciDettaglio()
{
    const DettaglioPartita &d = *s_detCopia;
    char buf[128];

    uiSetText(s_detCasa, d.casa[0] ? d.casa : "");
    uiSetText(s_detOsp, d.ospite[0] ? d.ospite : "");
    if (d.golCasa >= 0 && d.golOspite >= 0) snprintf(buf, sizeof(buf), "%d - %d", d.golCasa, d.golOspite);
    else if (d.inizio) fmtOra(d.inizio, buf, sizeof(buf));
    else snprintf(buf, sizeof(buf), "-");
    uiSetText(s_detRis, buf);
    uiSetColor(s_detRis, d.stato == 'i' ? C_GOLD : C_TEXT);

    char data[24] = "", ora[8] = "";
    if (d.inizio) { fmtGiorno(d.inizio, data, sizeof(data)); fmtOra(d.inizio, ora, sizeof(ora)); }
    const char *stato = d.stato == 'i' ? (d.minuto[0] ? d.minuto : "IN PROGRESS") : (d.stato == 'f' ? "FULL TIME" : "NOT STARTED");
    if (d.spettatori > 0)
        snprintf(buf, sizeof(buf), "%s   -   %s   -   %s %s   -   %s   -   %lu spectators", stato,
                 CALCIO_NOME[d.comp < CALCIO_COMP_COUNT ? d.comp : 0], data, ora, d.stadio, (unsigned long)d.spettatori);
    else
        snprintf(buf, sizeof(buf), "%s   -   %s   -   %s %s   -   %s", stato,
                 CALCIO_NOME[d.comp < CALCIO_COMP_COUNT ? d.comp : 0], data, ora, d.stadio);
    uiSetText(s_detInfo, buf);
    uiSetColor(s_detInfo, d.stato == 'i' ? C_RED : C_MUTED);

    /* ---- timeline ---- */
    const lv_coord_t scCron = lv_obj_get_scroll_y(s_detCron);
    lv_obj_clean(s_detCron);
    const lv_coord_t WC = DET_SX_W - 16 - 12;
    etichetta(s_detCron, 0, 0, WC, "TIMELINE", &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *lc = etichetta(s_detCron, 100, 0, WC - 100, "", &lv_font_montserrat_14, C_CASA, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text_fmt(lc, "%s", d.casa);
    lv_obj_set_width(lc, (WC - 100) / 2 - 4);
    lv_obj_t *lo = etichetta(s_detCron, 100 + (WC - 100) / 2 + 4, 0, (WC - 100) / 2 - 4, d.ospite, &lv_font_montserrat_14, C_OSPITE, LV_TEXT_ALIGN_RIGHT);
    (void)lo;

    lv_coord_t y = 26;
    if (!d.pronto) {
        etichetta(s_detCron, 0, 60, WC, d.errore[0] ? d.errore : "Loading...", &lv_font_montserrat_20, C_MUTED, LV_TEXT_ALIGN_CENTER);
    } else if (d.nEv == 0) {
        etichetta(s_detCron, 0, 60, WC, d.stato == 'p' ? "The match has not started yet" : "No events",
                  &lv_font_montserrat_20, C_MUTED, LV_TEXT_ALIGN_CENTER);
    }
    for (uint8_t i = 0; i < d.nEv; ++i) {
        const EventoPartita &ev = d.ev[i];
        const char *sigla = "";
        lv_color_t sf = C_CARD2, tx = C_TEXT;
        switch (ev.tipo) {
            case 'G': sigla = "GOAL"; sf = C_GREEN;  tx = C_DARK; break;
            case 'R': sigla = "PEN";  sf = C_GREEN;  tx = C_DARK; break;
            case 'A': sigla = "OG";   sf = C_ORANGE; tx = C_DARK; break;
            case 'Y': sigla = "YEL";  sf = C_GOLD;   tx = C_DARK; break;
            case 'X': sigla = "RED";  sf = C_RED;    tx = C_TEXT; break;
            case 'S': sigla = "SUB";  sf = C_CARD2;  tx = C_TEXT; break;
            default: break;
        }
        uiBox(s_detCron, 0, y + 2, 4, 26, ev.casa ? C_CASA : C_OSPITE);
        etichetta(s_detCron, 8, y + 6, 54, ev.minuto, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
        lv_obj_t *chip = uiBox(s_detCron, 64, y + 3, 50, 24, sf);
        lv_obj_t *cl = uiLabel(chip, sigla, &lv_font_montserrat_12, tx);
        lv_obj_center(cl);
        if (ev.tipo == 'S')
            snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %s   " LV_SYMBOL_DOWN " %s", ev.nome, ev.nome2);
        else
            snprintf(buf, sizeof(buf), "%s", ev.nome);
        etichetta(s_detCron, 122, y + 5, WC - 122, buf, &lv_font_montserrat_16,
                  ev.tipo == 'G' || ev.tipo == 'R' || ev.tipo == 'A' ? C_TEXT : C_MUTED, LV_TEXT_ALIGN_LEFT);
        y += 32;
    }
    lv_obj_update_layout(s_detCron);
    lv_obj_scroll_to_y(s_detCron, scCron, LV_ANIM_OFF);

    /* ---- statistics and line-ups ---- */
    const lv_coord_t scStat = lv_obj_get_scroll_y(s_detStat);
    lv_obj_clean(s_detStat);
    const lv_coord_t WS = DET_DX_W - 16 - 12;
    etichetta(s_detStat, 0, 0, WS, "STATISTICS", &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
    y = 24;
    if (d.pronto && d.nStat == 0) {
        etichetta(s_detStat, 0, y, WS, "Statistics not available", &lv_font_montserrat_16, C_MUTED, LV_TEXT_ALIGN_LEFT);
        y += 30;
    }
    for (uint8_t i = 0; i < d.nStat; ++i) {
        etichetta(s_detStat, 0, y, 80, d.statCasa[i], &lv_font_montserrat_18, C_CASA, LV_TEXT_ALIGN_LEFT);
        etichetta(s_detStat, 80, y + 2, WS - 160, d.statNome[i], &lv_font_montserrat_14, C_TEXT, LV_TEXT_ALIGN_CENTER);
        etichetta(s_detStat, WS - 80, y, 80, d.statOsp[i], &lv_font_montserrat_18, C_OSPITE, LV_TEXT_ALIGN_RIGHT);
        /* proportional bar */
        const float a = atof(d.statCasa[i]), b = atof(d.statOsp[i]);
        const lv_coord_t wTot = WS;
        lv_coord_t wa = (a + b) > 0 ? (lv_coord_t)(wTot * a / (a + b)) : wTot / 2;
        if (wa < 2) wa = 2;
        if (wa > wTot - 2) wa = wTot - 2;
        uiBox(s_detStat, 0, y + 24, wa - 1, 4, C_CASA);
        uiBox(s_detStat, wa + 1, y + 24, wTot - wa - 1, 4, C_OSPITE);
        y += 36;
    }

    if (d.nCasa > 0 || d.nOsp > 0) {
        y += 8;
        etichetta(s_detStat, 0, y, WS, "LINE-UPS", &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
        y += 22;
        const lv_coord_t colW = WS / 2 - 6;
        snprintf(buf, sizeof(buf), "%s  %s", d.casa, d.moduloCasa);
        etichetta(s_detStat, 0, y, colW, buf, &lv_font_montserrat_16, C_CASA, LV_TEXT_ALIGN_LEFT);
        snprintf(buf, sizeof(buf), "%s  %s", d.ospite, d.moduloOsp);
        etichetta(s_detStat, colW + 12, y, colW, buf, &lv_font_montserrat_16, C_OSPITE, LV_TEXT_ALIGN_LEFT);
        y += 24;
        const uint8_t righe = d.nCasa > d.nOsp ? d.nCasa : d.nOsp;
        for (uint8_t i = 0; i < righe; ++i) {
            for (uint8_t lato = 0; lato < 2; ++lato) {
                const uint8_t n = lato == 0 ? d.nCasa : d.nOsp;
                if (i >= n) continue;
                const Giocatore &g = lato == 0 ? d.gCasa[i] : d.gOsp[i];
                const bool primoEntrato = !g.titolare && (i == 0 || (lato == 0 ? d.gCasa[i - 1] : d.gOsp[i - 1]).titolare);
                const lv_coord_t x = lato == 0 ? 0 : colW + 12;
                if (g.titolare)
                    snprintf(buf, sizeof(buf), "%2s  %s%s", g.num, g.nome, g.uscito ? "  " LV_SYMBOL_DOWN : "");
                else
                    snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %s  %s", g.num, g.nome);
                lv_obj_t *l = etichetta(s_detStat, x, y, colW, buf, &lv_font_montserrat_14,
                                        g.titolare ? C_TEXT : C_GREEN, LV_TEXT_ALIGN_LEFT);
                (void)l;
                (void)primoEntrato;
            }
            y += 19;
        }
    }
    lv_obj_update_layout(s_detStat);
    lv_obj_scroll_to_y(s_detStat, scStat, LV_ANIM_OFF);
}

static void aggiornaDettaglio(bool forza)
{
    if (!dataLock(30)) return;
    const bool cambiato = forza || g_det->versione != s_detVer;
    if (cambiato) {
        memcpy(s_detCopia, g_det, sizeof(DettaglioPartita));
        s_detVer = g_det->versione;
    }
    dataUnlock();
    if (cambiato) costruisciDettaglio();
}

static void apriDettaglio(const char *id)
{
    if (s_detPag == nullptr || s_detCopia == nullptr) return;
    const uint8_t c = g_compVisibile < CALCIO_COMP_COUNT ? g_compVisibile : 0;
    if (dataLock(200)) {
        /* provisional heading from the list, the rest comes from the football task */
        const uint32_t ver = g_det->versione;
        memset(g_det, 0, sizeof(DettaglioPartita));
        g_det->versione = ver + 1;
        snprintf(g_det->id, sizeof(g_det->id), "%s", id);
        g_det->comp = c;
        g_det->golCasa = g_det->golOspite = -1;
        for (uint16_t i = 0; i < g_comp[c].n; ++i) {
            const Partita &m = g_comp[c].p[i];
            if (strcmp(m.id, id) != 0) continue;
            snprintf(g_det->casa, sizeof(g_det->casa), "%s", m.casa);
            snprintf(g_det->ospite, sizeof(g_det->ospite), "%s", m.ospite);
            g_det->golCasa = m.golCasa;
            g_det->golOspite = m.golOspite;
            g_det->stato = m.stato;
            snprintf(g_det->minuto, sizeof(g_det->minuto), "%s", m.minuto);
            g_det->inizio = m.inizio;
        }
        snprintf(g_detId, sizeof(g_detId), "%s", id);
        g_detComp = c;
        dataUnlock();
    }
    g_detAperto = true;
    g_detRichiesta = true;
    s_detVisibile = true;
    lv_obj_remove_flag(s_detPag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_detCron, 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(s_detStat, 0, LV_ANIM_OFF);
    aggiornaDettaglio(true);
}

void uiCalcioChiudiDettaglio()
{
    if (!s_detVisibile) return;
    s_detVisibile = false;
    g_detAperto = false;
    if (s_detPag != nullptr) lv_obj_add_flag(s_detPag, LV_OBJ_FLAG_HIDDEN);
    s_ver = 0xFFFFFFFF;          /* the list must be redrawn */
}

/* ---------------------------------------------------------------------------
 *  API
 * ------------------------------------------------------------------------- */
void uiCalcioCreate(lv_obj_t *pg)
{
    s_copia = static_cast<Competizione *>(heap_caps_calloc(1, sizeof(Competizione), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    s_titolo = uiCol(pg, UI_PAD + 4, 8, 640, &lv_font_montserrat_22, C_TEXT, LV_TEXT_ALIGN_LEFT);
    s_info   = uiCol(pg, UI_PAD + 650, 12, UI_INNER_W - 654, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);

    s_lista = lv_obj_create(pg);
    lv_obj_set_pos(s_lista, UI_PAD, LISTA_Y);
    lv_obj_set_size(s_lista, UI_INNER_W, LISTA_H);
    lv_obj_set_style_bg_opa(s_lista, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lista, 0, 0);
    lv_obj_set_style_pad_all(s_lista, 0, 0);
    lv_obj_set_style_radius(s_lista, 0, 0);
    lv_obj_set_scroll_dir(s_lista, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_lista, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_lista, C_MUTED, LV_PART_SCROLLBAR);

    creaClassifica(pg);
    lv_obj_add_flag(s_class, LV_OBJ_FLAG_HIDDEN);
    creaDettaglio(pg);           /* last: it sits above list and standings */
}

void uiCalcioUpdate(bool forza)
{
    if (s_copia == nullptr) return;
    if (s_detVisibile) {         /* detail card open: only that one is updated */
        aggiornaDettaglio(forza);
        return;
    }
    const uint8_t c = g_compVisibile < CALCIO_COMP_COUNT ? g_compVisibile : 0;
    const bool nuovaComp = c != s_compMostrata;

    if (!dataLock(30)) return;
    const uint8_t flash = contaFlash(g_comp[c]);
    const bool cambiato = forza || nuovaComp || g_comp[c].versione != s_ver || flash != s_flash;
    if (cambiato) {
        memcpy(s_copia, &g_comp[c], sizeof(Competizione));
        s_ver = g_comp[c].versione;
        s_flash = flash;
    }
    dataUnlock();

    const bool serieA = c == 0;
    if (nuovaComp) {
        s_compMostrata = c;
        lv_obj_set_width(s_lista, serieA ? LISTA_W_SERIEA : UI_INNER_W);
        if (serieA) lv_obj_remove_flag(s_class, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_class, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(s_lista);
    }
    if (serieA) aggiornaClassifica(forza || nuovaComp);
    if (!cambiato) return;

    const Competizione &cp = *s_copia;
    char buf[96];
    if (cp.ripiego && cp.n > 0) snprintf(buf, sizeof(buf), "%s  -  latest matches", CALCIO_NOME[c]);
    else                        snprintf(buf, sizeof(buf), "%s", CALCIO_NOME[c]);
    uiSetText(s_titolo, buf);

    if (cp.live > 0) {
        snprintf(buf, sizeof(buf), "%u in progress  -  updated every %lu s", cp.live,
                 (unsigned long)(CALCIO_LIVE_ATTIVO_MS / 1000));
        uiSetColor(s_info, C_RED);
    } else if (cp.caricata) {
        snprintf(buf, sizeof(buf), "%u matches  -  from %d days ago to +%d", cp.n, CALCIO_GIORNI_PRIMA, CALCIO_GIORNI_DOPO);
        uiSetColor(s_info, C_MUTED);
    } else {
        buf[0] = 0;
    }
    uiSetText(s_info, buf);

    costruisciLista(cp, forza || nuovaComp);
}

void uiCalcioStato(char *out, size_t n)
{
    uint8_t live[CALCIO_COMP_COUNT] = { 0 };
    uint8_t tot = 0;
    if (dataLock(20)) {
        for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) { live[c] = g_comp[c].live; tot += live[c]; }
        dataUnlock();
    }
    if (tot == 0) {
        snprintf(out, n, "ESPN data  -  real-time results: checked every %lu s, every %lu s during matches",
                 (unsigned long)(CALCIO_LIVE_IDLE_MS / 1000), (unsigned long)(CALCIO_LIVE_ATTIVO_MS / 1000));
        return;
    }
    size_t o = snprintf(out, n, "LIVE: ");
    for (uint8_t c = 0; c < CALCIO_COMP_COUNT && o < n; ++c) {
        if (live[c] == 0) continue;
        o += snprintf(out + o, n - o, "%s %u   ", CALCIO_TAB[c], live[c]);
    }
}
