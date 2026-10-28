/* ============================================================================
 *  F1Client - Jolpica (https://api.jolpi.ca/ergast/f1/), no API key
 *
 *  Drivers' standings, constructors' standings, result of the last race and
 *  calendar of the next GP (also used by the live task to know when to
 *  connect). Jolpica limit: few requests per second, so the
 *  four calls are spaced out.
 * ========================================================================== */
#include <ArduinoJson.h>
#include "PsramJson.h"
#include <HTTPClient.h>

#include "AppConfig.h"
#include "Clients.h"
#include "DataModel.h"
#include "esp_heap_caps.h"

static const char BASE[] = "https://api.jolpi.ca/ergast/f1/current/";

static F1Dati *s_nuovo = nullptr;   /* working copy in PSRAM */

static bool scarica(const char *percorso, JsonDocument &doc, JsonDocument *filtro)
{
    String body;
    const int code = httpGet(String(BASE) + percorso, body);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[F1] %s HTTP %d\n", percorso, code);
        return false;
    }
    DeserializationError err = filtro != nullptr
        ? deserializeJson(doc, body, DeserializationOption::Filter(*filtro), DeserializationOption::NestingLimit(40))
        : deserializeJson(doc, body, DeserializationOption::NestingLimit(40));
    if (err) {
        Serial.printf("[F1] %s JSON %s\n", percorso, err.c_str());
        return false;
    }
    return true;
}

static void copia(char *dst, size_t n, const char *src)
{
    asciiFold(src != nullptr ? src : "", dst, n);
}

/* ---------------------------------------------------------------------------
 *  Drivers' standings
 * ------------------------------------------------------------------------- */
static bool leggiPiloti(F1Dati &d)
{
    JsonDocument doc(PSRAM_JSON);
    if (!scarica("driverStandings.json", doc, nullptr)) return false;
    JsonObject lista = doc["MRData"]["StandingsTable"]["StandingsLists"][0];
    if (lista.isNull()) return false;
    copia(d.stagione, sizeof(d.stagione), lista["season"] | "");
    d.round = (uint8_t)atoi(lista["round"] | "0");
    d.nPiloti = 0;
    for (JsonObject s : lista["DriverStandings"].as<JsonArray>()) {
        if (d.nPiloti >= 26) break;
        F1PilotaClass &p = d.piloti[d.nPiloti++];
        p.pos      = (uint8_t)atoi(s["position"] | "0");
        p.punti    = atof(s["points"] | "0");
        p.vittorie = (uint8_t)atoi(s["wins"] | "0");
        copia(p.codice,  sizeof(p.codice),  s["Driver"]["code"] | "---");
        copia(p.cognome, sizeof(p.cognome), s["Driver"]["familyName"] | "");
        copia(p.team,    sizeof(p.team),    s["Constructors"][0]["name"] | "");
        p.colore = coloreTeam(s["Constructors"][0]["constructorId"] | "");
        if (p.pos == 0) p.pos = d.nPiloti;
    }
    return d.nPiloti > 0;
}

/* ---------------------------------------------------------------------------
 *  Constructors' standings
 * ------------------------------------------------------------------------- */
static bool leggiTeam(F1Dati &d)
{
    JsonDocument doc(PSRAM_JSON);
    if (!scarica("constructorStandings.json", doc, nullptr)) return false;
    JsonObject lista = doc["MRData"]["StandingsTable"]["StandingsLists"][0];
    if (lista.isNull()) return false;
    d.nTeam = 0;
    for (JsonObject s : lista["ConstructorStandings"].as<JsonArray>()) {
        if (d.nTeam >= 12) break;
        F1TeamClass &t = d.team[d.nTeam++];
        t.pos      = (uint8_t)atoi(s["position"] | "0");
        t.punti    = atof(s["points"] | "0");
        t.vittorie = (uint8_t)atoi(s["wins"] | "0");
        copia(t.nome, sizeof(t.nome), s["Constructor"]["name"] | "");
        t.colore = coloreTeam(s["Constructor"]["constructorId"] | "");
        if (t.pos == 0) t.pos = d.nTeam;
    }
    return d.nTeam > 0;
}

/* ---------------------------------------------------------------------------
 *  Last race
 * ------------------------------------------------------------------------- */
static void traduciStato(const char *st, char *out, size_t n)
{
    if (st == nullptr || *st == 0)              { snprintf(out, n, "--"); return; }
    if (strncmp(st, "+", 1) == 0) {
        /* "+1 Lap", "+2 Laps" */
        const int giri = atoi(st + 1);
        snprintf(out, n, "+%d %s", giri, giri == 1 ? "lap" : "laps");
        return;
    }
    if (strstr(st, "Lapped"))                   { snprintf(out, n, "lapped"); return; }
    if (strstr(st, "Disqualified"))             { snprintf(out, n, "Disqualified"); return; }
    if (strstr(st, "Did not start") || strstr(st, "Withdrew")) { snprintf(out, n, "Did not start"); return; }
    if (strstr(st, "Finished"))                 { snprintf(out, n, "Finished"); return; }
    snprintf(out, n, "Ret. %s", st);
}

static bool leggiUltimaGara(F1Dati &d)
{
    JsonDocument doc(PSRAM_JSON);
    if (!scarica("last/results.json", doc, nullptr)) return false;
    JsonObject gara = doc["MRData"]["RaceTable"]["Races"][0];
    if (gara.isNull()) return false;

    char nome[64];
    copia(nome, sizeof(nome), gara["raceName"] | "");
    /* "Azerbaijan Grand Prix" -> "Azerbaijan GP" */
    char *gp = strstr(nome, "Grand Prix");
    if (gp != nullptr) { gp[0] = 'G'; gp[1] = 'P'; memmove(gp + 2, gp + 10, strlen(gp + 10) + 1); }
    snprintf(d.garaNome, sizeof(d.garaNome), "%s", nome);
    copia(d.garaData, sizeof(d.garaData), gara["date"] | "");

    d.nRis = 0;
    for (JsonObject r : gara["Results"].as<JsonArray>()) {
        if (d.nRis >= 26) break;
        F1Risultato &x = d.ris[d.nRis++];
        x.pos     = (uint8_t)atoi(r["position"] | "0");
        x.griglia = (uint8_t)atoi(r["grid"] | "0");
        x.giri    = (uint8_t)atoi(r["laps"] | "0");
        x.punti   = atof(r["points"] | "0");
        copia(x.codice,  sizeof(x.codice),  r["Driver"]["code"] | "---");
        copia(x.cognome, sizeof(x.cognome), r["Driver"]["familyName"] | "");
        copia(x.team,    sizeof(x.team),    r["Constructor"]["name"] | "");
        x.colore = coloreTeam(r["Constructor"]["constructorId"] | "");
        x.giroVeloce = strcmp(r["FastestLap"]["rank"] | "", "1") == 0;
        const char *tempo = r["Time"]["time"] | "";
        if (tempo[0] != 0) copia(x.tempo, sizeof(x.tempo), tempo);
        else               traduciStato(r["status"] | "", x.tempo, sizeof(x.tempo));
    }
    return d.nRis > 0;
}

/* ---------------------------------------------------------------------------
 *  Next GP and session times
 * ------------------------------------------------------------------------- */
static void aggiungiSessione(F1Dati &d, const char *nome, JsonObject s)
{
    if (s.isNull() || d.nSessioni >= 6) return;
    char iso[32];
    snprintf(iso, sizeof(iso), "%sT%s", s["date"] | "", s["time"] | "00:00:00Z");
    const time_t t = parseIsoUtc(iso);
    if (t == 0) return;
    F1Sessione &x = d.sessioni[d.nSessioni++];
    snprintf(x.nome, sizeof(x.nome), "%s", nome);
    x.inizio = t;
}

static bool leggiProssimo(F1Dati &d)
{
    JsonDocument doc(PSRAM_JSON);
    if (!scarica("next.json", doc, nullptr)) return false;
    d.nSessioni = 0;
    d.prossimoNome[0] = 0;
    d.prossimoLuogo[0] = 0;
    JsonObject g = doc["MRData"]["RaceTable"]["Races"][0];
    if (g.isNull()) return true;   /* season over: no GP scheduled */

    char nome[64];
    copia(nome, sizeof(nome), g["raceName"] | "");
    char *gp = strstr(nome, "Grand Prix");
    if (gp != nullptr) { gp[0] = 'G'; gp[1] = 'P'; memmove(gp + 2, gp + 10, strlen(gp + 10) + 1); }
    snprintf(d.prossimoNome, sizeof(d.prossimoNome), "%s", nome);

    char luogo[64];
    char loc[40], paese[40];
    copia(loc,   sizeof(loc),   g["Circuit"]["Location"]["locality"] | "");
    copia(paese, sizeof(paese), g["Circuit"]["Location"]["country"] | "");
    snprintf(luogo, sizeof(luogo), "%s, %s", loc, paese);
    snprintf(d.prossimoLuogo, sizeof(d.prossimoLuogo), "%s", luogo);

    aggiungiSessione(d, "Practice 1",  g["FirstPractice"]);
    aggiungiSessione(d, "Practice 2",  g["SecondPractice"]);
    aggiungiSessione(d, "Practice 3",  g["ThirdPractice"]);
    aggiungiSessione(d, "Sprint Q.",   g["SprintQualifying"].isNull() ? g["SprintShootout"] : g["SprintQualifying"]);
    aggiungiSessione(d, "Sprint",      g["Sprint"]);
    aggiungiSessione(d, "Qualifying",  g["Qualifying"]);
    JsonObject gara = g;
    if (d.nSessioni < 6) {
        char iso[32];
        snprintf(iso, sizeof(iso), "%sT%s", gara["date"] | "", gara["time"] | "00:00:00Z");
        const time_t t = parseIsoUtc(iso);
        if (t != 0) {
            F1Sessione &x = d.sessioni[d.nSessioni++];
            snprintf(x.nome, sizeof(x.nome), "RACE");
            x.inizio = t;
        }
    }
    /* in chronological order (the sprint weekend mixes up the order) */
    for (uint8_t i = 1; i < d.nSessioni; ++i) {
        F1Sessione x = d.sessioni[i];
        int j = i - 1;
        while (j >= 0 && d.sessioni[j].inizio > x.inizio) { d.sessioni[j + 1] = d.sessioni[j]; --j; }
        d.sessioni[j + 1] = x;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 *  Full cycle
 * ------------------------------------------------------------------------- */
static bool pollF1Once()
{
    if (dataLock(500)) { memcpy(s_nuovo, g_f1, sizeof(F1Dati)); dataUnlock(); }
    else return false;

    bool ok = true;
    ok &= leggiPiloti(*s_nuovo);
    vTaskDelay(pdMS_TO_TICKS(400));
    ok &= leggiTeam(*s_nuovo);
    vTaskDelay(pdMS_TO_TICKS(400));
    ok &= leggiUltimaGara(*s_nuovo);
    vTaskDelay(pdMS_TO_TICKS(400));
    ok &= leggiProssimo(*s_nuovo);

    if (dataLock(1000)) {
        s_nuovo->ok = ok || g_f1->ok;
        s_nuovo->versione = g_f1->versione + 1;
        memcpy(g_f1, s_nuovo, sizeof(F1Dati));
        dataUnlock();
    }
    Serial.printf("[F1] Jolpica %s: %u drivers, %u teams, last race %s, next %s\n",
                  ok ? "ok" : "partial", s_nuovo->nPiloti, s_nuovo->nTeam,
                  s_nuovo->garaNome, s_nuovo->prossimoNome);
    return ok;
}

/* true if a race or sprint finished less than 4 hours ago: the official results
 * reach Jolpica a few tens of minutes late */
static bool garaAppenaFinita()
{
    bool si = false;
    const time_t ora = time(nullptr);
    if (dataLock(200)) {
        for (uint8_t i = 0; i < g_f1->nSessioni; ++i) {
            const F1Sessione &s = g_f1->sessioni[i];
            if ((strcmp(s.nome, "RACE") == 0 || strcmp(s.nome, "Sprint") == 0) &&
                ora > s.inizio + 3600 && ora < s.inizio + 6 * 3600) si = true;
        }
        dataUnlock();
    }
    return si;
}

void f1Task(void *arg)
{
    (void)arg;
    s_nuovo = static_cast<F1Dati *>(heap_caps_calloc(1, sizeof(F1Dati), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_nuovo == nullptr) s_nuovo = static_cast<F1Dati *>(calloc(1, sizeof(F1Dati)));

    uint32_t ultimo = 0;
    bool ultimoOk = false;
    while (true) {
        if (!reteOk()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        uint32_t periodo = F1_JOLPICA_MS;
        if (!ultimoOk) periodo = 120000;                       /* error: retry in 2 min */
        else if (garaAppenaFinita()) periodo = F1_JOLPICA_POSTGARA_MS;
        if (ultimo == 0 || millis() - ultimo >= periodo) {
            ultimoOk = pollF1Once();
            ultimo = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
