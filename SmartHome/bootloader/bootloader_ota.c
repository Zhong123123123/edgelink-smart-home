#include "bootloader_ota.h"

#include "bootloader_config.h"
#include "bootloader_flash.h"
#include "bootloader_protocol.h"
#include "bootloader_uart.h"
#include "board_pins.h"

#include <string.h>
#include "stm32f4xx_hal.h"

static void bl_dbg_putc(char c) {
    while ((USART1->SR & USART_SR_TXE) == 0U) { }
    USART1->DR = (uint16_t)c;
}

static void bl_dbg_puts(const char* s) {
    while (*s != '\0') {
        if (*s == '\n') bl_dbg_putc('\r');
        bl_dbg_putc(*s++);
    }
}

static void bl_dbg_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    bl_dbg_puts("0x");
    for (i = 7; i >= 0; --i) {
        bl_dbg_putc(hex[(v >> (i * 4)) & 0xFU]);
    }
}

static void bl_dbg_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0U) {
        bl_dbg_putc('0');
        return;
    }
    while (v > 0U && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) {
        bl_dbg_putc(buf[--i]);
    }
}

static void bl_dbg_uart_snapshot(const char* tag) {
    USART_TypeDef* u = BOARD_GW_UART_INSTANCE;
    bl_dbg_puts("[BL][UART] ");
    bl_dbg_puts(tag);
    bl_dbg_puts(" sr=");
    bl_dbg_hex32((uint32_t)u->SR);
    bl_dbg_puts(" cr1=");
    bl_dbg_hex32((uint32_t)u->CR1);
    bl_dbg_puts(" cr2=");
    bl_dbg_hex32((uint32_t)u->CR2);
    bl_dbg_puts(" cr3=");
    bl_dbg_hex32((uint32_t)u->CR3);
    bl_dbg_puts(" brr=");
    bl_dbg_hex32((uint32_t)u->BRR);
    bl_dbg_puts("\n");
}

enum {
    BL_ERR_BAD_PAYLOAD = 1U,
    BL_ERR_BAD_CHUNK_SIZE = 2U,
    BL_ERR_BAD_TARGET = 3U,
    BL_ERR_ERASE_FAILED = 4U,
    BL_ERR_BAD_OFFSET = 5U,
    BL_ERR_WRITE_FAILED = 6U,
    BL_ERR_BAD_CRC32 = 7U,
    BL_ERR_BAD_VECTOR = 8U,
    BL_ERR_META_COMMIT = 9U,
    BL_ERR_ROLLBACK = 10U
};

typedef struct {
    uint8_t active;
    uint16_t expected_seq;
    uint32_t target_base;
    uint32_t image_size;
    uint32_t image_crc32;
    uint16_t chunk_size;
    uint32_t image_version;
    uint32_t write_offset;
} bl_ota_ctx_t;

static bl_ota_ctx_t g_ctx;

static uint16_t bl_crc16_modbus(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t bit;
    for (i = 0U; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static void bl_push_u16(uint8_t* out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void bl_push_u32(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)((v >> 8U) & 0xFFU);
    out[2] = (uint8_t)((v >> 16U) & 0xFFU);
    out[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static uint16_t bl_read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static uint32_t bl_read_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static int bl_send_frame(uint8_t type, const uint8_t* payload, uint8_t payload_len) {
    uint8_t frame[64];
    uint16_t crc;
    uint8_t idx = 0U;
    uint8_t wire_len = (uint8_t)(payload_len + 1U);
    if (wire_len > (uint8_t)(sizeof(frame) - 5U)) {
        return 0;
    }
    frame[idx++] = 0xAAU;
    frame[idx++] = 0x55U;
    frame[idx++] = wire_len;
    frame[idx++] = type;
    if (payload_len > 0U && payload != 0) {
        memcpy(&frame[idx], payload, payload_len);
        idx = (uint8_t)(idx + payload_len);
    }
    crc = bl_crc16_modbus(&frame[2], (uint16_t)(wire_len + 1U));
    frame[idx++] = (uint8_t)(crc & 0xFFU);
    frame[idx++] = (uint8_t)((crc >> 8U) & 0xFFU);
    {
        int wr = bl_uart_write_bytes(frame, idx);
        bl_dbg_puts("[BL][OTA] tx type=");
        bl_dbg_hex32((uint32_t)type);
        bl_dbg_puts(" payload_len=");
        bl_dbg_dec((uint32_t)payload_len);
        bl_dbg_puts(" bytes=");
        bl_dbg_dec((uint32_t)idx);
        bl_dbg_puts(" wr=");
        bl_dbg_dec((uint32_t)wr);
        bl_dbg_puts("\n");
        return wr;
    }
}

static void bl_send_ack(uint16_t seq) {
    uint8_t payload[2];
    int ok;
    uint32_t now_ms;
    bl_push_u16(payload, seq);
    bl_dbg_puts("[BL][OTA] ack begin seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts("\n");
    ok = bl_send_frame(BL_UP_OTA_ACK, payload, sizeof(payload));
    bl_dbg_puts("[BL][OTA] ack end seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" ok=");
    bl_dbg_dec((uint32_t)ok);
    bl_dbg_puts("\n");
    now_ms = HAL_GetTick();
    bl_dbg_puts("[BL][OTA] ack sent seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" t=");
    bl_dbg_dec(now_ms);
    bl_dbg_puts(" exp=");
    bl_dbg_hex32((uint32_t)g_ctx.expected_seq);
    bl_dbg_puts(" woff=");
    bl_dbg_hex32(g_ctx.write_offset);
    bl_dbg_puts("\n");
}

static void bl_send_nack(uint16_t seq, uint16_t err) {
    uint8_t payload[4];
    int ok;
    bl_push_u16(payload, seq);
    bl_push_u16(&payload[2], err);
    bl_dbg_puts("[BL][OTA] nack begin seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" err=");
    bl_dbg_hex32((uint32_t)err);
    bl_dbg_puts("\n");
    ok = bl_send_frame(BL_UP_OTA_NACK, payload, sizeof(payload));
    bl_dbg_puts("[BL][OTA] nack end seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" err=");
    bl_dbg_hex32((uint32_t)err);
    bl_dbg_puts(" ok=");
    bl_dbg_dec((uint32_t)ok);
    bl_dbg_puts("\n");
}

static int bl_read_frame(uint8_t* type, uint8_t* payload, uint8_t* payload_len, uint32_t timeout_ms) {
    uint8_t b = 0U;
    uint8_t wire_len = 0U;
    uint16_t i;
    uint8_t crc_buf[260];
    uint16_t wire_crc;
    uint16_t calc_crc;
    uint8_t c0;
    uint8_t c1;

    while (1) {
        if (!bl_uart_read_byte(&b, timeout_ms)) {
            return 0;
        }
        if (b == 0xAAU) {
            if (!bl_uart_read_byte(&b, timeout_ms)) {
                return 0;
            }
            if (b == 0x55U) {
                break;
            }
        }
    }

    if (!bl_uart_read_byte(&wire_len, timeout_ms) || wire_len < 1U || wire_len > 255U) {
        return 0;
    }
    if (!bl_uart_read_byte(type, timeout_ms)) {
        return 0;
    }
    *payload_len = (uint8_t)(wire_len - 1U);
    for (i = 0U; i < *payload_len; ++i) {
        if (!bl_uart_read_byte(&payload[i], timeout_ms)) {
            return 0;
        }
    }
    if (!bl_uart_read_byte(&c0, timeout_ms) || !bl_uart_read_byte(&c1, timeout_ms)) {
        return 0;
    }
    wire_crc = (uint16_t)c0 | ((uint16_t)c1 << 8U);

    crc_buf[0] = wire_len;
    crc_buf[1] = *type;
    if (*payload_len > 0U) {
        memcpy(&crc_buf[2], payload, *payload_len);
    }
    calc_crc = bl_crc16_modbus(crc_buf, (uint16_t)(wire_len + 1U));
    return (wire_crc == calc_crc) ? 1 : 0;
}

static int bl_vector_valid_for_slot(uint32_t slot_base, uint32_t slot_size) {
    uint32_t sp = *(const uint32_t*)slot_base;
    uint32_t pc = *(const uint32_t*)(slot_base + 4U);
    if (sp < 0x20000000UL || sp >= 0x20030000UL) {
        return 0;
    }
    if (pc < slot_base || pc >= (slot_base + slot_size)) {
        return 0;
    }
    return 1;
}

static uint32_t bl_pick_target_slot(const ota_metadata_t* meta) {
    if (meta->active_slot == BL_SLOT_A) {
        return BL_APP_B_BASE;
    }
    return BL_APP_A_BASE;
}

static int bl_prepare_target_allowed(const ota_metadata_t* meta, uint32_t target_base) {
    if (!bl_flash_is_slot_base(target_base)) {
        return 0;
    }
    if ((meta->app_a_state == IMG_INVALID || meta->app_a_state == IMG_ROLLBACK) &&
        (meta->app_b_state == IMG_INVALID || meta->app_b_state == IMG_ROLLBACK)) {
        return 1;
    }
    if (meta->active_slot == BL_SLOT_NONE || meta->confirmed_slot == BL_SLOT_NONE) {
        return 1;
    }
    if (meta->active_slot == BL_SLOT_A && target_base == BL_APP_A_BASE) {
        return 0;
    }
    if (meta->active_slot == BL_SLOT_B && target_base == BL_APP_B_BASE) {
        return 0;
    }
    if (meta->confirmed_slot == BL_SLOT_A && target_base == BL_APP_A_BASE) {
        return 0;
    }
    if (meta->confirmed_slot == BL_SLOT_B && target_base == BL_APP_B_BASE) {
        return 0;
    }
    return 1;
}

void bl_ota_send_hello(const ota_metadata_t* meta) {
    uint8_t payload[8];
    int wr;
    payload[0] = (uint8_t)BL_META_SCHEMA_VERSION;
    payload[1] = meta->active_slot;
    payload[2] = meta->pending_slot;
    payload[3] = meta->confirmed_slot;
    bl_push_u32(&payload[4], meta->rollback_count);
    bl_dbg_puts("[BL][UART] hello cfg uart=");
    bl_dbg_hex32((uint32_t)BOARD_GW_UART_INSTANCE);
    bl_dbg_puts(" tx_port=");
    bl_dbg_hex32((uint32_t)BOARD_GW_TX_PORT);
    bl_dbg_puts(" tx_pin=");
    bl_dbg_hex32((uint32_t)BOARD_GW_TX_PIN);
    bl_dbg_puts(" rx_port=");
    bl_dbg_hex32((uint32_t)BOARD_GW_RX_PORT);
    bl_dbg_puts(" rx_pin=");
    bl_dbg_hex32((uint32_t)BOARD_GW_RX_PIN);
    bl_dbg_puts(" baud=");
    bl_dbg_dec((uint32_t)BOARD_UART_BAUD_DEFAULT);
    bl_dbg_puts(" 8N1 flow=none\n");
    bl_dbg_uart_snapshot("hello pre");
    wr = bl_send_frame(BL_UP_BOOT_HELLO, payload, sizeof(payload));
    bl_dbg_puts("[BL][UART] hello tx uart=");
    bl_dbg_hex32((uint32_t)BOARD_GW_UART_INSTANCE);
    bl_dbg_puts(" len=");
    bl_dbg_dec((uint32_t)bl_uart_last_tx_req_len());
    bl_dbg_puts(" ret=");
    bl_dbg_dec((uint32_t)bl_uart_last_tx_hal_status());
    bl_dbg_puts(" wr=");
    bl_dbg_dec((uint32_t)wr);
    bl_dbg_puts("\n");
    bl_dbg_uart_snapshot("hello post");
}

static void bl_ctx_reset(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));
}

static void bl_handle_prepare(const uint8_t* payload, uint8_t len, ota_metadata_t* meta) {
    uint16_t seq;
    uint32_t target_base;
    int allowed;
    int erase_ok;
    if (len < 12U) {
        bl_dbg_puts("[BL][OTA] prepare reject: len<12\n");
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_BAD_PAYLOAD);
        bl_dbg_puts("\n");
        bl_send_nack(0U, BL_ERR_BAD_PAYLOAD);
        return;
    }

    seq = bl_read_u16(&payload[0]);
    g_ctx.image_size = bl_read_u32(&payload[2]);
    g_ctx.image_crc32 = bl_read_u32(&payload[6]);
    g_ctx.chunk_size = bl_read_u16(&payload[10]);
    g_ctx.image_version = 0U;
    target_base = bl_pick_target_slot(meta);

    if (len >= 16U) {
        target_base = bl_read_u32(&payload[12]);
    }
    if (len >= 20U) {
        g_ctx.image_version = bl_read_u32(&payload[16]);
    }
    bl_dbg_puts("[BL][OTA] prepare in seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" size=");
    bl_dbg_hex32(g_ctx.image_size);
    bl_dbg_puts(" chunk=");
    bl_dbg_hex32((uint32_t)g_ctx.chunk_size);
    bl_dbg_puts(" crc=");
    bl_dbg_hex32(g_ctx.image_crc32);
    bl_dbg_puts(" target=");
    bl_dbg_hex32(target_base);
    bl_dbg_puts(" ver=");
    bl_dbg_hex32(g_ctx.image_version);
    bl_dbg_puts("\n");
    if (g_ctx.image_size == 0U || g_ctx.image_size > BL_APP_A_SIZE) {
        bl_dbg_puts("[BL][OTA] prepare reject: bad size\n");
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_BAD_PAYLOAD);
        bl_dbg_puts("\n");
        bl_send_nack(seq, BL_ERR_BAD_PAYLOAD);
        return;
    }
    if (g_ctx.chunk_size == 0U || g_ctx.chunk_size > 246U) {
        bl_dbg_puts("[BL][OTA] prepare reject: bad chunk\n");
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_BAD_CHUNK_SIZE);
        bl_dbg_puts("\n");
        bl_send_nack(seq, BL_ERR_BAD_CHUNK_SIZE);
        return;
    }
    allowed = bl_prepare_target_allowed(meta, target_base);
    bl_dbg_puts("[BL][OTA] prepare target_allowed=");
    bl_dbg_dec((uint32_t)allowed);
    bl_dbg_puts("\n");
    if (!allowed) {
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_BAD_TARGET);
        bl_dbg_puts("\n");
        bl_send_nack(seq, BL_ERR_BAD_TARGET);
        return;
    }
    if (g_ctx.image_version != 0U && g_ctx.image_version < bl_metadata_min_allowed_version(meta)) {
        bl_dbg_puts("[BL][OTA] prepare reject: rollback version\n");
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_ROLLBACK);
        bl_dbg_puts("\n");
        bl_send_nack(seq, BL_ERR_ROLLBACK);
        return;
    }
    bl_dbg_puts("[BL][OTA] prepare erase begin target=");
    bl_dbg_hex32(target_base);
    bl_dbg_puts("\n");
    erase_ok = bl_flash_erase_slot(target_base);
    bl_dbg_puts("[BL][OTA] prepare erase result=");
    bl_dbg_dec((uint32_t)erase_ok);
    bl_dbg_puts("\n");
    if (!erase_ok) {
        bl_dbg_puts("[BL][OTA] prepare nack code=");
        bl_dbg_hex32(BL_ERR_ERASE_FAILED);
        bl_dbg_puts("\n");
        bl_send_nack(seq, BL_ERR_ERASE_FAILED);
        return;
    }

    bl_ctx_reset();
    g_ctx.active = 1U;
    g_ctx.expected_seq = (uint16_t)(seq + 1U);
    g_ctx.target_base = target_base;
    g_ctx.image_size = bl_read_u32(&payload[2]);
    g_ctx.image_crc32 = bl_read_u32(&payload[6]);
    g_ctx.chunk_size = bl_read_u16(&payload[10]);
    if (len >= 20U) {
        g_ctx.image_version = bl_read_u32(&payload[16]);
    }
    g_ctx.write_offset = 0U;
    bl_dbg_puts("[BL][OTA] prepare ack path seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts("\n");
    bl_send_ack(seq);
}

static void bl_handle_data(const uint8_t* payload, uint8_t len) {
    uint16_t seq;
    uint32_t offset;
    uint16_t chunk_len;
    uint16_t prev_seq;
    if (!g_ctx.active || len < 8U) {
        bl_send_nack(0U, BL_ERR_BAD_PAYLOAD);
        return;
    }
    seq = bl_read_u16(&payload[0]);
    offset = bl_read_u32(&payload[2]);
    chunk_len = bl_read_u16(&payload[6]);
    bl_dbg_puts("[BL][OTA] data in seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" exp=");
    bl_dbg_hex32((uint32_t)g_ctx.expected_seq);
    bl_dbg_puts(" off=");
    bl_dbg_hex32(offset);
    bl_dbg_puts(" chunk=");
    bl_dbg_hex32((uint32_t)chunk_len);
    bl_dbg_puts(" woff=");
    bl_dbg_hex32(g_ctx.write_offset);
    bl_dbg_puts(" len=");
    bl_dbg_hex32((uint32_t)len);
    bl_dbg_puts("\n");
    prev_seq = (uint16_t)(g_ctx.expected_seq - 1U);
    if (seq == prev_seq &&
        (uint32_t)chunk_len == (uint32_t)(len - 8U) &&
        ((offset + (uint32_t)chunk_len) == g_ctx.write_offset)) {
        bl_dbg_puts("[BL][OTA] data duplicate idempotent ack seq=");
        bl_dbg_hex32((uint32_t)seq);
        bl_dbg_puts(" offset=");
        bl_dbg_hex32(offset);
        bl_dbg_puts(" chunk=");
        bl_dbg_hex32((uint32_t)chunk_len);
        bl_dbg_puts(" write_offset=");
        bl_dbg_hex32(g_ctx.write_offset);
        bl_dbg_puts("\n");
        bl_send_ack(seq);
        return;
    }
    if (seq != g_ctx.expected_seq) {
        bl_dbg_puts("[BL][OTA] data reject: seq mismatch\n");
        bl_send_nack(seq, BL_ERR_BAD_OFFSET);
        return;
    }
    if ((uint32_t)chunk_len != (uint32_t)(len - 8U) || chunk_len > g_ctx.chunk_size) {
        bl_dbg_puts("[BL][OTA] data reject: bad chunk or len mismatch\n");
        bl_send_nack(seq, BL_ERR_BAD_PAYLOAD);
        return;
    }
    if (offset != g_ctx.write_offset || (offset + chunk_len) > g_ctx.image_size) {
        bl_dbg_puts("[BL][OTA] data reject: bad offset window\n");
        bl_send_nack(seq, BL_ERR_BAD_OFFSET);
        return;
    }
    bl_dbg_puts("[BL][OTA] data write begin addr=");
    bl_dbg_hex32(g_ctx.target_base + offset);
    bl_dbg_puts(" bytes=");
    bl_dbg_hex32((uint32_t)chunk_len);
    bl_dbg_puts("\n");
    if (!bl_flash_write(g_ctx.target_base + offset, &payload[8], chunk_len)) {
        bl_dbg_puts("[BL][OTA] data reject: flash write failed\n");
        bl_send_nack(seq, BL_ERR_WRITE_FAILED);
        return;
    }
    g_ctx.write_offset += chunk_len;
    g_ctx.expected_seq = (uint16_t)(g_ctx.expected_seq + 1U);
    bl_dbg_puts("[BL][OTA] data write ok new_woff=");
    bl_dbg_hex32(g_ctx.write_offset);
    bl_dbg_puts(" next_seq=");
    bl_dbg_hex32((uint32_t)g_ctx.expected_seq);
    bl_dbg_puts(" ack_seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts("\n");
    bl_send_ack(seq);
}

static void bl_handle_verify(const uint8_t* payload, uint8_t len) {
    const uint8_t* data;
    uint32_t crc;
    uint16_t seq;
    uint32_t image_crc32;
    if (!g_ctx.active || len < 6U) {
        bl_send_nack(0U, BL_ERR_BAD_PAYLOAD);
        return;
    }
    seq = bl_read_u16(&payload[0]);
    image_crc32 = bl_read_u32(&payload[2]);
    if (seq != g_ctx.expected_seq || image_crc32 != g_ctx.image_crc32 || g_ctx.write_offset != g_ctx.image_size) {
        bl_send_nack(seq, BL_ERR_BAD_PAYLOAD);
        return;
    }
    data = (const uint8_t*)g_ctx.target_base;
    crc = bl_crc32(data, g_ctx.image_size);
    bl_dbg_puts("[BL][OTA] verify seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts(" size=");
    bl_dbg_hex32(g_ctx.image_size);
    bl_dbg_puts(" expect=");
    bl_dbg_hex32(g_ctx.image_crc32);
    bl_dbg_puts(" calc=");
    bl_dbg_hex32(crc);
    bl_dbg_puts("\n");
    if (crc != g_ctx.image_crc32) {
        bl_send_nack(seq, BL_ERR_BAD_CRC32);
        return;
    }
    if (!bl_vector_valid_for_slot(g_ctx.target_base, BL_APP_A_SIZE)) {
        bl_send_nack(seq, BL_ERR_BAD_VECTOR);
        return;
    }
    g_ctx.expected_seq = (uint16_t)(g_ctx.expected_seq + 1U);
    bl_send_ack(seq);
}

static void bl_handle_commit(const uint8_t* payload, uint8_t len, ota_metadata_t* meta) {
    uint16_t seq;
    ota_metadata_t next;
    uint8_t slot;
    if (!g_ctx.active || len < 2U) {
        bl_send_nack(0U, BL_ERR_BAD_PAYLOAD);
        return;
    }
    seq = bl_read_u16(&payload[0]);
    if (seq != g_ctx.expected_seq) {
        bl_send_nack(seq, BL_ERR_BAD_PAYLOAD);
        return;
    }

    next = *meta;
    slot = (g_ctx.target_base == BL_APP_B_BASE) ? BL_SLOT_B : BL_SLOT_A;
    next.pending_slot = slot;
    next.boot_attempts = 0U;
    if (slot == BL_SLOT_A) {
        next.app_a_state = IMG_PENDING;
        next.app_a_size = g_ctx.image_size;
        next.app_a_crc32 = g_ctx.image_crc32;
        next.app_a_version = g_ctx.image_version;
    } else {
        next.app_b_state = IMG_PENDING;
        next.app_b_size = g_ctx.image_size;
        next.app_b_crc32 = g_ctx.image_crc32;
        next.app_b_version = g_ctx.image_version;
    }
    next.meta_seq += 1U;

    if (!bl_metadata_commit(&next)) {
        bl_send_nack(seq, BL_ERR_META_COMMIT);
        return;
    }

    *meta = next;
    bl_send_ack(seq);
    bl_dbg_puts("[BL][OTA] commit ack sent seq=");
    bl_dbg_hex32((uint32_t)seq);
    bl_dbg_puts("\n");
    bl_dbg_puts("[BL][OTA] commit wait before reset ms=500\n");
    HAL_Delay(500U);
    bl_dbg_puts("[BL][OTA] commit reset now\n");
    NVIC_SystemReset();
}

int bl_ota_loop(ota_metadata_t* meta, uint32_t idle_timeout_ms) {
    uint8_t type = 0U;
    uint8_t payload[255];
    uint8_t payload_len = 0U;
    const uint32_t read_timeout_ms = 100U;
    const uint32_t hello_interval_ms = 300U;
    uint32_t idle_elapsed_ms = 0U;
    uint32_t hello_elapsed_ms = 0U;
    uint32_t hello_idx = 0U;
    uint8_t hello_periodic_enabled = 1U;

    bl_ctx_reset();
    bl_ota_send_hello(meta);
    while (1) {
        if (!bl_read_frame(&type, payload, &payload_len, read_timeout_ms)) {
            if (idle_timeout_ms > 0U) {
                idle_elapsed_ms += read_timeout_ms;
            }
            if (hello_periodic_enabled) {
                hello_elapsed_ms += read_timeout_ms;
                if (hello_elapsed_ms >= hello_interval_ms) {
                    ++hello_idx;
                    bl_dbg_puts("[BL][OTA] hello periodic idx=");
                    bl_dbg_dec(hello_idx);
                    bl_dbg_puts(" ts=");
                    bl_dbg_dec(HAL_GetTick());
                    bl_dbg_puts("\n");
                    bl_ota_send_hello(meta);
                    hello_elapsed_ms = 0U;
                }
            }
            if (idle_timeout_ms > 0U && idle_elapsed_ms >= idle_timeout_ms) {
                return 0;
            }
            continue;
        }

        if (type == BL_DN_OTA_PREPARE) {
            idle_elapsed_ms = 0U;
            if (hello_periodic_enabled) {
                hello_periodic_enabled = 0U;
                bl_dbg_puts("[BL][OTA] rx prepare, stop hello periodic\n");
            }
            bl_handle_prepare(payload, payload_len, meta);
        } else if (type == BL_DN_OTA_DATA) {
            idle_elapsed_ms = 0U;
            bl_handle_data(payload, payload_len);
        } else if (type == BL_DN_OTA_VERIFY) {
            idle_elapsed_ms = 0U;
            bl_handle_verify(payload, payload_len);
        } else if (type == BL_DN_OTA_COMMIT) {
            idle_elapsed_ms = 0U;
            bl_handle_commit(payload, payload_len, meta);
        } else if (type == BL_DN_OTA_ABORT) {
            uint16_t seq = (payload_len >= 2U) ? bl_read_u16(payload) : 0U;
            idle_elapsed_ms = 0U;
            bl_ctx_reset();
            bl_send_ack(seq);
        } else if (type == BL_DN_GET_VERSION) {
            uint8_t resp[8];
            idle_elapsed_ms = 0U;
            bl_push_u32(resp, meta->app_a_version);
            bl_push_u32(&resp[4], meta->app_b_version);
            (void)bl_send_frame(BL_UP_VERSION_REPORT, resp, sizeof(resp));
        }
    }
    return 0;
}
