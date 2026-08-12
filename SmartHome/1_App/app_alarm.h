#ifndef __APP_ALARM_H
#define __APP_ALARM_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void vStartAlarmTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

#endif
