#ifndef __DRIVER_BUFFER_H
#define __DRIVER_BUFFER_H

#include "main.h"

typedef struct{
	uint8_t *fifo;
	uint16_t pw;
	uint16_t pr;
	uint16_t buf_size;
} RingBuffer, *ptRingBuffer;

int Driver_Buffer_Init(ptRingBuffer buffer, uint16_t size);
int Driver_Buffer_Write(ptRingBuffer buffer, const uint8_t data);
int Driver_Buffer_WriteBytes(ptRingBuffer buffer, const uint8_t *data_stream, uint8_t len);
int Driver_Buffer_Read(ptRingBuffer buffer, uint8_t *data);
int Driver_Buffer_ReadBytes(ptRingBuffer buffer, uint8_t *data_stream, uint8_t len);
int Driver_Buffer_Clean(ptRingBuffer buffer);

#endif
