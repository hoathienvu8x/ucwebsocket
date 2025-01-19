/* websocket.c - websocket lib
 *
 * Copyright (C) 2016 Borislav Sapundzhiev
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "websocket.h"

#if BYTE_ORDER == LITTLE_ENDIAN
#define HTONS(v) (v >> 8) | (v << 8)
#else
#define HTONS(v) v
#endif

#define MASK_LEN 4

void ws_create_frame(struct ws_frame *frame, uint8_t *out_data, int *out_len)
{
    assert(frame->type);
    out_data[0] = 0x80 | frame->type;

    if(frame->payload_length <= 0x7D) {
        out_data[1] = frame->payload_length;
        *out_len = 2;
    } else if (frame->payload_length >= 0x7E && frame->payload_length <= 0xFFFF) {
        out_data[1] = 0x7E;
        out_data[2] = (uint8_t)( frame->payload_length >> 8 ) & 0xFF;
        out_data[3] = (uint8_t)( frame->payload_length      ) & 0xFF;
        *out_len = 4;
    } else {
        //assert(0);
        out_data[1] = 0x7F;
        out_data[2] = (uint8_t)( frame->payload_length >> 56 ) & 0xFF;
        out_data[3] = (uint8_t)( frame->payload_length >> 48 ) & 0xFF;
        out_data[4] = (uint8_t)( frame->payload_length >> 40 ) & 0xFF;
        out_data[5] = (uint8_t)( frame->payload_length >> 32 ) & 0xFF;
        out_data[6] = (uint8_t)( frame->payload_length >> 24 ) & 0xFF;
        out_data[7] = (uint8_t)( frame->payload_length >> 16 ) & 0xFF;
        out_data[8] = (uint8_t)( frame->payload_length >>  8 ) & 0xFF;
        out_data[9] = (uint8_t)( frame->payload_length       ) & 0xFF;
        *out_len = 10;
    }

    memcpy(&out_data[*out_len], frame->payload, frame->payload_length);
    *out_len += frame->payload_length;
}

static int ws_parse_opcode(struct ws_frame *frame)
{
    switch(frame->opcode) {
    case WS_TEXT_FRAME:
    case WS_BINARY_FRAME:
    case WS_CLOSING_FRAME:
    case WS_PING_FRAME:
    case WS_PONG_FRAME:
        frame->type = frame->opcode;
        break;
    default:
        frame->type = WS_ERROR_FRAME; //Reserved frames are also treated as errors
        break;
    }

    return frame->type;
}

ssize_t ws_asserted_read(struct ws_ctx *ctx, void *buf, size_t len) {
    size_t i = 0;
    char *p = buf;
    ssize_t n;
    for (; i < len; i++) {
        if (ctx->buf.pos == 0 || ctx->buf.pos == ctx->buf.len) {
            n = recv(ctx->fd, ctx->buf.data, sizeof(ctx->buf.data), 0);
            if (n <= 0) {
                ctx->buf.pos -= i;
                return n;
            }
        }
        *(p++) = ctx->buf.data[ctx->buf.pos++];
    }
    return (ssize_t)i;
}

ssize_t ws_asserted_write(int fd, const void *buf, size_t len) {
    size_t left = len;
    const char * buf2 = (const char *)buf;
    do {
        ssize_t sent = send(fd, buf2, left, 0);
        if (sent == -1) return -1;
        left -= sent;
        if (sent > 0) buf2 += sent;
    } while (left > 0);
    return (left == 0 ? (ssize_t)len : -1);
}

void ws_parse_frame(struct ws_frame *frame, struct ws_ctx *ctx)
{
    int masked = 0;
    int payloadLength;
    int byte_count = 0;
    int i;
    uint8_t maskingKey[MASK_LEN];
    uint8_t data[BUF_LEN];

    if (ws_asserted_read(ctx, data, 2) < 2) {
        printf("00\n");
        return;
    }

    uint8_t b = data[0];
    frame->fin  = ((b & 0x80) != 0);
    frame->rsv1 = ((b & 0x40) != 0);
    frame->rsv2 = ((b & 0x20) != 0);
    frame->rsv3 = ((b & 0x10) != 0);

    frame->opcode = (uint8_t)(b & 0x0F);

    // TODO: add control frame fin validation here
    // TODO: add frame RSV validation here
    if(ws_parse_opcode(frame) == WS_ERROR_FRAME) {
        return;
    }

    // Masked + Payload Length
    b = data[1];
    masked = ((b & 0x80) != 0);
    payloadLength = (uint8_t)(0x7F & b);

    if (payloadLength == 0x7F) {
        // 8 byte extended payload length
        if (ws_asserted_read(ctx, data, 8) < 8) {
            printf("0\n");
            return;
        }
        byte_count = 8;
    } else if (payloadLength == 0x7E) {
        // 2 bytes extended payload length
        if (ws_asserted_read(ctx, data, 2) < 2) {
            printf("1\n");
            return;
        }
        byte_count = 2;
    }

    if (byte_count > 0) {

        payloadLength = data[byte_count - 1];

        for (i = byte_count - 2; i >= 0; i--) {
            uint8_t bytenum = i;
            payloadLength |= (data[i] << 8 * bytenum);
        }
    }

    if (masked) {
        if (ws_asserted_read(ctx, maskingKey, MASK_LEN) < MASK_LEN) {
            printf("2\n");
            return;
        }
    }

    // TODO: add masked + masking key validation here
    frame->payload = calloc(payloadLength + 1, sizeof(uint8_t));
    assert(frame->payload);

    if (ws_asserted_read(ctx, frame->payload, payloadLength) < payloadLength) {
        free(frame->payload);
        frame->type = WS_INCOMPLETE_FRAME;
        printf("3\n");
        return;
    }

    frame->payload_length = payloadLength;

    if (masked) {
        uint64_t len;
        for (len = 0; len < frame->payload_length; len++) {
            frame->payload[len] ^= maskingKey[len % MASK_LEN];
        }
    }
}

void ws_create_closing_frame(uint8_t *out_data, int *out_len)
{
    struct ws_frame frame;
    frame.payload_length = 0;
    frame.payload = NULL;
    frame.type = WS_CLOSING_FRAME;
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_text_frame(const char *text, uint8_t *out_data, int *out_len)
{
    struct ws_frame frame;
    frame.payload_length = strlen(text);
    frame.payload = (uint8_t *)text;
    frame.type = WS_TEXT_FRAME;
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_binary_frame(const uint8_t *data,uint16_t datalen, uint8_t *out_data, int *out_len)
{
    struct ws_frame frame;
    frame.payload_length = datalen;
    frame.payload = (uint8_t *)data;
    frame.type = WS_BINARY_FRAME;
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_control_frame(enum wsFrameType type, const uint8_t *data, int data_len, uint8_t *out_data, int *out_len)
{
    struct ws_frame frame;
    frame.payload_length = data_len;
    frame.payload = (uint8_t *)data;
    frame.type = type;
    ws_create_frame(&frame, out_data, out_len);
}

