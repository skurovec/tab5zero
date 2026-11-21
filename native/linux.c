#define _GNU_SOURCE
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIDTH 1280
#define HEIGHT 720

struct framebuffer {
    int fd;
    unsigned char *memory;
    struct fb_fix_screeninfo fixed;
    struct fb_var_screeninfo variable;
};

static int checked_ioctl(int fd, unsigned long request, void *argument) {
    int result;
    do { result = ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : result;
}

static int valid_framebuffer(struct framebuffer *capture) {
    struct fb_var_screeninfo *v = &capture->variable;
    unsigned bytes = v->bits_per_pixel / 8;
    if (capture->fixed.type != FB_TYPE_PACKED_PIXELS ||
        capture->fixed.visual != FB_VISUAL_TRUECOLOR || v->nonstd || v->grayscale ||
        (v->bits_per_pixel != 16 && v->bits_per_pixel != 24 && v->bits_per_pixel != 32) ||
        !v->xres || !v->yres || v->rotate ||
        (uint64_t)v->xoffset + v->xres > v->xres_virtual ||
        (uint64_t)v->yoffset + v->yres > v->yres_virtual ||
        ((uint64_t)v->xoffset + v->xres) * bytes > capture->fixed.line_length ||
        ((uint64_t)v->yoffset + v->yres - 1) * capture->fixed.line_length +
            ((uint64_t)v->xoffset + v->xres) * bytes > capture->fixed.smem_len)
        return -ENOTSUP;
    struct fb_bitfield fields[] = {v->red, v->green, v->blue};
    uint32_t used_bits = 0;
    for (unsigned index = 0; index < 3; ++index) {
        struct fb_bitfield field = fields[index];
        if (!field.length || field.length > 8 || field.msb_right ||
            field.offset >= v->bits_per_pixel || field.length > v->bits_per_pixel - field.offset)
            return -ENOTSUP;
        uint32_t mask = ((1u << field.length) - 1) << field.offset;
        if (used_bits & mask) return -ENOTSUP;
        used_bits |= mask;
    }
    return 0;
}

void *tab5_fb_open(const char *path) {
    struct framebuffer *capture = calloc(1, sizeof(*capture));
    if (!capture) return NULL;
    capture->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (capture->fd < 0) { free(capture); return NULL; }
    int result = checked_ioctl(capture->fd, FBIOGET_FSCREENINFO, &capture->fixed);
    if (result >= 0) result = checked_ioctl(capture->fd, FBIOGET_VSCREENINFO, &capture->variable);
    if (result >= 0) result = valid_framebuffer(capture);
    if (result < 0) {
        close(capture->fd); free(capture); errno = -result; return NULL;
    }
    capture->memory = mmap(NULL, capture->fixed.smem_len, PROT_READ, MAP_SHARED, capture->fd, 0);
    if (capture->memory == MAP_FAILED) {
        int error = errno; close(capture->fd); free(capture); errno = error; return NULL;
    }
    return capture;
}

static unsigned char channel(uint32_t pixel, struct fb_bitfield field) {
    unsigned maximum = (1u << field.length) - 1;
    return (unsigned char)((((pixel >> field.offset) & maximum) * 255u + maximum / 2) / maximum);
}

int tab5_fb_capture(void *handle, unsigned char *rgb, size_t length) {
    struct framebuffer *capture = handle;
    if (length != WIDTH * HEIGHT * 3) return -EINVAL;
    struct fb_var_screeninfo current = {0};
    struct fb_fix_screeninfo fixed = {0};
    int result = checked_ioctl(capture->fd, FBIOGET_VSCREENINFO, &current);
    if (result < 0) return result;
    result = checked_ioctl(capture->fd, FBIOGET_FSCREENINFO, &fixed);
    if (result < 0) return result;
    /* Pan offsets may change; format/geometry changes require reopening the mapping. */
    struct fb_var_screeninfo original = capture->variable;
    if (fixed.smem_start != capture->fixed.smem_start || fixed.smem_len != capture->fixed.smem_len ||
        fixed.line_length != capture->fixed.line_length || fixed.type != capture->fixed.type ||
        fixed.visual != capture->fixed.visual || current.xres != original.xres ||
        current.yres != original.yres || current.xres_virtual != original.xres_virtual ||
        current.yres_virtual != original.yres_virtual || current.bits_per_pixel != original.bits_per_pixel ||
        current.nonstd != original.nonstd || current.grayscale != original.grayscale ||
        current.rotate != original.rotate || memcmp(&current.red, &original.red, sizeof(current.red)) ||
        memcmp(&current.green, &original.green, sizeof(current.green)) ||
        memcmp(&current.blue, &original.blue, sizeof(current.blue))) return -ESTALE;
    capture->variable = current;
    result = valid_framebuffer(capture);
    if (result < 0) return result;
    unsigned bytes = current.bits_per_pixel / 8;
    for (unsigned y = 0; y < HEIGHT; ++y) {
        unsigned source_y = current.yoffset + (uint64_t)y * current.yres / HEIGHT;
        const unsigned char *line = capture->memory + (size_t)source_y * capture->fixed.line_length;
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned source_x = current.xoffset + (uint64_t)x * current.xres / WIDTH;
            uint32_t pixel = 0;
            memcpy(&pixel, line + (size_t)source_x * bytes, bytes);
            size_t output = ((size_t)y * WIDTH + x) * 3;
            rgb[output] = channel(pixel, current.red);
            rgb[output + 1] = channel(pixel, current.green);
            rgb[output + 2] = channel(pixel, current.blue);
        }
    }
    return 0;
}

int tab5_fb_close(void *handle) {
    struct framebuffer *capture = handle;
    int result = munmap(capture->memory, capture->fixed.smem_len) < 0 ? -errno : 0;
    if (close(capture->fd) < 0 && result == 0) result = -errno;
    free(capture);
    return result;
}

#include <dlfcn.h>
#include <stdio.h>
#include <linux/uinput.h>

int tab5_input_open(int pointer, const unsigned short *keys, size_t count) {
    int descriptor = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) return -errno;
    if (ioctl(descriptor, UI_SET_EVBIT, EV_KEY) < 0) goto failed;
    /* Only transitions arrive over USB; the kernel generates repeats for held keys. */
    if (!pointer && ioctl(descriptor, UI_SET_EVBIT, EV_REP) < 0) goto failed;
    for (size_t i = 0; i < count; ++i) {
        if (ioctl(descriptor, UI_SET_KEYBIT, keys[i]) < 0) goto failed;
    }
    if (pointer) {
        if (ioctl(descriptor, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(descriptor, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) goto failed;
        for (int axis = ABS_X; axis <= ABS_Y; ++axis) {
            struct uinput_abs_setup range = {
                .code = axis, .absinfo = {.maximum = axis == ABS_X ? 1279 : 719}
            };
            if (ioctl(descriptor, UI_SET_ABSBIT, axis) < 0 ||
                ioctl(descriptor, UI_ABS_SETUP, &range) < 0) goto failed;
        }
    }
    struct uinput_setup setup = {.id = {.bustype = BUS_USB, .vendor = 0x1209, .product = 1}};
    snprintf(setup.name, sizeof(setup.name), "Tab5 %s", pointer ? "touchscreen" : "keyboard");
    if (ioctl(descriptor, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(descriptor, UI_DEV_CREATE) < 0) goto failed;
    return descriptor;
failed:;
    int error = errno;
    if (close(descriptor) < 0) perror("closing failed uinput device");
    return -error;
}

int tab5_input_event(int descriptor, unsigned short type, unsigned short code, int value) {
    struct input_event event = {.type = type, .code = code, .value = value};
    ssize_t written;
    do {
        written = write(descriptor, &event, sizeof(event));
    } while (written < 0 && errno == EINTR);
    return written == sizeof(event) ? 0 : written < 0 ? -errno : -EIO;
}

struct jpeg_encoder {
    void *library;
    void *compressor;
    int (*destroy)(void *);
    void (*release)(unsigned char *);
    int (*compress)(void *, const unsigned char *, int, int, int, int,
                    unsigned char **, unsigned long *, int, int, int);
};

void tab5_jpeg_close(struct jpeg_encoder *encoder) {
    if (encoder->compressor && encoder->destroy(encoder->compressor) < 0)
        fprintf(stderr, "destroying TurboJPEG compressor failed\n");
    if (dlclose(encoder->library) != 0) fprintf(stderr, "closing TurboJPEG: %s\n", dlerror());
    free(encoder);
}

struct jpeg_encoder *tab5_jpeg_open(void) {
    struct jpeg_encoder *encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return NULL;
    encoder->library = dlopen("libturbojpeg.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!encoder->library) {
        fprintf(stderr, "loading TurboJPEG: %s\n", dlerror());
        free(encoder);
        return NULL;
    }
    void *(*initialize)(void) = dlsym(encoder->library, "tjInitCompress");
    encoder->destroy = dlsym(encoder->library, "tjDestroy");
    encoder->release = dlsym(encoder->library, "tjFree");
    encoder->compress = dlsym(encoder->library, "tjCompress2");
    if (!initialize || !encoder->destroy || !encoder->release || !encoder->compress ||
        !(encoder->compressor = initialize())) {
        tab5_jpeg_close(encoder);
        return NULL;
    }
    return encoder;
}

int tab5_jpeg_encode(struct jpeg_encoder *encoder, const unsigned char *rgb, int quality,
                    unsigned char *output, size_t capacity) {
    unsigned char *jpeg = NULL;
    unsigned long length = 0;
    // TurboJPEG legacy ABI: TJPF_RGB = 0, TJSAMP_420 = 2.
    int result = encoder->compress(encoder->compressor, rgb, 1280, 0, 720, 0,
                                   &jpeg, &length, 2, quality, 0);
    if (result == 0 && length && length <= capacity) memcpy(output, jpeg, length);
    encoder->release(jpeg);
    return result ? -EIO : !length || length > capacity ? -EMSGSIZE : (int)length;
}

#include <linux/usb/functionfs.h>

int tab5_endpoint_packet_size(int descriptor) {
    struct usb_endpoint_descriptor endpoint;
    if (ioctl(descriptor, FUNCTIONFS_ENDPOINT_DESC, &endpoint) < 0) return -errno;
    return le16toh(endpoint.wMaxPacketSize);
}
