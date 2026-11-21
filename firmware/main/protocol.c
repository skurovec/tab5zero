#include "protocol.h"
#include <string.h>

static const uint8_t magic[] = "T5F1";

static uint16_t read_short(const uint8_t *bytes)
{
    return bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void discard_byte(video_stream_t *stream, void (*invalid)(const char *), const char *reason)
{
    if (!stream->resyncing) {
        invalid(reason);
        stream->resyncing = true;
    }
    --stream->header_used;
    memmove(stream->header, stream->header + 1, stream->header_used);
}

void video_stream_feed(video_stream_t *stream, const uint8_t *bytes, size_t length,
                       void (*frame)(const uint8_t *, size_t),
                       void (*invalid)(const char *))
{
    while (length) {
        if (stream->jpeg_length) {
            size_t copy = stream->jpeg_length - stream->jpeg_used;
            if (copy > length) copy = length;
            memcpy(stream->jpeg + stream->jpeg_used, bytes, copy);
            stream->jpeg_used += copy;
            bytes += copy;
            length -= copy;
            if (stream->jpeg_used == stream->jpeg_length) {
                frame(stream->jpeg, stream->jpeg_length);
                stream->jpeg_used = stream->jpeg_length = 0;
            }
            continue;
        }

        stream->header[stream->header_used++] = *bytes++;
        --length;
        while (stream->header_used) {
            size_t prefix = stream->header_used < 4 ? stream->header_used : 4;
            if (memcmp(stream->header, magic, prefix) != 0) {
                discard_byte(stream, invalid, "Bad frame magic");
                continue;
            }
            if (stream->header_used < sizeof(stream->header)) break;
            size_t jpeg_length = read_short(stream->header + 8) |
                                 ((uint32_t)read_short(stream->header + 10) << 16);
            if (!jpeg_length || jpeg_length > VIDEO_MAX_JPEG) {
                discard_byte(stream, invalid, "Invalid JPEG length");
                continue;
            }
            if (read_short(stream->header + 12) != VIDEO_WIDTH ||
                read_short(stream->header + 14) != VIDEO_HEIGHT) {
                discard_byte(stream, invalid, "Invalid frame dimensions");
                continue;
            }
            stream->jpeg_length = jpeg_length;
            stream->header_used = 0;
            stream->resyncing = false;
        }
    }
}

static void encode_prefix(uint8_t record[RECORD_SIZE], uint8_t type)
{
    memset(record, 0, RECORD_SIZE);
    memcpy(record, "T5I1", 4);
    record[4] = type;
}

void encode_keyboard(uint8_t record[RECORD_SIZE], const uint8_t report[8])
{
    encode_prefix(record, 1);
    memcpy(record + 8, report, 8);
}

void encode_pointer(uint8_t record[RECORD_SIZE], uint16_t x, uint16_t y, bool pressed)
{
    encode_prefix(record, 2);
    record[8] = x & 0xff;
    record[9] = x >> 8;
    record[10] = y & 0xff;
    record[11] = y >> 8;
    record[12] = pressed;
}
