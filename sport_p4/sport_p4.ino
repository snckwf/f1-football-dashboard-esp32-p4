/* ============================================================================
 *  sport_p4
 *  Formula 1 and football on the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 *  F1        LIVE (with tyres), real-time circuit MAP, DRIVERS, CONSTRUCTORS,
 *            LAST RACE  -  sources: livetiming.formula1.com + api.jolpi.ca
 *  FOOTBALL  Serie A (with standings), Nations League, Champions League,
 *            Europa League, Euros, World Cup  -  source: ESPN
 *            Real-time results: alert + sound for goals, red cards and whistles.
 *            Tap a match: commentary, statistics and line-ups.
 *
 *  No API key, no account.
 *
 *  Configuration from the browser: http://sport-p4.local (volume, brightness,
 *  standby, alerts, competitions shown).
 *
 *  The hardware layer (BoardLvgl + Audio + lv_conf + the two panel conf
 *  files) is taken unchanged from bitaxe_dashboard_p4 / meteo_p4.
 *
 *  Required libraries (Arduino IDE Library Manager):
 *    - ESP32_Display_Panel   (espressif)
 *    - ESP32_IO_Expander     (dependency of the previous one)
 *    - esp-lib-utils         (dependency of the previous one)
 *    - lvgl 9.x
 *    - ArduinoJson 7.x
 *
 *  Board: "ESP32P4 Dev Module", PSRAM Enabled, Flash 16MB,
 *  Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)".
 * ========================================================================== */
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_heap_caps.h"

#include "AppConfig.h"
#include "Audio.h"
#include "BoardLvgl.h"
#include "Clients.h"
#include "DataModel.h"
#include "Notify.h"
#include "Ui.h"
#include "Settings.h"

/* The RSSI goes through an RPC call to the C6 (ESP-Hosted): if the C6 is
 * busy the reply is late and the call blocks for seconds. So it is read
 * rarely and the last value is kept. */
static void aggiornaStatoWifi()
{
    static int8_t   rssi = 0;
    static uint32_t ultimoRssi = 0;
    char text[80];
    if (WiFi.status() == WL_CONNECTED) {
        if (ultimoRssi == 0 || millis() - ultimoRssi >= 60000) {
            rssi = (int8_t)WiFi.RSSI();
            ultimoRssi = millis() | 1;
        }
        const IPAddress ip = WiFi.localIP();
        if (rssi != 0) snprintf(text, sizeof(text), "%s  %u.%u.%u.%u  %d dBm", WIFI_SSID, ip[0], ip[1], ip[2], ip[3], rssi);
        else           snprintf(text, sizeof(text), "%s  %u.%u.%u.%u", WIFI_SSID, ip[0], ip[1], ip[2], ip[3]);
    } else {
        snprintf(text, sizeof(text), "connecting to %s", WIFI_SSID);
    }
    setWifiStatusText(text);
}

static void connettiWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("Connecting to %s\n", WIFI_SSID);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        setWifiStatusText("connecting WiFi...");
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    aggiornaStatoWifi();
    Serial.printf("WiFi: %s\n", getWifiStatusText());

    /* local time with automatic daylight saving (also needed for the ESPN dates) */
    configTzTime(TZ_ITALIA, "it.pool.ntp.org", "pool.ntp.org", "time.google.com");
    if (WiFi.status() == WL_CONNECTED) {
        struct tm t;
        for (uint8_t i = 0; i < 20 && !getLocalTime(&t, 500); ++i) delay(100);
    }
}

/* keeps an eye on the WiFi and updates the text in the header */
static void wifiTask(void *arg)
{
    (void)arg;
    while (true) {
        aggiornaStatoWifi();
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== sport_p4 ===");

    initDataModel();
    impostazioniCarica();
    notifyInit();

    if (initBoardLvgl()) {
        const unsigned primaUi = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (lvglLock(-1)) {
            uiCreate();
            lvglUnlock();
        }
        Serial.printf("[MEM] free internal RAM: %u before the UI, %u after\n", primaUi,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        Serial.printf("Touch: %s\n", boardTouchStatus());
    } else {
        Serial.println("Display/LVGL init failed");
    }

    /* Audio: if the codec does not answer the sketch goes on muted, without blocking */
    audioSetVolume(imp().volume);
    if (audioBegin()) {
        audioPlay(SND_STARTUP);
    } else {
        Serial.println("Audio not available, on-screen alerts only");
    }

    impostazioniApplica();       /* saved brightness and standby */

    connettiWifi();

    xTaskCreatePinnedToCore(wifiTask,   "wifi",   4096,  nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(calcioTask, "calcio", 16384, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(f1Task,     "f1",     12288, nullptr, 1, nullptr, 0);
    /* 32 KB: WebSocket TLS + HTTPS download of the circuit outline */
    xTaskCreatePinnedToCore(f1LiveTask, "f1live", 32768, nullptr, 2, nullptr, 0);
    webBegin();
}

void loop()
{
    /* serial commands (115200): d = sound test, m = alert sounds on/off,
     * g = fake goal in Nations League, f = fake F1 alert (wake-up test) */
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'g') {
            notifySchermata(1, 1);
            notifica(NOT_GOL, SND_GOL, "TEST: GOAL Italy!   Italy 1-0 Norway   23'   NATIONS");
        } else if (c == 'f') {
            notifySchermata(0, 0);
            notifica(NOT_F1, SND_F1_PASSAGGIO, "TEST: F1  New leader");
        } else if (c == 'd') {
            Serial.println("[AUDIO] sound test");
            notifyDemoSuoni();
        } else if (c == 'm') {
            notifySetSuoni(!notifySuoni());
            Serial.printf("[AUDIO] alert sounds %s\n", notifySuoni() ? "ON" : "OFF");
        }
    }

    static uint32_t ultimo = 0;
    if (millis() - ultimo >= 60000) {
        ultimo = millis();
        Serial.printf("[MEM] free internal %u, free PSRAM %u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    delay(1000);
}
