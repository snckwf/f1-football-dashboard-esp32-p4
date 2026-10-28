#include "Settings.h"

#include <Preferences.h>
#include "AppConfig.h"
#include "Audio.h"
#include "BoardLvgl.h"

static Impostazioni g_imp = {
    AUDIO_VOLUME, 100, (uint16_t)(SCREEN_TIMEOUT_MS / 60000), true, true, true, true, 0x3F,
};

Impostazioni &imp() { return g_imp; }

bool compAttiva(uint8_t c)
{
    return c < 8 && (g_imp.compAttive & (1u << c)) != 0;
}

void impostazioniCarica()
{
    Preferences p;
    if (p.begin("sport_p4", true)) {
        g_imp.volume       = p.getUChar("volume", g_imp.volume);
        g_imp.luminosita   = p.getUChar("luce", g_imp.luminosita);
        g_imp.standbyMin   = p.getUShort("standby", g_imp.standbyMin);
        g_imp.suoni        = p.getBool("suoni", g_imp.suoni);
        g_imp.avvisiCalcio = p.getBool("avvCalcio", g_imp.avvisiCalcio);
        g_imp.avvisiF1     = p.getBool("avvF1", g_imp.avvisiF1);
        g_imp.avvisiRossi  = p.getBool("avvRossi", g_imp.avvisiRossi);
        g_imp.compAttive   = p.getUChar("comp", g_imp.compAttive);
        p.end();
    }
    if ((g_imp.compAttive & 0x3F) == 0) g_imp.compAttive = 0x3F;   /* at least one */
    if (g_imp.volume > 100) g_imp.volume = 100;
    if (g_imp.luminosita < 5 || g_imp.luminosita > 100) g_imp.luminosita = 100;
    Serial.printf("[SETTINGS] volume %u, brightness %u%%, standby %u min, sounds %d, comp 0x%02X\n",
                  g_imp.volume, g_imp.luminosita, g_imp.standbyMin, g_imp.suoni, g_imp.compAttive);
}

void impostazioniSalva()
{
    Preferences p;
    if (!p.begin("sport_p4", false)) return;
    p.putUChar("volume", g_imp.volume);
    p.putUChar("luce", g_imp.luminosita);
    p.putUShort("standby", g_imp.standbyMin);
    p.putBool("suoni", g_imp.suoni);
    p.putBool("avvCalcio", g_imp.avvisiCalcio);
    p.putBool("avvF1", g_imp.avvisiF1);
    p.putBool("avvRossi", g_imp.avvisiRossi);
    p.putUChar("comp", g_imp.compAttive);
    p.end();
}

void impostazioniApplica()
{
    audioSetVolume(g_imp.volume);
    if (lvglLock(500)) {
        boardSetBrightness(g_imp.luminosita);
        boardSetScreenTimeoutMs((uint32_t)g_imp.standbyMin * 60000UL);
        lvglUnlock();
    }
}
