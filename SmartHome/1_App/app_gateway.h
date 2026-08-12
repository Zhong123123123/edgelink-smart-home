#ifndef __APP_GATEWAY_H
#define __APP_GATEWAY_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

void Gateway_RequestImmediateReport(void);
void Gateway_ReportAlarmEvent(uint8_t event_code);
void Gateway_ConfirmSlotEarly(void);
void vStartGatewayTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

#endif
