#include "usb_link.h"

#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "video.h"

enum { READ_COUNT = 4, READ_SIZE = 16 * 1024, INPUT_QUEUE_SIZE = 64 };
static const char *TAG = "usb_link";

static usb_host_client_handle_t client;
static usb_device_handle_t device;
static usb_transfer_t *reads[READ_COUNT], *write_transfer;
static uint8_t pending_address, interface_number;
/* A transfer error on a still-attached device is retried; only unplug ends the link. */
static uint8_t open_address, retry_address;
static bool device_gone, retrying;
static TickType_t retry_at;
static uint8_t input_endpoint, video_endpoint;
static unsigned active_reads;
static bool interface_claimed, write_active, stopping;
static video_stream_t stream;

static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t current[2][RECORD_SIZE] = {
    {'T', '5', 'I', '1', 1}, {'T', '5', 'I', '1', 2},
};
static uint8_t records[INPUT_QUEUE_SIZE][RECORD_SIZE];
static unsigned record_head, record_count;
static bool capture_input, input_overflow;

static void queue_record(const uint8_t record[RECORD_SIZE])
{
    unsigned tail = (record_head + record_count) % INPUT_QUEUE_SIZE;
    memcpy(records[tail], record, RECORD_SIZE);
    ++record_count;
}

void usb_link_input(const uint8_t record[RECORD_SIZE])
{
    portENTER_CRITICAL(&input_lock);
    uint8_t *state = current[record[4] - 1];
    if (memcmp(state, record, RECORD_SIZE) != 0) {
        memcpy(state, record, RECORD_SIZE);
        if (capture_input) {
            if (record_count == INPUT_QUEUE_SIZE) {
                // Recover from backpressure by resending both current states.
                record_head = record_count = 0;
                queue_record(current[0]);
                queue_record(current[1]);
                input_overflow = true;
            } else {
                queue_record(record);
            }
        }
    }
    portEXIT_CRITICAL(&input_lock);
}

static bool check(esp_err_t result, const char *operation)
{
    if (result == ESP_OK) return true;
    ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(result));
    return false;
}

static void invalid_frame(const char *reason)
{
    ESP_LOGW(TAG, "%s; searching for T5F1", reason);
}

static void submit_read(usb_transfer_t *transfer)
{
    if (check(usb_host_transfer_submit(transfer), "Submit video transfer")) {
        ++active_reads;
    } else {
        stopping = true;
    }
}

static void read_complete(usb_transfer_t *transfer)
{
    --active_reads;
    if (stopping) return;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "Video transfer status %d; closing link until reconnect", transfer->status);
        stopping = true;
        return;
    }
    video_stream_feed(&stream, transfer->data_buffer, transfer->actual_num_bytes,
                      video_submit, invalid_frame);
    submit_read(transfer);
}

static void write_complete(usb_transfer_t *transfer)
{
    write_active = false;
    if (stopping) return;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED ||
        transfer->actual_num_bytes != RECORD_SIZE) {
        ESP_LOGW(TAG, "Input transfer status %d, bytes %d; closing link until reconnect",
                 transfer->status, transfer->actual_num_bytes);
        stopping = true;
    }
}

static void send_input(void)
{
    if (write_active) return;
    portENTER_CRITICAL(&input_lock);
    bool ready = record_count != 0;
    bool overflow = input_overflow;
    input_overflow = false;
    if (ready) {
        memcpy(write_transfer->data_buffer, records[record_head], RECORD_SIZE);
        record_head = (record_head + 1) % INPUT_QUEUE_SIZE;
        --record_count;
    }
    portEXIT_CRITICAL(&input_lock);
    if (overflow) ESP_LOGW(TAG, "Input queue full; resending current states");
    if (ready) {
        write_active = check(usb_host_transfer_submit(write_transfer), "Submit input transfer");
        if (!write_active) stopping = true;
    }
}

static bool find_interface(const usb_config_desc_t *configuration)
{
    const uint8_t *descriptor = (const uint8_t *)configuration;
    size_t remaining = configuration->wTotalLength;
    unsigned endpoints = 0;
    bool matching = false;
    while (remaining) {
        if (remaining < 2 || descriptor[0] < 2 || descriptor[0] > remaining) return false;
        if (descriptor[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (matching && endpoints == 2 && video_endpoint && input_endpoint) return true;
            if (descriptor[0] < USB_INTF_DESC_SIZE) return false;
            const usb_intf_desc_t *interface = (const void *)descriptor;
            matching = interface->bInterfaceClass == 0xff &&
                       interface->bInterfaceSubClass == 0x54 &&
                       interface->bInterfaceProtocol == 0x35 &&
                       interface->bAlternateSetting == 0 && interface->bNumEndpoints == 2;
            interface_number = interface->bInterfaceNumber;
            endpoints = video_endpoint = input_endpoint = 0;
        } else if (matching && descriptor[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            if (descriptor[0] < USB_EP_DESC_SIZE) return false;
            const usb_ep_desc_t *endpoint = (const void *)descriptor;
            ++endpoints;
            // 512 at High Speed; 64 when a poor cable forces Full Speed.
            if (USB_EP_DESC_GET_XFERTYPE(endpoint) == USB_TRANSFER_TYPE_BULK &&
                (endpoint->wMaxPacketSize == 512 || endpoint->wMaxPacketSize == 64) &&
                USB_EP_DESC_GET_EP_NUM(endpoint)) {
                if (USB_EP_DESC_GET_EP_DIR(endpoint)) video_endpoint = endpoint->bEndpointAddress;
                else input_endpoint = endpoint->bEndpointAddress;
            }
        }
        remaining -= descriptor[0];
        descriptor += descriptor[0];
    }
    return matching && endpoints == 2 && video_endpoint && input_endpoint;
}

static bool open_link(uint8_t address)
{
    if (!check(usb_host_device_open(client, address, &device), "Open USB device")) return false;
    const usb_config_desc_t *configuration;
    if (!check(usb_host_get_active_config_descriptor(device, &configuration), "Read USB configuration")) return false;
    if (!find_interface(configuration)) {
        ESP_LOGW(TAG, "USB device at address %u has no Tab5 display interface (FF/54/35 with two bulk endpoints); ignoring it", address);
        return false;
    }
    if (!check(usb_host_interface_claim(client, device, interface_number, 0), "Claim Tab5 interface")) return false;
    interface_claimed = true;

    stream = (video_stream_t){.jpeg = stream.jpeg};
    for (unsigned index = 0; index < READ_COUNT; ++index) {
        usb_transfer_t *transfer = reads[index];
        transfer->device_handle = device;
        transfer->bEndpointAddress = video_endpoint;
        transfer->callback = read_complete;
        transfer->num_bytes = READ_SIZE;
        submit_read(transfer);
        if (stopping) return false;
    }
    write_transfer->device_handle = device;
    write_transfer->bEndpointAddress = input_endpoint;
    write_transfer->callback = write_complete;
    write_transfer->num_bytes = RECORD_SIZE;
    portENTER_CRITICAL(&input_lock);
    record_head = record_count = 0;
    input_overflow = false;
    queue_record(current[0]);
    queue_record(current[1]);
    capture_input = true;
    portEXIT_CRITICAL(&input_lock);
    open_address = address;
    ESP_LOGI(TAG, "Tab5 interface attached at USB address %u", address);
    return true;
}

static void handle_client_events(TickType_t timeout)
{
    esp_err_t result = usb_host_client_handle_events(client, timeout);
    if (result != ESP_ERR_TIMEOUT) ESP_ERROR_CHECK(result);
}

static void stop_endpoint(uint8_t endpoint)
{
    // On unplug the host library may already have halted and flushed the pipe.
    esp_err_t halted = usb_host_endpoint_halt(device, endpoint);
    if (halted != ESP_ERR_INVALID_STATE) check(halted, "Halt pipe");
    if (!check(usb_host_endpoint_flush(device, endpoint), "Flush pipe")) {
        ESP_LOGW(TAG, "Waiting for the host stack to retire transfers on endpoint 0x%02x", endpoint);
    }
}

static void close_link(void)
{
    portENTER_CRITICAL(&input_lock);
    capture_input = false;
    portEXIT_CRITICAL(&input_lock);
    if (active_reads) stop_endpoint(video_endpoint);
    if (write_active) stop_endpoint(input_endpoint);
    while (active_reads || write_active) handle_client_events(portMAX_DELAY);
    if (interface_claimed) {
        ESP_ERROR_CHECK(usb_host_interface_release(client, device, interface_number));
        interface_claimed = false;
    }
    if (device) {
        ESP_ERROR_CHECK(usb_host_device_close(client, device));
        device = NULL;
    }
}

static void client_event(const usb_host_client_event_msg_t *event, void *context)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        pending_address = event->new_dev.address;
        retry_address = 0;
        retrying = false;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == device) {
        stopping = true;
        device_gone = true;
    }
}

static void schedule_retry(uint8_t address)
{
    ESP_LOGW(TAG, "Reopening USB address %u in 1 s", address);
    retry_address = address;
    retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
}

static void client_task(void *argument)
{
    for (;;) {
        handle_client_events(device || pending_address || retry_address ? 1 : portMAX_DELAY);
        if (stopping) {
            close_link();
            stopping = false;
            if (open_address && !device_gone) schedule_retry(open_address);
            open_address = 0;
            device_gone = false;
        }
        if (!device && !pending_address && retry_address &&
            (int32_t)(xTaskGetTickCount() - retry_at) >= 0) {
            pending_address = retry_address;
            retry_address = 0;
            retrying = true;
        }
        if (!device && pending_address) {
            uint8_t address = pending_address;
            pending_address = 0;
            if (open_link(address)) {
                retrying = false;
            } else {
                // An open handle means the device is still enumerated; only a retry of a
                // previously working link is repeated, never a foreign device.
                bool attached = device != NULL;
                stopping = true;
                close_link();
                stopping = false;
                if (retrying && attached && !device_gone) schedule_retry(address);
                else retrying = false;
                device_gone = false;
            }
        }
        if (device && !stopping) send_input();
    }
}

static void host_task(void *argument)
{
    for (;;) {
        uint32_t events;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &events));
    }
}

esp_err_t usb_link_start(void)
{
    stream.jpeg = heap_caps_malloc(VIDEO_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(stream.jpeg, ESP_ERR_NO_MEM, TAG, "Video stream buffer");
    const usb_host_config_t host_config = {
        .peripheral_map = BIT0,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "Install high-speed host");
    const usb_host_client_config_t client_config = {
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event},
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&client_config, &client), TAG, "Register host client");
    for (unsigned index = 0; index < READ_COUNT; ++index) {
        ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(READ_SIZE, 0, &reads[index]), TAG, "Allocate video transfer");
    }
    ESP_RETURN_ON_ERROR(usb_host_transfer_alloc(RECORD_SIZE, 0, &write_transfer), TAG, "Allocate input transfer");
    ESP_RETURN_ON_FALSE(xTaskCreate(host_task, "usb_host", 4096, NULL, 7, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Create host daemon");
    ESP_RETURN_ON_FALSE(xTaskCreate(client_task, "usb_client", 6144, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Create host client");
    return ESP_OK;
}
