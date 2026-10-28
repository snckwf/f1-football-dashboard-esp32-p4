/* ============================================================================
 *  Audio - implementation
 *
 *  Chain: ESP32-P4  --I2S-->  ES8311  -->  amplifier  -->  speaker
 *  The codec sits on the same I2C bus as the touch (GPIO 7/8, port 1) at 0x18.
 *
 *  The register sequence comes from the official Espressif esp_codec_dev driver
 *  (device/es8311/es8311.c), not from reconstructions: reset, clock manager,
 *  16-bit I2S format, DAC power-up, volume.
 * ========================================================================== */
#include "Audio.h"
#include "AppConfig.h"

#include <math.h>
#include "driver/i2s_std.h"
#include "driver/i2c.h"      /* ESP32_Display_Panel opens the bus with the legacy driver */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ---- audio parameters ----------------------------------------------------- */
static constexpr int      AUDIO_RATE     = 32000;
static constexpr int      AUDIO_MCLK_DIV = 256;          /* MCLK = 8.192 MHz  */
static constexpr uint8_t  ES8311_ADDR    = 0x18;
static constexpr int      I2S_MCLK_IO    = 13;
static constexpr int      I2S_BCLK_IO    = 12;
static constexpr int      I2S_LRCK_IO    = 10;
static constexpr int      I2S_DOUT_IO    = 9;
static constexpr int      PA_ENABLE_IO   = 53;
static constexpr size_t   CHUNK_FRAMES   = 256;

/* Signal amplitude on the 16 available bits (full scale 32767).
 * At 22000, with the DAC volume at positive gain, the output stage
 * clipped. At 20000 there are about 4 dB of headroom and, keeping the DAC at 0 dB,
 * the chain stays clean. Above 22000 it distorts again. */
static constexpr int16_t  WAVE_AMPLITUDE = 20000;

/* One-pole low-pass filter on the output. Besides removing the last harshness,
 * it mimics the small TV speakers the 8-bit consoles actually played
 * through: the "round" square wave we remember never had sharp edges.
 * Raising the frequency makes the sound sharper again. */
static constexpr float LOWPASS_HZ = 3200.0f;

/* ---- ES8311 registers (names from the datasheet) -------------------------- */
#define R_RESET   0x00
#define R_CLK1    0x01
#define R_CLK2    0x02
#define R_CLK3    0x03
#define R_CLK4    0x04
#define R_CLK5    0x05
#define R_CLK6    0x06
#define R_CLK7    0x07
#define R_CLK8    0x08
#define R_SDPIN   0x09
#define R_SDPOUT  0x0A
#define R_SYS0D   0x0D
#define R_SYS0E   0x0E
#define R_SYS12   0x12
#define R_SYS14   0x14
#define R_ADC15   0x15
#define R_ADC17   0x17
#define R_DACMUTE 0x31
#define R_DACVOL  0x32
#define R_DAC37   0x37
#define R_GP45    0x45
#define R_CHIPID1 0xFD
#define R_CHIPID2 0xFE

/* ---- state ---------------------------------------------------------------- */
static i2s_chan_handle_t s_tx = nullptr;
static bool      s_ready = false;
static uint8_t   s_volume = 75;
static QueueHandle_t s_queue = nullptr;
static char      s_status[80] = "audio not initialised";

/* ============================================================ I2C access ====
 * The bus is already opened by ESP32_Display_Panel for the GT911 touch: the library
 * uses the legacy driver (i2c_param_config + i2c_driver_install), so we only
 * run transactions on the same port, without reinitialising anything. */
static bool i2cWrite(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device((i2c_port_t)TOUCH_I2C_NUM, ES8311_ADDR,
                                      buf, 2, pdMS_TO_TICKS(200)) == ESP_OK;
}

static bool i2cRead(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device((i2c_port_t)TOUCH_I2C_NUM, ES8311_ADDR,
                                        &reg, 1, value, 1, pdMS_TO_TICKS(200)) == ESP_OK;
}

/* Reads the chip ID: 0x83 / 0x11. It is also the test that tells whether the bus works. */
static bool codecProbe()
{
    uint8_t id1 = 0, id2 = 0;
    if (!i2cRead(R_CHIPID1, &id1)) return false;
    if (!i2cRead(R_CHIPID2, &id2)) return false;
    return (id1 == 0x83 && id2 == 0x11);
}

static bool openCodecBus()
{
    if (codecProbe()) {
        snprintf(s_status, sizeof(s_status), "ES8311 found at 0x%02X on I2C%d",
                 ES8311_ADDR, TOUCH_I2C_NUM);
        return true;
    }
    snprintf(s_status, sizeof(s_status), "ES8311 not answering at 0x%02X on I2C%d",
             ES8311_ADDR, TOUCH_I2C_NUM);
    return false;
}

/* ============================================================ init codec ==== */
static void codecSetVolumeReg(uint8_t percent)
{
    /* REG32 is a logarithmic scale: 0x00 = mute, each step is 0.5 dB,
     * 0xBF (191) = 0 dB, 0xFF (255) = about +32 dB.
     *
     * The previous version mapped 0..100% onto 0..0xBF: at 70% it ended at 133,
     * i.e. 58 steps below zero = -29 dB. Added to the -11 dB of the sample
     * amplitude that made -40 dB, hence the barely audible sound.
     *
     * Now the scale covers the whole register: 75% = 0 dB, 100% = maximum. */
    if (percent > 100) percent = 100;
    const uint8_t reg = (uint8_t)(((uint16_t)percent * 0xFF) / 100);
    i2cWrite(R_DACVOL, reg);
}

static bool codecInit()
{
    /* reset */
    if (!i2cWrite(R_RESET, 0x1F)) return false;
    delay(20);
    i2cWrite(R_RESET, 0x00);
    delay(10);

    /* slave mode, MCLK from the ESP32, not inverted */
    i2cWrite(R_RESET, 0x80);
    i2cWrite(R_CLK1,  0x3F);

    /* --- clock dividers for MCLK 8.192 MHz and 32 kHz ----------------------
     * Row of the official table: {8192000, 32000, pre_div 1, pre_multi 1,
     * adc_div 1, dac_div 1, fs_mode 0, lrck_h 0x00, lrck_l 0xFF, bclk_div 4,
     * adc_osr 0x10, dac_osr 0x10}. I apply the same formulas as the driver. */
    uint8_t r;
    if (i2cRead(R_CLK2, &r)) i2cWrite(R_CLK2, (uint8_t)((r & 0x07) | (0 << 5) | (0 << 3)));
    i2cWrite(R_CLK5, 0x00);                                   /* (adc_div-1)<<4 | (dac_div-1) */
    if (i2cRead(R_CLK3, &r)) i2cWrite(R_CLK3, (uint8_t)((r & 0x80) | (0 << 6) | 0x10));
    if (i2cRead(R_CLK4, &r)) i2cWrite(R_CLK4, (uint8_t)((r & 0x80) | 0x10));
    if (i2cRead(R_CLK7, &r)) i2cWrite(R_CLK7, (uint8_t)((r & 0xC0) | 0x00));
    i2cWrite(R_CLK8, 0xFF);
    if (i2cRead(R_CLK6, &r)) i2cWrite(R_CLK6, (uint8_t)((r & 0xE0) | (4 - 1)));

    /* standard I2S format, 16 bit, output not muted */
    i2cWrite(R_SDPIN,  0x0C);
    i2cWrite(R_SDPOUT, 0x0C);

    /* power-up */
    i2cWrite(R_ADC17,   0xBF);
    i2cWrite(R_SYS0E,   0x02);
    i2cWrite(R_SYS12,   0x00);   /* enable the DAC */
    i2cWrite(R_SYS14,   0x1A);
    i2cWrite(R_SYS0D,   0x01);
    i2cWrite(R_ADC15,   0x40);
    i2cWrite(R_DAC37,   0x08);
    i2cWrite(R_GP45,    0x00);
    i2cWrite(R_DACMUTE, 0x00);   /* unmute the DAC */
    codecSetVolumeReg(s_volume);
    return true;
}

static bool i2sInit()
{
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 4;
    chanCfg.dma_frame_num = CHUNK_FRAMES;
    chanCfg.auto_clear    = true;
    if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std = {};
    std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK_IO;
    std.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK_IO;
    std.gpio_cfg.ws   = (gpio_num_t)I2S_LRCK_IO;
    std.gpio_cfg.dout = (gpio_num_t)I2S_DOUT_IO;
    std.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std.gpio_cfg.invert_flags.mclk_inv = false;
    std.gpio_cfg.invert_flags.bclk_inv = false;
    std.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return false;
    return i2s_channel_enable(s_tx) == ESP_OK;
}

/* ============================================================ synthesis ===== */
/* notes in Hz, fourth and fifth octave: the scale 8-bit games used */
#define N_REST 0
#define N_C3   131
#define N_E3   165
#define N_G3   196
#define N_A3   220
#define N_C4   262
#define N_D4   294
#define N_E4   330
#define N_F4   349
#define N_G4   392
#define N_A4   440
#define N_B4   494
#define N_C5   523
#define N_D5   587
#define N_E5   659
#define N_F5   698
#define N_G5   784
#define N_A5   880
#define N_B5   988
#define N_C6  1047

enum NoteFx : uint8_t { FX_NONE = 0, FX_SLIDE_DOWN, FX_SLIDE_UP, FX_VIBRATO };

struct Note {
    uint16_t freq;    /* Hz, 0 = rest                                        */
    uint16_t ms;      /* duration                                            */
    uint8_t  duty;    /* brightness: 12 = pure sine (the softest),
                       * 25 = with second harmonic, 50 = third as well       */
    uint8_t  fx;
};

/* --- touch: a soft "ping", pure sine --- */
static const Note J_TOUCH[] = {
    { N_A4, 55, 12, FX_NONE },
};

/* --- miner down: warm descent fading out --- */
static const Note J_MINER_DOWN[] = {
    { N_G4, 110, 25, FX_NONE },
    { N_E4, 110, 25, FX_NONE },
    { N_C4, 130, 25, FX_NONE },
    { N_G3, 190, 12, FX_NONE },
    { N_E3, 420, 12, FX_SLIDE_DOWN },
};

/* --- critical temperature: two alternating tones, insistent but not shrill --- */
static const Note J_TEMP[] = {
    { N_A4, 130, 25, FX_NONE }, { N_E4, 130, 25, FX_NONE }, { N_REST, 60, 12, FX_NONE },
    { N_A4, 130, 25, FX_NONE }, { N_E4, 130, 25, FX_NONE }, { N_REST, 60, 12, FX_NONE },
    { N_A4, 150, 25, FX_NONE }, { N_E4, 260, 25, FX_NONE },
};

/* --- new best share: rising arpeggio, gentle --- */
static const Note J_BEST[] = {
    { N_C4,  70, 12, FX_NONE },
    { N_E4,  70, 12, FX_NONE },
    { N_G4,  70, 12, FX_NONE },
    { N_C5, 300, 25, FX_VIBRATO },
};

/* --- block found: run-up and fanfare, the moment you have always waited for --- */
static const Note J_BLOCK[] = {
    { N_C4,  90, 12, FX_NONE }, { N_E4,  90, 12, FX_NONE },
    { N_G4,  90, 12, FX_NONE }, { N_C5,  90, 12, FX_NONE },
    { N_E5,  90, 25, FX_NONE }, { N_G5,  90, 25, FX_NONE },
    { N_C6, 260, 25, FX_NONE },
    { N_REST, 90, 12, FX_NONE },
    { N_G5, 170, 25, FX_NONE },
    { N_E5, 170, 25, FX_NONE },
    { N_C5, 170, 25, FX_NONE },
    { N_G4, 150, 25, FX_NONE },
    { N_C5, 900, 25, FX_VIBRATO },
};

/* --- startup: short arpeggio --- */
static const Note J_STARTUP[] = {
    { N_C4, 120, 12, FX_NONE },
    { N_E4, 120, 12, FX_NONE },
    { N_G4, 120, 12, FX_NONE },
    { N_C5, 380, 25, FX_VIBRATO },
};

struct Jingle { const Note *notes; uint8_t count; };
static const Jingle JINGLES[SND_PRIMO_SINTETIZZATO] = {
    { J_TOUCH,      sizeof(J_TOUCH)      / sizeof(Note) },
    { J_MINER_DOWN, sizeof(J_MINER_DOWN) / sizeof(Note) },
    { J_TEMP,       sizeof(J_TEMP)       / sizeof(Note) },
    { J_BEST,       sizeof(J_BEST)       / sizeof(Note) },
    { J_BLOCK,      sizeof(J_BLOCK)      / sizeof(Note) },
    { J_STARTUP,    sizeof(J_STARTUP)    / sizeof(Note) },
};

/* ---------------------------------------------------------------- PolyBLEP --
 * A brute-force square wave ("above the threshold = +1, below = -1")
 * has two discontinuities per period. A discontinuity contains harmonics up
 * to infinite frequency: everything above half the sampling frequency
 * folds back onto NON-harmonic lines, and that is what the ear
 * hears as "metallic".
 *
 * PolyBLEP fixes the problem at the root: it replaces every sharp step with
 * a one-sample polynomial blend, which amounts to removing the
 * harmonics above Nyquist. It costs two multiplications per sample.          */
static inline float polyBlep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* low-pass state, reset at the start of every jingle */
static float s_lpState = 0.0f;
static float s_lpAlpha = 0.0f;

/* Generates a note and sends it to the I2S.
 * The envelope (2 ms attack, 8 ms release) avoids the "tick" at the
 * ends: a sharply truncated square wave clicks on the speaker. */
static void renderNote(const Note &n)
{
    const uint32_t totalFrames = (uint32_t)AUDIO_RATE * n.ms / 1000;
    if (totalFrames == 0) return;

    uint32_t attack  = (uint32_t)AUDIO_RATE * 12 / 1000;
    uint32_t release = (uint32_t)AUDIO_RATE * 15 / 1000;
    /* on very short notes attack and release cannot exceed the note */
    if (attack + release > totalFrames) {
        attack  = totalFrames / 3;
        release = totalFrames / 3;
    }
    static int16_t buf[CHUNK_FRAMES * 2];

    float phase = 0.0f;
    uint32_t done = 0;

    while (done < totalFrames) {
        const uint32_t frames = min<uint32_t>(CHUNK_FRAMES, totalFrames - done);

        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t pos = done + i;
            int16_t sample = 0;

            if (n.freq > 0) {
                float f = (float)n.freq;
                const float t = (float)pos / (float)AUDIO_RATE;
                if (n.fx == FX_SLIDE_DOWN) f *= (1.0f - 0.35f * (float)pos / (float)totalFrames);
                else if (n.fx == FX_SLIDE_UP) f *= (1.0f + 0.25f * (float)pos / (float)totalFrames);
                else if (n.fx == FX_VIBRATO) f *= (1.0f + 0.02f * sinf(2.0f * (float)M_PI * 6.0f * t));

                const float dt = f / (float)AUDIO_RATE;
                phase += dt;
                if (phase >= 1.0f) phase -= 1.0f;

                /* Additive synthesis: fundamental plus a few weak harmonics.
                 * The square wave has ALL odd harmonics at full amplitude
                 * (1/n), and that is what makes it harsh. Here the fundamental
                 * dominates and the harmonics only add body, not bite.
                 * The "duty" field now sets how bright the timbre is. */
                const float w = 2.0f * (float)M_PI * phase;
                float v = sinf(w);
                float norm = 1.0f;
                if (n.duty >= 25) { v += 0.20f * sinf(2.0f * w); norm += 0.20f; }
                if (n.duty >= 50) { v += 0.10f * sinf(3.0f * w); norm += 0.10f; }
                v /= norm;

                /* Soft envelope: slow attack (12 ms instead of 2) because
                 * a sharp attack is perceived as a metallic "tick", and
                 * exponential decay that softens the tail of the note. */
                float env = expf(-1.4f * (float)pos / (float)totalFrames);
                if (pos < attack) env *= (float)pos / (float)attack;
                else if (pos > totalFrames - release && totalFrames > release) {
                    env *= (float)(totalFrames - pos) / (float)release;
                }

                /* one-pole low-pass: rounds off the remaining edges */
                const float target = v * env * (float)WAVE_AMPLITUDE;
                s_lpState += s_lpAlpha * (target - s_lpState);

                float out = s_lpState;
                if (out >  32000.0f) out =  32000.0f;
                if (out < -32000.0f) out = -32000.0f;
                sample = (int16_t)out;
            } else {
                /* rest: let the filter decay instead of zeroing it
                 * abruptly, otherwise the jump to zero is itself a click */
                s_lpState += s_lpAlpha * (0.0f - s_lpState);
                sample = (int16_t)s_lpState;
            }

            buf[i * 2 + 0] = sample;
            buf[i * 2 + 1] = sample;
        }

        size_t written = 0;
        i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        done += frames;
    }
}

/* ============================================== procedural sounds =========
 *  They are not recorded samples: every sound is computed sample by sample
 *  (additive synthesis + filtered noise), so they take no flash.
 *  They all go through the same 3200 Hz low-pass as the jingles.
 * ========================================================================= */
static constexpr float DUE_PI = 2.0f * (float)M_PI;

/* fast white noise (xorshift32), output -1..+1 */
static uint32_t s_rng = 0x9E3779B9u;
static inline float rumore()
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

/* state-variable band-pass filter (Chamberlin) */
struct PassaBanda {
    float lp = 0.0f, bp = 0.0f, F = 0.1f, q = 1.0f;
    void imposta(float hz, float smorzamento)
    {
        F = 2.0f * sinf((float)M_PI * hz / (float)AUDIO_RATE);
        q = smorzamento;
    }
    float passo(float x)
    {
        const float hp = x - lp - q * bp;
        bp += F * hp;
        lp += F * bp;
        return bp;
    }
};

/* Engine voice: fundamental + harmonics weighted 1/k (softened sawtooth)
 * and a sub-harmonic at half frequency, which gives the "rumble"
 * of the engine cycle. "brillantezza" 0..1 weights the high harmonics. */
struct Motore {
    float fase = 0.0f;
    float faseSub = 0.0f;
    float campione(float f, float brillantezza)
    {
        fase += f / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        faseSub += 0.5f * f / (float)AUDIO_RATE;
        if (faseSub >= 1.0f) faseSub -= 1.0f;
        const float w = DUE_PI * fase;
        float v = sinf(w);
        float norm = 1.0f;
        for (int k = 2; k <= 6; ++k) {
            const float a = brillantezza / (float)k;
            v += a * sinf((float)k * w);
            norm += a;
        }
        v /= norm;
        return 0.8f * v + 0.2f * sinf(DUE_PI * faseSub);
    }
};

/* Generates "secondi" of audio calling gen(t) for each sample (value -1..1).
 * 8 ms ramp at the start and at the end against clicks. */
template <typename F>
static void renderGen(float secondi, F gen)
{
    const uint32_t totale = (uint32_t)(secondi * (float)AUDIO_RATE);
    static int16_t buf[CHUNK_FRAMES * 2];
    const float rampa = 0.008f;
    uint32_t fatto = 0;
    while (fatto < totale) {
        const uint32_t frames = min<uint32_t>(CHUNK_FRAMES, totale - fatto);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t pos = fatto + i;
            const float t = (float)pos / (float)AUDIO_RATE;
            const float resto = (float)(totale - pos) / (float)AUDIO_RATE;
            float v = gen(t);
            if (t < rampa)     v *= t / rampa;
            if (resto < rampa) v *= resto / rampa;
            s_lpState += s_lpAlpha * (v * (float)WAVE_AMPLITUDE - s_lpState);
            float out = s_lpState;
            if (out >  32000.0f) out =  32000.0f;
            if (out < -32000.0f) out = -32000.0f;
            buf[i * 2 + 0] = (int16_t)out;
            buf[i * 2 + 1] = (int16_t)out;
        }
        size_t scritti = 0;
        i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &scritti, portMAX_DELAY);
        fatto += frames;
    }
}

static void pausa(uint16_t ms)
{
    const Note n = { N_REST, ms, 12, FX_NONE };
    renderNote(n);
}

/* ---- F1: car passing by --------------------------------------------------
 * It arrives from afar (low volume, high pitch due to the Doppler effect), passes
 * in front (maximum volume, brighter timbre) and moves away (pitch dropping
 * by about a third, volume fading out).                                    */
static void suonaPassaggio()
{
    Motore m;
    const float durata = 2.4f;
    const float tc = 1.05f;           /* moment of the pass */
    renderGen(durata, [&](float t) {
        const float x = t - tc;
        const float f = 365.0f * (1.0f + 0.16f * tanhf(-x / 0.10f));   /* ~423 -> ~307 Hz */
        const float vicino = 1.0f / (1.0f + (x / 0.20f) * (x / 0.20f));
        const float amp = 0.06f + 0.94f * vicino;
        float v = m.campione(f, 0.35f + 0.65f * vicino);
        v += 0.22f * vicino * rumore();                       /* air and tyres */
        v *= 1.0f + 0.10f * sinf(DUE_PI * 31.0f * t);         /* vibration    */
        float fade = 1.0f;
        if (t < 0.5f) fade *= t / 0.5f;
        if (t > durata - 0.7f) fade *= (durata - t) / 0.7f;
        return 0.85f * amp * v * fade;
    });
}

/* ---- F1: race start ------------------------------------------------------
 * Five lights coming on (low beeps), lights out, then the engine
 * revving up with two gear changes.                                         */
static void suonaPartenza()
{
    const Note luce = { N_C4, 90, 12, FX_NONE };
    for (int i = 0; i < 5; ++i) {
        renderNote(luce);
        pausa(230);
    }
    pausa(450);
    Motore m;
    const float durata = 2.3f;
    renderGen(durata, [&](float t) {
        float f;
        if (t < 0.8f)      f = 190.0f + 330.0f * (t / 0.8f);            /* first   */
        else if (t < 1.5f) f = 330.0f + 220.0f * ((t - 0.8f) / 0.7f);   /* second  */
        else               f = 390.0f + 160.0f * ((t - 1.5f) / 0.8f);   /* third   */
        float amp = t < 0.12f ? t / 0.12f : 1.0f;
        if (fabsf(t - 0.8f) < 0.045f || fabsf(t - 1.5f) < 0.045f) amp *= 0.35f;   /* shift */
        if (t > durata - 0.6f) amp *= (durata - t) / 0.6f;
        const float v = m.campione(f, 0.75f) + 0.15f * rumore();
        return 0.8f * amp * v;
    });
}

/* ---- football: referee's whistle -----------------------------------------
 * Pea whistle: tone around 2.1 kHz modulated at ~28 Hz by the
 * pea spinning in the chamber, plus a hint of breath.                       */
static void fischio(float durata)
{
    float fase = 0.0f;
    renderGen(durata, [&](float t) {
        const float mod = sinf(DUE_PI * 28.0f * t);
        const float f = 2150.0f * (1.0f + 0.012f * mod);
        fase += f / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        const float trillo = 0.55f + 0.45f * mod;
        float v = (sinf(DUE_PI * fase) + 0.15f * sinf(2.0f * DUE_PI * fase)) / 1.15f;
        v = v * trillo + 0.06f * rumore();
        float env = t < 0.025f ? t / 0.025f : 1.0f;
        if (t > durata - 0.05f) env *= (durata - t) / 0.05f;
        return 0.8f * v * env;
    });
}

/* ---- football: goal ------------------------------------------------------
 * Stadium horn (major chord of three rich tones) and crowd roar
 * (noise filtered around 900 Hz that explodes and then fades in waves).     */
static void suonaGol()
{
    float f1 = 0.0f, f2 = 0.0f, f3 = 0.0f;
    PassaBanda folla;
    folla.imposta(900.0f, 0.8f);
    auto voce = [](float &fase, float hz) {
        fase += hz / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        const float w = DUE_PI * fase;
        return (sinf(w) + 0.5f * sinf(2.0f * w) + 0.33f * sinf(3.0f * w) + 0.25f * sinf(4.0f * w)) / 2.08f;
    };
    const float durata = 3.6f;
    renderGen(durata, [&](float t) {
        float sirena = 0.0f;
        if (t < 1.6f) {
            float e = t < 0.04f ? t / 0.04f : 1.0f;
            if (t > 1.3f) e *= (1.6f - t) / 0.3f;
            sirena = e * (voce(f1, 277.0f) + voce(f2, 349.0f) + voce(f3, 415.0f)) / 3.0f;
        }
        const float n = folla.passo(rumore());
        float ef = t < 0.5f ? t / 0.5f : expf(-(t - 0.5f) / 1.4f);
        ef *= 1.0f + 0.25f * sinf(DUE_PI * 2.3f * t);
        return 0.6f * sirena + 1.5f * ef * n;
    });
}

static void suonaProcedurale(SoundEvent e)
{
    switch (e) {
        case SND_F1_PASSAGGIO: suonaPassaggio(); break;
        case SND_F1_PARTENZA:  suonaPartenza();  break;
        case SND_FISCHIO_INIZIO:
            fischio(1.1f);
            break;
        case SND_FISCHIO_INTERVALLO:
            fischio(0.35f); pausa(180); fischio(0.9f);
            break;
        case SND_FISCHIO_FINE:
            fischio(0.35f); pausa(180); fischio(0.35f); pausa(180); fischio(1.3f);
            break;
        case SND_GOL: suonaGol(); break;
        case SND_ESPULSIONE: {
            fischio(0.6f);
            pausa(120);
            const Note giu[] = { { N_E4, 160, 25, FX_NONE }, { N_C4, 160, 25, FX_NONE }, { N_G3, 420, 12, FX_SLIDE_DOWN } };
            for (const Note &n : giu) renderNote(n);
            break;
        }
        default: break;
    }
}

static void audioTask(void *arg)
{
    (void)arg;
    SoundEvent e;
    while (true) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) == pdTRUE) {
            if (e >= SND_COUNT) continue;
            s_lpState = 0.0f;               /* clean filter at every jingle */
            if (e >= SND_PRIMO_SINTETIZZATO) {
                suonaProcedurale(e);
            } else {
                const Jingle &j = JINGLES[e];
                for (uint8_t i = 0; i < j.count; ++i) {
                    renderNote(j.notes[i]);
                }
            }
            /* half a frame of silence: closes the DMA tail without hum */
            static int16_t silence[64] = { 0 };
            size_t w = 0;
            i2s_channel_write(s_tx, silence, sizeof(silence), &w, pdMS_TO_TICKS(50));
        }
    }
}

/* ============================================================ API =========== */
bool audioBegin()
{
    if (!openCodecBus()) {
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }

    /* amplifier */
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << PA_ENABLE_IO;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)PA_ENABLE_IO, 1);

    if (!i2sInit()) {
        snprintf(s_status, sizeof(s_status), "I2S not initialised");
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }
    if (!codecInit()) {
        snprintf(s_status, sizeof(s_status), "ES8311 init failed");
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }

    /* one-pole low-pass coefficient: alpha = 1 - e^(-2*pi*fc/fs) */
    s_lpAlpha = 1.0f - expf(-2.0f * (float)M_PI * LOWPASS_HZ / (float)AUDIO_RATE);
    s_lpState = 0.0f;

    s_queue = xQueueCreate(10, sizeof(SoundEvent));
    if (s_queue == nullptr) return false;
    /* priority 4, above LVGL (3): the synthesised sounds are computed in real
     * time and a full-screen redraw must not leave gaps in the audio */
    if (xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 4, nullptr, 1) != pdPASS) {
        return false;
    }

    s_ready = true;
    Serial.printf("Audio: %s\n", s_status);
    return true;
}

void audioPlay(SoundEvent event)
{
    if (!s_ready || s_queue == nullptr) return;
    /* if the queue is full we let it go: better to skip a sound than
     * block the task that requested it (often the LVGL one) */
    xQueueSend(s_queue, &event, 0);
}

bool audioReady() { return s_ready; }

void audioSetVolume(uint8_t percent)
{
    s_volume = (percent > 100) ? 100 : percent;
    if (s_ready) codecSetVolumeReg(s_volume);
}

uint8_t audioGetVolume() { return s_volume; }

const char *audioStatus() { return s_status; }
