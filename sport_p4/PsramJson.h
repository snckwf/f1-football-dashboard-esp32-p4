/* ============================================================================
 *  PsramJson - ArduinoJson allocator in PSRAM
 *
 *  ArduinoJson 7 allocates in small blocks: with malloc() they would end up in
 *  internal RAM, which is needed instead for the TLS handshakes. Usage:
 *      JsonDocument doc(PSRAM_JSON);
 * ========================================================================== */
#pragma once

#include <ArduinoJson.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override
    {
        void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p != nullptr ? p : malloc(n);
    }
    void deallocate(void *p) override
    {
        heap_caps_free(p);
    }
    void *reallocate(void *p, size_t n) override
    {
        void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return q != nullptr ? q : realloc(p, n);
    }
    static ArduinoJson::Allocator *instance()
    {
        static SpiRamAllocator a;
        return &a;
    }
};

#define PSRAM_JSON SpiRamAllocator::instance()
