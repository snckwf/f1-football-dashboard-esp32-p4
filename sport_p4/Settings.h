/* ============================================================================
 *  Settings - values adjustable from the web page, saved in NVS
 *  (namespace "sport_p4"), so they survive reboots and updates.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

struct Impostazioni {
    uint8_t  volume;        /* 0..100, 75 = 0 dB on the DAC                 */
    uint8_t  luminosita;    /* brightness 5..100 %                          */
    uint16_t standbyMin;    /* minutes without touches before standby, 0 = never */
    bool     suoni;         /* alert sounds                                 */
    bool     avvisiCalcio;  /* banner + sound for goals, whistles...        */
    bool     avvisiF1;      /* banner + sound for F1 events                 */
    bool     avvisiRossi;   /* alert for red cards                          */
    uint8_t  compAttive;    /* bits 0..5 = competitions shown               */
};

/* loads from NVS and applies volume, brightness and standby */
void          impostazioniCarica();
void          impostazioniSalva();
Impostazioni &imp();
/* applies the current values to the hardware (volume, brightness, standby) */
void          impostazioniApplica();
/* true if competition c is active */
bool          compAttiva(uint8_t c);

/* configuration web server (port 80, http://sport-p4.local) */
void          webBegin();
