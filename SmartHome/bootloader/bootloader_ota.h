#ifndef EDGEOTA_BOOTLOADER_OTA_H
#define EDGEOTA_BOOTLOADER_OTA_H

#include "bootloader_metadata.h"

void bl_ota_send_hello(const ota_metadata_t* meta);
int bl_ota_loop(ota_metadata_t* meta, uint32_t idle_timeout_ms);

#endif
