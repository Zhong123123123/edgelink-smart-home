#ifndef __DRIVER_LED_KEY_H
#define __DRIVER_LED_KEY_H

#include "main.h"
#include "board_pins.h"
#include "dev_io.h"
/* LED */
#define LED_PORT	BOARD_LED_PORT
#define LED_PIN	BOARD_LED_PIN
#define LED(STATUS)	HAL_GPIO_WritePin(LED_PORT, LED_PIN, STATUS?GPIO_PIN_RESET:GPIO_PIN_SET)
#define LED_SHINE()	HAL_GPIO_TogglePin(LED_PORT, LED_PIN)

/* Buzzer */
#define BEEP_PORT	BOARD_BEEP_PORT
#define BEEP_PIN	BOARD_BEEP_PIN
#define BEEP(STATUS)	HAL_GPIO_WritePin(BEEP_PORT, BEEP_PIN, STATUS?GPIO_PIN_SET:GPIO_PIN_RESET)

/* Key */
#define KEY_PORT	BOARD_KEY_PORT
#define KEY_PIN	BOARD_KEY_PIN
#define KEY_STATUE()	HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN)



int Driver_LED_Init(void);
int Driver_LED_WriteStatus(uint8_t status);
int Driver_Beep_Init(void);
int Driver_Beep_WriteStatus(uint8_t status);

int Driver_Key_Init(void);
int Driver_Key_Read(uint8_t *buf, uint16_t len);
#endif
