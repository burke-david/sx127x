#include "sx127x_test_utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_err.h>
#include <test_utils.h>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <driver/spi_master.h>
#include <esp_intr_alloc.h>

#define ERROR_CHECK(x)           \
  do {                           \
    int __err_rc = (x);          \
    if (__err_rc != SX127X_OK) { \
      return __err_rc;           \
    }                            \
  } while (0)

TaskHandle_t handle_interrupt;
static const char *TAG = "sx127x_test";
static SemaphoreHandle_t tx_done_sem;

void IRAM_ATTR handle_interrupt_fromisr(void *arg) {
    xTaskResumeFromISR(handle_interrupt);
}

void handle_interrupt_task(void *arg) {
    while (1) {
        vTaskSuspend(NULL);
        sx127x_handle_interrupt((sx127x *) arg);
    }
}


void tx_callback(sx127x *device) {
    ESP_LOGI(TAG, "sent");
}

void sx127x_test_wait_for_tx() {

}

void rx_callback(sx127x *device, uint8_t *data, uint16_t data_length) {
    uint8_t payload[514];
    const char SYMBOLS[] = "0123456789ABCDEF";
    for (size_t i = 0; i < data_length; i++) {
        uint8_t cur = data[i];
        payload[2 * i] = SYMBOLS[cur >> 4];
        payload[2 * i + 1] = SYMBOLS[cur & 0x0F];
    }
    payload[data_length * 2] = '\0';

    int16_t rssi;
    ESP_ERROR_CHECK(sx127x_rx_get_packet_rssi(device, &rssi));
    float snr;
    ESP_ERROR_CHECK(sx127x_lora_rx_get_packet_snr(device, &snr));
    int32_t frequency_error;
    ESP_ERROR_CHECK(sx127x_rx_get_frequency_error(device, &frequency_error));
    ESP_LOGI(TAG, "received: %d %s rssi: %d snr: %f freq_error: %" PRId32, data_length, payload, rssi, snr, frequency_error);
}

void cad_callback(sx127x *device, int cad_detected) {
    if (cad_detected == 0) {
        ESP_LOGI(TAG, "cad not detected");
        ESP_ERROR_CHECK(sx127x_set_opmod(SX127x_MODE_CAD, SX127x_MODULATION_LORA, device));
        return;
    }
    // put into RX mode first to handle interrupt as soon as possible
    ESP_ERROR_CHECK(sx127x_set_opmod(SX127x_MODE_RX_CONT, SX127x_MODULATION_LORA, device));
    ESP_LOGI(TAG, "cad detected\n");
}

void setup_gpio_interrupts(gpio_num_t gpio, sx127x *device) {
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_pulldown_en(gpio);
    gpio_pullup_dis(gpio);
    gpio_set_intr_type(gpio, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(gpio, handle_interrupt_fromisr, (void *) device);
}

int sx127x_test_create_lora(sx127x_test_spi_config_t *spi_config, sx127x **result) {
    ESP_LOGI(TAG, "starting up");
    spi_bus_config_t config = {
            .mosi_io_num = spi_config->mosi,
            .miso_io_num = spi_config->miso,
            .sclk_io_num = spi_config->sck,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 0,
    };
    ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &config, 1));
    spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = 3000000,
            .spics_io_num = spi_config->ss,
            .queue_size = 16,
            .command_bits = 0,
            .address_bits = 8,
            .dummy_bits = 0,
            .mode = 0};
    spi_device_handle_t spi_device;
    sx127x *device = NULL;
    ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &dev_cfg, &spi_device));
    ERROR_CHECK(sx127x_create(spi_device, &device));
    ERROR_CHECK(sx127x_set_opmod(SX127x_MODE_SLEEP, SX127x_MODULATION_LORA, device));
    ERROR_CHECK(sx127x_set_frequency(437200012, device));
    ERROR_CHECK(sx127x_lora_reset_fifo(device));
    ERROR_CHECK(sx127x_set_opmod(SX127x_MODE_STANDBY, SX127x_MODULATION_LORA, device));
    ERROR_CHECK(sx127x_lora_set_bandwidth(SX127x_BW_125000, device));
    ERROR_CHECK(sx127x_lora_set_implicit_header(NULL, device));
    ERROR_CHECK(sx127x_lora_set_modem_config_2(SX127x_SF_9, device));
    ERROR_CHECK(sx127x_lora_set_syncword(18, device));
    ERROR_CHECK(sx127x_set_preamble_length(8, device));
    sx127x_tx_set_callback(tx_callback, device);
    sx127x_rx_set_callback(rx_callback, device);
    sx127x_lora_cad_set_callback(cad_callback, device);

    BaseType_t task_code = xTaskCreatePinnedToCore(handle_interrupt_task, "handle interrupt", 8196, device, 2, &handle_interrupt, xPortGetCoreID());
    if (task_code != pdPASS) {
        ESP_LOGE(TAG, "can't create task %d", task_code);
        sx127x_destroy(device);
        return SX127X_ERR_INVALID_ARG;
    }
    tx_done_sem = xSemaphoreCreateBinary();
    gpio_install_isr_service(0);
    setup_gpio_interrupts((gpio_num_t) spi_config->dio0, device);
    *result = device;
    return SX127X_OK;
}