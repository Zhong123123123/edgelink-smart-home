#ifndef EDGEOTA_BOOTLOADER_UART_H
#define EDGEOTA_BOOTLOADER_UART_H

#include <stddef.h>
#include <stdint.h>

void bl_uart_init(void);
int bl_uart_read_byte(uint8_t* out, uint32_t timeout_ms);
int bl_uart_write_bytes(const uint8_t* data, size_t len);
uint32_t bl_uart_last_tx_hal_status(void);
uint32_t bl_uart_last_tx_req_len(void);
void bl_uart_irq_handler(void);
void bl_uart_dma_irq_handler(void);

#endif
