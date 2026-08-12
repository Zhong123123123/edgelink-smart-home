#ifndef __PLATFORM_DISPLAY_H
#define __PLATFORM_DISPLAY_H

#include "dev_display.h"

int platform_display_init(struct DisplayDev *dev);
int platform_display_show(struct DisplayDev *dev, const DisplayStatus *status);

#endif
