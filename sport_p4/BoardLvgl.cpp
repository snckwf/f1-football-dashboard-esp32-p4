#include "BoardLvgl.h"

#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "AppConfig.h"
#include "Audio.h"
#include "Ui.h"

using namespace esp_panel::drivers;

static LCD                *g_lcd       = nullptr;
static TouchGT911         *g_touch     = nullptr;
static BusI2C             *g_touchBus  = nullptr;
static BacklightPWM_LEDC  *g_backlight = nullptr;
static SemaphoreHandle_t   g_lvglMutex = nullptr;
static TaskHandle_t        g_lvglTask  = nullptr;
static bool     g_lvglReady = false;
static bool     g_useDisplayFrameBuffers = false;
static uint8_t  g_nextFrameBufferIndex = 0;
static void    *g_drawBuf1 = nullptr;
static void    *g_drawBuf2 = nullptr;
static char     g_touchStatus[72] = "Touch: NO";
static uint32_t g_lastActivityMs = 0;
static bool     g_screenOn = true;
/* adjustable from the web page (sport_p4) */
static volatile uint8_t  g_luminosita = 100;
static volatile uint32_t g_timeoutMs  = SCREEN_TIMEOUT_MS;

static uint32_t lvTickMillis() { return millis(); }

/* The panel is mounted upside down: we rotate the whole frame before sending it
 * to the controller. It is a reversed linear copy, cheap, and it does not need
 * LVGL software rotation (which would also transform the touch). */
static void rotateFullFrame180(const uint8_t *src, uint8_t *dst)
{
    const uint16_t *src16 = reinterpret_cast<const uint16_t *>(src);
    uint16_t *dst16 = reinterpret_cast<uint16_t *>(dst);
    const size_t pixelCount = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT;
    for (size_t i = 0; i < pixelCount; ++i) {
        dst16[pixelCount - 1 - i] = src16[i];
    }
}

static void setScreenOn(bool on)
{
    if (g_screenOn == on) return;
    g_screenOn = on;
    if (g_backlight != nullptr) g_backlight->setBrightness(on ? g_luminosita : 0);
    if (g_lcd != nullptr && g_lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(
            LCD::BasicBusSpecification::FUNC_DISPLAY_ON_OFF)) {
        g_lcd->setDisplayOnOff(on);
    }
}

static void lvFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap)
{
    if (g_lcd != nullptr) {
        if (DISPLAY_ROTATE_180) {
            if (g_useDisplayFrameBuffers) {
                uint8_t *target = static_cast<uint8_t *>(g_lcd->getFrameBufferByIndex(g_nextFrameBufferIndex));
                if (target != nullptr) {
                    rotateFullFrame180(pxMap, target);
                    if (lv_display_flush_is_last(disp)) {
                        ulTaskNotifyValueClear(nullptr, ULONG_MAX);
                        g_lcd->switchFrameBufferTo(target);
                        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                        g_nextFrameBufferIndex = (g_nextFrameBufferIndex + 1) % 2;
                    }
                }
            } else if (g_drawBuf2 != nullptr) {
                rotateFullFrame180(pxMap, static_cast<uint8_t *>(g_drawBuf2));
                g_lcd->drawBitmap(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, static_cast<uint8_t *>(g_drawBuf2), -1);
            }
        } else if (g_useDisplayFrameBuffers) {
            if (lv_display_flush_is_last(disp)) {
                ulTaskNotifyValueClear(nullptr, ULONG_MAX);
                g_lcd->switchFrameBufferTo(pxMap);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            }
        } else {
            g_lcd->drawBitmap(area->x1, area->y1,
                              area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, pxMap, -1);
        }
    }
    lv_display_flush_ready(disp);
}

static IRAM_ATTR bool lcdRefreshFinishCb(void *userData)
{
    TaskHandle_t task = static_cast<TaskHandle_t>(userData);
    if (task == nullptr) return false;
    BaseType_t needYield = pdFALSE;
    vTaskNotifyGiveFromISR(task, &needYield);
    return needYield == pdTRUE;
}

/* true while the finger stays down: used to play the beep only once */
static bool g_fingerDown = false;

static void lvTouchReadCb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    if (g_touch == nullptr) return;

    TouchPoint points[1];
    if (g_touch->readPoints(points, 1, 0) <= 0) {
        g_fingerDown = false;
        return;
    }

    int32_t x = points[0].x;
    int32_t y = points[0].y;

    if (TOUCH_SWAP_XY)  { const int32_t t = x; x = y; y = t; }
    if (TOUCH_MIRROR_X) x = SCREEN_WIDTH  - 1 - x;
    if (TOUCH_MIRROR_Y) y = SCREEN_HEIGHT - 1 - y;
    x = constrain(x, 0, SCREEN_WIDTH  - 1);
    y = constrain(y, 0, SCREEN_HEIGHT - 1);

    /* First touch with the screen off: it only wakes up, it does not navigate */
    if (!g_screenOn) {
        setScreenOn(true);
        g_lastActivityMs = millis();
        return;
    }
    g_lastActivityMs = millis();

    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = (int16_t)x;
    data->point.y = (int16_t)y;

    /* beep only on the finger's falling edge, not for the whole press */
    if (AUDIO_TOUCH_BEEP && !g_fingerDown) audioPlay(SND_TOUCH);
    g_fingerDown = true;

#if SHOW_TOUCH_DEBUG
    char t[48];
    snprintf(t, sizeof(t), "T %ld,%ld", (long)x, (long)y);
    uiSetTouchDebug(t);
#endif
}

static void lvglTask(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t waitMs = 10;
        if (g_lvglMutex != nullptr && xSemaphoreTake(g_lvglMutex, portMAX_DELAY) == pdTRUE) {
            waitMs = lv_timer_handler();
            if (g_screenOn && g_timeoutMs > 0 &&
                (millis() - g_lastActivityMs) >= g_timeoutMs) {
                setScreenOn(false);
            }
            xSemaphoreGive(g_lvglMutex);
        }
        waitMs = constrain(waitMs, 20U, 50U);
        vTaskDelay(pdMS_TO_TICKS(waitMs));
    }
}

static bool initBacklight()
{
    g_backlight = new BacklightPWM_LEDC(LCD_BL_IO, LCD_BL_ON_LEVEL);
    if (g_backlight == nullptr || !g_backlight->begin()) {
        Serial.println("Backlight init failed");
        return false;
    }
    g_backlight->setBrightness(g_luminosita);
    return true;
}

static bool initLcd()
{
    BusDSI *bus = new BusDSI(
        LCD_DSI_LANE_NUM, LCD_DSI_LANE_RATE_MBPS, LCD_DPI_CLOCK_MHZ,
        ESP_PANEL_LCD_COLOR_BITS_RGB565, SCREEN_WIDTH, SCREEN_HEIGHT,
        LCD_HSYNC_PULSE_WIDTH, LCD_HSYNC_BACK_PORCH, LCD_HSYNC_FRONT_PORCH,
        LCD_VSYNC_PULSE_WIDTH, LCD_VSYNC_BACK_PORCH, LCD_VSYNC_FRONT_PORCH,
        LCD_DSI_PHY_LDO_ID
    );
    if (bus == nullptr) return false;

    g_lcd = new LCD_EK79007(bus, SCREEN_WIDTH, SCREEN_HEIGHT,
                            ESP_PANEL_LCD_COLOR_BITS_RGB565, LCD_RST_IO);
    if (g_lcd == nullptr) return false;

    if (!g_lcd->configFrameBufferNumber(2)) {
        Serial.println("Double framebuffer not available: using the LVGL buffers");
    }
    if (!g_lcd->begin()) {
        Serial.println("LCD begin failed");
        return false;
    }
    if (g_lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(
            LCD::BasicBusSpecification::FUNC_DISPLAY_ON_OFF)) {
        g_lcd->setDisplayOnOff(true);
    }
    return true;
}

static bool beginTouchAtAddress(uint8_t address)
{
    esp_lcd_panel_io_i2c_config_t touchIo = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG_WITH_ADDR(address);

    BusI2C::Config cfg = {};
    cfg.host_id = TOUCH_I2C_NUM;
    cfg.host = BusI2C::HostPartialConfig{
        .sda_io_num = TOUCH_SDA_IO,
        .scl_io_num = TOUCH_SCL_IO,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .clk_speed = 400000,
    };
    cfg.control_panel = touchIo;

    BusI2C *bus = new BusI2C(cfg);
    if (bus == nullptr) return false;

    TouchGT911 *touch = new TouchGT911(bus, SCREEN_WIDTH, SCREEN_HEIGHT, TOUCH_RST_IO, TOUCH_INT_IO);
    if (touch == nullptr) { delete bus; return false; }

    touch->configResetActiveLevel(0);
    touch->configInterruptActiveLevel(0);

    if (!touch->begin()) {
        Serial.printf("GT911 begin failed at 0x%02X\n", address);
        snprintf(g_touchStatus, sizeof(g_touchStatus), "Touch: fail 0x%02X", address);
        delete touch;
        delete bus;
        return false;
    }

    /* We apply the transformations ourselves in the read callback, not here */
    touch->swapXY(false);
    touch->mirrorX(false);
    touch->mirrorY(false);

    g_touchBus = bus;
    g_touch = touch;
    Serial.printf("GT911 ready at 0x%02X\n", address);
    snprintf(g_touchStatus, sizeof(g_touchStatus), "Touch OK 0x%02X", address);
    return true;
}

static bool initTouch()
{
    if (beginTouchAtAddress(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS)) return true;
    if (beginTouchAtAddress(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)) return true;
    Serial.println("GT911 not found: continuing without touch");
    snprintf(g_touchStatus, sizeof(g_touchStatus), "Touch: NO");
    g_touch = nullptr;
    g_touchBus = nullptr;
    return false;
}

static void *allocLvglBuffer(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}

bool initBoardLvgl()
{
    Serial.println("Init display Waveshare ESP32-P4 7B");

    initBacklight();
    if (!initLcd()) return false;
    initTouch();

    lv_init();
    lv_tick_set_cb(lvTickMillis);

    g_lvglMutex = xSemaphoreCreateMutex();
    if (g_lvglMutex == nullptr) return false;

    lv_display_t *display = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (display == nullptr) return false;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvFlushCb);
    lv_timer_set_period(lv_display_get_refr_timer(display), 50);

    const size_t frameBytes = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(lv_color_t);
    void *fb0 = g_lcd->getFrameBufferByIndex(0);
    void *fb1 = g_lcd->getFrameBufferByIndex(1);
    g_useDisplayFrameBuffers = (fb0 != nullptr && fb1 != nullptr);

    g_drawBuf1 = allocLvglBuffer(frameBytes);
    if (g_drawBuf1 == nullptr) {
        Serial.println("LVGL buffer allocation failed");
        return false;
    }
    if (DISPLAY_ROTATE_180 && !g_useDisplayFrameBuffers) {
        g_drawBuf2 = allocLvglBuffer(frameBytes);
        if (g_drawBuf2 == nullptr) {
            Serial.println("Rotation buffer allocation failed");
            return false;
        }
    }
    lv_display_set_buffers(display, g_drawBuf1, nullptr, frameBytes, LV_DISPLAY_RENDER_MODE_FULL);

    if (g_touch != nullptr) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev, display);
        lv_indev_set_read_cb(indev, lvTouchReadCb);
    }

    g_lvglReady = true;
    g_lastActivityMs = millis();
    if (xTaskCreatePinnedToCore(lvglTask, "lvgl", 8192, nullptr, 3, &g_lvglTask, ARDUINO_RUNNING_CORE) != pdPASS) {
        Serial.println("LVGL task creation failed");
        return false;
    }
    if (g_useDisplayFrameBuffers) {
        g_lcd->attachRefreshFinishCallback(lcdRefreshFinishCb, g_lvglTask);
    }
    return true;
}

bool lvglLock(int timeoutMs)
{
    if (g_lvglMutex == nullptr) return false;
    const TickType_t ticks = timeoutMs < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTake(g_lvglMutex, ticks) == pdTRUE;
}

void lvglUnlock()
{
    if (g_lvglMutex != nullptr) xSemaphoreGive(g_lvglMutex);
}

bool        isLvglReady()       { return g_lvglReady; }
bool        boardTouchReady()   { return g_touch != nullptr; }
const char *boardTouchStatus()  { return g_touchStatus; }
bool        boardIsScreenOn()   { return g_screenOn; }

void boardResetScreenTimeout()
{
    g_lastActivityMs = millis();
    if (!g_screenOn) setScreenOn(true);
}

/* call with the LVGL lock taken (or from the LVGL task) */
void boardSetBrightness(uint8_t percent)
{
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    g_luminosita = percent;
    if (g_screenOn && g_backlight != nullptr) g_backlight->setBrightness(percent);
}

uint8_t boardGetBrightness() { return g_luminosita; }

void boardSetScreenTimeoutMs(uint32_t ms)
{
    g_timeoutMs = ms;
    g_lastActivityMs = millis();
}

uint32_t boardGetScreenTimeoutMs() { return g_timeoutMs; }
