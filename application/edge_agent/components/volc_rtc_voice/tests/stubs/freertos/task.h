#pragma once
#include "FreeRTOS.h"
static inline TaskHandle_t xTaskCreateStaticPinnedToCore(void (*a)(void *), const char *b,
    unsigned c, void *d, unsigned e, StackType_t *f, StaticTask_t *g, unsigned h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; return g; }
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t a) { (void)a; return 4096; }
static inline int xTaskCreate(void (*a)(void *), const char *b, unsigned c,
    void *d, unsigned e, TaskHandle_t *f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; *f = (void *)1; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t a) { (void)a; }
