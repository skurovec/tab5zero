#include "video.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ppa.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "menu.h"
#include "nvs.h"

/* The panel is natively portrait: VIDEO_HEIGHT pixels wide and VIDEO_WIDTH pixels tall. */
enum {
    PANEL_WIDTH = VIDEO_HEIGHT,
    PANEL_HEIGHT = VIDEO_WIDTH,
    FRAME_BYTES = VIDEO_WIDTH * VIDEO_HEIGHT * 2,
    /* Portrait shows the frame at 9/16 scale, exactly PANEL_WIDTH wide, centred vertically. */
    PORTRAIT_HEIGHT = VIDEO_HEIGHT * 9 / 16,
    PORTRAIT_OFFSET_Y = (PANEL_HEIGHT - PORTRAIT_HEIGHT) / 2,
    MENU_REFRESH_MS = 1000,
};

typedef struct {
    ppa_srm_rotation_angle_t angle; /* counter-clockwise, as PPA defines it */
    bool portrait;
} orientation_t;

static const orientation_t orientations[ROTATION_COUNT] = {
    [ROTATION_0] = {PPA_SRM_ROTATION_ANGLE_270, false},
    [ROTATION_90] = {PPA_SRM_ROTATION_ANGLE_180, true},
    [ROTATION_180] = {PPA_SRM_ROTATION_ANGLE_90, false},
    [ROTATION_270] = {PPA_SRM_ROTATION_ANGLE_0, true},
};

static const char *TAG = "video";
static const char *SETTINGS_NAMESPACE = "display", *ROTATION_KEY = "rotation";
static SemaphoreHandle_t frame_lock, frame_ready, refresh_done;
static uint8_t *pending_jpeg, *decode_jpeg;
static size_t pending_length;
static ppa_client_handle_t rotation_client, fill_client;
static uint16_t *decoded, *composed, *framebuffers[2];
/* Whether each framebuffer's portrait letterbox bars are known to be black. */
static bool letterbox_black[2];
static size_t decoded_capacity;
static jpeg_decoder_handle_t decoder;
static esp_lcd_panel_handle_t panel;
static atomic_uint rotation = ROTATION_0;

void video_submit(const uint8_t *data, size_t length)
{
    if (!data || !length || length > VIDEO_MAX_JPEG) return;
    if (xSemaphoreTake(frame_lock, portMAX_DELAY) != pdTRUE) abort();
    memcpy(pending_jpeg, data, length);
    pending_length = length;
    xSemaphoreGive(frame_lock);
    xSemaphoreGive(frame_ready);
}

void video_refresh(void)
{
    xSemaphoreGive(frame_ready);
}

rotation_t video_rotation(void)
{
    return atomic_load(&rotation);
}

void video_set_rotation(rotation_t value)
{
    if (value >= ROTATION_COUNT || atomic_exchange(&rotation, value) == value) return;
    video_refresh();
    nvs_handle_t settings;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &settings);
    if (result == ESP_OK) {
        result = nvs_set_u8(settings, ROTATION_KEY, value);
        if (result == ESP_OK) result = nvs_commit(settings);
        nvs_close(settings);
    }
    if (result != ESP_OK) ESP_LOGW(TAG, "Rotation applied but not saved: %s", esp_err_to_name(result));
}

bool video_touch_to_frame(uint16_t panel_x, uint16_t panel_y, uint16_t *x, uint16_t *y)
{
    if (panel_x >= PANEL_WIDTH || panel_y >= PANEL_HEIGHT) return false;
    int frame_x, frame_y;
    rotation_t current = video_rotation();
    if (current == ROTATION_0) {
        frame_x = panel_y;
        frame_y = PANEL_WIDTH - 1 - panel_x;
    } else if (current == ROTATION_180) {
        frame_x = PANEL_HEIGHT - 1 - panel_y;
        frame_y = panel_x;
    } else {
        int block_x = panel_x, block_y = (int)panel_y - PORTRAIT_OFFSET_Y;
        if (block_y < 0 || block_y >= PORTRAIT_HEIGHT) return false;
        if (current == ROTATION_90) {
            block_x = PANEL_WIDTH - 1 - block_x;
            block_y = PORTRAIT_HEIGHT - 1 - block_y;
        }
        frame_x = block_x * 16 / 9;
        frame_y = block_y * 16 / 9;
    }
    *x = frame_x < VIDEO_WIDTH ? frame_x : VIDEO_WIDTH - 1;
    *y = frame_y < VIDEO_HEIGHT ? frame_y : VIDEO_HEIGHT - 1;
    return true;
}

static bool refresh_callback(esp_lcd_panel_handle_t handle, esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(refresh_done, &wake);
    return wake == pdTRUE;
}

static void present(const uint16_t *source, unsigned *next)
{
    const orientation_t *orientation = &orientations[video_rotation()];
    uint16_t *target = framebuffers[*next];
    if (orientation->portrait && !letterbox_black[*next]) {
        const ppa_fill_oper_config_t clear = {
            .out = {
                .buffer = target,
                .buffer_size = FRAME_BYTES,
                .pic_w = PANEL_WIDTH,
                .pic_h = PANEL_HEIGHT,
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
            },
            .fill_block_w = PANEL_WIDTH,
            .fill_block_h = PANEL_HEIGHT,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        ESP_ERROR_CHECK(ppa_do_fill(fill_client, &clear));
    }
    letterbox_black[*next] = orientation->portrait;
    const float scale = orientation->portrait ? 9.0f / 16 : 1;
    const ppa_srm_oper_config_t transform = {
        .in = {
            .buffer = source,
            .pic_w = VIDEO_WIDTH,
            .pic_h = VIDEO_HEIGHT,
            .block_w = VIDEO_WIDTH,
            .block_h = VIDEO_HEIGHT,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = target,
            .buffer_size = FRAME_BYTES,
            .pic_w = PANEL_WIDTH,
            .pic_h = PANEL_HEIGHT,
            .block_offset_y = orientation->portrait ? PORTRAIT_OFFSET_Y : 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = orientation->angle,
        .scale_x = scale,
        .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(rotation_client, &transform));
    xSemaphoreTake(refresh_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, PANEL_WIDTH, PANEL_HEIGHT, target));
    /* Two refreshes ensure the previous framebuffer is no longer scanned. */
    for (unsigned i = 0; i < 2; ++i) {
        ESP_ERROR_CHECK(xSemaphoreTake(refresh_done, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    }
    *next ^= 1;
}

static bool decode(size_t jpeg_length)
{
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    jpeg_decode_picture_info_t info;
    esp_err_t result = jpeg_decoder_get_info(decode_jpeg, jpeg_length, &info);
    if (result == ESP_OK && (info.width != VIDEO_WIDTH || info.height != VIDEO_HEIGHT)) {
        result = ESP_ERR_INVALID_SIZE;
    }
    uint32_t produced = 0;
    if (result == ESP_OK) {
        result = jpeg_decoder_process(decoder, &config, decode_jpeg, jpeg_length,
            (uint8_t *)decoded, decoded_capacity, &produced);
    }
    if (result != ESP_OK || produced != FRAME_BYTES) {
        ESP_LOGW(TAG, "Rejected 1280x720 JPEG: %s, output=%lu", esp_err_to_name(result), (unsigned long)produced);
        return false;
    }
    return true;
}

static void decode_task(void *argument)
{
    unsigned next = 1;
    for (;;) {
        // While the menu is open, redraw periodically so its battery reading stays current.
        xSemaphoreTake(frame_ready, menu_is_open() ? pdMS_TO_TICKS(MENU_REFRESH_MS) : portMAX_DELAY);
        if (xSemaphoreTake(frame_lock, portMAX_DELAY) != pdTRUE) abort();
        size_t jpeg_length = pending_length;
        pending_length = 0;
        // Swap buffers so USB can replace the pending frame while JPEG DMA runs.
        uint8_t *previous = decode_jpeg;
        decode_jpeg = pending_jpeg;
        pending_jpeg = previous;
        xSemaphoreGive(frame_lock);
        if (jpeg_length && !decode(jpeg_length)) continue;
        const uint16_t *source = decoded;
        if (menu_is_open()) {
            memcpy(composed, decoded, FRAME_BYTES);
            menu_draw(composed);
            source = composed;
        }
        present(source, &next);
    }
}

static void load_rotation(void)
{
    nvs_handle_t settings;
    uint8_t saved;
    esp_err_t result = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &settings);
    if (result == ESP_OK) {
        result = nvs_get_u8(settings, ROTATION_KEY, &saved);
        nvs_close(settings);
    }
    if (result == ESP_OK && saved < ROTATION_COUNT) {
        atomic_store(&rotation, saved);
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Using default rotation; saved value unavailable: %s", esp_err_to_name(result));
    }
}

esp_err_t video_start(void)
{
    load_rotation();
    frame_lock = xSemaphoreCreateMutex();
    frame_ready = xSemaphoreCreateBinary();
    refresh_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(frame_lock && frame_ready && refresh_done, ESP_ERR_NO_MEM, TAG, "Frame synchronization allocation");
    const ppa_client_config_t rotation_config = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    ESP_RETURN_ON_ERROR(ppa_register_client(&rotation_config, &rotation_client), TAG, "Rotation engine");
    const ppa_client_config_t fill_config = {.oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1};
    ESP_RETURN_ON_ERROR(ppa_register_client(&fill_config, &fill_client), TAG, "Fill engine");
    jpeg_decode_memory_alloc_cfg_t memory = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    size_t capacity;
    pending_jpeg = jpeg_alloc_decoder_mem(VIDEO_MAX_JPEG, &memory, &capacity);
    decode_jpeg = jpeg_alloc_decoder_mem(VIDEO_MAX_JPEG, &memory, &capacity);
    memory.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    decoded = jpeg_alloc_decoder_mem(FRAME_BYTES, &memory, &decoded_capacity);
    composed = heap_caps_aligned_calloc(128, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(pending_jpeg && decode_jpeg && decoded && composed, ESP_ERR_NO_MEM, TAG, "Frame buffer allocation");
    // Black until the host sends a frame, so the menu also works without a host.
    memset(decoded, 0, FRAME_BYTES);
    const jpeg_decode_engine_cfg_t engine = {.timeout_ms = 200};
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine, &decoder), TAG, "JPEG engine");
    const bsp_display_config_t display = {.dsi_bus = {
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    }};
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(bsp_display_new(&display, &panel, &io), TAG, "Panel initialization");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, (void **)&framebuffers[0], (void **)&framebuffers[1]), TAG, "Panel framebuffers");
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = refresh_callback};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL), TAG, "Panel callbacks");
    esp_err_t visible = esp_lcd_panel_disp_on_off(panel, true);
    ESP_RETURN_ON_FALSE(visible == ESP_OK || visible == ESP_ERR_NOT_SUPPORTED, visible, TAG, "Panel display enable");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "Panel backlight");
    ESP_RETURN_ON_FALSE(xTaskCreate(decode_task, "jpeg_display", 6144, NULL, 6, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "Decoder task");
    return ESP_OK;
}
