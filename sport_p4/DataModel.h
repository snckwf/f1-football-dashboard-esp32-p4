/* ============================================================================
 *  DataModel - state shared between the network tasks and the LVGL task
 *  Everything in RAM (the large structures in PSRAM), no writes to flash.
 *  Every block has a "versione" counter: the UI redraws only if it changes.
 * ========================================================================== */
#pragma once

#include <Arduino.h>
#include <time.h>
#include "AppConfig.h"

/* =========================== FOOTBALL ==================================== */
#define CALCIO_MAX_PARTITE 64

struct Partita {
    char     id[16];
    time_t   inizio;          /* UTC                                       */
    char     casa[28];
    char     ospite[28];
    int8_t   golCasa;         /* -1 = not available                         */
    int8_t   golOspite;
    int8_t   rigCasa;         /* penalties, -1 if there are none            */
    int8_t   rigOspite;
    char     stato;           /* 'p' to be played, 'i' in progress, 'f' finished, 'x' postponed */
    char     minuto[12];      /* "67'", "HT"                                */
    char     dettaglio[16];   /* "FT", "AET", "PENS"                        */
    char     nota[48];
    uint32_t golMs;           /* millis() of the last score change, 0 = never */
    uint8_t  rossiCasa;       /* red cards (from the scoreboard "details")  */
    uint8_t  rossiOspite;
    char     ultimoRosso[40]; /* "A. Rossi 67'"                                */
    char     marcatore[40];   /* last goal: "L. Retegui 23'"                   */
};

struct Competizione {
    uint16_t n;
    Partita  p[CALCIO_MAX_PARTITE];
    uint32_t versione;
    uint32_t aggMs;           /* millis() of the last list load             */
    bool     caricata;
    bool     ripiego;         /* empty window: ESPN matchday shown          */
    uint8_t  live;            /* matches in progress                        */
    char     errore[40];
};

/* ---- detail card of a match (ESPN summary) ----------------------------- */
struct EventoPartita {
    char minuto[10];
    char tipo;               /* G goal, R penalty, A own goal, Y yellow, X red, S substitution */
    bool casa;
    char nome[28];
    char nome2[28];          /* substitution: player coming off */
};

struct Giocatore {
    char num[4];
    char nome[26];
    char ruolo[6];
    bool titolare;
    bool entrato;
    bool uscito;
};

#define DET_MAX_EVENTI 40
#define DET_MAX_STAT   12
#define DET_MAX_GIOC   26

struct DettaglioPartita {
    char     id[16];
    uint8_t  comp;
    bool     pronto;
    char     errore[32];
    uint32_t versione;
    uint32_t aggMs;
    char     casa[28];
    char     ospite[28];
    int8_t   golCasa, golOspite;
    char     stato;
    char     minuto[12];
    char     dettaglio[24];
    time_t   inizio;
    char     stadio[48];
    uint32_t spettatori;
    uint8_t  nEv;
    EventoPartita ev[DET_MAX_EVENTI];
    uint8_t  nStat;
    char     statNome[DET_MAX_STAT][22];
    char     statCasa[DET_MAX_STAT][8];
    char     statOsp[DET_MAX_STAT][8];
    char     moduloCasa[10];
    char     moduloOsp[10];
    uint8_t  nCasa, nOsp;
    Giocatore gCasa[DET_MAX_GIOC];
    Giocatore gOsp[DET_MAX_GIOC];
};

struct RigaClassifica {
    uint8_t pos;
    char    squadra[28];
    uint8_t g, v, n, p;
    int16_t dr;
    int16_t pt;
};

struct Classifica {
    uint8_t        n;
    RigaClassifica r[24];
    uint32_t       versione;
    bool           ok;
};

/* =========================== FORMULA 1 =================================== */
struct F1PilotaClass {
    uint8_t  pos;
    char     codice[4];
    char     cognome[24];
    char     team[24];
    uint32_t colore;
    float    punti;
    uint8_t  vittorie;
};

struct F1TeamClass {
    uint8_t  pos;
    char     nome[24];
    uint32_t colore;
    float    punti;
    uint8_t  vittorie;
};

struct F1Risultato {
    uint8_t  pos;
    char     codice[4];
    char     cognome[24];
    char     team[24];
    uint32_t colore;
    uint8_t  griglia;
    uint8_t  giri;
    char     tempo[20];       /* "1:38:02.339", "+0.196", "+1 Lap", "Retired" */
    float    punti;
    bool     giroVeloce;
};

struct F1Sessione {
    char   nome[16];
    time_t inizio;            /* UTC */
};

struct F1Dati {
    char          stagione[6];
    uint8_t       round;
    uint8_t       nPiloti;
    F1PilotaClass piloti[26];
    uint8_t       nTeam;
    F1TeamClass   team[12];

    char          garaNome[48];
    char          garaData[12];
    uint8_t       nRis;
    F1Risultato   ris[26];

    char          prossimoNome[48];
    char          prossimoLuogo[48];
    uint8_t       nSessioni;
    F1Sessione    sessioni[6];

    uint32_t      versione;
    bool          ok;
};

struct F1LivePilota {
    char     num[4];
    char     tla[4];
    char     cognome[20];
    char     team[24];
    uint32_t colore;
    uint8_t  pos;
    char     gap[14];
    char     intervallo[14];
    char     ultimo[12];
    char     migliore[12];
    uint32_t miglioreMs;      /* best lap ms, 0 = none */
    uint8_t  giri;
    uint8_t  pit;
    bool     inPit;
    bool     ritirato;
    bool     fermo;
    int8_t   delta;           /* positions gained (+) or lost (-) */
    uint32_t deltaMs;
    /* tyres (TimingAppData) */
    uint8_t  stint;           /* index of the current stint                */
    char     gomma;           /* S M H I W, '?' unknown, 0 = no data       */
    uint8_t  giriGomma;
    bool     gommaNuova;
    /* to estimate the position on track: Position.z of the live timing needs
     * an F1 TV subscription, so the last crossing of the line is used       */
    uint32_t passaggioMs;     /* millis() of the last crossing, 0 = unknown */
    uint32_t giroMs;          /* last lap time in ms                        */
};

/* circuit outline (MultiViewer), saved in NVS: one per circuit.
 * t[] = reference time from the finish line to that point (hundredths of s) or,
 * if the circuit does not provide it, the distance covered; tGiro = t[n-1]. */
#define TRK_MAX 800
struct Tracciato {
    uint16_t chiave;          /* Meeting.Circuit.Key                        */
    uint16_t n;
    bool     completo;        /* outline available                          */
    bool     tempiVeri;       /* t[] in hundredths of a second              */
    int16_t  x[TRK_MAX];
    int16_t  y[TRK_MAX];
    uint16_t t[TRK_MAX];
    uint16_t tGiro;
    uint32_t versione;
};

struct F1Live {
    bool         connesso;
    char         meeting[48];
    char         sessione[24];
    char         tipo[16];      /* "Race", "Qualifying", "Practice" */
    char         stato[16];     /* "Started", "Finished", "Inactive", "Aborted" */
    uint8_t      giro;
    uint8_t      giriTot;
    uint8_t      pista;         /* 1 green 2 yellow 4 SC 5 red 6 VSC 7 VSC ending */
    char         messaggio[96]; /* last race control message */
    time_t       inizio;        /* UTC */
    time_t       fine;
    uint8_t      n;
    F1LivePilota p[24];
    uint16_t     circuito;      /* Meeting.Circuit.Key */
    uint32_t     versione;
    uint32_t     aggMs;
};

/* =========================== INSTANCES =================================== */
extern Competizione *g_comp;          /* [CALCIO_COMP_COUNT] in PSRAM */
extern Classifica    g_classSerieA;
extern F1Dati       *g_f1;
extern F1Live       *g_f1live;
extern Tracciato    *g_trk;
extern DettaglioPartita *g_det;

/* detail requested by the UI (ESPN event id + competition) */
extern volatile bool    g_detRichiesta;
extern volatile bool    g_detAperto;
extern char             g_detId[16];
extern volatile uint8_t g_detComp;

/* Competition shown on screen: the football task gives it priority */
extern volatile uint8_t g_compVisibile;
extern volatile bool    g_richiestaLista;

void initDataModel();
bool dataLock(int timeoutMs);
void dataUnlock();

void        setWifiStatusText(const char *text);
const char *getWifiStatusText();

/* =========================== HELPER ====================================== */
/* UTF-8 -> ASCII: the LVGL Montserrat fonts only have ASCII characters */
void   asciiFold(const char *in, char *out, size_t n);
/* "2026-10-10T13:00Z", "2026-10-10T13:00:00Z", "2026-10-04T15:00:00" -> UTC epoch */
time_t parseIsoUtc(const char *iso);
/* "08:00:00" or "-04:00:00" -> seconds */
long   parseOffset(const char *s);
/* Display name of national teams and clubs (input: the ESPN name already in ASCII) */
const char *nomeItaliano(const char *eng);
/* "Sat 10/10" in Italian time */
void   fmtGiorno(time_t t, char *out, size_t n);
/* "20:45" in Italian time */
void   fmtOra(time_t t, char *out, size_t n);
/* key of the local day YYYYMMDD */
uint32_t chiaveGiorno(time_t t);
/* "1:35.631" -> 95631 ms, 0 if not valid */
uint32_t tempoGiroMs(const char *s);
/* team colour from id/name (Jolpica or F1) */
uint32_t coloreTeam(const char *idONome);
/* true if the system clock is already synchronised via NTP */
bool   orologioValido();
