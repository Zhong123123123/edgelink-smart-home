#ifndef __APP_SENSOR_H
#define __APP_SENSOR_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void vStartSensorTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

#endif
