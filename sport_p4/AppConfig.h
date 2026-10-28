/* ============================================================================
 *  sport_p4 - configuration
 *  Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (1024x600, EK79007, GT911)
 *
 *  The hardware section is identical to bitaxe_dashboard_p4 / meteo_p4: it is the
 *  layer already validated on this board, do not touch it.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

/* ---- Screen -------------------------------------------------------------- */
static constexpr uint16_t SCREEN_WIDTH  = 1024;
static constexpr uint16_t SCREEN_HEIGHT = 600;

/* ---- WiFi ---------------------------------------------------------------- */
/* Enter your WiFi network here (mind upper/lower case). */
static constexpr char WIFI_SSID[]     = "YOUR_NETWORK";
static constexpr char WIFI_PASSWORD[] = "YOUR_PASSWORD";

/* Italian time zone with automatic daylight saving change */
static constexpr char TZ_ITALIA[] = "CET-1CEST,M3.5.0,M10.5.0/3";

static constexpr uint32_t WEB_HTTP_TIMEOUT_MS = 12000;

/* ---- Football (ESPN, no API key) ------------------------------------------ */
static constexpr uint8_t CALCIO_COMP_COUNT = 6;
static const char *const CALCIO_SLUG[CALCIO_COMP_COUNT] = {
    "ita.1", "uefa.nations", "uefa.champions", "uefa.europa", "uefa.euro", "fifa.world",
};
static const char *const CALCIO_TAB[CALCIO_COMP_COUNT] = {
    "SERIE A", "NATIONS", "CHAMPIONS", "EUROPA L.", "EUROS", "WORLD CUP",
};
static const char *const CALCIO_NOME[CALCIO_COMP_COUNT] = {
    "Serie A", "UEFA Nations League", "UEFA Champions League",
    "UEFA Europa League", "UEFA European Championship", "FIFA World Cup",
};
/* Window of the matches shown: from 2 days ago to 7 days ahead
 * (a whole week also covers the international breaks).
 * If the window is empty (Euros/World Cup out of season) the matchday
 * ESPN considers "current" is shown: usually the last one played.          */
static constexpr int8_t   CALCIO_GIORNI_PRIMA       = 2;
static constexpr int8_t   CALCIO_GIORNI_DOPO        = 7;
static constexpr uint32_t CALCIO_LISTA_VISIBILE_MS  = 600000;   /* 10 min  */
static constexpr uint32_t CALCIO_LISTA_ALTRE_MS     = 3600000;  /* 60 min  */
static constexpr uint32_t CALCIO_LIVE_IDLE_MS       = 60000;    /* 60 s    */
static constexpr uint32_t CALCIO_LIVE_ATTIVO_MS     = 20000;    /* 20 s with matches in progress */
static constexpr uint32_t CALCIO_CLASSIFICA_MS      = 1800000;  /* 30 min  */
/* for how many ms the score stays highlighted after a goal */
static constexpr uint32_t CALCIO_GOL_EVIDENZA_MS    = 120000;

/* ---- Formula 1 ------------------------------------------------------------ */
/* Jolpica (formerly Ergast): standings, last race, calendar.
 * livetiming.formula1.com (SignalR Core, no login): real time.              */
static constexpr uint32_t F1_JOLPICA_MS        = 1800000;  /* 30 min          */
static constexpr uint32_t F1_JOLPICA_POSTGARA_MS = 600000; /* 10 min after a race */
static constexpr uint32_t F1_LIVE_CONTROLLO_MS = 600000;   /* outside sessions: every 10 min */
static constexpr uint32_t F1_LIVE_RETRY_MS     = 30000;    /* in session: retry every 30 s */
static constexpr uint32_t F1_LIVE_MARGINE_S    = 1800;     /* session window +-30 min */
static constexpr uint32_t F1_DELTA_POS_MS      = 15000;    /* overtaking arrows visible 15 s */

/* ---- Notifications -------------------------------------------------------- */
static constexpr uint32_t NOTIFICA_DURATA_MS = 20000;

/* ---- Screen saver -------------------------------------------------------- */
/* 0 = screen always on.
 * Wake-up: a tap (it only turns the screen back on, it presses nothing) or an
 * alert (goal, whistle, flag...), which also brings up the right screen.      */
static constexpr uint32_t SCREEN_TIMEOUT_MS = 120000;   /* standby after 2 minutes */

/* ---- Display: pins and timing (from the already validated sketch) -------- */
static constexpr int  LCD_RST_IO      = 33;
static constexpr int  LCD_BL_IO       = 32;
static constexpr bool LCD_BL_ON_LEVEL = 0;      /* backlight ACTIVE LOW */
static constexpr int  TOUCH_SDA_IO    = 7;
static constexpr int  TOUCH_SCL_IO    = 8;
static constexpr int  TOUCH_I2C_NUM   = 1;
static constexpr int  TOUCH_RST_IO    = -1;
static constexpr int  TOUCH_INT_IO    = -1;

static constexpr int LCD_DSI_PHY_LDO_ID     = 3;
static constexpr int LCD_DSI_LANE_NUM       = 2;
static constexpr int LCD_DSI_LANE_RATE_MBPS = 1000;
static constexpr int LCD_DPI_CLOCK_MHZ      = 52;
static constexpr int LCD_HSYNC_PULSE_WIDTH  = 10;
static constexpr int LCD_HSYNC_BACK_PORCH   = 160;
static constexpr int LCD_HSYNC_FRONT_PORCH  = 160;
static constexpr int LCD_VSYNC_PULSE_WIDTH  = 1;
static constexpr int LCD_VSYNC_BACK_PORCH   = 23;
static constexpr int LCD_VSYNC_FRONT_PORCH  = 12;

/* ---- Orientation --------------------------------------------------------- */
/* The panel is mounted upside down: the image is rotated by 180 degrees in
 * software. The touch, therefore, is left as it is.                          */
static constexpr bool DISPLAY_ROTATE_180 = true;
static constexpr bool TOUCH_MIRROR_X     = false;
static constexpr bool TOUCH_MIRROR_Y     = false;
static constexpr bool TOUCH_SWAP_XY      = false;

/* 1 = show the touch coordinates in the header (diagnostics) */
#define SHOW_TOUCH_DEBUG 0

/* ---- Audio (ES8311 codec + supplied speaker) ----------------------------- */
/* 75 = 0 dB on the DAC volume register. Above that it clips. */
static constexpr uint8_t AUDIO_VOLUME     = 75;
static constexpr bool    AUDIO_TOUCH_BEEP = true;
