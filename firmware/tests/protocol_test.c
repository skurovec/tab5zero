#include "protocol.h"
#include <assert.h>
#include <string.h>

static uint8_t jpeg[VIDEO_MAX_JPEG];
static unsigned frames, errors;

static void accept_frame(const uint8_t *data, size_t length)
{
    assert(length == 3);
    assert(memcmp(data, "jpg", 3) == 0);
    ++frames;
}

static void reject_header(const char *reason)
{
    assert(reason && *reason);
    ++errors;
}

int main(void)
{
    static const uint8_t frame[] = {
        'T', '5', 'F', '1', 255, 255, 255, 255, 3, 0, 0, 0, 0, 5, 208, 2, 'j', 'p', 'g',
    };
    video_stream_t stream = {.jpeg = jpeg};
    video_stream_feed(&stream, (const uint8_t *)"noiseT5", 7, accept_frame, reject_header);
    for (size_t index = 0; index < sizeof(frame); ++index) {
        video_stream_feed(&stream, frame + index, 1, accept_frame, reject_header);
    }
    assert(frames == 1 && errors == 1);

    uint8_t batch[4 * sizeof(frame)];
    for (size_t index = 0; index < 4; ++index) memcpy(batch + index * sizeof(frame), frame, sizeof(frame));
    batch[8] = 1;
    batch[10] = 16; // 1 MiB + 1
    batch[sizeof(frame) + 12] = 1;
    video_stream_feed(&stream, batch, sizeof(batch), accept_frame, reject_header);
    assert(frames == 3 && errors == 2);

    // A false header can already contain the beginning of the next valid one.
    uint8_t overlap[8 + sizeof(frame)] = {'T', '5', 'F', '1'};
    memcpy(overlap + 8, frame, sizeof(frame));
    video_stream_feed(&stream, overlap, sizeof(overlap), accept_frame, reject_header);
    assert(frames == 4 && errors == 3);

    uint8_t record[RECORD_SIZE];
    const uint8_t keyboard[] = {2, 0, 4, 5, 6, 7, 8, 9};
    const uint8_t keyboard_record[] = {'T', '5', 'I', '1', 1, 0, 0, 0, 2, 0, 4, 5, 6, 7, 8, 9};
    encode_keyboard(record, keyboard);
    assert(memcmp(record, keyboard_record, sizeof(record)) == 0);

    const uint8_t pointer_record[] = {'T', '5', 'I', '1', 2, 0, 0, 0, 255, 4, 207, 2, 1, 0, 0, 0};
    encode_pointer(record, 1279, 719, true);
    assert(memcmp(record, pointer_record, sizeof(record)) == 0);
    encode_pointer(record, 1279, 719, false);
    assert(record[12] == 0);
}
