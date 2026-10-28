#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef __ASSEMBLY__
#include <stdint.h>
#include <stdlib.h>
#endif

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_USE_DRAW_SW 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_NATIVE_HELIUM_ASM 0
#define LV_USE_NEMA_GFX 0
#define LV_USE_DRAW_PXP 0
#define LV_USE_ROTATE_PXP 0
#define LV_USE_DRAW_G2D 0
#define LV_USE_ROTATE_G2D 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_VG_LITE 0
#define LV_USE_DRAW_DMA2D 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_DRAW_EVE 0
#define LV_USE_DRAW_NANOVG 0

/* allocator in PSRAM: see LvMemPsram.cpp */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0

/* Fonts: 28/36/48 are for the big numbers of the dashboard */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets actually used, declared explicitly */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_BTN 1
#define LV_USE_LINE 1
#define LV_USE_IMAGE 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_CHART 1
#define LV_USE_TABLE 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif
#endif
