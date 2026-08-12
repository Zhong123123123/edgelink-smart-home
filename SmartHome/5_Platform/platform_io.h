#ifndef __PLATFORM_IO_H
#define __PLATFORM_IO_H

#include "platform.h"
#include "dev_io.h"

void platform_io_init(struct IODev *dev);
int platform_io_write(struct IODev *dev, uint8_t *buf, uint16_t len);
int platform_io_read(struct IODev *dev, uint8_t *buf, uint16_t len);

#endif


