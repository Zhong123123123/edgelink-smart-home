#ifndef __DRIVER_OLED_SSD1306_H
#define __DRIVER_OLED_SSD1306_H

#include <stdint.h>
#include "driver_display.h"

int Driver_OLED_Init(void);
int Driver_OLED_ShowStatus(const DisplayStatus *status);

#endif
