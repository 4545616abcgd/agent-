#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t a, unsigned b) { (void)a; (void)b; return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t a) { (void)a; return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t a) { (void)a; }
