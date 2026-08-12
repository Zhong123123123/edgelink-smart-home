#ifndef __APP_DISPLAY_H
#define __APP_DISPLAY_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void vStartDisplayTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

#endif
