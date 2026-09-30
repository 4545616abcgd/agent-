#pragma once
#include "FreeRTOS.h"
typedef void *EventGroupHandle_t;
typedef unsigned EventBits_t;
static inline EventGroupHandle_t xEventGroupCreate(void) { return (void *)1; }
static inline void vEventGroupDelete(EventGroupHandle_t a) { (void)a; }
static inline unsigned xEventGroupSetBits(EventGroupHandle_t a, unsigned b) { (void)a; return b; }
static inline unsigned xEventGroupClearBits(EventGroupHandle_t a, unsigned b) { (void)a; return b; }
static inline unsigned xEventGroupWaitBits(EventGroupHandle_t a, unsigned b, int c, int d, unsigned e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
