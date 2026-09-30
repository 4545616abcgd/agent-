#pragma once
#include "FreeRTOS.h"
static inline QueueHandle_t xQueueCreateStatic(unsigned a, unsigned b, uint8_t *c, StaticQueue_t *d)
{ (void)a; (void)b; (void)c; return d; }
static inline int xQueueReceive(QueueHandle_t a, void *b, unsigned c)
{ (void)a; (void)b; (void)c; return 0; }
static inline int xQueueSend(QueueHandle_t a, const void *b, unsigned c)
{ (void)a; (void)b; (void)c; return pdPASS; }
