#include "Notify.h"

#include <Preferences.h>
#include <stdarg.h>
#include "AppConfig.h"
#include "Settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct Notifica {
    char         testo[120];
    NotificaTipo tipo;
};

static constexpr uint8_t CODA = 8;
static Notifica          g_coda[CODA];
static uint8_t           g_testa = 0;
static uint8_t           g_conta = 0;
static uint32_t          g_inizioMs = 0;   /* when the one at the head was shown */
static SemaphoreHandle_t g_mutex = nullptr;

/* the settings (sounds, football/F1 alerts) live in Settings.cpp */
void notifyInit()
{
    if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
}

void notifySetSuoni(bool on)
{
    imp().suoni = on;
    impostazioniSalva();
}

bool notifySuoni() { return imp().suoni; }

/* screen of the next alert: set by the clients right before notifica() */
static volatile uint8_t g_prossModo = 255;
static volatile uint8_t g_prossSub  = 0;

/* wake-up from standby: written by the network tasks, read by the LVGL task */
static volatile bool    g_sveglia = false;
static volatile uint8_t g_svModo  = 255;
static volatile uint8_t g_svSub   = 0;

void notifySchermata(uint8_t modo, uint8_t sub)
{
    g_prossSub  = sub;
    g_prossModo = modo;
}

bool notifyPrendiSveglia(uint8_t *modo, uint8_t *sub)
{
    if (!g_sveglia) return false;
    *modo = g_svModo;
    *sub  = g_svSub;
    g_sveglia = false;
    g_svModo  = 255;
    return true;
}

void notifyDemoSuoni()
{
    static const SoundEvent DEMO[] = {
        SND_F1_PARTENZA, SND_F1_PASSAGGIO, SND_FISCHIO_INIZIO,
        SND_GOL, SND_ESPULSIONE, SND_FISCHIO_INTERVALLO, SND_FISCHIO_FINE,
    };
    for (SoundEvent e : DEMO) audioPlay(e);
}

void notifica(NotificaTipo tipo, SoundEvent suono, const char *fmt, ...)
{
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const uint8_t modo = g_prossModo;
    const uint8_t sub  = g_prossSub;
    g_prossModo = 255;
    /* alerts disabled from the web page: no banner, sound or wake-up */
    if ((modo == 1 && !imp().avvisiCalcio) || (modo == 0 && !imp().avvisiF1)) {
        Serial.printf("[ALERT disabled] %s\n", buf);
        return;
    }
    Serial.printf("[ALERT] %s\n", buf);

    if (g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (g_conta == CODA) {          /* queue full: drop the oldest */
            g_testa = (g_testa + 1) % CODA;
            g_conta--;
            g_inizioMs = millis();
        }
        const uint8_t idx = (g_testa + g_conta) % CODA;
        snprintf(g_coda[idx].testo, sizeof(g_coda[idx].testo), "%s", buf);
        g_coda[idx].tipo = tipo;
        if (g_conta == 0) g_inizioMs = millis();
        g_conta++;
        xSemaphoreGive(g_mutex);
    }
    if (suono != SND_COUNT && imp().suoni) audioPlay(suono);
    g_svSub   = sub;
    g_svModo  = modo;
    g_sveglia = true;
}

bool notificaCorrente(char *testo, size_t n, NotificaTipo *tipo)
{
    bool ok = false;
    if (g_mutex == nullptr || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    /* with other notifications queued each one stays on screen for less time */
    const uint32_t durata = g_conta > 1 ? NOTIFICA_DURATA_MS / 2 : NOTIFICA_DURATA_MS;
    while (g_conta > 0 && millis() - g_inizioMs >= durata) {
        g_testa = (g_testa + 1) % CODA;
        g_conta--;
        g_inizioMs = millis();
    }
    if (g_conta > 0) {
        snprintf(testo, n, "%s", g_coda[g_testa].testo);
        if (tipo != nullptr) *tipo = g_coda[g_testa].tipo;
        ok = true;
    }
    xSemaphoreGive(g_mutex);
    return ok;
}
