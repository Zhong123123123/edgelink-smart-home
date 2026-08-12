#ifndef __BOARD_PINS_H
#define __BOARD_PINS_H

#include "board_profile.h"

/*
 * Board pin profile.
 * These defaults preserve the existing F103 wiring behavior.
 * For a real F407 board, adjust pin mapping here according to your PCB/schematic.
 */

#if defined(SH_MCU_F407)

#define BOARD_DBG_UART_INSTANCE        USART1
#define BOARD_DBG_TX_PORT              GPIOA
#define BOARD_DBG_TX_PIN               GPIO_PIN_9
#define BOARD_DBG_RX_PORT              GPIOA
#define BOARD_DBG_RX_PIN               GPIO_PIN_10

#define BOARD_NET_UART_INSTANCE        USART2
#define BOARD_NET_TX_PORT              GPIOA
#define BOARD_NET_TX_PIN               GPIO_PIN_2
#define BOARD_NET_RX_PORT              GPIOA
#define BOARD_NET_RX_PIN               GPIO_PIN_3
#define BOARD_NET_UART_IRQn            USART2_IRQn
#define BOARD_NET_UART_IRQ_HANDLER     USART2_IRQHandler

#define BOARD_GW_UART_INSTANCE         USART3
#define BOARD_GW_TX_PORT               GPIOB
#define BOARD_GW_TX_PIN                GPIO_PIN_10
#define BOARD_GW_RX_PORT               GPIOB
#define BOARD_GW_RX_PIN                GPIO_PIN_11
#define BOARD_GW_UART_IRQn             USART3_IRQn
#define BOARD_GW_UART_IRQ_HANDLER      USART3_IRQHandler

#define BOARD_DBG_UART_AF              GPIO_AF7_USART1
#define BOARD_NET_UART_AF              GPIO_AF7_USART2
#define BOARD_GW_UART_AF               GPIO_AF7_USART3

#define BOARD_LED_PORT                 GPIOF
#define BOARD_LED_PIN                  GPIO_PIN_9
#define BOARD_LED1_PORT                GPIOF
#define BOARD_LED1_PIN                 GPIO_PIN_10
#define BOARD_KEY_PORT                 GPIOE
#define BOARD_KEY_PIN                  GPIO_PIN_4
#define BOARD_KEY_EXTI_IRQn            EXTI4_IRQn
#define BOARD_KEY_IRQ_HANDLER          EXTI4_IRQHandler
#define BOARD_DHT11_PORT               GPIOG
#define BOARD_DHT11_PIN                GPIO_PIN_9
#define BOARD_BEEP_PORT                GPIOF
#define BOARD_BEEP_PIN                 GPIO_PIN_8

#define BOARD_LIGHT_ADC_INSTANCE       ADC3
#define BOARD_LIGHT_ADC_CHANNEL        ADC_CHANNEL_5
#define BOARD_LIGHT_ADC_PORT           GPIOF
#define BOARD_LIGHT_ADC_PIN            GPIO_PIN_7

#define BOARD_OLED_I2C_INSTANCE        I2C1
#define BOARD_OLED_I2C_SCL_PORT        GPIOB
#define BOARD_OLED_I2C_SCL_PIN         GPIO_PIN_8
#define BOARD_OLED_I2C_SDA_PORT        GPIOB
#define BOARD_OLED_I2C_SDA_PIN         GPIO_PIN_9
#define BOARD_OLED_I2C_AF              GPIO_AF4_I2C1

#define BOARD_SPI_FLASH_SCK_PORT       GPIOB
#define BOARD_SPI_FLASH_SCK_PIN        GPIO_PIN_3
#define BOARD_SPI_FLASH_MISO_PORT      GPIOB
#define BOARD_SPI_FLASH_MISO_PIN       GPIO_PIN_4
#define BOARD_SPI_FLASH_MOSI_PORT      GPIOB
#define BOARD_SPI_FLASH_MOSI_PIN       GPIO_PIN_5
#define BOARD_SPI_FLASH_CS_PORT        GPIOB
#define BOARD_SPI_FLASH_CS_PIN         GPIO_PIN_14

#define BOARD_UART_BAUD_DEFAULT        115200U

#define BOARD_ENABLE_DBG_UART_CLK()    __HAL_RCC_USART1_CLK_ENABLE()
#define BOARD_ENABLE_NET_UART_CLK()    __HAL_RCC_USART2_CLK_ENABLE()
#define BOARD_ENABLE_GW_UART_CLK()     __HAL_RCC_USART3_CLK_ENABLE()
#define BOARD_ENABLE_SPI1_CLK()        __HAL_RCC_SPI1_CLK_ENABLE()
#define BOARD_ENABLE_OLED_I2C_CLK()    __HAL_RCC_I2C1_CLK_ENABLE()
#define BOARD_ENABLE_LIGHT_ADC_CLK()   __HAL_RCC_ADC3_CLK_ENABLE()

#define BOARD_ENABLE_GPIOA_CLK()       __HAL_RCC_GPIOA_CLK_ENABLE()
#define BOARD_ENABLE_GPIOB_CLK()       __HAL_RCC_GPIOB_CLK_ENABLE()
#define BOARD_ENABLE_GPIOC_CLK()       __HAL_RCC_GPIOC_CLK_ENABLE()
#define BOARD_ENABLE_GPIOD_CLK()       __HAL_RCC_GPIOD_CLK_ENABLE()
#define BOARD_ENABLE_GPIOE_CLK()       __HAL_RCC_GPIOE_CLK_ENABLE()
#define BOARD_ENABLE_GPIOF_CLK()       __HAL_RCC_GPIOF_CLK_ENABLE()
#define BOARD_ENABLE_GPIOG_CLK()       __HAL_RCC_GPIOG_CLK_ENABLE()

#else

#define BOARD_DBG_UART_INSTANCE        USART1
#define BOARD_DBG_TX_PORT              GPIOA
#define BOARD_DBG_TX_PIN               GPIO_PIN_9
#define BOARD_DBG_RX_PORT              GPIOA
#define BOARD_DBG_RX_PIN               GPIO_PIN_10

#define BOARD_NET_UART_INSTANCE        USART2
#define BOARD_NET_TX_PORT              GPIOA
#define BOARD_NET_TX_PIN               GPIO_PIN_2
#define BOARD_NET_RX_PORT              GPIOA
#define BOARD_NET_RX_PIN               GPIO_PIN_3
#define BOARD_NET_UART_IRQn            USART2_IRQn
#define BOARD_NET_UART_IRQ_HANDLER     USART2_IRQHandler

#define BOARD_GW_UART_INSTANCE         USART3
#define BOARD_GW_TX_PORT               GPIOB
#define BOARD_GW_TX_PIN                GPIO_PIN_10
#define BOARD_GW_RX_PORT               GPIOB
#define BOARD_GW_RX_PIN                GPIO_PIN_11
#define BOARD_GW_UART_IRQn             USART3_IRQn
#define BOARD_GW_UART_IRQ_HANDLER      USART3_IRQHandler

#define BOARD_DBG_UART_AF              0U
#define BOARD_NET_UART_AF              0U
#define BOARD_GW_UART_AF               0U

#define BOARD_LED_PORT                 GPIOA
#define BOARD_LED_PIN                  GPIO_PIN_7
#define BOARD_KEY_PORT                 GPIOB
#define BOARD_KEY_PIN                  GPIO_PIN_15
#define BOARD_KEY_EXTI_IRQn            EXTI15_10_IRQn
#define BOARD_KEY_IRQ_HANDLER          EXTI15_10_IRQHandler
#define BOARD_DHT11_PORT               GPIOB
#define BOARD_DHT11_PIN                GPIO_PIN_14
#define BOARD_BEEP_PORT                GPIOB
#define BOARD_BEEP_PIN                 GPIO_PIN_13

#define BOARD_LIGHT_ADC_INSTANCE       ADC3
#define BOARD_LIGHT_ADC_CHANNEL        ADC_CHANNEL_5
#define BOARD_LIGHT_ADC_PORT           GPIOF
#define BOARD_LIGHT_ADC_PIN            GPIO_PIN_7

#define BOARD_OLED_I2C_INSTANCE        I2C1
#define BOARD_OLED_I2C_SCL_PORT        GPIOB
#define BOARD_OLED_I2C_SCL_PIN         GPIO_PIN_8
#define BOARD_OLED_I2C_SDA_PORT        GPIOB
#define BOARD_OLED_I2C_SDA_PIN         GPIO_PIN_9
#define BOARD_OLED_I2C_AF              0U

#define BOARD_SPI_FLASH_SCK_PORT       GPIOB
#define BOARD_SPI_FLASH_SCK_PIN        GPIO_PIN_3
#define BOARD_SPI_FLASH_MISO_PORT      GPIOB
#define BOARD_SPI_FLASH_MISO_PIN       GPIO_PIN_4
#define BOARD_SPI_FLASH_MOSI_PORT      GPIOB
#define BOARD_SPI_FLASH_MOSI_PIN       GPIO_PIN_5
#define BOARD_SPI_FLASH_CS_PORT        GPIOB
#define BOARD_SPI_FLASH_CS_PIN         GPIO_PIN_14

#define BOARD_UART_BAUD_DEFAULT        115200U

#define BOARD_ENABLE_DBG_UART_CLK()    __HAL_RCC_USART1_CLK_ENABLE()
#define BOARD_ENABLE_NET_UART_CLK()    __HAL_RCC_USART2_CLK_ENABLE()
#define BOARD_ENABLE_GW_UART_CLK()     __HAL_RCC_USART3_CLK_ENABLE()
#define BOARD_ENABLE_SPI1_CLK()        __HAL_RCC_SPI1_CLK_ENABLE()
#define BOARD_ENABLE_OLED_I2C_CLK()    __HAL_RCC_I2C1_CLK_ENABLE()
#define BOARD_ENABLE_LIGHT_ADC_CLK()   __HAL_RCC_ADC3_CLK_ENABLE()

#define BOARD_ENABLE_GPIOA_CLK()       __HAL_RCC_GPIOA_CLK_ENABLE()
#define BOARD_ENABLE_GPIOB_CLK()       __HAL_RCC_GPIOB_CLK_ENABLE()
#define BOARD_ENABLE_GPIOC_CLK()       __HAL_RCC_GPIOC_CLK_ENABLE()
#define BOARD_ENABLE_GPIOD_CLK()       __HAL_RCC_GPIOD_CLK_ENABLE()
#define BOARD_ENABLE_GPIOE_CLK()       __HAL_RCC_GPIOE_CLK_ENABLE()
#define BOARD_ENABLE_GPIOF_CLK()       __HAL_RCC_GPIOF_CLK_ENABLE()
#define BOARD_ENABLE_GPIOG_CLK()       __HAL_RCC_GPIOG_CLK_ENABLE()

#endif

__STATIC_INLINE void Board_UART_GPIO_Init_TX(GPIO_TypeDef *port, uint16_t pin, uint32_t alternate)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = pin;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
#if defined(SH_MCU_F407)
	GPIO_InitStruct.Alternate = alternate;
#else
	(void)alternate;
#endif
	HAL_GPIO_Init(port, &GPIO_InitStruct);
}

__STATIC_INLINE void Board_UART_GPIO_Init_RX(GPIO_TypeDef *port, uint16_t pin, uint32_t alternate)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	GPIO_InitStruct.Pin = pin;
#if defined(SH_MCU_F407)
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Alternate = alternate;
#else
	GPIO_InitStruct.Mode = GPIO_MODE_AF_INPUT;
	(void)alternate;
#endif
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(port, &GPIO_InitStruct);
}

#endif
