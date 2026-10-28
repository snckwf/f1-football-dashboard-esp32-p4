#include "DataModel.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

Competizione *g_comp   = nullptr;
Classifica    g_classSerieA;
F1Dati       *g_f1     = nullptr;
F1Live       *g_f1live = nullptr;
Tracciato    *g_trk    = nullptr;
DettaglioPartita *g_det = nullptr;

volatile bool    g_detRichiesta = false;
volatile bool    g_detAperto    = false;
char             g_detId[16]    = "";
volatile uint8_t g_detComp      = 0;

volatile uint8_t g_compVisibile   = 0;
volatile bool    g_richiestaLista = false;

static SemaphoreHandle_t g_dataMutex = nullptr;
static char g_wifiStatus[80] = "starting";

static void *allocaPsram(size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = calloc(1, bytes);
    return p;
}

void initDataModel()
{
    if (g_dataMutex == nullptr) g_dataMutex = xSemaphoreCreateMutex();
    g_comp   = static_cast<Competizione *>(allocaPsram(sizeof(Competizione) * CALCIO_COMP_COUNT));
    g_f1     = static_cast<F1Dati *>(allocaPsram(sizeof(F1Dati)));
    g_f1live = static_cast<F1Live *>(allocaPsram(sizeof(F1Live)));
    g_trk    = static_cast<Tracciato *>(allocaPsram(sizeof(Tracciato)));
    g_det    = static_cast<DettaglioPartita *>(allocaPsram(sizeof(DettaglioPartita)));
    memset(&g_classSerieA, 0, sizeof(g_classSerieA));
    Serial.printf("[DATA] football %u B, f1 %u B, live %u B\n",
                  (unsigned)(sizeof(Competizione) * CALCIO_COMP_COUNT),
                  (unsigned)sizeof(F1Dati), (unsigned)sizeof(F1Live));
}

bool dataLock(int timeoutMs)
{
    if (g_dataMutex == nullptr) return false;
    const TickType_t ticks = timeoutMs < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTake(g_dataMutex, ticks) == pdTRUE;
}

void dataUnlock()
{
    if (g_dataMutex != nullptr) xSemaphoreGive(g_dataMutex);
}

void setWifiStatusText(const char *text)
{
    if (text != nullptr) snprintf(g_wifiStatus, sizeof(g_wifiStatus), "%s", text);
}
const char *getWifiStatusText() { return g_wifiStatus; }

/* ---------------------------------------------------------------------------
 *  UTF-8 -> ASCII
 * ------------------------------------------------------------------------- */
/* Latin Extended-A, from U+0100 to U+017F: one base letter for each code */
static const char LATIN_EXT_A[] =
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj"
    "Kkk" "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo" "RrRrRr" "SsSsSsSs" "TtTtTt"
    "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";

static const char *foldCp(uint32_t cp, char *tmp)
{
    tmp[1] = 0;
    if (cp >= 0xC0 && cp <= 0xFF) {
        if (cp <= 0xC5) { tmp[0] = 'A'; return tmp; }
        if (cp == 0xC6) return "AE";
        if (cp == 0xC7) { tmp[0] = 'C'; return tmp; }
        if (cp <= 0xCB) { tmp[0] = 'E'; return tmp; }
        if (cp <= 0xCF) { tmp[0] = 'I'; return tmp; }
        if (cp == 0xD0) { tmp[0] = 'D'; return tmp; }
        if (cp == 0xD1) { tmp[0] = 'N'; return tmp; }
        if (cp <= 0xD6 || cp == 0xD8) { tmp[0] = 'O'; return tmp; }
        if (cp == 0xD7) { tmp[0] = 'x'; return tmp; }
        if (cp <= 0xDC) { tmp[0] = 'U'; return tmp; }
        if (cp == 0xDD) { tmp[0] = 'Y'; return tmp; }
        if (cp == 0xDE) return "Th";
        if (cp == 0xDF) return "ss";
        if (cp <= 0xE5) { tmp[0] = 'a'; return tmp; }
        if (cp == 0xE6) return "ae";
        if (cp == 0xE7) { tmp[0] = 'c'; return tmp; }
        if (cp <= 0xEB) { tmp[0] = 'e'; return tmp; }
        if (cp <= 0xEF) { tmp[0] = 'i'; return tmp; }
        if (cp == 0xF0) { tmp[0] = 'd'; return tmp; }
        if (cp == 0xF1) { tmp[0] = 'n'; return tmp; }
        if (cp <= 0xF6 || cp == 0xF8) { tmp[0] = 'o'; return tmp; }
        if (cp == 0xF7) { tmp[0] = '/'; return tmp; }
        if (cp <= 0xFC) { tmp[0] = 'u'; return tmp; }
        if (cp == 0xFE) return "th";
        tmp[0] = 'y'; return tmp;
    }
    if (cp >= 0x100 && cp <= 0x17F) { tmp[0] = LATIN_EXT_A[cp - 0x100]; return tmp; }
    switch (cp) {
        case 0x218: tmp[0] = 'S'; return tmp;
        case 0x219: tmp[0] = 's'; return tmp;
        case 0x21A: tmp[0] = 'T'; return tmp;
        case 0x21B: tmp[0] = 't'; return tmp;
        case 0x2018: case 0x2019: case 0x00B4: tmp[0] = '\''; return tmp;
        case 0x201C: case 0x201D: tmp[0] = '"'; return tmp;
        case 0x2013: case 0x2014: tmp[0] = '-'; return tmp;
        case 0x00A0: tmp[0] = ' '; return tmp;
        default: return "";
    }
}

void asciiFold(const char *in, char *out, size_t n)
{
    if (out == nullptr || n == 0) return;
    out[0] = 0;
    if (in == nullptr) return;
    size_t o = 0;
    const uint8_t *s = reinterpret_cast<const uint8_t *>(in);
    char tmp[2];
    while (*s && o + 1 < n) {
        const uint8_t c = *s;
        if (c < 0x80) { out[o++] = (char)c; ++s; continue; }
        uint32_t cp = 0;
        int len = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; len = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 3; }
        else { ++s; continue; }
        ++s;
        bool ok = true;
        for (int i = 0; i < len; ++i) {
            if ((*s & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*s & 0x3F);
            ++s;
        }
        if (!ok) continue;
        const char *r = foldCp(cp, tmp);
        while (*r && o + 1 < n) out[o++] = *r++;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------------------
 *  Dates and times
 * ------------------------------------------------------------------------- */
/* days since 1970-01-01 (H. Hinnant's algorithm) */
static int64_t giorniDaCivile(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

time_t parseIsoUtc(const char *iso)
{
    if (iso == nullptr || strlen(iso) < 10) return 0;
    const int y  = atoi(iso);
    const int mo = atoi(iso + 5);
    const int d  = atoi(iso + 8);
    int h = 0, mi = 0, se = 0;
    if (strlen(iso) >= 16 && iso[10] == 'T') {
        h  = atoi(iso + 11);
        mi = atoi(iso + 14);
        if (strlen(iso) >= 19 && iso[16] == ':') se = atoi(iso + 17);
    }
    if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return (time_t)(giorniDaCivile(y, mo, d) * 86400LL + h * 3600 + mi * 60 + se);
}

long parseOffset(const char *s)
{
    if (s == nullptr || *s == 0) return 0;
    int segno = 1;
    if (*s == '-') { segno = -1; ++s; }
    else if (*s == '+') ++s;
    const int h = atoi(s);
    const char *c = strchr(s, ':');
    const int m = c ? atoi(c + 1) : 0;
    return segno * (h * 3600L + m * 60L);
}

void fmtGiorno(time_t t, char *out, size_t n)
{
    static const char *const gg[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(out, n, "%s %02d/%02d", gg[lt.tm_wday % 7], lt.tm_mday, lt.tm_mon + 1);
}

void fmtOra(time_t t, char *out, size_t n)
{
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

uint32_t chiaveGiorno(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return (uint32_t)(lt.tm_year + 1900) * 10000U + (uint32_t)(lt.tm_mon + 1) * 100U + (uint32_t)lt.tm_mday;
}

bool orologioValido()
{
    return time(nullptr) > 1700000000;
}

uint32_t tempoGiroMs(const char *s)
{
    if (s == nullptr || *s == 0) return 0;
    uint32_t min = 0;
    const char *p = s;
    const char *dp = strchr(s, ':');
    if (dp != nullptr) { min = (uint32_t)atoi(s); p = dp + 1; }
    const float sec = atof(p);
    if (sec <= 0.0f && min == 0) return 0;
    return min * 60000U + (uint32_t)(sec * 1000.0f + 0.5f);
}

/* ---------------------------------------------------------------------------
 *  Short display names
 * ------------------------------------------------------------------------- */
struct Traduzione { const char *en; const char *it; };
static const Traduzione NOMI[] = {
    /* national teams: shorter or more common names */
    { "Republic of Ireland", "Ireland" }, { "Czech Republic", "Czechia" },
    { "Turkiye", "Turkey" }, { "Bosnia-Herzegovina", "Bosnia" },
    { "Bosnia and Herzegovina", "Bosnia" }, { "Cote d'Ivoire", "Ivory Coast" },
    { "United States", "USA" },
    /* clubs: ESPN names shortened */
    { "Inter Milan", "Inter" }, { "Internazionale", "Inter" }, { "AC Milan", "Milan" },
    { "AS Roma", "Roma" }, { "Hellas Verona", "Verona" }, { "Bayern Munich", "Bayern" },
    { "Atletico Madrid", "Atletico Madrid" }, { "Sporting CP", "Sporting" },
};

const char *nomeItaliano(const char *eng)
{
    if (eng == nullptr) return "";
    for (const auto &t : NOMI) {
        if (strcmp(eng, t.en) == 0) return t.it;
    }
    return eng;
}

/* ---------------------------------------------------------------------------
 *  F1 team colours
 * ------------------------------------------------------------------------- */
uint32_t coloreTeam(const char *s)
{
    if (s == nullptr) return 0x8B949E;
    char low[40];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof(low); ++i) low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    if (strstr(low, "mercedes"))                       return 0x00D7B6;
    if (strstr(low, "red_bull") || strstr(low, "red bull")) return 0x4781D7;
    if (strstr(low, "ferrari"))                        return 0xED1131;
    if (strstr(low, "mclaren"))                        return 0xF47600;
    if (strstr(low, "aston"))                          return 0x229971;
    if (strstr(low, "alpine"))                         return 0x00A1E8;
    if (strstr(low, "williams"))                       return 0x1868DB;
    if (strcmp(low, "rb") == 0 || strstr(low, "racing bulls") || strstr(low, "visa")) return 0x6C98FF;
    if (strstr(low, "sauber") || strstr(low, "audi"))  return 0xF50537;
    if (strstr(low, "haas"))                           return 0x9C9FA2;
    if (strstr(low, "cadillac"))                       return 0xC9A55C;
    return 0x8B949E;
}
