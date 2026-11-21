#include "video.h"
#include <stdlib.h>
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ppa.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "video";
static SemaphoreHandle_t frame_lock, frame_ready, refresh_done;
static uint8_t *pending_jpeg, *decode_jpeg;
static size_t pending_length;
static ppa_client_handle_t rotation;
static uint16_t *decoded, *framebuffers[2];
static size_t decoded_capacity;
static jpeg_decoder_handle_t decoder;
static esp_lcd_panel_handle_t panel;
void video_submit(const uint8_t *data, size_t length)
{
    if (!data || !length || length > VIDEO_MAX_JPEG) return;
    if (xSemaphoreTake(frame_lock, portMAX_DELAY) != pdTRUE) abort();
    memcpy(pending_jpeg, data, length);
    pending_length = length;
    xSemaphoreGive(frame_lock);
    xSemaphoreGive(frame_ready);
}

static bool refresh_callback(esp_lcd_panel_handle_t handle, esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(refresh_done, &wake);
    return wake == pdTRUE;
}

static void decode_task(void *argument)
{
    unsigned next = 1;
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    for (;;) {
        xSemaphoreTake(frame_ready, portMAX_DELAY);
        if (xSemaphoreTake(frame_lock, portMAX_DELAY) != pdTRUE) abort();
        size_t jpeg_length = pending_length;
        pending_length = 0;
        // Swap buffers so USB can replace the pending frame while JPEG DMA runs.
        uint8_t *previous = decode_jpeg;
        decode_jpeg = pending_jpeg;
        pending_jpeg = previous;
        xSemaphoreGive(frame_lock);
        if (!jpeg_length) continue;

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
        if (result != ESP_OK || produced != VIDEO_WIDTH * VIDEO_HEIGHT * 2) {
            ESP_LOGW(TAG, "Rejected 1280x720 JPEG: %s, output=%lu", esp_err_to_name(result), (unsigned long)produced);
            continue;
        }
        uint16_t *target = framebuffers[next];
        const ppa_srm_oper_config_t transform = {
            .in = {
                .buffer = decoded,
                .pic_w = VIDEO_WIDTH,
                .pic_h = VIDEO_HEIGHT,
                .block_w = VIDEO_WIDTH,
                .block_h = VIDEO_HEIGHT,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = target,
                .buffer_size = VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(*target),
                .pic_w = VIDEO_HEIGHT,
                .pic_h = VIDEO_WIDTH,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
            .scale_x = 1,
            .scale_y = 1,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(rotation, &transform));
        xSemaphoreTake(refresh_done, 0);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, VIDEO_HEIGHT, VIDEO_WIDTH, target));
        /* Two refreshes ensure the previous framebuffer is no longer scanned. */
        for (unsigned i = 0; i < 2; ++i) {
            ESP_ERROR_CHECK(xSemaphoreTake(refresh_done, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
        }
        next ^= 1;
    }
}

esp_err_t video_start(void)
{
    frame_lock = xSemaphoreCreateMutex();
    frame_ready = xSemaphoreCreateBinary();
    refresh_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(frame_lock && frame_ready && refresh_done, ESP_ERR_NO_MEM, TAG, "Frame synchronization allocation");
    const ppa_client_config_t rotation_config = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    ESP_RETURN_ON_ERROR(ppa_register_client(&rotation_config, &rotation), TAG, "Rotation engine");
    jpeg_decode_memory_alloc_cfg_t memory = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    size_t capacity;
    pending_jpeg = jpeg_alloc_decoder_mem(VIDEO_MAX_JPEG, &memory, &capacity);
    decode_jpeg = jpeg_alloc_decoder_mem(VIDEO_MAX_JPEG, &memory, &capacity);
    memory.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    decoded = jpeg_alloc_decoder_mem(VIDEO_WIDTH * VIDEO_HEIGHT * 2, &memory, &decoded_capacity);
    ESP_RETURN_ON_FALSE(pending_jpeg && decode_jpeg && decoded, ESP_ERR_NO_MEM, TAG, "JPEG DMA allocation");
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
