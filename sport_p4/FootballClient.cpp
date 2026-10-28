/* ============================================================================
 *  FootballClient - ESPN (site.api.espn.com), no API key
 *
 *  For each competition:
 *    list    one request per day in the -2..+5 day window
 *            (the endpoint does not accept date ranges, only single days)
 *    live    today only, every 60 s, every 20 s if a match is
 *            in progress or about to start: compares the scores and reports goals
 *  Serie A standings every 30 minutes.
 *
 *  As in the other projects, HTTPS goes through the plain HTTPClient: without
 *  a certificate the core calls setInsecure() by itself.
 * ========================================================================== */
#include <ArduinoJson.h>
#include "PsramJson.h"
#include <HTTPClient.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "Clients.h"
#include "DataModel.h"
#include "Notify.h"
#include "Settings.h"
#include "esp_heap_caps.h"

static const char BASE_SB[]  = "https://site.api.espn.com/apis/site/v2/sports/soccer/";
static const char BASE_STD[] = "https://site.api.espn.com/apis/v2/sports/soccer/";

static Partita  *s_tmp = nullptr;                   /* CALCIO_MAX_PARTITE, PSRAM */
static uint32_t  s_tentativoLista[CALCIO_COMP_COUNT] = { 0 };
static uint32_t  s_ultimoLive[CALCIO_COMP_COUNT]     = { 0 };

/* ---------------------------------------------------------------------------
 *  HTTP (also shared with the F1 client)
 * ------------------------------------------------------------------------- */
/* The body is read by hand instead of using getString(): getString() waits for
 * data in a loop without ever yielding the CPU and, with a slow reply, it
 * triggers the core 0 watchdog (verified with addr2line on 04/10/2026). */
static bool leggiRigaHttp(NetworkClient &s, char *buf, size_t n)
{
    size_t i = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < WEB_HTTP_TIMEOUT_MS) {
        if (s.available() > 0) {
            const int c = s.read();
            if (c < 0) continue;
            t0 = millis();
            if (c == '\n') {
                if (i > 0 && buf[i - 1] == '\r') --i;
                buf[i] = 0;
                return true;
            }
            if (i + 1 < n) buf[i++] = (char)c;
        } else if (!s.connected()) {
            buf[i] = 0;
            return i > 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return false;
}

static bool leggiBlocco(NetworkClient &s, String &out, long quanti)
{
    uint8_t buf[1024];
    uint32_t t0 = millis();
    long letti = 0;
    while (quanti < 0 || letti < quanti) {
        const int disp = s.available();
        if (disp > 0) {
            size_t vuoi = sizeof(buf);
            if (quanti >= 0 && (long)vuoi > quanti - letti) vuoi = (size_t)(quanti - letti);
            const int r = s.read(buf, vuoi);
            if (r > 0) {
                out.concat(reinterpret_cast<const char *>(buf), (unsigned int)r);
                letti += r;
                t0 = millis();
                continue;
            }
        } else if (!s.connected()) {
            return quanti < 0;           /* no length: end = connection closed */
        }
        if (millis() - t0 > WEB_HTTP_TIMEOUT_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

/* Only one HTTPS connection at a time: the WiFi goes through the C6 via SDIO
 * (ESP-Hosted) and every TLS handshake takes a lot of internal RAM. */
static SemaphoreHandle_t s_reteMutex = nullptr;

bool reteLock(uint32_t timeoutMs)
{
    if (s_reteMutex == nullptr) s_reteMutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_reteMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void reteUnlock()
{
    if (s_reteMutex != nullptr) xSemaphoreGive(s_reteMutex);
}

static int httpGetInterno(const String &url, String &out);

int httpGet(const String &url, String &out)
{
    if (!reteLock(60000)) return -20;
    const int code = httpGetInterno(url, out);
    reteUnlock();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[HTTP] %d  %s  (free internal RAM %u)\n", code, url.c_str(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    return code;
}

static int httpGetInterno(const String &url, String &out)
{
    out = "";
    HTTPClient http;
    http.setTimeout(WEB_HTTP_TIMEOUT_MS);
    http.setConnectTimeout(WEB_HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.setUserAgent("sport_p4/1.0 (ESP32-P4)");
    const char *chiavi[] = { "Transfer-Encoding" };
    http.collectHeaders(chiavi, 1);
    if (!http.begin(url)) return -1;
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return code;
    }

    NetworkClient *s = http.getStreamPtr();
    if (s == nullptr) { http.end(); return -2; }
    const bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    const int size = http.getSize();
    bool ok = true;
    if (chunked) {
        out.reserve(32768);
        char riga[32];
        while (true) {
            if (!leggiRigaHttp(*s, riga, sizeof(riga))) { ok = false; break; }
            const long n = strtol(riga, nullptr, 16);
            if (n <= 0) break;
            if (!leggiBlocco(*s, out, n)) { ok = false; break; }
            leggiRigaHttp(*s, riga, sizeof(riga));     /* CRLF after the block */
        }
    } else {
        if (size > 0) out.reserve(size + 1);
        ok = leggiBlocco(*s, out, size);
    }
    http.end();
    if (!ok) {
        out = "";
        return -12;
    }
    return code;
}

bool reteOk()
{
    return WiFi.status() == WL_CONNECTED && orologioValido();
}

/* ---------------------------------------------------------------------------
 *  Parsing an ESPN scoreboard
 * ------------------------------------------------------------------------- */
static void filtroScoreboard(JsonDocument &f)
{
    f["events"][0]["id"]   = true;
    f["events"][0]["date"] = true;
    f["events"][0]["status"]["displayClock"]         = true;
    f["events"][0]["status"]["type"]["state"]        = true;
    f["events"][0]["status"]["type"]["name"]         = true;
    f["events"][0]["status"]["type"]["shortDetail"]  = true;
    JsonObject comp = f["events"][0]["competitions"][0].to<JsonObject>();
    comp["competitors"][0]["homeAway"]                  = true;
    comp["competitors"][0]["score"]                     = true;
    comp["competitors"][0]["shootoutScore"]             = true;
    comp["competitors"][0]["team"]["shortDisplayName"]  = true;
    comp["notes"][0]["headline"]                        = true;
    comp["competitors"][0]["id"]                        = true;
    /* match events: goals and cards with minute and player */
    comp["details"][0]["redCard"]                       = true;
    comp["details"][0]["scoringPlay"]                   = true;
    comp["details"][0]["ownGoal"]                       = true;
    comp["details"][0]["penaltyKick"]                   = true;
    comp["details"][0]["shootout"]                      = true;
    comp["details"][0]["clock"]["displayValue"]         = true;
    comp["details"][0]["team"]["id"]                    = true;
    comp["details"][0]["athletesInvolved"][0]["shortName"] = true;
}

static int8_t leggiGol(JsonVariant v)
{
    if (v.isNull()) return -1;
    if (v.is<const char *>()) {
        const char *s = v.as<const char *>();
        if (s == nullptr || *s == 0) return -1;
        return (int8_t)atoi(s);
    }
    return (int8_t)v.as<int>();
}

static bool leggiEvento(JsonObject e, Partita &m)
{
    memset(&m, 0, sizeof(m));
    m.golCasa = m.golOspite = m.rigCasa = m.rigOspite = -1;
    snprintf(m.id, sizeof(m.id), "%s", e["id"] | "");
    if (m.id[0] == 0) return false;
    m.inizio = parseIsoUtc(e["date"] | "");

    const char *state = e["status"]["type"]["state"] | "pre";
    const char *name  = e["status"]["type"]["name"] | "";
    const char *sd    = e["status"]["type"]["shortDetail"] | "";

    if (strcmp(state, "in") == 0)        m.stato = 'i';
    else if (strcmp(state, "post") == 0) m.stato = 'f';
    else                                 m.stato = 'p';

    if (strstr(name, "POSTPONED")) { m.stato = 'x'; snprintf(m.dettaglio, sizeof(m.dettaglio), "PPD"); }
    else if (strstr(name, "CANCELED") || strstr(name, "CANCELLED")) { m.stato = 'x'; snprintf(m.dettaglio, sizeof(m.dettaglio), "CANC"); }
    else if (strstr(name, "ABANDONED") || strstr(name, "SUSPENDED")) { m.stato = 'x'; snprintf(m.dettaglio, sizeof(m.dettaglio), "SUSP"); }

    if (m.stato == 'i') {
        if (strstr(name, "HALFTIME"))      snprintf(m.minuto, sizeof(m.minuto), "HT");
        else if (strstr(name, "SHOOTOUT")) snprintf(m.minuto, sizeof(m.minuto), "PENS");
        else asciiFold(e["status"]["displayClock"] | "", m.minuto, sizeof(m.minuto));
    } else if (m.stato == 'f') {
        if (strstr(sd, "Pen"))      snprintf(m.dettaglio, sizeof(m.dettaglio), "PENS");
        else if (strstr(sd, "AET")) snprintf(m.dettaglio, sizeof(m.dettaglio), "AET");
        else                        snprintf(m.dettaglio, sizeof(m.dettaglio), "FT");
    }

    JsonObject comp = e["competitions"][0];
    char idCasa[12] = "";
    for (JsonObject t : comp["competitors"].as<JsonArray>()) {
        const bool casa = strcmp(t["homeAway"] | "home", "home") == 0;
        if (casa) snprintf(idCasa, sizeof(idCasa), "%s", t["id"] | "");
        char nome[40];
        asciiFold(t["team"]["shortDisplayName"] | "?", nome, sizeof(nome));
        const char *it = nomeItaliano(nome);
        const int8_t gol = (m.stato == 'p' || m.stato == 'x') ? -1 : leggiGol(t["score"]);
        const int8_t rig = leggiGol(t["shootoutScore"]);
        if (casa) {
            snprintf(m.casa, sizeof(m.casa), "%s", it);
            m.golCasa = gol;
            m.rigCasa = rig;
        } else {
            snprintf(m.ospite, sizeof(m.ospite), "%s", it);
            m.golOspite = gol;
            m.rigOspite = rig;
        }
    }
    asciiFold(comp["notes"][0]["headline"] | "", m.nota, sizeof(m.nota));

    /* red cards and last scorer */
    for (JsonObject d : comp["details"].as<JsonArray>()) {
        if (d["shootout"] | false) continue;
        const bool casa = strcmp(d["team"]["id"] | "", idCasa) == 0;
        char chi[30];
        asciiFold(d["athletesInvolved"][0]["shortName"] | "", chi, sizeof(chi));
        const char *min = d["clock"]["displayValue"] | "";
        if (d["redCard"] | false) {
            if (casa) m.rossiCasa++; else m.rossiOspite++;
            snprintf(m.ultimoRosso, sizeof(m.ultimoRosso), "%s %s", chi[0] ? chi : (casa ? m.casa : m.ospite), min);
        }
        if (d["scoringPlay"] | false) {
            const char *nota = (d["ownGoal"] | false) ? " (o.g.)" : ((d["penaltyKick"] | false) ? " (pen.)" : "");
            snprintf(m.marcatore, sizeof(m.marcatore), "%s %s%s", chi[0] ? chi : "", min, nota);
        }
    }
    return true;
}

/* adds the events of the body to dst, skipping duplicates; returns false if the JSON is broken */
static bool parseScoreboard(const String &body, Partita *dst, uint16_t &n)
{
    JsonDocument filtro(PSRAM_JSON);
    filtroScoreboard(filtro);
    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, body, DeserializationOption::Filter(filtro), DeserializationOption::NestingLimit(40))) return false;
    for (JsonObject e : doc["events"].as<JsonArray>()) {
        if (n >= CALCIO_MAX_PARTITE) break;
        Partita m;
        if (!leggiEvento(e, m)) continue;
        bool doppio = false;
        for (uint16_t k = 0; k < n; ++k) {
            if (strcmp(dst[k].id, m.id) == 0) { doppio = true; break; }
        }
        if (!doppio) dst[n++] = m;
    }
    return true;
}

static void ordinaPerOra(Partita *v, uint16_t n)
{
    for (uint16_t i = 1; i < n; ++i) {
        Partita x = v[i];
        int j = (int)i - 1;
        while (j >= 0 && v[j].inizio > x.inizio) { v[j + 1] = v[j]; --j; }
        v[j + 1] = x;
    }
}

static void giornoEspn(time_t t, char *out, size_t n)
{
    snprintf(out, n, "%lu", (unsigned long)chiaveGiorno(t));
}

/* ---------------------------------------------------------------------------
 *  Old/new comparison: goals, kick-off, full time
 *  Called with dataLock already taken.
 * ------------------------------------------------------------------------- */
static bool confronta(const Partita &vecchia, Partita &nuova, uint8_t c, bool notificare)
{
    nuova.golMs = vecchia.golMs;
    if (nuova.nota[0] == 0 && vecchia.nota[0] != 0) memcpy(nuova.nota, vecchia.nota, sizeof(nuova.nota));

    const bool cambiata = vecchia.golCasa != nuova.golCasa || vecchia.golOspite != nuova.golOspite ||
                          vecchia.rossiCasa != nuova.rossiCasa || vecchia.rossiOspite != nuova.rossiOspite ||
                          vecchia.stato != nuova.stato || strcmp(vecchia.minuto, nuova.minuto) != 0;
    if (!notificare) return cambiata;

    const bool noto = vecchia.golCasa >= 0 && vecchia.golOspite >= 0 &&
                      nuova.golCasa >= 0 && nuova.golOspite >= 0;
    if (vecchia.stato == 'i' && noto &&
        (vecchia.golCasa != nuova.golCasa || vecchia.golOspite != nuova.golOspite)) {
        nuova.golMs = millis() | 1;
        const int prima = vecchia.golCasa + vecchia.golOspite;
        const int dopo  = nuova.golCasa + nuova.golOspite;
        if (dopo > prima) {
            const char *chi = nuova.golCasa > vecchia.golCasa ? nuova.casa : nuova.ospite;
            if (nuova.marcatore[0]) {
                notifySchermata(1, c), notifica(NOT_GOL, SND_GOL, "GOAL %s!  %s   %s %d-%d %s   %s",
                         chi, nuova.marcatore, nuova.casa, nuova.golCasa, nuova.golOspite, nuova.ospite,
                         CALCIO_TAB[c]);
            } else {
                notifySchermata(1, c), notifica(NOT_GOL, SND_GOL, "GOAL %s!   %s %d-%d %s   %s   %s",
                         chi, nuova.casa, nuova.golCasa, nuova.golOspite, nuova.ospite,
                         nuova.minuto, CALCIO_TAB[c]);
            }
        } else {
            notifySchermata(1, c), notifica(NOT_INFO, SND_MINER_DOWN, "Goal disallowed:  %s %d-%d %s   %s",
                     nuova.casa, nuova.golCasa, nuova.golOspite, nuova.ospite, CALCIO_TAB[c]);
        }
    }
    /* red card (only with the match in progress, not red cards after the final whistle) */
    if (vecchia.stato == 'i' && nuova.stato == 'i' && imp().avvisiRossi &&
        nuova.rossiCasa + nuova.rossiOspite > vecchia.rossiCasa + vecchia.rossiOspite) {
        const char *squadra = nuova.rossiCasa > vecchia.rossiCasa ? nuova.casa : nuova.ospite;
        notifySchermata(1, c), notifica(NOT_F1, SND_ESPULSIONE, "SENT OFF  %s (%s)   %s %d-%d %s   %s",
                 nuova.ultimoRosso, squadra, nuova.casa, nuova.golCasa, nuova.golOspite, nuova.ospite,
                 CALCIO_TAB[c]);
    }
    if (vecchia.stato == 'p' && nuova.stato == 'i') {
        notifySchermata(1, c), notifica(NOT_INFO, SND_FISCHIO_INIZIO, "Kick-off:  %s - %s   %s",
                 nuova.casa, nuova.ospite, CALCIO_TAB[c]);
    }
    /* half time and restart: ESPN puts "HALFTIME" in the status, here it becomes "HT" */
    if (vecchia.stato == 'i' && nuova.stato == 'i') {
        const bool eraInt = strcmp(vecchia.minuto, "HT") == 0;
        const bool eInt   = strcmp(nuova.minuto, "HT") == 0;
        if (!eraInt && eInt && noto) {
            notifySchermata(1, c), notifica(NOT_INFO, SND_FISCHIO_INTERVALLO, "Half time:  %s %d-%d %s   %s",
                     nuova.casa, nuova.golCasa, nuova.golOspite, nuova.ospite, CALCIO_TAB[c]);
        } else if (eraInt && !eInt) {
            notifySchermata(1, c), notifica(NOT_INFO, SND_FISCHIO_INIZIO, "Second half under way:  %s - %s   %s",
                     nuova.casa, nuova.ospite, CALCIO_TAB[c]);
        }
    }
    if (vecchia.stato == 'i' && nuova.stato == 'f' && noto) {
        notifySchermata(1, c), notifica(NOT_OK, SND_FISCHIO_FINE, "Full time:  %s %d-%d %s   %s", nuova.casa, nuova.golCasa,
                 nuova.golOspite, nuova.ospite, CALCIO_TAB[c]);
    }
    return cambiata;
}

static uint8_t contaLive(const Competizione &cp)
{
    uint8_t n = 0;
    for (uint16_t i = 0; i < cp.n; ++i) if (cp.p[i].stato == 'i') ++n;
    return n;
}

/* ---------------------------------------------------------------------------
 *  Full list of a competition
 * ------------------------------------------------------------------------- */
static bool pollLista(uint8_t c)
{
    s_tentativoLista[c] = millis();
    uint16_t n = 0;
    bool risposta = false;
    int ultimoCodice = 0;
    const time_t ora = time(nullptr);
    String body;

    for (int d = -CALCIO_GIORNI_PRIMA; d <= CALCIO_GIORNI_DOPO; ++d) {
        char giorno[12];
        giornoEspn(ora + (time_t)d * 86400, giorno, sizeof(giorno));
        String url = String(BASE_SB) + CALCIO_SLUG[c] + "/scoreboard?dates=" + giorno;
        ultimoCodice = httpGet(url, body);
        if (ultimoCodice == HTTP_CODE_OK && parseScoreboard(body, s_tmp, n)) risposta = true;
        body = "";
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    bool ripiego = false;
    if (risposta && n == 0) {
        /* empty window: "current" matchday according to ESPN (often the last one played) */
        String url = String(BASE_SB) + CALCIO_SLUG[c] + "/scoreboard";
        if (httpGet(url, body) == HTTP_CODE_OK) parseScoreboard(body, s_tmp, n);
        body = "";
        ripiego = true;
    }

    if (!risposta) {
        if (dataLock(500)) {
            snprintf(g_comp[c].errore, sizeof(g_comp[c].errore), "HTTP %d", ultimoCodice);
            g_comp[c].versione++;
            dataUnlock();
        }
        Serial.printf("[FOOTBALL] %s list FAILED (%d)\n", CALCIO_SLUG[c], ultimoCodice);
        return false;
    }

    ordinaPerOra(s_tmp, n);

    if (dataLock(1000)) {
        Competizione &cp = g_comp[c];
        for (uint16_t i = 0; i < n; ++i) {
            for (uint16_t k = 0; k < cp.n; ++k) {
                if (strcmp(cp.p[k].id, s_tmp[i].id) == 0) {
                    confronta(cp.p[k], s_tmp[i], c, cp.caricata);
                    break;
                }
            }
        }
        memcpy(cp.p, s_tmp, sizeof(Partita) * n);
        cp.n        = n;
        cp.caricata = true;
        cp.ripiego  = ripiego;
        cp.aggMs    = millis();
        cp.errore[0] = 0;
        cp.live     = contaLive(cp);
        cp.versione++;
        dataUnlock();
    }
    Serial.printf("[FOOTBALL] %s: %u matches%s\n", CALCIO_SLUG[c], n, ripiego ? " (fallback)" : "");
    return true;
}

/* ---------------------------------------------------------------------------
 *  Live update (today only)
 * ------------------------------------------------------------------------- */
static bool serveLiveVeloce(uint8_t c)
{
    bool veloce = false;
    const time_t ora = time(nullptr);
    if (dataLock(200)) {
        const Competizione &cp = g_comp[c];
        if (cp.live > 0) veloce = true;
        for (uint16_t i = 0; i < cp.n && !veloce; ++i) {
            /* match starting in the next 5 minutes or "to be played" for less than 3 hours */
            if (cp.p[i].stato == 'p' && cp.p[i].inizio > 0 &&
                cp.p[i].inizio <= ora + 300 && cp.p[i].inizio >= ora - 3 * 3600) veloce = true;
        }
        dataUnlock();
    }
    return veloce;
}

static void pollLive(uint8_t c)
{
    s_ultimoLive[c] = millis();
    const time_t ora = time(nullptr);
    struct tm lt;
    localtime_r(&ora, &lt);

    uint16_t n = 0;
    String body;
    char giorno[12];

    giornoEspn(ora, giorno, sizeof(giorno));
    String url = String(BASE_SB) + CALCIO_SLUG[c] + "/scoreboard?dates=" + giorno;
    if (httpGet(url, body) == HTTP_CODE_OK) parseScoreboard(body, s_tmp, n);

    /* at night (World Cup in America) the match may be on the previous day */
    if (lt.tm_hour < 8) {
        giornoEspn(ora - 86400, giorno, sizeof(giorno));
        url = String(BASE_SB) + CALCIO_SLUG[c] + "/scoreboard?dates=" + giorno;
        if (httpGet(url, body) == HTTP_CODE_OK) parseScoreboard(body, s_tmp, n);
    }
    body = "";
    if (n == 0) return;

    if (!dataLock(1000)) return;
    Competizione &cp = g_comp[c];
    bool cambiato = false;
    bool inserito = false;
    for (uint16_t i = 0; i < n; ++i) {
        bool trovato = false;
        for (uint16_t k = 0; k < cp.n; ++k) {
            if (strcmp(cp.p[k].id, s_tmp[i].id) == 0) {
                if (confronta(cp.p[k], s_tmp[i], c, cp.caricata)) cambiato = true;
                cp.p[k] = s_tmp[i];
                trovato = true;
                break;
            }
        }
        if (!trovato && cp.n < CALCIO_MAX_PARTITE) {
            cp.p[cp.n++] = s_tmp[i];
            inserito = true;
        }
    }
    if (inserito) { ordinaPerOra(cp.p, cp.n); cambiato = true; }
    cp.live = contaLive(cp);
    if (cambiato) cp.versione++;
    dataUnlock();
}

/* ---------------------------------------------------------------------------
 *  Serie A standings
 * ------------------------------------------------------------------------- */
static bool pollClassifica()
{
    String body;
    const int code = httpGet(String(BASE_STD) + "ita.1/standings", body);
    if (code != HTTP_CODE_OK) {
        Serial.printf("[FOOTBALL] standings HTTP %d\n", code);
        return false;
    }

    JsonDocument filtro(PSRAM_JSON);
    JsonObject en = filtro["children"][0]["standings"]["entries"][0].to<JsonObject>();
    en["team"]["shortDisplayName"] = true;
    en["stats"][0]["name"]  = true;
    en["stats"][0]["value"] = true;

    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, body, DeserializationOption::Filter(filtro), DeserializationOption::NestingLimit(40))) return false;
    body = "";

    Classifica cl;
    memset(&cl, 0, sizeof(cl));
    for (JsonObject e : doc["children"][0]["standings"]["entries"].as<JsonArray>()) {
        if (cl.n >= 24) break;
        RigaClassifica &r = cl.r[cl.n];
        char nome[40];
        asciiFold(e["team"]["shortDisplayName"] | "?", nome, sizeof(nome));
        snprintf(r.squadra, sizeof(r.squadra), "%s", nomeItaliano(nome));
        for (JsonObject s : e["stats"].as<JsonArray>()) {
            const char *k = s["name"] | "";
            const float v = s["value"] | 0.0f;
            if (strcmp(k, "rank") == 0)                   r.pos = (uint8_t)v;
            else if (strcmp(k, "gamesPlayed") == 0)       r.g   = (uint8_t)v;
            else if (strcmp(k, "wins") == 0)              r.v   = (uint8_t)v;
            else if (strcmp(k, "ties") == 0)              r.n   = (uint8_t)v;
            else if (strcmp(k, "losses") == 0)            r.p   = (uint8_t)v;
            else if (strcmp(k, "pointDifferential") == 0) r.dr  = (int16_t)v;
            else if (strcmp(k, "points") == 0)            r.pt  = (int16_t)v;
        }
        cl.n++;
    }
    if (cl.n == 0) return false;

    /* sort by position (ESPN does not guarantee the order) */
    for (uint8_t i = 1; i < cl.n; ++i) {
        RigaClassifica x = cl.r[i];
        int j = i - 1;
        while (j >= 0 && cl.r[j].pos > x.pos) { cl.r[j + 1] = cl.r[j]; --j; }
        cl.r[j + 1] = x;
    }
    cl.ok = true;

    if (dataLock(1000)) {
        cl.versione = g_classSerieA.versione + 1;
        g_classSerieA = cl;
        dataUnlock();
    }
    Serial.printf("[FOOTBALL] Serie A standings: %u teams\n", cl.n);
    return true;
}

/* ---------------------------------------------------------------------------
 *  Match detail (ESPN summary, ~350 KB filtered down to a few KB)
 * ------------------------------------------------------------------------- */
static DettaglioPartita *s_det = nullptr;      /* working copy in PSRAM */

struct NomeStat { const char *espn; const char *it; bool percentuale; };
static const NomeStat STAT[] = {
    { "possessionPct",   "Possession",       true  },
    { "totalShots",      "Shots",            false },
    { "shotsOnTarget",   "Shots on target",  false },
    { "wonCorners",      "Corners",          false },
    { "foulsCommitted",  "Fouls",            false },
    { "yellowCards",     "Yellow cards",     false },
    { "redCards",        "Red cards",        false },
    { "offsides",        "Offsides",         false },
    { "saves",           "Saves",            false },
    { "totalPasses",     "Passes",           false },
    { "passPct",         "Pass accuracy",    true  },
};

static void nomeBreve(const char *in, char *out, size_t n)
{
    char tmp[48];
    asciiFold(in, tmp, sizeof(tmp));
    snprintf(out, n, "%s", tmp);
}

static bool pollDettaglio(uint8_t c, const char *id)
{
    String body;
    const String url = String(BASE_SB) + CALCIO_SLUG[c] + "/summary?event=" + id;
    const int code = httpGet(url, body);
    DettaglioPartita &d = *s_det;
    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", id);
    d.comp = c;
    d.golCasa = d.golOspite = -1;
    if (code != HTTP_CODE_OK) {
        snprintf(d.errore, sizeof(d.errore), "HTTP %d", code);
    } else {
        JsonDocument f(PSRAM_JSON);
        JsonObject h = f["header"]["competitions"][0].to<JsonObject>();
        h["date"] = true;
        h["status"]["displayClock"] = true;
        h["status"]["type"]["state"] = true;
        h["status"]["type"]["name"] = true;
        h["status"]["type"]["shortDetail"] = true;
        h["competitors"][0]["homeAway"] = true;
        h["competitors"][0]["score"] = true;
        h["competitors"][0]["team"]["id"] = true;
        h["competitors"][0]["team"]["shortDisplayName"] = true;
        JsonObject k = f["keyEvents"][0].to<JsonObject>();
        k["type"]["type"] = true;
        k["clock"]["displayValue"] = true;
        k["team"]["id"] = true;
        k["participants"][0]["athlete"]["shortName"] = true;
        k["participants"][0]["athlete"]["displayName"] = true;
        JsonObject b = f["boxscore"]["teams"][0].to<JsonObject>();
        b["homeAway"] = true;
        b["statistics"][0]["name"] = true;
        b["statistics"][0]["displayValue"] = true;
        JsonObject ro = f["rosters"][0].to<JsonObject>();
        ro["homeAway"] = true;
        ro["formation"] = true;
        ro["roster"][0]["starter"] = true;
        ro["roster"][0]["jersey"] = true;
        ro["roster"][0]["subbedIn"] = true;
        ro["roster"][0]["subbedOut"] = true;
        ro["roster"][0]["athlete"]["shortName"] = true;
        ro["roster"][0]["position"]["abbreviation"] = true;
        f["gameInfo"]["venue"]["fullName"] = true;
        f["gameInfo"]["attendance"] = true;

        JsonDocument doc(PSRAM_JSON);
        const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(f),
                                                         DeserializationOption::NestingLimit(40));
        body = "";
        if (err) {
            snprintf(d.errore, sizeof(d.errore), "JSON %s", err.c_str());
        } else {
            JsonObject hc = doc["header"]["competitions"][0];
            d.inizio = parseIsoUtc(hc["date"] | "");
            const char *state = hc["status"]["type"]["state"] | "pre";
            const char *name  = hc["status"]["type"]["name"] | "";
            d.stato = strcmp(state, "in") == 0 ? 'i' : (strcmp(state, "post") == 0 ? 'f' : 'p');
            if (d.stato == 'i') {
                if (strstr(name, "HALFTIME")) snprintf(d.minuto, sizeof(d.minuto), "HALF TIME");
                else asciiFold(hc["status"]["displayClock"] | "", d.minuto, sizeof(d.minuto));
            }
            asciiFold(hc["status"]["type"]["shortDetail"] | "", d.dettaglio, sizeof(d.dettaglio));
            char idCasa[12] = "";
            for (JsonObject t : hc["competitors"].as<JsonArray>()) {
                const bool casa = strcmp(t["homeAway"] | "home", "home") == 0;
                char nome[40];
                asciiFold(t["team"]["shortDisplayName"] | "?", nome, sizeof(nome));
                const int8_t gol = d.stato == 'p' ? -1 : leggiGol(t["score"]);
                if (casa) {
                    snprintf(idCasa, sizeof(idCasa), "%s", t["team"]["id"] | "");
                    snprintf(d.casa, sizeof(d.casa), "%s", nomeItaliano(nome));
                    d.golCasa = gol;
                } else {
                    snprintf(d.ospite, sizeof(d.ospite), "%s", nomeItaliano(nome));
                    d.golOspite = gol;
                }
            }

            for (JsonObject e : doc["keyEvents"].as<JsonArray>()) {
                if (d.nEv >= DET_MAX_EVENTI) break;
                const char *tp = e["type"]["type"] | "";
                char tipo = 0;
                if (strcmp(tp, "own-goal") == 0)                 tipo = 'A';
                else if (strncmp(tp, "penalty---scored", 16) == 0) tipo = 'R';
                else if (strncmp(tp, "goal", 4) == 0)            tipo = 'G';
                else if (strcmp(tp, "yellow-card") == 0)         tipo = 'Y';
                else if (strcmp(tp, "red-card") == 0)            tipo = 'X';
                else if (strcmp(tp, "substitution") == 0)        tipo = 'S';
                if (tipo == 0) continue;
                EventoPartita &ev = d.ev[d.nEv++];
                ev.tipo = tipo;
                ev.casa = strcmp(e["team"]["id"] | "", idCasa) == 0;
                asciiFold(e["clock"]["displayValue"] | "", ev.minuto, sizeof(ev.minuto));
                JsonArray pa = e["participants"];
                if (pa.size() > 0) nomeBreve(pa[0]["athlete"]["shortName"] | (pa[0]["athlete"]["displayName"] | ""), ev.nome, sizeof(ev.nome));
                if (pa.size() > 1) nomeBreve(pa[1]["athlete"]["shortName"] | (pa[1]["athlete"]["displayName"] | ""), ev.nome2, sizeof(ev.nome2));
            }

            /* statistics, in the order of the STAT table */
            JsonObject sc, so;
            for (JsonObject t : doc["boxscore"]["teams"].as<JsonArray>()) {
                if (strcmp(t["homeAway"] | "", "home") == 0) sc = t; else so = t;
            }
            for (const NomeStat &s : STAT) {
                if (d.nStat >= DET_MAX_STAT) break;
                const char *vc = nullptr, *vo = nullptr;
                for (JsonObject x : sc["statistics"].as<JsonArray>()) if (strcmp(x["name"] | "", s.espn) == 0) vc = x["displayValue"].as<const char *>();
                for (JsonObject x : so["statistics"].as<JsonArray>()) if (strcmp(x["name"] | "", s.espn) == 0) vo = x["displayValue"].as<const char *>();
                if (vc == nullptr && vo == nullptr) continue;
                const uint8_t i = d.nStat++;
                snprintf(d.statNome[i], sizeof(d.statNome[i]), "%s", s.it);
                if (s.percentuale) {
                    /* "38.1" -> "38%", "0.7" -> "70%" */
                    float a = vc ? atof(vc) : 0, bb = vo ? atof(vo) : 0;
                    if (a <= 1.0f && bb <= 1.0f) { a *= 100; bb *= 100; }
                    snprintf(d.statCasa[i], sizeof(d.statCasa[i]), "%.0f%%", a);
                    snprintf(d.statOsp[i], sizeof(d.statOsp[i]), "%.0f%%", bb);
                } else {
                    snprintf(d.statCasa[i], sizeof(d.statCasa[i]), "%s", vc ? vc : "-");
                    snprintf(d.statOsp[i], sizeof(d.statOsp[i]), "%s", vo ? vo : "-");
                }
            }

            /* line-ups: starters first, then substitutes who came on */
            for (JsonObject r : doc["rosters"].as<JsonArray>()) {
                const bool casa = strcmp(r["homeAway"] | "", "home") == 0;
                Giocatore *g = casa ? d.gCasa : d.gOsp;
                uint8_t &n = casa ? d.nCasa : d.nOsp;
                asciiFold(r["formation"] | "", casa ? d.moduloCasa : d.moduloOsp, sizeof(d.moduloCasa));
                for (int giro = 0; giro < 2; ++giro) {
                    for (JsonObject p : r["roster"].as<JsonArray>()) {
                        const bool tit = p["starter"] | false;
                        const bool ent = p["subbedIn"] | false;
                        if (giro == 0 ? !tit : (tit || !ent)) continue;
                        if (n >= DET_MAX_GIOC) break;
                        Giocatore &x = g[n++];
                        snprintf(x.num, sizeof(x.num), "%s", p["jersey"] | "");
                        nomeBreve(p["athlete"]["shortName"] | "", x.nome, sizeof(x.nome));
                        asciiFold(p["position"]["abbreviation"] | "", x.ruolo, sizeof(x.ruolo));
                        x.titolare = tit;
                        x.entrato = ent;
                        x.uscito = p["subbedOut"] | false;
                    }
                }
            }
            asciiFold(doc["gameInfo"]["venue"]["fullName"] | "", d.stadio, sizeof(d.stadio));
            d.spettatori = doc["gameInfo"]["attendance"] | 0;
            d.pronto = true;
        }
    }
    d.aggMs = millis();
    if (dataLock(1000)) {
        /* discard the result if in the meantime the user opened another match */
        if (strcmp(g_detId, id) == 0) {
            d.versione = g_det->versione + 1;
            memcpy(g_det, &d, sizeof(DettaglioPartita));
        }
        dataUnlock();
    }
    Serial.printf("[FOOTBALL] detail %s: %s, %u events, %u stats, %u+%u players\n", id,
                  d.pronto ? "ok" : d.errore, d.nEv, d.nStat, d.nCasa, d.nOsp);
    return d.pronto;
}

/* ---------------------------------------------------------------------------
 *  Task
 * ------------------------------------------------------------------------- */
void calcioTask(void *arg)
{
    (void)arg;
    s_tmp = static_cast<Partita *>(heap_caps_calloc(CALCIO_MAX_PARTITE, sizeof(Partita),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_tmp == nullptr) s_tmp = static_cast<Partita *>(calloc(CALCIO_MAX_PARTITE, sizeof(Partita)));
    s_det = static_cast<DettaglioPartita *>(heap_caps_calloc(1, sizeof(DettaglioPartita),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint32_t ultimoDettaglio = 0;

    uint32_t ultimaClassifica = 0;
    uint8_t  prossimaAltra = 0;

    while (true) {
        if (!reteOk()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

        /* 0. detail card: immediately if requested, then every 30 s if the match is in progress */
        if (g_detAperto) {
            char id[16];
            uint8_t c = 0;
            bool live = false;
            if (dataLock(200)) {
                snprintf(id, sizeof(id), "%s", g_detId);
                c = g_detComp;
                live = g_det->stato == 'i' || !g_det->pronto;
                dataUnlock();
            }
            if (id[0] != 0 && c < CALCIO_COMP_COUNT &&
                (g_detRichiesta || (live && millis() - ultimoDettaglio >= 30000))) {
                g_detRichiesta = false;
                pollDettaglio(c, id);
                ultimoDettaglio = millis();
            }
        }

        /* 1. competition on screen */
        const uint8_t vis = g_compVisibile < CALCIO_COMP_COUNT ? g_compVisibile : 0;
        bool caricata = false;
        uint32_t agg = 0;
        if (dataLock(200)) { caricata = g_comp[vis].caricata; agg = g_comp[vis].aggMs; dataUnlock(); }
        const uint32_t ora = millis();
        const bool ritentaOk = s_tentativoLista[vis] == 0 || ora - s_tentativoLista[vis] >= 60000;
        if (ritentaOk && (!caricata || ora - agg >= CALCIO_LISTA_VISIBILE_MS ||
                          (g_richiestaLista && ora - agg >= 120000))) {
            pollLista(vis);
        }
        g_richiestaLista = false;

        /* 2. live on all competitions */
        for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) {
            const uint32_t periodo = serveLiveVeloce(c) ? CALCIO_LIVE_ATTIVO_MS : CALCIO_LIVE_IDLE_MS;
            if (!compAttiva(c)) continue;
            if (s_ultimoLive[c] == 0 || millis() - s_ultimoLive[c] >= periodo) pollLive(c);
        }

        /* 3. standings */
        if (compAttiva(0) && (ultimaClassifica == 0 || millis() - ultimaClassifica >= CALCIO_CLASSIFICA_MS)) {
            pollClassifica();
            ultimaClassifica = millis();
        }

        /* 4. one non-visible list at a time, in the background */
        for (uint8_t k = 0; k < CALCIO_COMP_COUNT; ++k) {
            const uint8_t c = (prossimaAltra + k) % CALCIO_COMP_COUNT;
            if (c == g_compVisibile || !compAttiva(c)) continue;
            bool car = false;
            uint32_t a = 0;
            if (dataLock(200)) { car = g_comp[c].caricata; a = g_comp[c].aggMs; dataUnlock(); }
            const bool ritenta = s_tentativoLista[c] == 0 || millis() - s_tentativoLista[c] >= 120000;
            if (ritenta && (!car || millis() - a >= CALCIO_LISTA_ALTRE_MS)) {
                pollLista(c);
                prossimaAltra = (c + 1) % CALCIO_COMP_COUNT;
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
