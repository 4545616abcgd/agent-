#pragma once
#include <stdint.h>
typedef uint8_t StackType_t;
typedef struct { int dummy; } StaticTask_t;
typedef struct { int dummy; } StaticQueue_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define BIT0 1
#define BIT1 2
#define BIT2 4
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
