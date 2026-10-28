/* ============================================================================
 *  F1Live - real time from the official live timing (livetiming.formula1.com)
 *
 *  Protocol: SignalR Core over WebSocket, JSON format. No login is needed for the
 *  topics used here (verified on 04/10/2026):
 *    1. POST /signalrcore/negotiate?negotiateVersion=1  -> connectionToken + AWSALB cookie
 *    2. GET  /signalrcore?id=<token>  with Upgrade: websocket and the same cookie
 *    3. {"protocol":"json","version":1}<0x1E>
 *    4. Subscribe to the topics -> type 3 reply with the full state,
 *       then type 1 messages, target "feed", with the differences only.
 *
 *  The WebSocket client is written here on top of NetworkClientSecure: this way there
 *  are no size limits on the messages (the initial state exceeds 40 KB) and
 *  the buffer lives in PSRAM.
 *
 *  Outside sessions it connects every 10 minutes only to read the state
 *  and then closes; during a session window it stays connected.
 * ========================================================================== */
#include <ArduinoJson.h>
#include "PsramJson.h"
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "Clients.h"
#include "DataModel.h"
#include "Notify.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <Preferences.h>

static const char HOST[] = "livetiming.formula1.com";
static const char TOPIC_SUBSCRIBE[] =
    "{\"type\":1,\"target\":\"Subscribe\",\"invocationId\":\"0\",\"arguments\":[["
    "\"SessionInfo\",\"SessionStatus\",\"LapCount\",\"TrackStatus\","
    "\"DriverList\",\"TimingData\",\"RaceControlMessages\","
    "\"TimingAppData\"]]}\x1e";

static constexpr size_t MSG_CAP = 512 * 1024;

static NetworkClientSecure s_cli;
static char    *s_msg = nullptr;
static size_t   s_len = 0;
static bool     s_overflow = false;
static bool     s_connesso = false;
static char     s_cookie[512];
static char     s_token[128];
static char     s_leader[4] = "";

/* circuit outline: loading from NVS or from MultiViewer done by the task,
 * outside the data lock */
static volatile uint16_t s_trkDaCaricare = 0;
static uint32_t          s_trkRiprovaMs  = 0;

/* ===========================================================================
 *  Socket: read with timeout
 * ========================================================================= */
static bool leggiEsatti(NetworkClientSecure &c, uint8_t *buf, size_t n, uint32_t timeoutMs)
{
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < n) {
        const int disp = c.available();
        if (disp > 0) {
            const int r = c.read(buf + got, n - got);
            if (r > 0) { got += (size_t)r; t0 = millis(); continue; }
        } else if (!c.connected()) {
            return false;
        }
        if (millis() - t0 > timeoutMs) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool leggiRiga(NetworkClientSecure &c, char *buf, size_t n, uint32_t timeoutMs)
{
    size_t i = 0;
    uint32_t t0 = millis();
    while (true) {
        if (c.available() > 0) {
            const int ch = c.read();
            if (ch < 0) continue;
            t0 = millis();
            if (ch == '\n') {
                if (i > 0 && buf[i - 1] == '\r') --i;
                buf[i] = 0;
                return true;
            }
            if (i + 1 < n) buf[i++] = (char)ch;
        } else {
            if (!c.connected()) { buf[i] = 0; return i > 0; }
            if (millis() - t0 > timeoutMs) return false;
            vTaskDelay(1);
        }
    }
}

/* ===========================================================================
 *  1. negotiate
 * ========================================================================= */
static void urlEncode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; ++in) {
        const char c = *in;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[((uint8_t)c) >> 4];
            out[o++] = hex[((uint8_t)c) & 0x0F];
        }
    }
    out[o] = 0;
}

static bool negozia()
{
    NetworkClientSecure c;
    c.setInsecure();
    c.setTimeout(10);
    if (!c.connect(HOST, 443)) {
        Serial.println("[F1LIVE] negotiate: TLS connection failed");
        return false;
    }
    c.print("POST /signalrcore/negotiate?negotiateVersion=1 HTTP/1.1\r\n"
            "Host: livetiming.formula1.com\r\n"
            "User-Agent: BestHTTP\r\n"
            "Accept: application/json\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");

    char riga[600];
    if (!leggiRiga(c, riga, sizeof(riga), 10000)) { c.stop(); return false; }
    const char *sp = strchr(riga, ' ');
    const int status = sp ? atoi(sp + 1) : 0;
    if (status != 200) {
        Serial.printf("[F1LIVE] negotiate HTTP %d\n", status);
        c.stop();
        return false;
    }

    s_cookie[0] = 0;
    long clen = -1;
    bool chunked = false;
    while (leggiRiga(c, riga, sizeof(riga), 10000)) {
        if (riga[0] == 0) break;
        if (strncasecmp(riga, "set-cookie:", 11) == 0) {
            const char *v = riga + 11;
            while (*v == ' ') ++v;
            const size_t l = strcspn(v, ";");
            const size_t cur = strlen(s_cookie);
            if (cur + l + 3 < sizeof(s_cookie)) {
                if (cur > 0) strcat(s_cookie, "; ");
                strncat(s_cookie, v, l);
            }
        } else if (strncasecmp(riga, "content-length:", 15) == 0) {
            clen = atol(riga + 15);
        } else if (strncasecmp(riga, "transfer-encoding:", 18) == 0 && strstr(riga, "chunked")) {
            chunked = true;
        }
    }

    String body;
    if (chunked) {
        while (leggiRiga(c, riga, sizeof(riga), 10000)) {
            const long sz = strtol(riga, nullptr, 16);
            if (sz <= 0) break;
            uint8_t tmp[256];
            long resto = sz;
            while (resto > 0) {
                const size_t blocco = resto > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)resto;
                if (!leggiEsatti(c, tmp, blocco, 10000)) { resto = -1; break; }
                body.concat((const char *)tmp, blocco);
                resto -= (long)blocco;
            }
            if (resto < 0) break;
            leggiRiga(c, riga, sizeof(riga), 5000);   /* CRLF after the block */
        }
    } else {
        const uint32_t t0 = millis();
        while ((clen < 0 || (long)body.length() < clen) && millis() - t0 < 10000) {
            if (c.available() > 0) {
                const int ch = c.read();
                if (ch >= 0) body += (char)ch;
            } else if (!c.connected()) {
                break;
            } else {
                vTaskDelay(1);
            }
        }
    }
    c.stop();

    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, body, DeserializationOption::NestingLimit(40))) {
        Serial.println("[F1LIVE] negotiate: invalid JSON");
        return false;
    }
    const char *tok = doc["connectionToken"] | (doc["connectionId"] | "");
    if (tok[0] == 0) return false;
    snprintf(s_token, sizeof(s_token), "%s", tok);
    return true;
}

/* ===========================================================================
 *  2. WebSocket
 * ========================================================================= */
static bool inviaFrame(uint8_t opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | opcode;
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i) hdr[h++] = (uint8_t)(((uint64_t)len >> (8 * i)) & 0xFF);
    }
    const uint32_t r = esp_random();
    uint8_t mask[4];
    memcpy(mask, &r, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;

    uint8_t *buf = static_cast<uint8_t *>(malloc(h + len));
    if (buf == nullptr) return false;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; ++i) buf[h + i] = data[i] ^ mask[i & 3];
    const size_t scritti = s_cli.write(buf, h + len);
    free(buf);
    return scritti == h + len;
}

static bool inviaTesto(const char *s)
{
    return inviaFrame(0x1, reinterpret_cast<const uint8_t *>(s), strlen(s));
}

static void chiudi()
{
    if (s_connesso) Serial.println("[F1LIVE] disconnected");
    s_cli.stop();
    s_connesso = false;
    s_len = 0;
    if (dataLock(500)) {
        if (g_f1live->connesso) { g_f1live->connesso = false; g_f1live->versione++; }
        dataUnlock();
    }
}

static bool connettiInterno();

static bool connetti()
{
    if (!reteLock(60000)) return false;
    const bool ok = connettiInterno();
    reteUnlock();
    if (!ok) Serial.printf("[F1LIVE] free internal RAM %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ok;
}

static bool connettiInterno()
{
    if (!negozia()) return false;

    s_cli.setInsecure();
    s_cli.setTimeout(10);
    if (!s_cli.connect(HOST, 443)) {
        Serial.println("[F1LIVE] websocket: TLS connection failed");
        return false;
    }

    uint8_t chiave[16];
    esp_fill_random(chiave, sizeof(chiave));
    unsigned char chiave64[32];
    size_t l64 = 0;
    mbedtls_base64_encode(chiave64, sizeof(chiave64), &l64, chiave, sizeof(chiave));
    chiave64[l64] = 0;

    char tok[256];
    urlEncode(s_token, tok, sizeof(tok));

    String req;
    req.reserve(900);
    req += "GET /signalrcore?id=";
    req += tok;
    req += " HTTP/1.1\r\nHost: livetiming.formula1.com\r\n"
           "User-Agent: BestHTTP\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
    req += (const char *)chiave64;
    req += "\r\n";
    if (s_cookie[0] != 0) {
        req += "Cookie: ";
        req += s_cookie;
        req += "\r\n";
    }
    req += "\r\n";
    s_cli.print(req);

    char riga[300];
    if (!leggiRiga(s_cli, riga, sizeof(riga), 10000) || strstr(riga, " 101") == nullptr) {
        Serial.printf("[F1LIVE] upgrade refused: %s\n", riga);
        s_cli.stop();
        return false;
    }
    while (leggiRiga(s_cli, riga, sizeof(riga), 10000)) {
        if (riga[0] == 0) break;
    }

    s_connesso = true;
    s_len = 0;
    s_overflow = false;
    if (!inviaTesto("{\"protocol\":\"json\",\"version\":1}\x1e") || !inviaTesto(TOPIC_SUBSCRIBE)) {
        chiudi();
        return false;
    }
    if (dataLock(500)) { g_f1live->connesso = true; g_f1live->versione++; dataUnlock(); }
    Serial.println("[F1LIVE] connected to the live timing");
    return true;
}

/* 1 = complete message in s_msg, 0 = nothing new, -1 = connection closed */
static int leggiFrame()
{
    if (s_cli.available() <= 0) return s_cli.connected() ? 0 : -1;

    uint8_t h[2];
    if (!leggiEsatti(s_cli, h, 2, 5000)) return -1;
    const bool fin = (h[0] & 0x80) != 0;
    const uint8_t op = h[0] & 0x0F;
    const bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t e[2];
        if (!leggiEsatti(s_cli, e, 2, 5000)) return -1;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!leggiEsatti(s_cli, e, 8, 5000)) return -1;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
    }
    uint8_t mask[4] = { 0, 0, 0, 0 };
    if (masked && !leggiEsatti(s_cli, mask, 4, 5000)) return -1;

    if (op == 0x8) return -1;                        /* close */
    if (op == 0x9 || op == 0xA) {                    /* ping / pong */
        uint8_t p[125];
        const size_t n = len > sizeof(p) ? sizeof(p) : (size_t)len;
        if (!leggiEsatti(s_cli, p, n, 5000)) return -1;
        if (op == 0x9) inviaFrame(0xA, p, n);
        return 0;
    }

    if (op == 0x1 || op == 0x2) { s_len = 0; s_overflow = false; }

    if (s_overflow || s_len + len + 1 > MSG_CAP) {
        /* too large: discard it without losing frame sync */
        s_overflow = true;
        uint8_t tmp[512];
        uint64_t resto = len;
        while (resto > 0) {
            const size_t b = resto > sizeof(tmp) ? sizeof(tmp) : (size_t)resto;
            if (!leggiEsatti(s_cli, tmp, b, 10000)) return -1;
            resto -= b;
        }
    } else {
        if (!leggiEsatti(s_cli, reinterpret_cast<uint8_t *>(s_msg + s_len), (size_t)len, 15000)) return -1;
        if (masked) for (uint64_t i = 0; i < len; ++i) s_msg[s_len + i] ^= mask[i & 3];
        s_len += (size_t)len;
    }

    if (!fin) return 0;
    if (s_overflow) {
        Serial.println("[F1LIVE] message too large, discarded");
        s_overflow = false;
        s_len = 0;
        return 0;
    }
    s_msg[s_len] = 0;
    return 1;
}

/* ===========================================================================
 *  3. Applying the data (with dataLock taken)
 * ========================================================================= */
static void nomeSessione(const char *en, char *out, size_t n)
{
    if (strcmp(en, "Race") == 0)                    snprintf(out, n, "Race");
    else if (strcmp(en, "Qualifying") == 0)         snprintf(out, n, "Qualifying");
    else if (strncmp(en, "Sprint Q", 8) == 0 ||
             strncmp(en, "Sprint S", 8) == 0)       snprintf(out, n, "Sprint Q.");
    else if (strncmp(en, "Practice ", 9) == 0)      snprintf(out, n, "Practice %s", en + 9);
    else                                            asciiFold(en, out, n);
}

static F1LivePilota *pilota(F1Live &L, const char *num, bool crea)
{
    for (uint8_t i = 0; i < L.n; ++i) {
        if (strcmp(L.p[i].num, num) == 0) return &L.p[i];
    }
    if (!crea || L.n >= 24 || num[0] == '_') return nullptr;
    F1LivePilota &p = L.p[L.n++];
    memset(&p, 0, sizeof(p));
    snprintf(p.num, sizeof(p.num), "%s", num);
    snprintf(p.tla, sizeof(p.tla), "%s", num);
    p.colore = 0x8B949E;
    return &p;
}

static void ordinaPiloti(F1Live &L)
{
    for (uint8_t i = 1; i < L.n; ++i) {
        F1LivePilota x = L.p[i];
        const uint8_t kx = x.pos == 0 ? 255 : x.pos;
        int j = i - 1;
        while (j >= 0 && (L.p[j].pos == 0 ? 255 : L.p[j].pos) > kx) { L.p[j + 1] = L.p[j]; --j; }
        L.p[j + 1] = x;
    }
}

static bool garaInCorso(const F1Live &L)
{
    return strcmp(L.tipo, "Race") == 0 && strcmp(L.stato, "Started") == 0;
}

static void copiaValore(JsonVariant v, char *out, size_t n)
{
    if (v.isNull()) return;
    if (v.is<JsonObject>()) {
        if (v["Value"].isNull()) return;
        asciiFold(v["Value"] | "", out, n);
    } else if (v.is<const char *>()) {
        asciiFold(v.as<const char *>(), out, n);
    }
}

static void applicaSessionInfo(F1Live &L, JsonObject o)
{
    if (!o["Meeting"]["Name"].isNull()) asciiFold(o["Meeting"]["Name"] | "", L.meeting, sizeof(L.meeting));
    if (!o["Name"].isNull())            nomeSessione(o["Name"] | "", L.sessione, sizeof(L.sessione));
    if (!o["Type"].isNull())            asciiFold(o["Type"] | "", L.tipo, sizeof(L.tipo));
    const long off = parseOffset(o["GmtOffset"] | "00:00:00");
    if (!o["StartDate"].isNull()) L.inizio = parseIsoUtc(o["StartDate"] | "") - off;
    if (!o["EndDate"].isNull())   L.fine   = parseIsoUtc(o["EndDate"] | "") - off;
    if (!o["Meeting"]["Circuit"]["Key"].isNull()) {
        L.circuito = (uint16_t)(o["Meeting"]["Circuit"]["Key"] | 0);
        if (L.circuito != 0 && g_trk->chiave != L.circuito) s_trkDaCaricare = L.circuito;
    }
}

static void applicaStato(F1Live &L, JsonObject o, bool snapshot)
{
    if (o["Status"].isNull()) return;
    char nuovo[16];
    asciiFold(o["Status"] | "", nuovo, sizeof(nuovo));
    if (!snapshot && strcmp(nuovo, L.stato) != 0) {
        if (strcmp(nuovo, "Started") == 0) {
            notifySchermata(0, 0), notifica(NOT_F1, strcmp(L.tipo, "Race") == 0 ? SND_F1_PARTENZA : SND_F1_PASSAGGIO,
                     "F1  %s - %s: green light!", L.meeting, L.sessione);
        } else if (strcmp(nuovo, "Finished") == 0) {
            if (strcmp(L.tipo, "Race") == 0 && L.n > 0 && L.p[0].pos == 1) {
                notifySchermata(0, 0), notifica(NOT_F1, SND_BLOCK_FOUND, "F1  Chequered flag: %s wins (%s)",
                         L.p[0].cognome[0] ? L.p[0].cognome : L.p[0].tla, L.p[0].team);
            } else {
                notifySchermata(0, 0), notifica(NOT_F1, SND_COUNT, "F1  End of %s", L.sessione);
            }
        } else if (strcmp(nuovo, "Aborted") == 0) {
            notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  Session suspended - RED FLAG");
        }
    }
    snprintf(L.stato, sizeof(L.stato), "%s", nuovo);
}

static void applicaPista(F1Live &L, JsonObject o, bool snapshot)
{
    if (o["Status"].isNull()) return;
    const uint8_t nuovo = (uint8_t)atoi(o["Status"] | "1");
    if (!snapshot && nuovo != L.pista) {
        switch (nuovo) {
            case 4: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  SAFETY CAR on track - lap %u", L.giro); break;
            case 5: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  RED FLAG - lap %u", L.giro); break;
            case 6: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  VIRTUAL SAFETY CAR - lap %u", L.giro); break;
            case 1:
                if (L.pista >= 4) notifySchermata(0, 0), notifica(NOT_OK, SND_F1_PASSAGGIO, "F1  Green flag, racing resumes");
                break;
            default: break;
        }
    }
    L.pista = nuovo;
}

static void applicaPiloti(F1Live &L, JsonObject o)
{
    for (JsonPair kv : o) {
        if (!kv.value().is<JsonObject>()) continue;
        JsonObject d = kv.value().as<JsonObject>();
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;
        if (!d["Tla"].isNull())      asciiFold(d["Tla"] | "", p->tla, sizeof(p->tla));
        if (!d["LastName"].isNull()) asciiFold(d["LastName"] | "", p->cognome, sizeof(p->cognome));
        if (!d["TeamName"].isNull()) asciiFold(d["TeamName"] | "", p->team, sizeof(p->team));
        if (!d["TeamColour"].isNull()) p->colore = (uint32_t)strtoul(d["TeamColour"] | "8B949E", nullptr, 16);
        if (!d["Line"].isNull() && p->pos == 0) p->pos = (uint8_t)(d["Line"] | 0);
    }
    ordinaPiloti(L);
}

static void applicaTiming(F1Live &L, JsonObject o, bool snapshot)
{
    JsonObject lines = o["Lines"];
    if (lines.isNull()) return;
    const bool gara = garaInCorso(L);

    for (JsonPair kv : lines) {
        if (!kv.value().is<JsonObject>()) continue;
        JsonObject d = kv.value().as<JsonObject>();
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;

        int nuovaPos = -1;
        if (!d["Position"].isNull())  nuovaPos = atoi(d["Position"] | "0");
        else if (!d["Line"].isNull()) nuovaPos = d["Line"] | 0;
        if (nuovaPos > 0 && nuovaPos != p->pos) {
            if (!snapshot && gara && p->pos > 0) {
                p->delta = (int8_t)((int)p->pos - nuovaPos);
                p->deltaMs = millis() | 1;
            }
            p->pos = (uint8_t)nuovaPos;
        }

        copiaValore(d["GapToLeader"], p->gap, sizeof(p->gap));
        copiaValore(d["IntervalToPositionAhead"], p->intervallo, sizeof(p->intervallo));
        if (!d["LastLapTime"].isNull()) {
            copiaValore(d["LastLapTime"], p->ultimo, sizeof(p->ultimo));
            const uint32_t ms = tempoGiroMs(p->ultimo);
            if (ms > 0) p->giroMs = ms;
        }
        if (!d["NumberOfLaps"].isNull()) {
            const uint8_t g = (uint8_t)(d["NumberOfLaps"] | 0);
            /* one more lap = crossing the line now */
            if (!snapshot && g > p->giri) p->passaggioMs = millis() | 1;
            p->giri = g;
        }
        if (!d["NumberOfPitStops"].isNull()) p->pit   = (uint8_t)(d["NumberOfPitStops"] | 0);
        if (!d["InPit"].isNull())            p->inPit = d["InPit"] | false;
        if (!d["Retired"].isNull())          p->ritirato = d["Retired"] | false;
        if (!d["Stopped"].isNull())          p->fermo = d["Stopped"] | false;

        /* best lap: BestLapTime (practice, race) or BestLapTimes (qualifying) */
        if (!d["BestLapTime"].isNull()) {
            char t[12] = "";
            copiaValore(d["BestLapTime"], t, sizeof(t));
            const uint32_t ms = tempoGiroMs(t);
            if (ms > 0) { snprintf(p->migliore, sizeof(p->migliore), "%s", t); p->miglioreMs = ms; }
        }
        JsonVariant blt = d["BestLapTimes"];
        if (!blt.isNull()) {
            auto prova = [&](JsonVariant v) {
                const char *s = v["Value"] | "";
                const uint32_t ms = tempoGiroMs(s);
                if (ms > 0 && (p->miglioreMs == 0 || ms < p->miglioreMs)) {
                    asciiFold(s, p->migliore, sizeof(p->migliore));
                    p->miglioreMs = ms;
                }
            };
            if (blt.is<JsonArray>())       for (JsonVariant v : blt.as<JsonArray>()) prova(v);
            else if (blt.is<JsonObject>()) for (JsonPair q : blt.as<JsonObject>()) prova(q.value());
        }
    }
    ordinaPiloti(L);

    /* leader change in the race (from the second lap: at the start it is normal) */
    bool nuovoLeader = false;
    if (L.n > 0 && L.p[0].pos == 1) {
        if (!snapshot && gara && L.giro >= 2 && s_leader[0] != 0 && strcmp(s_leader, L.p[0].num) != 0) {
            notifySchermata(0, 0), notifica(NOT_F1, SND_F1_PASSAGGIO, "F1  New leader: %s %s - lap %u/%u",
                     L.p[0].tla, L.p[0].cognome, L.giro, L.giriTot);
            nuovoLeader = true;
        }
        snprintf(s_leader, sizeof(s_leader), "%s", L.p[0].num);
    }

    /* overtake for 2nd or 3rd place: passing roar, at most one every
     * 30 s so the panel does not turn into a race track in the first laps */
    static char     s_podio[2][4] = { "", "" };
    static uint32_t s_ultimoPodioMs = 0;
    if (L.n >= 3 && L.p[1].pos == 2 && L.p[2].pos == 3) {
        const bool cambiato = strcmp(s_podio[0], L.p[1].num) != 0 || strcmp(s_podio[1], L.p[2].num) != 0;
        if (cambiato && !snapshot && gara && !nuovoLeader && L.giro >= 2 && s_podio[0][0] != 0 &&
            (s_ultimoPodioMs == 0 || millis() - s_ultimoPodioMs >= 30000)) {
            const bool secondo = strcmp(s_podio[0], L.p[1].num) != 0;
            const F1LivePilota &chi = secondo ? L.p[1] : L.p[2];
            notifySchermata(0, 0), notifica(NOT_F1, SND_F1_PASSAGGIO, "F1  %s %s up to P%u - lap %u/%u   (podium: %s %s %s)",
                     chi.tla, chi.cognome, secondo ? 2 : 3, L.giro, L.giriTot,
                     L.p[0].tla, L.p[1].tla, L.p[2].tla);
            s_ultimoPodioMs = millis() | 1;
        }
        snprintf(s_podio[0], sizeof(s_podio[0]), "%s", L.p[1].num);
        snprintf(s_podio[1], sizeof(s_podio[1]), "%s", L.p[2].num);
    }
}

static void applicaMessaggi(F1Live &L, JsonVariant v)
{
    JsonVariant m = v["Messages"];
    /* JsonObject can be reassigned (it is a reference), JsonVariant cannot */
    JsonObject ultimo;
    if (m.is<JsonArray>()) {
        JsonArray a = m.as<JsonArray>();
        if (a.size() > 0) ultimo = a[a.size() - 1].as<JsonObject>();
    } else if (m.is<JsonObject>()) {
        for (JsonPair q : m.as<JsonObject>()) ultimo = q.value().as<JsonObject>();
    }
    if (ultimo.isNull()) return;
    char testo[80];
    asciiFold(ultimo["Message"] | "", testo, sizeof(testo));
    const int giro = ultimo["Lap"] | 0;
    if (giro > 0) snprintf(L.messaggio, sizeof(L.messaggio), "G%d  %s", giro, testo);
    else          snprintf(L.messaggio, sizeof(L.messaggio), "%s", testo);
}

/* ---- tyres (TimingAppData) ---------------------------------------------- */
static char letteraGomma(const char *c)
{
    if (c == nullptr) return '?';
    if (strcmp(c, "SOFT") == 0)         return 'S';
    if (strcmp(c, "MEDIUM") == 0)       return 'M';
    if (strcmp(c, "HARD") == 0)         return 'H';
    if (strcmp(c, "INTERMEDIATE") == 0) return 'I';
    if (strcmp(c, "WET") == 0)          return 'W';
    return '?';
}

static void applicaStint(F1LivePilota *p, int idx, JsonVariant o)
{
    if (idx < 0 || !o.is<JsonObject>()) return;
    if (p->gomma != 0 && idx < p->stint) return;          /* old stint */
    if (idx > p->stint || p->gomma == 0) {
        p->stint = (uint8_t)idx;
        if (p->gomma == 0) p->gomma = '?';
    }
    if (!o["Compound"].isNull())  p->gomma = letteraGomma(o["Compound"] | "");
    if (!o["TotalLaps"].isNull()) p->giriGomma = (uint8_t)(o["TotalLaps"] | 0);
    if (!o["New"].isNull())       p->gommaNuova = strcmp(o["New"] | "", "true") == 0;
}

static void applicaGomme(F1Live &L, JsonObject o)
{
    JsonObject lines = o["Lines"];
    if (lines.isNull()) return;
    for (JsonPair kv : lines) {
        if (!kv.value().is<JsonObject>()) continue;
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;
        JsonVariant st = kv.value()["Stints"];
        if (st.is<JsonArray>()) {
            int i = 0;
            for (JsonVariant s : st.as<JsonArray>()) applicaStint(p, i++, s);
        } else if (st.is<JsonObject>()) {
            for (JsonPair q : st.as<JsonObject>()) applicaStint(p, atoi(q.key().c_str()), q.value());
        }
    }
}

/* ---- circuit outline -------------------------------------------------------
 * Source: MultiViewer (api.multiviewer.app), the same one used by the app of the
 * same name for live timing. It returns x[], y[] and, for many circuits, the
 * reference time from the finish line to each point (trackPositionTime): used to place
 * the cars on the track starting from their last crossing of the line.
 * Saved in NVS (namespace "sport_trk2", key "c<circuit>"):
 * blob = n, tempiVeri, tGiro, x[n], y[n], t[n] (all 16-bit).                */
static bool caricaDaNvs(uint16_t chiave, Tracciato &T)
{
    static int16_t buf[3 + 3 * TRK_MAX];
    char k[12];
    snprintf(k, sizeof(k), "c%u", chiave);
    size_t letti = 0;
    Preferences p;
    if (p.begin("sport_trk2", true)) {
        if (p.isKey(k)) letti = p.getBytes(k, buf, sizeof(buf));
        p.end();
    }
    if (letti < 6) return false;
    const uint16_t n = (uint16_t)buf[0];
    if (n < 10 || n > TRK_MAX || letti < (size_t)(3 + 3 * n) * 2) return false;
    T.n = n;
    T.tempiVeri = buf[1] != 0;
    T.tGiro = (uint16_t)buf[2];
    memcpy(T.x, buf + 3, n * 2);
    memcpy(T.y, buf + 3 + n, n * 2);
    memcpy(T.t, buf + 3 + 2 * n, n * 2);
    T.completo = true;
    return true;
}

static void salvaInNvs(const Tracciato &T)
{
    static int16_t buf[3 + 3 * TRK_MAX];
    const uint16_t n = T.n;
    buf[0] = (int16_t)n;
    buf[1] = T.tempiVeri ? 1 : 0;
    buf[2] = (int16_t)T.tGiro;
    memcpy(buf + 3, T.x, n * 2);
    memcpy(buf + 3 + n, T.y, n * 2);
    memcpy(buf + 3 + 2 * n, T.t, n * 2);
    char k[12];
    snprintf(k, sizeof(k), "c%u", T.chiave);
    Preferences p;
    if (p.begin("sport_trk2", false)) {
        p.putBytes(k, buf, (size_t)(3 + 3 * n) * 2);
        p.end();
    }
}

/* downloads the outline; true if successful */
static bool scaricaDaMultiViewer(uint16_t chiave, Tracciato &T)
{
    time_t ora = time(nullptr);
    struct tm tmv;
    localtime_r(&ora, &tmv);
    int anno = tmv.tm_year + 1900;
    if (anno < 2024) anno = 2026;
    for (int prova = 0; prova < 3; ++prova, --anno) {
        char url[96];
        snprintf(url, sizeof(url), "https://api.multiviewer.app/api/v1/circuits/%u/%d", chiave, anno);
        String body;
        const int code = httpGet(url, body);
        if (code != 200) continue;
        JsonDocument filtro;
        filtro["x"] = true;
        filtro["y"] = true;
        filtro["trackPositionTime"] = true;
        filtro["circuitName"] = true;
        JsonDocument doc(PSRAM_JSON);
        if (deserializeJson(doc, body, DeserializationOption::Filter(filtro))) continue;
        body = String();
        JsonArray ax = doc["x"], ay = doc["y"], at = doc["trackPositionTime"];
        const size_t tot = ax.size();
        if (tot < 10 || ay.size() != tot) continue;
        const bool tempi = at.size() == tot;
        const size_t passo = (tot + TRK_MAX - 1) / TRK_MAX;     /* thins out beyond 800 points */
        uint16_t n = 0;
        float dist = 0;
        int32_t px = 0, py = 0;
        for (size_t i = 0; i < tot && n < TRK_MAX; i += passo) {
            const int32_t x = (int32_t)(ax[i] | 0.0f), y = (int32_t)(ay[i] | 0.0f);
            if (n > 0) dist += sqrtf((float)(x - px) * (x - px) + (float)(y - py) * (y - py));
            T.x[n] = (int16_t)constrain(x, -32000, 32000);
            T.y[n] = (int16_t)constrain(y, -32000, 32000);
            T.t[n] = tempi ? (uint16_t)lroundf((at[i] | 0.0f) * 100.0f) : (uint16_t)0;
            if (!tempi) T.t[n] = (uint16_t)fminf(dist / 4.0f, 65000.0f);  /* distance in 40 cm units */
            px = x; py = y;
            n++;
        }
        T.n = n;
        T.tempiVeri = tempi;
        T.tGiro = T.t[n - 1];
        if (T.tGiro == 0) T.tGiro = 1;
        T.completo = true;
        Serial.printf("[F1LIVE] outline %s (circuit %u, %d) from MultiViewer: %u points%s\n",
                      doc["circuitName"] | "?", chiave, anno, n, tempi ? ", with times" : "");
        return true;
    }
    return false;
}

static void caricaTracciato(uint16_t chiave)
{
    static Tracciato tmp;           /* ~5 KB, off the task stack */
    memset(&tmp, 0, sizeof(tmp));
    tmp.chiave = chiave;
    bool ok = caricaDaNvs(chiave, tmp);
    if (ok) {
        Serial.printf("[F1LIVE] circuit %u: outline loaded from NVS (%u points)\n", chiave, tmp.n);
    } else {
        ok = scaricaDaMultiViewer(chiave, tmp);
        if (ok) salvaInNvs(tmp);
    }
    if (!ok) {
        Serial.printf("[F1LIVE] circuit %u: outline not available, retrying in 10 min\n", chiave);
        s_trkDaCaricare = chiave;
        s_trkRiprovaMs = millis() | 1;
        return;
    }
    if (dataLock(1000)) {
        tmp.versione = g_trk->versione + 1;
        memcpy(g_trk, &tmp, sizeof(Tracciato));
        dataUnlock();
    }
}

static void applica(const char *topic, JsonVariant v, bool snapshot)
{
    F1Live &L = *g_f1live;
    if (strcmp(topic, "SessionInfo") == 0)          applicaSessionInfo(L, v.as<JsonObject>());
    else if (strcmp(topic, "SessionStatus") == 0)   applicaStato(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "TrackStatus") == 0)     applicaPista(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "DriverList") == 0)      applicaPiloti(L, v.as<JsonObject>());
    else if (strcmp(topic, "TimingData") == 0)      applicaTiming(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "RaceControlMessages") == 0) applicaMessaggi(L, v);
    else if (strcmp(topic, "TimingAppData") == 0)   applicaGomme(L, v.as<JsonObject>());
    else if (strcmp(topic, "LapCount") == 0) {
        if (!v["CurrentLap"].isNull()) L.giro    = (uint8_t)(v["CurrentLap"] | 0);
        if (!v["TotalLaps"].isNull())  L.giriTot = (uint8_t)(v["TotalLaps"] | 0);
    }
}

/* Splits the WebSocket message into SignalR records (separator 0x1E) */
static bool processa()
{
    char *p = s_msg;
    char *fine = s_msg + s_len;
    while (p < fine) {
        char *sep = static_cast<char *>(memchr(p, 0x1E, fine - p));
        const size_t len = sep ? (size_t)(sep - p) : (size_t)(fine - p);
        if (len > 2) {
            JsonDocument doc(PSRAM_JSON);
            if (!deserializeJson(doc, p, len, DeserializationOption::NestingLimit(40))) {
                const int tipo = doc["type"] | 0;
                if (tipo == 7) return false;           /* close from the server */
                if (tipo == 3 && strcmp(doc["invocationId"] | "", "0") == 0) {
                    JsonObject res = doc["result"];
                    if (!res.isNull() && dataLock(1000)) {
                        /* full state: start again from scratch */
                        F1Live &L = *g_f1live;
                        const bool conn = L.connesso;
                        const uint32_t ver = L.versione;
                        memset(&L, 0, sizeof(L));
                        L.connesso = conn;
                        L.versione = ver;
                        s_leader[0] = 0;
                        /* order matters: first the session, then drivers and times */
                        static const char *const ORDINE[] = {
                            "SessionInfo", "SessionStatus", "LapCount", "TrackStatus",
                            "DriverList", "TimingData", "RaceControlMessages",
                            "TimingAppData",
                        };
                        for (const char *t : ORDINE) {
                            if (!res[t].isNull()) applica(t, res[t], true);
                        }
                        L.versione++;
                        L.aggMs = millis();
                        Serial.printf("[F1LIVE] state: %s - %s (%s), %u drivers, lap %u/%u\n",
                                      L.meeting, L.sessione, L.stato, L.n, L.giro, L.giriTot);
                        dataUnlock();
                    }
                } else if (tipo == 1 && strcmp(doc["target"] | "", "feed") == 0) {
                    JsonArray a = doc["arguments"];
                    const char *topic = a[0] | "";
                    if (topic[0] != 0 && dataLock(1000)) {
                        applica(topic, a[1], false);
                        g_f1live->versione++;
                        g_f1live->aggMs = millis();
                        dataUnlock();
                    }
                }
            }
        }
        if (sep == nullptr) break;
        p = sep + 1;
    }
    return true;
}

/* ===========================================================================
 *  4. When to stay connected
 * ========================================================================= */
static bool inFinestra()
{
    const time_t ora = time(nullptr);
    bool si = false;
    if (dataLock(200)) {
        const F1Live &L = *g_f1live;
        if (strcmp(L.stato, "Started") == 0 || strcmp(L.stato, "Aborted") == 0) si = true;
        if (L.inizio > 0 && L.fine > 0 &&
            ora >= L.inizio - (time_t)F1_LIVE_MARGINE_S && ora <= L.fine + (time_t)F1_LIVE_MARGINE_S) si = true;
        for (uint8_t i = 0; i < g_f1->nSessioni && !si; ++i) {
            const F1Sessione &s = g_f1->sessioni[i];
            const time_t durata = strcmp(s.nome, "RACE") == 0 ? 3 * 3600 : 90 * 60;
            if (ora >= s.inizio - (time_t)F1_LIVE_MARGINE_S && ora <= s.inizio + durata + (time_t)F1_LIVE_MARGINE_S) si = true;
        }
        dataUnlock();
    }
    return si;
}

void f1LiveTask(void *arg)
{
    (void)arg;
    s_msg = static_cast<char *>(heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_msg == nullptr) {
        Serial.println("[F1LIVE] PSRAM buffer not available, live disabled");
        vTaskDelete(nullptr);
        return;
    }

    uint32_t ultimoTentativo = 0;
    uint32_t attesa = 0;
    uint32_t ultimoPing = 0;
    uint32_t ultimoMsg = 0;
    uint32_t connessoDa = 0;

    /* let football and Jolpica start first */
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (true) {
        if (!reteOk()) {
            if (s_connesso) chiudi();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* circuit outline (even with the session closed) */
        if (s_trkDaCaricare != 0 && (s_trkRiprovaMs == 0 || millis() - s_trkRiprovaMs >= 600000)) {
            const uint16_t k = s_trkDaCaricare;
            s_trkDaCaricare = 0;
            s_trkRiprovaMs = 0;
            caricaTracciato(k);
        }

        if (!s_connesso) {
            if (ultimoTentativo == 0 || millis() - ultimoTentativo >= attesa) {
                ultimoTentativo = millis();
                if (connetti()) {
                    connessoDa = ultimoMsg = ultimoPing = millis();
                } else {
                    attesa = inFinestra() ? F1_LIVE_RETRY_MS : F1_LIVE_CONTROLLO_MS;
                    Serial.printf("[F1LIVE] connection failed, retrying in %lu s\n", (unsigned long)(attesa / 1000));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* drain what has arrived */
        int giri = 0;
        while (giri++ < 50) {
            const int r = leggiFrame();
            if (r == 0) break;
            if (r < 0 || !processa()) { chiudi(); break; }
            ultimoMsg = millis();
            s_len = 0;
        }
        if (!s_connesso) {
            ultimoTentativo = millis();
            attesa = inFinestra() ? 5000 : F1_LIVE_CONTROLLO_MS;
            continue;
        }

        if (millis() - ultimoPing >= 15000) {
            inviaTesto("{\"type\":6}\x1e");
            ultimoPing = millis();
        }
        static uint32_t ultimoLog = 0;
        if (millis() - ultimoLog >= 60000) {
            ultimoLog = millis();
            if (dataLock(200)) {
                const F1Live &L = *g_f1live;
                Serial.printf("[F1LIVE] %s %s: lap %u/%u, track %u, leader %s, updates %lu\n",
                              L.sessione, L.stato, L.giro, L.giriTot, L.pista,
                              L.n > 0 ? L.p[0].tla : "-", (unsigned long)L.versione);
                dataUnlock();
            }
        }
        if (millis() - ultimoMsg >= 60000) {
            Serial.println("[F1LIVE] no message for 60 s, reconnecting");
            chiudi();
            ultimoTentativo = millis();
            attesa = 2000;
            continue;
        }
        /* outside sessions: once the state is read, close and check again later */
        if (millis() - connessoDa >= 45000 && !inFinestra()) {
            chiudi();
            ultimoTentativo = millis();
            attesa = F1_LIVE_CONTROLLO_MS;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
