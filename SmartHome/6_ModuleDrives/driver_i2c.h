#ifndef __DRIVER_I2C_H
#define __DRIVER_I2C_H

#include <stdint.h>
#include "board_pins.h"

I2C_HandleTypeDef *Driver_I2C_GetHandle(void);
int Driver_I2C_Init(void);
int Driver_I2C_Write(uint16_t dev_addr, const uint8_t *data, uint16_t len, uint32_t timeout);

#endif
