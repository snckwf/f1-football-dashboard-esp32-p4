/* ============================================================================
 *  Audio - chiptune jingles on the board speaker (ES8311 codec)
 *
 *  Square waves with variable duty cycle, like the NES pulse channels.
 *  No audio files: everything synthesised, a few hundred bytes of tables.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

enum SoundEvent : uint8_t {
    SND_TOUCH = 0,     /* short blip at every touch                          */
    SND_MINER_DOWN,    /* "power down" descent: a miner no longer answers    */
    SND_TEMP_ALARM,    /* two-tone siren: critical temperature               */
    SND_BEST_SHARE,    /* rising arpeggio: new best share record             */
    SND_BLOCK_FOUND,   /* victory fanfare: block found                       */
    SND_STARTUP,       /* short chord at startup, confirms the audio works   */

    /* ---- sounds synthesised in real time (sport_p4) ---- */
    SND_F1_PASSAGGIO,        /* roar of a passing car (Doppler effect)             */
    SND_F1_PARTENZA,         /* 5 lights, lights out, engine revving up            */
    SND_FISCHIO_INIZIO,      /* long referee whistle                               */
    SND_FISCHIO_INTERVALLO,  /* two whistles                                       */
    SND_FISCHIO_FINE,        /* three whistles: short, short, long                 */
    SND_GOL,                 /* stadium horn + crowd roar                          */
    SND_ESPULSIONE,          /* sharp whistle + three falling notes (red card)     */
    SND_COUNT
};

/* the sounds from here on are not note jingles but procedural generators */
static constexpr uint8_t SND_PRIMO_SINTETIZZATO = SND_F1_PASSAGGIO;

/* Initialises codec and I2S. Returns false if the codec does not answer:
 * in that case audioPlay() becomes a no-op and the rest keeps working.        */
bool audioBegin();

/* Non-blocking: queues the jingle and returns at once. Callable from any task. */
void audioPlay(SoundEvent event);

bool audioReady();

/* 0..100. Acts on the DAC volume register. */
void audioSetVolume(uint8_t percent);
uint8_t audioGetVolume();

/* Readable diagnostics, to print on the serial port */
const char *audioStatus();
