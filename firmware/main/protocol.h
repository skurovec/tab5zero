#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VIDEO_WIDTH = 1280,
    VIDEO_HEIGHT = 720,
    VIDEO_MAX_JPEG = 1048576,
    RECORD_SIZE = 16,
};

typedef struct {
    uint8_t *jpeg;
    uint8_t header[16];
    size_t header_used;
    size_t jpeg_used;
    size_t jpeg_length;
    bool resyncing;
} video_stream_t;

void video_stream_feed(video_stream_t *stream, const uint8_t *bytes, size_t length,
                       void (*frame)(const uint8_t *, size_t),
                       void (*invalid)(const char *));
void encode_keyboard(uint8_t record[RECORD_SIZE], const uint8_t report[8]);
void encode_pointer(uint8_t record[RECORD_SIZE], uint16_t x, uint16_t y, bool pressed);
