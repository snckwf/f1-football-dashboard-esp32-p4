/* ============================================================================
 *  UiF1 - LIVE, DRIVERS, CONSTRUCTORS, LAST RACE
 *
 *  All rows are created only once at startup and then filled:
 *  no LVGL object is recreated while running.
 * ========================================================================== */
#include "Ui.h"
#include "BoardLvgl.h"
#include "esp_heap_caps.h"
#include <Preferences.h>

lv_color_t uiColoreGomma(char g)
{
    switch (g) {
        case 'S': return C_F1RED;
        case 'M': return C_GOLD;
        case 'H': return C_TEXT;
        case 'I': return C_GREEN;
        case 'W': return C_BLUE;
        default:  return C_MUTED;
    }
}

/* ===========================================================================
 *  LIVE
 * ========================================================================= */
#define LIVE_RIGHE      12          /* per column: 24 drivers at most */
#define LIVE_RIGA_H     30
#define LIVE_GRIGLIA_Y  86

struct RigaLive {
    lv_obj_t *pos, *bar, *tla, *nome, *gomma, *freccia, *v1, *v2, *tag;
};

static RigaLive  s_live[LIVE_RIGHE * 2];
static lv_obj_t *s_liveTitolo = nullptr;
static lv_obj_t *s_liveSotto  = nullptr;
static lv_obj_t *s_chipGiro   = nullptr;
static lv_obj_t *s_chipPista  = nullptr;
static lv_obj_t *s_chipStato  = nullptr;
static lv_obj_t *s_lblPista   = nullptr;
static lv_obj_t *s_lblStato   = nullptr;
static lv_obj_t *s_liveVuoto  = nullptr;
static lv_obj_t *s_intest[2][2] = { { nullptr } };
static uint32_t  s_liveVer = 0xFFFFFFFF;

/* local copy so the lock is not held while drawing */
static F1Live *s_L = nullptr;
static F1Dati *s_D = nullptr;

static void creaLive(lv_obj_t *pg)
{
    lv_obj_t *card = uiCard(pg, UI_PAD, UI_PAD, UI_INNER_W, 70);
    s_liveTitolo = uiCol(card, 0, 0, 640, &lv_font_montserrat_22, C_TEXT, LV_TEXT_ALIGN_LEFT);
    s_liveSotto  = uiCol(card, 0, 30, 640, &lv_font_montserrat_16, C_MUTED, LV_TEXT_ALIGN_LEFT);

    s_chipGiro = uiCol(card, 650, 4, 120, &lv_font_montserrat_22, C_TEXT, LV_TEXT_ALIGN_RIGHT);

    s_chipPista = uiBox(card, 780, 2, 100, 48, C_GREEN);
    s_lblPista = uiLabel(s_chipPista, "GREEN", &lv_font_montserrat_18, C_DARK);
    lv_obj_center(s_lblPista);

    s_chipStato = uiBox(card, 888, 2, 102, 48, C_CARD2);
    s_lblStato = uiLabel(s_chipStato, "--", &lv_font_montserrat_16, C_TEXT);
    lv_obj_center(s_lblStato);

    for (uint8_t col = 0; col < 2; ++col) {
        const lv_coord_t x0 = UI_PAD + col * 512;
        s_intest[col][0] = uiCol(pg, x0 + 236, LIVE_GRIGLIA_Y - 6, 114, &lv_font_montserrat_12, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        s_intest[col][1] = uiCol(pg, x0 + 352, LIVE_GRIGLIA_Y - 6, 94,  &lv_font_montserrat_12, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        for (uint8_t r = 0; r < LIVE_RIGHE; ++r) {
            RigaLive &R = s_live[col * LIVE_RIGHE + r];
            const lv_coord_t y = LIVE_GRIGLIA_Y + 10 + r * LIVE_RIGA_H;
            R.pos     = uiCol(pg, x0,       y, 30,  &lv_font_montserrat_18, C_TEXT,  LV_TEXT_ALIGN_RIGHT);
            R.bar     = uiBox(pg, x0 + 36,  y + 1, 5, 20, C_MUTED);
            R.tla     = uiCol(pg, x0 + 46,  y, 52,  &lv_font_montserrat_18, C_TEXT,  LV_TEXT_ALIGN_LEFT);
            R.nome    = uiCol(pg, x0 + 100, y + 3, 70,  &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
            R.gomma   = uiCol(pg, x0 + 172, y + 2, 40,  &lv_font_montserrat_16, C_MUTED, LV_TEXT_ALIGN_LEFT);
            R.freccia = uiCol(pg, x0 + 214, y + 2, 20,  &lv_font_montserrat_16, C_GREEN, LV_TEXT_ALIGN_CENTER);
            R.v1      = uiCol(pg, x0 + 236, y + 1, 114, &lv_font_montserrat_16, C_TEXT,  LV_TEXT_ALIGN_RIGHT);
            R.v2      = uiCol(pg, x0 + 352, y + 1, 94,  &lv_font_montserrat_16, C_MUTED, LV_TEXT_ALIGN_RIGHT);
            R.tag     = uiCol(pg, x0 + 450, y + 2, 46,  &lv_font_montserrat_14, C_YELLOW, LV_TEXT_ALIGN_CENTER);
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_liveVuoto = uiCol(pg, UI_PAD + 40, LIVE_GRIGLIA_Y + 30, UI_INNER_W - 80, &lv_font_montserrat_22, C_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_liveVuoto, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(s_liveVuoto, LV_OBJ_FLAG_HIDDEN);
}

static void testoPista(uint8_t s, const char **txt, lv_color_t *bg, lv_color_t *fg)
{
    *fg = C_DARK;
    switch (s) {
        case 2: *txt = "YELLOW"; *bg = C_GOLD;  break;
        case 4: *txt = "SC";     *bg = C_GOLD;  break;
        case 5: *txt = "RED";    *bg = C_F1RED; *fg = C_TEXT; break;
        case 6: *txt = "VSC";    *bg = C_GOLD;  break;
        case 7: *txt = "VSC END"; *bg = C_YELLOW; break;
        case 1: *txt = "GREEN";  *bg = C_GREEN; break;
        default: *txt = "--";    *bg = C_CARD2; *fg = C_MUTED; break;
    }
}

static void testoStato(const char *st, bool connesso, const char **txt, lv_color_t *bg)
{
    if (!connesso)                              { *txt = "OFFLINE"; *bg = C_CARD2; return; }
    if (strcmp(st, "Started") == 0)             { *txt = "LIVE";    *bg = C_F1RED; return; }
    if (strcmp(st, "Aborted") == 0)             { *txt = "STOPPED"; *bg = C_F1RED; return; }
    if (strcmp(st, "Finished") == 0 ||
        strcmp(st, "Finalised") == 0 ||
        strcmp(st, "Ends") == 0)                { *txt = "FINISHED"; *bg = C_CARD2; return; }
    if (strcmp(st, "Inactive") == 0)            { *txt = "WAITING"; *bg = C_CARD2; return; }
    *txt = "--"; *bg = C_CARD2;
}

/* next session from the Jolpica calendar */
static bool prossimaSessione(const F1Dati &d, char *out, size_t n)
{
    const time_t ora = time(nullptr);
    for (uint8_t i = 0; i < d.nSessioni; ++i) {
        if (d.sessioni[i].inizio + 7200 > ora) {
            char g[16], h[8];
            fmtGiorno(d.sessioni[i].inizio, g, sizeof(g));
            fmtOra(d.sessioni[i].inizio, h, sizeof(h));
            snprintf(out, n, "Next: %s - %s %s %s", d.prossimoNome, d.sessioni[i].nome, g, h);
            return true;
        }
    }
    return false;
}

static void aggiornaLive(bool forza)
{
    if (!dataLock(30)) return;
    const bool cambiato = forza || g_f1live->versione != s_liveVer;
    if (cambiato) {
        memcpy(s_L, g_f1live, sizeof(F1Live));
        memcpy(s_D, g_f1, sizeof(F1Dati));
        s_liveVer = g_f1live->versione;
    }
    dataUnlock();

    /* the overtaking arrows expire by themselves: redraw if there is any */
    bool frecce = false;
    for (uint8_t i = 0; i < s_L->n; ++i) {
        if (s_L->p[i].deltaMs != 0 && millis() - s_L->p[i].deltaMs < F1_DELTA_POS_MS + 2000) frecce = true;
    }
    if (!cambiato && !frecce) return;

    const F1Live &L = *s_L;
    const bool gara = strcmp(L.tipo, "Race") == 0;
    char buf[128];

    /* ---- heading ---- */
    if (L.meeting[0]) snprintf(buf, sizeof(buf), "%s  -  %s", L.meeting, L.sessione);
    else              snprintf(buf, sizeof(buf), "Live timing F1");
    uiSetText(s_liveTitolo, buf);

    const bool attiva = strcmp(L.stato, "Started") == 0 || strcmp(L.stato, "Aborted") == 0;
    if (attiva && L.messaggio[0]) uiSetText(s_liveSotto, L.messaggio);
    else if (prossimaSessione(*s_D, buf, sizeof(buf))) uiSetText(s_liveSotto, buf);
    else uiSetText(s_liveSotto, L.messaggio[0] ? L.messaggio : "");

    if (gara && L.giriTot > 0) snprintf(buf, sizeof(buf), "LAP %u/%u", L.giro, L.giriTot);
    else buf[0] = 0;
    uiSetText(s_chipGiro, buf);

    const char *t;
    lv_color_t bg, fg;
    testoPista(L.pista, &t, &bg, &fg);
    uiSetText(s_lblPista, t);
    lv_obj_set_style_bg_color(s_chipPista, bg, 0);
    uiSetColor(s_lblPista, fg);
    testoStato(L.stato, L.connesso, &t, &bg);
    uiSetText(s_lblStato, t);
    lv_obj_set_style_bg_color(s_chipStato, bg, 0);

    for (uint8_t c = 0; c < 2; ++c) {
        uiSetText(s_intest[c][0], L.n == 0 ? "" : (gara ? "GAP" : "BEST LAP"));
        uiSetText(s_intest[c][1], L.n == 0 ? "" : (gara ? "INTERVAL" : "DELTA"));
    }

    /* ---- no data: calendar of the next GP ---- */
    if (L.n == 0) {
        String s = "No data from the live timing.\n\n";
        if (s_D->prossimoNome[0]) {
            s += s_D->prossimoNome;
            s += "  -  ";
            s += s_D->prossimoLuogo;
            s += "\n";
            for (uint8_t i = 0; i < s_D->nSessioni; ++i) {
                char g[16], h[8], r[64];
                fmtGiorno(s_D->sessioni[i].inizio, g, sizeof(g));
                fmtOra(s_D->sessioni[i].inizio, h, sizeof(h));
                snprintf(r, sizeof(r), "\n%s   %s  %s", s_D->sessioni[i].nome, g, h);
                s += r;
            }
        }
        uiSetText(s_liveVuoto, s.c_str());
        lv_obj_remove_flag(s_liveVuoto, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_liveVuoto, LV_OBJ_FLAG_HIDDEN);
    }

    /* absolute best time for the gaps in practice/qualifying */
    uint32_t migliore = 0;
    for (uint8_t i = 0; i < L.n; ++i) {
        if (L.p[i].miglioreMs > 0 && (migliore == 0 || L.p[i].miglioreMs < migliore)) migliore = L.p[i].miglioreMs;
    }

    for (uint8_t k = 0; k < LIVE_RIGHE * 2; ++k) {
        RigaLive &R = s_live[k];
        if (k >= L.n) {
            uiSetText(R.pos, ""); uiSetText(R.tla, ""); uiSetText(R.nome, ""); uiSetText(R.gomma, "");
            uiSetText(R.freccia, ""); uiSetText(R.v1, ""); uiSetText(R.v2, ""); uiSetText(R.tag, "");
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const F1LivePilota &p = L.p[k];
        if (p.pos > 0) snprintf(buf, sizeof(buf), "%u", p.pos);
        else           snprintf(buf, sizeof(buf), "-");
        uiSetText(R.pos, buf);
        lv_obj_remove_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(R.bar, lv_color_hex(p.colore), 0);
        uiSetText(R.tla, p.tla);
        uiSetText(R.nome, p.cognome);
        if (p.gomma != 0) {
            snprintf(buf, sizeof(buf), "%c%u", p.gomma, p.giriGomma);
            uiSetText(R.gomma, buf);
            uiSetColor(R.gomma, uiColoreGomma(p.gomma));
        } else {
            uiSetText(R.gomma, "");
        }

        if (p.deltaMs != 0 && millis() - p.deltaMs < F1_DELTA_POS_MS && p.delta != 0) {
            uiSetText(R.freccia, p.delta > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
            uiSetColor(R.freccia, p.delta > 0 ? C_GREEN : C_RED);
        } else {
            uiSetText(R.freccia, "");
        }

        if (gara) {
            if (p.pos == 1) snprintf(buf, sizeof(buf), "%u laps", p.giri);
            else            snprintf(buf, sizeof(buf), "%s", p.gap);
            uiSetText(R.v1, buf);
            uiSetText(R.v2, p.pos == 1 ? "" : p.intervallo);
        } else {
            uiSetText(R.v1, p.migliore);
            if (p.miglioreMs > 0 && migliore > 0 && p.miglioreMs > migliore) {
                snprintf(buf, sizeof(buf), "+%.3f", (p.miglioreMs - migliore) / 1000.0f);
            } else {
                buf[0] = 0;
            }
            uiSetText(R.v2, buf);
        }

        if (p.ritirato)   { uiSetText(R.tag, "RET");  uiSetColor(R.tag, C_RED); }
        else if (p.fermo) { uiSetText(R.tag, "STOP"); uiSetColor(R.tag, C_RED); }
        else if (p.inPit) { uiSetText(R.tag, "PIT");  uiSetColor(R.tag, C_YELLOW); }
        else if (gara && p.pit > 0) {
            snprintf(buf, sizeof(buf), "%uS", p.pit);
            uiSetText(R.tag, buf);
            uiSetColor(R.tag, C_MUTED);
        } else {
            uiSetText(R.tag, "");
        }
        uiSetColor(R.pos, p.ritirato ? C_MUTED : C_TEXT);
        uiSetColor(R.tla, p.ritirato ? C_MUTED : C_TEXT);
    }
}

/* ===========================================================================
 *  DRIVERS
 * ========================================================================= */
#define CLASS_RIGHE 13

struct RigaPilota {
    lv_obj_t *pos, *bar, *cod, *nome, *team, *punti, *vitt;
};
static RigaPilota s_piloti[CLASS_RIGHE * 2];
static lv_obj_t  *s_pilotiTitolo = nullptr;
static uint32_t   s_pilotiVer = 0xFFFFFFFF;

static void creaPiloti(lv_obj_t *pg)
{
    s_pilotiTitolo = uiCol(pg, UI_PAD + 4, 6, UI_INNER_W, &lv_font_montserrat_20, C_TEXT, LV_TEXT_ALIGN_LEFT);
    for (uint8_t col = 0; col < 2; ++col) {
        const lv_coord_t x0 = UI_PAD + col * 512;
        for (uint8_t r = 0; r < CLASS_RIGHE; ++r) {
            RigaPilota &R = s_piloti[col * CLASS_RIGHE + r];
            const lv_coord_t y = 38 + r * 32;
            R.pos   = uiCol(pg, x0,       y, 32,  &lv_font_montserrat_20, C_TEXT,   LV_TEXT_ALIGN_RIGHT);
            R.bar   = uiBox(pg, x0 + 38,  y + 1, 5, 22, C_MUTED);
            R.cod   = uiCol(pg, x0 + 48,  y, 56,  &lv_font_montserrat_20, C_TEXT,   LV_TEXT_ALIGN_LEFT);
            R.nome  = uiCol(pg, x0 + 106, y + 2, 140, &lv_font_montserrat_16, C_TEXT, LV_TEXT_ALIGN_LEFT);
            R.team  = uiCol(pg, x0 + 248, y + 4, 128, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
            R.punti = uiCol(pg, x0 + 378, y, 70,  &lv_font_montserrat_20, C_ORANGE, LV_TEXT_ALIGN_RIGHT);
            R.vitt  = uiCol(pg, x0 + 452, y + 4, 42,  &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void fmtPunti(float p, char *out, size_t n)
{
    if (p == (float)(int)p) snprintf(out, n, "%d", (int)p);
    else                    snprintf(out, n, "%.1f", p);
}

static void aggiornaPiloti(bool forza)
{
    if (!dataLock(30)) return;
    if (!forza && g_f1->versione == s_pilotiVer) { dataUnlock(); return; }
    memcpy(s_D, g_f1, sizeof(F1Dati));
    s_pilotiVer = g_f1->versione;
    dataUnlock();

    const F1Dati &d = *s_D;
    char buf[96];
    if (d.nPiloti == 0) snprintf(buf, sizeof(buf), "Drivers' standings: waiting for data...");
    else snprintf(buf, sizeof(buf), "Drivers' standings %s  -  after %u races", d.stagione, d.round);
    uiSetText(s_pilotiTitolo, buf);

    const uint8_t perCol = d.nPiloti > 2 * 11 ? CLASS_RIGHE : 11;
    for (uint8_t col = 0; col < 2; ++col) {
        for (uint8_t r = 0; r < CLASS_RIGHE; ++r) {
            RigaPilota &R = s_piloti[col * CLASS_RIGHE + r];
            const uint8_t k = col * perCol + r;
            if (r >= perCol || k >= d.nPiloti) {
                uiSetText(R.pos, ""); uiSetText(R.cod, ""); uiSetText(R.nome, "");
                uiSetText(R.team, ""); uiSetText(R.punti, ""); uiSetText(R.vitt, "");
                lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            const F1PilotaClass &p = d.piloti[k];
            snprintf(buf, sizeof(buf), "%u", p.pos);
            uiSetText(R.pos, buf);
            lv_obj_remove_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(R.bar, lv_color_hex(p.colore), 0);
            uiSetText(R.cod, p.codice);
            uiSetText(R.nome, p.cognome);
            uiSetText(R.team, p.team);
            fmtPunti(p.punti, buf, sizeof(buf));
            uiSetText(R.punti, buf);
            if (p.vittorie > 0) snprintf(buf, sizeof(buf), "%uW", p.vittorie);
            else buf[0] = 0;
            uiSetText(R.vitt, buf);
        }
    }
}

/* ===========================================================================
 *  CONSTRUCTORS
 * ========================================================================= */
struct RigaTeam {
    lv_obj_t *pos, *bar, *nome, *barra, *punti, *vitt;
};
static RigaTeam  s_team[12];
static lv_obj_t *s_teamTitolo = nullptr;
static uint32_t  s_teamVer = 0xFFFFFFFF;

static void creaTeam(lv_obj_t *pg)
{
    s_teamTitolo = uiCol(pg, UI_PAD + 4, 6, UI_INNER_W, &lv_font_montserrat_20, C_TEXT, LV_TEXT_ALIGN_LEFT);
    for (uint8_t r = 0; r < 12; ++r) {
        RigaTeam &R = s_team[r];
        const lv_coord_t y = 40 + r * 34;
        const lv_coord_t x0 = UI_PAD;
        R.pos  = uiCol(pg, x0, y, 34, &lv_font_montserrat_20, C_TEXT, LV_TEXT_ALIGN_RIGHT);
        R.bar  = uiBox(pg, x0 + 42, y + 1, 6, 24, C_MUTED);
        R.nome = uiCol(pg, x0 + 58, y, 250, &lv_font_montserrat_20, C_TEXT, LV_TEXT_ALIGN_LEFT);
        R.barra = lv_bar_create(pg);
        lv_obj_set_pos(R.barra, x0 + 316, y + 6);
        lv_obj_set_size(R.barra, 480, 14);
        lv_bar_set_range(R.barra, 0, 1000);
        lv_obj_set_style_bg_color(R.barra, C_CARD2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(R.barra, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(R.barra, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(R.barra, 4, LV_PART_INDICATOR);
        R.punti = uiCol(pg, x0 + 806, y, 100, &lv_font_montserrat_22, C_ORANGE, LV_TEXT_ALIGN_RIGHT);
        R.vitt  = uiCol(pg, x0 + 912, y + 4, 90, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(R.barra, LV_OBJ_FLAG_HIDDEN);
    }
}

static void aggiornaTeam(bool forza)
{
    if (!dataLock(30)) return;
    if (!forza && g_f1->versione == s_teamVer) { dataUnlock(); return; }
    memcpy(s_D, g_f1, sizeof(F1Dati));
    s_teamVer = g_f1->versione;
    dataUnlock();

    const F1Dati &d = *s_D;
    char buf[96];
    if (d.nTeam == 0) snprintf(buf, sizeof(buf), "Constructors' standings: waiting for data...");
    else snprintf(buf, sizeof(buf), "Constructors' standings %s  -  after %u races", d.stagione, d.round);
    uiSetText(s_teamTitolo, buf);

    const float max = d.nTeam > 0 && d.team[0].punti > 0 ? d.team[0].punti : 1.0f;
    for (uint8_t r = 0; r < 12; ++r) {
        RigaTeam &R = s_team[r];
        if (r >= d.nTeam) {
            uiSetText(R.pos, ""); uiSetText(R.nome, ""); uiSetText(R.punti, ""); uiSetText(R.vitt, "");
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(R.barra, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const F1TeamClass &t = d.team[r];
        snprintf(buf, sizeof(buf), "%u", t.pos);
        uiSetText(R.pos, buf);
        lv_obj_remove_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(R.bar, lv_color_hex(t.colore), 0);
        uiSetText(R.nome, t.nome);
        lv_obj_remove_flag(R.barra, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(R.barra, lv_color_hex(t.colore), LV_PART_INDICATOR);
        lv_bar_set_value(R.barra, (int32_t)(t.punti * 1000.0f / max), LV_ANIM_OFF);
        fmtPunti(t.punti, buf, sizeof(buf));
        uiSetText(R.punti, buf);
        if (t.vittorie > 0) snprintf(buf, sizeof(buf), "%u wins", t.vittorie);
        else buf[0] = 0;
        uiSetText(R.vitt, buf);
    }
}

/* ===========================================================================
 *  LAST RACE
 * ========================================================================= */
struct RigaRis {
    lv_obj_t *pos, *bar, *cod, *nome, *tempo, *griglia, *punti, *gv;
};
static RigaRis   s_ris[CLASS_RIGHE * 2];
static lv_obj_t *s_risTitolo = nullptr;
static uint32_t  s_risVer = 0xFFFFFFFF;

static void creaRis(lv_obj_t *pg)
{
    s_risTitolo = uiCol(pg, UI_PAD + 4, 6, UI_INNER_W, &lv_font_montserrat_20, C_TEXT, LV_TEXT_ALIGN_LEFT);
    for (uint8_t col = 0; col < 2; ++col) {
        const lv_coord_t x0 = UI_PAD + col * 512;
        for (uint8_t r = 0; r < CLASS_RIGHE; ++r) {
            RigaRis &R = s_ris[col * CLASS_RIGHE + r];
            const lv_coord_t y = 38 + r * 32;
            R.pos     = uiCol(pg, x0,       y, 30,  &lv_font_montserrat_20, C_TEXT,   LV_TEXT_ALIGN_RIGHT);
            R.bar     = uiBox(pg, x0 + 36,  y + 1, 5, 22, C_MUTED);
            R.cod     = uiCol(pg, x0 + 46,  y, 54,  &lv_font_montserrat_20, C_TEXT,   LV_TEXT_ALIGN_LEFT);
            R.nome    = uiCol(pg, x0 + 102, y + 3, 116, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
            R.tempo   = uiCol(pg, x0 + 220, y + 2, 122, &lv_font_montserrat_16, C_TEXT, LV_TEXT_ALIGN_RIGHT);
            R.griglia = uiCol(pg, x0 + 346, y + 4, 58,  &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
            R.punti   = uiCol(pg, x0 + 406, y + 2, 46,  &lv_font_montserrat_16, C_ORANGE, LV_TEXT_ALIGN_RIGHT);
            R.gv      = uiCol(pg, x0 + 456, y + 4, 38,  &lv_font_montserrat_14, C_PURPLE, LV_TEXT_ALIGN_CENTER);
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void aggiornaRis(bool forza)
{
    if (!dataLock(30)) return;
    if (!forza && g_f1->versione == s_risVer) { dataUnlock(); return; }
    memcpy(s_D, g_f1, sizeof(F1Dati));
    s_risVer = g_f1->versione;
    dataUnlock();

    const F1Dati &d = *s_D;
    char buf[96];
    if (d.nRis == 0) {
        snprintf(buf, sizeof(buf), "Last race: waiting for data...");
    } else {
        char data[16] = "";
        if (strlen(d.garaData) >= 10) snprintf(data, sizeof(data), "%.2s/%.2s/%.4s", d.garaData + 8, d.garaData + 5, d.garaData);
        snprintf(buf, sizeof(buf), "%s  -  %s", d.garaNome, data);
    }
    uiSetText(s_risTitolo, buf);

    const uint8_t perCol = d.nRis > 2 * 11 ? CLASS_RIGHE : 11;
    for (uint8_t col = 0; col < 2; ++col) {
        for (uint8_t r = 0; r < CLASS_RIGHE; ++r) {
            RigaRis &R = s_ris[col * CLASS_RIGHE + r];
            const uint8_t k = col * perCol + r;
            if (r >= perCol || k >= d.nRis) {
                uiSetText(R.pos, ""); uiSetText(R.cod, ""); uiSetText(R.nome, ""); uiSetText(R.tempo, "");
                uiSetText(R.griglia, ""); uiSetText(R.punti, ""); uiSetText(R.gv, "");
                lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            const F1Risultato &x = d.ris[k];
            snprintf(buf, sizeof(buf), "%u", x.pos);
            uiSetText(R.pos, buf);
            lv_obj_remove_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(R.bar, lv_color_hex(x.colore), 0);
            uiSetText(R.cod, x.codice);
            uiSetText(R.nome, x.cognome);
            uiSetText(R.tempo, x.tempo);
            uiSetColor(R.tempo, (x.tempo[0] == 'R' || x.tempo[0] == 'D') ? C_RED : C_TEXT);
            if (x.griglia > 0) snprintf(buf, sizeof(buf), "from %u", x.griglia);
            else               snprintf(buf, sizeof(buf), "pit");
            uiSetText(R.griglia, buf);
            if (x.punti > 0) { char p[12]; fmtPunti(x.punti, p, sizeof(p)); snprintf(buf, sizeof(buf), "+%s", p); }
            else buf[0] = 0;
            uiSetText(R.punti, buf);
            uiSetText(R.gv, x.giroVeloce ? "FL" : "");
        }
    }
}

/* ===========================================================================
 *  MAP: circuit outline and estimated position of the cars
 *
 *  The GPS coordinates of the cars (Position.z) are given by the live timing only to
 *  F1 TV subscribers. Here the position is an ESTIMATE: from the last crossing of the
 *  line (NumberOfLaps) and the last lap time the fraction of the lap
 *  covered is derived, and mapped onto the MultiViewer outline using its
 *  point-by-point reference times. Cars without a known crossing yet
 *  are placed from their gap to the leader. Pit, retired and stopped: hidden.
 * ========================================================================= */
#define MAPPA_X   UI_PAD
#define MAPPA_Y   UI_PAD
#define MAPPA_W   700
#define MAPPA_H   UI_INNER_H
#define MAPPA_MARGINE 30
#define ELENCO_X  (UI_PAD + MAPPA_W + 8)
#define ELENCO_W  (UI_INNER_W - MAPPA_W - 8)
#define ELENCO_RIGHE 22

struct PuntoAuto {
    char tla[4]; char num[4]; uint32_t colore; uint8_t pos; char gomma; uint8_t giriGomma; char gap[14];
    uint32_t passaggioMs, giroMs; char gapLeader[14]; bool nascosta;
};

static lv_obj_t *s_mappaPagina = nullptr;
static lv_obj_t *s_mappa       = nullptr;
static lv_obj_t *s_linea       = nullptr;
static lv_obj_t *s_traguardo   = nullptr;
static lv_obj_t *s_mappaInfo   = nullptr;
static lv_obj_t *s_dot[24]     = { nullptr };
static lv_obj_t *s_dotLbl[24]  = { nullptr };
static lv_obj_t *s_elTit       = nullptr;
struct RigaElenco { lv_obj_t *pos, *bar, *tla, *gomma, *gap, *tocco; };
static RigaElenco s_el[ELENCO_RIGHE];

static lv_point_precise_t s_pts[TRK_MAX + 1];
static int16_t   *s_tx = nullptr, *s_ty = nullptr;    /* copy of the outline */
static uint16_t  *s_tt = nullptr;
static uint16_t   s_tn = 0, s_tGiro = 1;
static bool       s_tCompleta = false, s_tTempi = false;
static uint32_t   s_trkVer = 0xFFFFFFFF;
static uint32_t   s_mapLiveVer = 0xFFFFFFFF;
static PuntoAuto  s_auto[24];
static uint8_t    s_nAuto = 0;
static bool       s_sessioneAttiva = false, s_gara = false;
static char       s_sessione[80] = "";
static int32_t    s_minX = 0, s_maxX = 0, s_minY = 0, s_maxY = 0;

/* drivers hidden on the map: one bit per race number (0..127), in NVS.
 * Chosen by tapping the driver's row in the list on the right.             */
static uint8_t s_nascosti[16];

static bool pilotaNascosto(const char *num)
{
    const int n = atoi(num);
    return n > 0 && n < 128 && (s_nascosti[n >> 3] & (1u << (n & 7))) != 0;
}

static void salvaNascosti()
{
    Preferences p;
    if (p.begin("sport_p4", false)) { p.putBytes("f1nascosti", s_nascosti, sizeof(s_nascosti)); p.end(); }
}

static void caricaNascosti()
{
    memset(s_nascosti, 0, sizeof(s_nascosti));
    Preferences p;
    if (p.begin("sport_p4", true)) {
        if (p.isKey("f1nascosti")) p.getBytes("f1nascosti", s_nascosti, sizeof(s_nascosti));
        p.end();
    }
}

static void aggiornaMappa(bool forza);

static void rigaPilotaCb(lv_event_t *e)
{
    const uint32_t r = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (r >= s_nAuto) return;
    const int n = atoi(s_auto[r].num);
    if (n <= 0 || n >= 128) return;
    s_nascosti[n >> 3] ^= (uint8_t)(1u << (n & 7));
    salvaNascosti();
    aggiornaMappa(true);
}

static void tuttiCb(lv_event_t *e)
{
    const bool nessuno = (uintptr_t)lv_event_get_user_data(e) != 0;
    if (nessuno) {
        for (uint8_t i = 0; i < s_nAuto; ++i) {
            const int n = atoi(s_auto[i].num);
            if (n > 0 && n < 128) s_nascosti[n >> 3] |= (uint8_t)(1u << (n & 7));
        }
    } else {
        memset(s_nascosti, 0, sizeof(s_nascosti));
    }
    salvaNascosti();
    aggiornaMappa(true);
}

static lv_obj_t *bottoncino(lv_obj_t *parent, lv_coord_t x, lv_coord_t w, const char *txt, bool nessuno)
{
    lv_obj_t *b = uiBox(parent, x, 0, w, 22, C_CARD2);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_t *l = uiLabel(b, txt, &lv_font_montserrat_12, C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, tuttiCb, LV_EVENT_CLICKED, (void *)(uintptr_t)(nessuno ? 1 : 0));
    return b;
}

/* circuit coordinates -> pixels in the map (Y upwards in the data) */
static void proietta(int32_t x, int32_t y, int32_t *px, int32_t *py)
{
    const float w = (float)(s_maxX - s_minX), h = (float)(s_maxY - s_minY);
    const float areaW = MAPPA_W - 2 * MAPPA_MARGINE, areaH = MAPPA_H - 2 * MAPPA_MARGINE;
    float k = 1.0f;
    if (w > 0 && h > 0) k = fminf(areaW / w, areaH / h);
    const float offX = MAPPA_MARGINE + (areaW - w * k) / 2.0f;
    const float offY = MAPPA_MARGINE + (areaH - h * k) / 2.0f;
    *px = (int32_t)(offX + (x - s_minX) * k);
    *py = (int32_t)(offY + (s_maxY - y) * k);
}

/* lap fraction 0..1 -> point on the outline (interpolated) */
static void puntoSulGiro(float f, int32_t *x, int32_t *y)
{
    const float target = f * (float)s_tGiro;
    uint16_t lo = 0, hi = s_tn - 1;
    while (hi - lo > 1) {
        const uint16_t m = (lo + hi) / 2;
        if ((float)s_tt[m] <= target) lo = m; else hi = m;
    }
    const float t0 = s_tt[lo], t1 = s_tt[hi];
    const float a = t1 > t0 ? fminf(fmaxf((target - t0) / (t1 - t0), 0.0f), 1.0f) : 0.0f;
    *x = (int32_t)(s_tx[lo] + (s_tx[hi] - s_tx[lo]) * a);
    *y = (int32_t)(s_ty[lo] + (s_ty[hi] - s_ty[lo]) * a);
}

/* "+12.345" -> 12345 ms; 0 if it is not a gap in seconds (e.g. "1L") */
static uint32_t distaccoMs(const char *g)
{
    if (g == nullptr || g[0] != '+') return 0;
    if (strchr(g, 'L') != nullptr) return 0;
    return (uint32_t)(atof(g + 1) * 1000.0f);
}

static void creaMappa(lv_obj_t *pg)
{
    s_mappaPagina = pg;
    s_tx = static_cast<int16_t *>(heap_caps_calloc(TRK_MAX, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_ty = static_cast<int16_t *>(heap_caps_calloc(TRK_MAX, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_tt = static_cast<uint16_t *>(heap_caps_calloc(TRK_MAX, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    s_mappa = uiCard(pg, MAPPA_X, MAPPA_Y, MAPPA_W, MAPPA_H);
    lv_obj_set_style_pad_all(s_mappa, 0, 0);

    s_linea = lv_line_create(s_mappa);
    lv_obj_set_pos(s_linea, 0, 0);
    lv_obj_set_style_line_width(s_linea, 7, 0);
    lv_obj_set_style_line_color(s_linea, lv_color_hex(0x3A4250), 0);
    lv_obj_set_style_line_rounded(s_linea, true, 0);

    /* finish line: white notch on the first point */
    s_traguardo = uiBox(s_mappa, 0, 0, 12, 12, C_TEXT);
    lv_obj_set_style_radius(s_traguardo, 2, 0);
    lv_obj_add_flag(s_traguardo, LV_OBJ_FLAG_HIDDEN);

    for (uint8_t i = 0; i < 24; ++i) {
        lv_obj_t *d = uiBox(s_mappa, 0, 0, 14, 14, C_MUTED);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(d, C_BG, 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        s_dot[i] = d;
        lv_obj_t *l = uiLabel(s_mappa, "", &lv_font_montserrat_12, C_TEXT);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        s_dotLbl[i] = l;
    }

    s_mappaInfo = uiCol(s_mappa, 12, 8, MAPPA_W - 24, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);

    caricaNascosti();
    lv_obj_t *el = uiCard(pg, ELENCO_X, MAPPA_Y, ELENCO_W, MAPPA_H);
    s_elTit = uiCol(el, 0, 3, 120, &lv_font_montserrat_12, C_MUTED, LV_TEXT_ALIGN_LEFT);
    bottoncino(el, ELENCO_W - 16 - 152, 64, "ALL", false);
    bottoncino(el, ELENCO_W - 16 - 82, 82, "NONE", true);
    for (uint8_t r = 0; r < ELENCO_RIGHE; ++r) {
        RigaElenco &R = s_el[r];
        const lv_coord_t y = 28 + r * 18;
        R.pos   = uiCol(el, 0,   y, 24, &lv_font_montserrat_14, C_TEXT,  LV_TEXT_ALIGN_RIGHT);
        R.bar   = uiBox(el, 30,  y + 2, 4, 14, C_MUTED);
        R.tla   = uiCol(el, 40,  y, 50, &lv_font_montserrat_14, C_TEXT,  LV_TEXT_ALIGN_LEFT);
        R.gomma = uiCol(el, 92,  y, 40, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_LEFT);
        R.gap   = uiCol(el, 132, y, ELENCO_W - 16 - 132, &lv_font_montserrat_14, C_MUTED, LV_TEXT_ALIGN_RIGHT);
        lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        /* transparent area over the row: tap = show/hide on the map */
        R.tocco = lv_obj_create(el);
        lv_obj_remove_style_all(R.tocco);
        lv_obj_set_pos(R.tocco, 0, y - 1);
        lv_obj_set_size(R.tocco, ELENCO_W - 16, 18);
        lv_obj_add_flag(R.tocco, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(R.tocco, rigaPilotaCb, LV_EVENT_CLICKED, (void *)(uintptr_t)r);
    }
}

static void aggiornaMappa(bool forza)
{
    if (s_tx == nullptr || s_ty == nullptr || s_tt == nullptr) return;
    bool nuovaSagoma = false, nuoviDati = false;
    if (!dataLock(30)) return;
    if (forza || g_trk->versione != s_trkVer) {
        s_tn = g_trk->completo ? g_trk->n : 0;
        s_tCompleta = g_trk->completo && s_tn > 2;
        s_tTempi = g_trk->tempiVeri;
        s_tGiro = g_trk->tGiro ? g_trk->tGiro : 1;
        memcpy(s_tx, g_trk->x, s_tn * sizeof(int16_t));
        memcpy(s_ty, g_trk->y, s_tn * sizeof(int16_t));
        memcpy(s_tt, g_trk->t, s_tn * sizeof(uint16_t));
        s_trkVer = g_trk->versione;
        nuovaSagoma = true;
    }
    if (forza || nuovaSagoma || g_f1live->versione != s_mapLiveVer) {
        const F1Live &L = *g_f1live;
        s_gara = strcmp(L.tipo, "Race") == 0;
        s_sessioneAttiva = L.connesso && strcmp(L.stato, "Started") == 0;
        s_nAuto = L.n;
        for (uint8_t i = 0; i < L.n; ++i) {
            PuntoAuto &a = s_auto[i];
            const F1LivePilota &p = L.p[i];
            memcpy(a.tla, p.tla, sizeof(a.tla));
            memcpy(a.num, p.num, sizeof(a.num));
            a.colore = p.colore;
            a.giriGomma = p.giriGomma;
            a.pos = p.pos;
            a.gomma = p.gomma;
            a.passaggioMs = p.passaggioMs;
            a.giroMs = p.giroMs;
            a.nascosta = p.inPit || p.ritirato || p.fermo || pilotaNascosto(p.num);
            snprintf(a.gapLeader, sizeof(a.gapLeader), "%s", p.gap);
            if (s_gara) snprintf(a.gap, sizeof(a.gap), "%s", p.pos == 1 ? "LEADER" : p.intervallo);
            else        snprintf(a.gap, sizeof(a.gap), "%s", p.migliore);
        }
        if (L.meeting[0]) snprintf(s_sessione, sizeof(s_sessione), "%s - %s", L.meeting, L.sessione);
        else s_sessione[0] = 0;
        s_mapLiveVer = L.versione;
        nuoviDati = true;
    }
    dataUnlock();

    /* outline: limits and line, only when it changes */
    if (nuovaSagoma) {
        if (s_tCompleta) {
            s_minX = s_maxX = s_tx[0]; s_minY = s_maxY = s_ty[0];
            for (uint16_t i = 1; i < s_tn; ++i) {
                if (s_tx[i] < s_minX) s_minX = s_tx[i];
                if (s_tx[i] > s_maxX) s_maxX = s_tx[i];
                if (s_ty[i] < s_minY) s_minY = s_ty[i];
                if (s_ty[i] > s_maxY) s_maxY = s_ty[i];
            }
            uint16_t n = 0;
            for (uint16_t i = 0; i < s_tn; ++i) {
                int32_t px, py;
                proietta(s_tx[i], s_ty[i], &px, &py);
                s_pts[n].x = px; s_pts[n].y = py; n++;
            }
            s_pts[n++] = s_pts[0];                         /* closes the lap */
            lv_line_set_points(s_linea, s_pts, n);
            lv_obj_remove_flag(s_linea, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_traguardo, (int32_t)s_pts[0].x - 6, (int32_t)s_pts[0].y - 6);
            lv_obj_remove_flag(s_traguardo, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_linea, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_traguardo, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* estimated positions: recomputed at every timer tick (they move with time) */
    const uint32_t ora = millis();
    const uint32_t rifMs = s_tTempi ? (uint32_t)s_tGiro * 10 : 0;
    /* time elapsed since the leader's crossing: base for cars without a known crossing */
    int64_t eLeader = -1;
    uint32_t giroLeader = 0;
    if (s_nAuto > 0 && s_auto[0].passaggioMs != 0) {
        eLeader = (int64_t)(ora - s_auto[0].passaggioMs);
        giroLeader = s_auto[0].giroMs;
    }
    uint8_t visibili = 0;
    for (uint8_t i = 0; i < 24; ++i) {
        bool mostra = false;
        int32_t px = 0, py = 0;
        if (i < s_nAuto && s_tCompleta && s_sessioneAttiva && !s_auto[i].nascosta) {
            const PuntoAuto &a = s_auto[i];
            /* expected lap duration: the car's last lap, if plausible */
            uint32_t giro = a.giroMs ? a.giroMs : (giroLeader ? giroLeader : rifMs);
            if (rifMs > 0 && (giro < rifMs * 8 / 10 || giro > rifMs * 2)) giro = rifMs;
            int64_t e = -1;
            if (a.passaggioMs != 0) {
                e = (int64_t)(ora - a.passaggioMs);
            } else if (s_gara && eLeader >= 0) {
                const uint32_t d = i == 0 ? 0 : distaccoMs(a.gapLeader);
                if (i == 0 || d > 0) { e = eLeader - (int64_t)d; while (e < 0 && giro > 0) e += giro; }
            }
            if (giro > 0 && e >= 0 && e < (int64_t)giro * 25 / 10) {
                float f = (float)e / (float)giro;
                if (f > 0.995f) f = 0.995f;                 /* wait for the crossing of the line */
                int32_t x, y;
                puntoSulGiro(f, &x, &y);
                proietta(x, y, &px, &py);
                mostra = true;
            }
        }
        if (!mostra) {
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_dotLbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        visibili++;
        lv_obj_set_pos(s_dot[i], px - 7, py - 7);
        lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(s_auto[i].colore), 0);
        lv_obj_remove_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        uiSetText(s_dotLbl[i], s_auto[i].tla);
        lv_obj_set_pos(s_dotLbl[i], px + 9, py - 18);
        lv_obj_remove_flag(s_dotLbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_nAuto > 0) { lv_obj_move_foreground(s_dot[0]); lv_obj_move_foreground(s_dotLbl[0]); }

    char buf[128];
    if (!s_tCompleta)             snprintf(buf, sizeof(buf), "%s", "Circuit outline on its way (MultiViewer)...");
    else if (!s_sessioneAttiva)   snprintf(buf, sizeof(buf), "%s%sNo session in progress", s_sessione, s_sessione[0] ? "   -   " : "");
    else if (visibili == 0)       snprintf(buf, sizeof(buf), "%s   -   positions from the next crossing of the line", s_sessione);
    else                          snprintf(buf, sizeof(buf), "%s   -   estimated positions", s_sessione);
    uiSetText(s_mappaInfo, buf);

    if (!nuoviDati && !forza) return;
    uiSetText(s_elTit, "TAP TO SELECT");
    for (uint8_t r = 0; r < ELENCO_RIGHE; ++r) {
        RigaElenco &R = s_el[r];
        if (r >= s_nAuto) {
            uiSetText(R.pos, ""); uiSetText(R.tla, ""); uiSetText(R.gomma, ""); uiSetText(R.gap, "");
            lv_obj_add_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const PuntoAuto &a = s_auto[r];
        snprintf(buf, sizeof(buf), "%u", a.pos);
        uiSetText(R.pos, a.pos ? buf : "-");
        lv_obj_remove_flag(R.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(R.bar, lv_color_hex(a.colore), 0);
        uiSetText(R.tla, a.tla);
        if (a.gomma) { snprintf(buf, sizeof(buf), "%c", a.gomma); uiSetText(R.gomma, buf); uiSetColor(R.gomma, uiColoreGomma(a.gomma)); }
        else uiSetText(R.gomma, "");
        uiSetText(R.gap, a.gap);
        /* driver excluded from the map: dimmed row */
        const lv_opa_t opa = pilotaNascosto(a.num) ? LV_OPA_30 : LV_OPA_COVER;
        lv_obj_set_style_opa(R.pos, opa, 0);
        lv_obj_set_style_opa(R.bar, opa, 0);
        lv_obj_set_style_opa(R.tla, opa, 0);
        lv_obj_set_style_opa(R.gomma, opa, 0);
        lv_obj_set_style_opa(R.gap, opa, 0);
    }
}

/* the cars move continuously: redraw every 300 ms with the page visible */
static void timerMappa(lv_timer_t *t)
{
    (void)t;
    if (s_mappaPagina != nullptr && !lv_obj_has_flag(s_mappaPagina, LV_OBJ_FLAG_HIDDEN) && boardIsScreenOn()) {
        aggiornaMappa(false);
    }
}

/* ===========================================================================
 *  API
 * ========================================================================= */
void uiF1Create(lv_obj_t *pagine[UI_F1_PAGINE])
{
    s_L = static_cast<F1Live *>(heap_caps_calloc(1, sizeof(F1Live), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_D = static_cast<F1Dati *>(heap_caps_calloc(1, sizeof(F1Dati), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    creaLive(pagine[0]);
    creaMappa(pagine[1]);
    lv_timer_create(timerMappa, 300, nullptr);
    creaPiloti(pagine[2]);
    creaTeam(pagine[3]);
    creaRis(pagine[4]);
}

void uiF1Update(uint8_t pagina, bool forza)
{
    if (s_L == nullptr || s_D == nullptr) return;
    switch (pagina) {
        case 0: aggiornaLive(forza);   break;
        case 1: aggiornaMappa(forza);  break;
        case 2: aggiornaPiloti(forza); break;
        case 3: aggiornaTeam(forza);   break;
        case 4: aggiornaRis(forza);    break;
        default: break;
    }
}

void uiF1Stato(char *out, size_t n)
{
    bool live = false;
    bool connesso = false;
    char sess[80] = "";
    char prossima[128] = "";
    uint8_t giro = 0, tot = 0;
    if (dataLock(20)) {
        live = strcmp(g_f1live->stato, "Started") == 0;
        connesso = g_f1live->connesso;
        snprintf(sess, sizeof(sess), "%s - %s", g_f1live->meeting, g_f1live->sessione);
        giro = g_f1live->giro;
        tot = g_f1live->giriTot;
        prossimaSessione(*g_f1, prossima, sizeof(prossima));
        dataUnlock();
    }
    if (live) {
        if (tot > 0) snprintf(out, n, "LIVE:  %s   lap %u/%u", sess, giro, tot);
        else         snprintf(out, n, "LIVE:  %s", sess);
    } else if (prossima[0]) {
        snprintf(out, n, "%s%s", prossima, connesso ? "   (live timing connected)" : "");
    } else {
        snprintf(out, n, "Formula 1  -  Jolpica data and F1 live timing");
    }
}
