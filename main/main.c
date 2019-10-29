#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "esp_err.h"

#include "sdkconfig.h" // generated by "make menuconfig"

#include "ssd1366.h"
#include "font8x8_basic.h"
#include "bme280.h"
#include "hap.h"

#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22
// #define SDA_PIN GPIO_NUM_15
// #define SCL_PIN GPIO_NUM_2

#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

#define tag "SSD1306"
#define TAG_BME280 "BME280"

#define TAG "Thermostat"

#define ACCESSORY_NAME  "Thermostat"
#define MANUFACTURER_NAME   "wumbo"
#define MODEL_NAME  "wumbostat"
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

#define EXAMPLE_ESP_WIFI_SSID "Gloamer"
#define EXAMPLE_ESP_WIFI_PASS "40totarast"

s32 v_uncomp_pressure_s32;
s32 v_uncomp_temperature_s32;
s32 v_uncomp_humidity_s32;

bool sensors_updated = false;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

/* The event group allows multiple bits for each event,
 but we only care about one event - are we connected
 to the AP with an IP? */
static void* a;
static int state = false;

static int temperature = 0;
static int humidity = 0;
static void* _temperature_ev_handle = NULL;
static void* _humidity_ev_handle =  NULL;

static void* _temperature_read(void* arg)
{
    ESP_LOGI("MAIN", "_temperature_read");
    return (void*)temperature;
}

void _temperature_notify(void* arg, void* ev_handle, bool enable)
{
    ESP_LOGI("MAIN", "_temperature_notify");
    //xSemaphoreTake(ev_mutex, 0);

    if (enable) 
        _temperature_ev_handle = ev_handle;
    else 
        _temperature_ev_handle = NULL;

    //xSemaphoreGive(ev_mutex);
}

static void* _humidity_read(void* arg)
{
    ESP_LOGI("MAIN", "_humidity_read");
    return (void*)humidity;
}

void _humidity_notify(void* arg, void* ev_handle, bool enable)
{
    ESP_LOGI("MAIN", "_humidity_notify");
    //xSemaphoreTake(ev_mutex, 0);

    if (enable) 
        _humidity_ev_handle = ev_handle;
    else 
        _humidity_ev_handle = NULL;

    //xSemaphoreGive(ev_mutex);
}

static bool _identifed = false;
void* identify_read(void* arg)
{
    return (void*)true;
}

void hap_object_init(void* arg)
{
    void* accessory_object = hap_accessory_add(a);
    struct hap_characteristic cs[] = {
        {HAP_CHARACTER_IDENTIFY, (void*)true, NULL, identify_read, NULL, NULL},
        {HAP_CHARACTER_MANUFACTURER, (void*)MANUFACTURER_NAME, NULL, NULL, NULL, NULL},
        {HAP_CHARACTER_MODEL, (void*)MODEL_NAME, NULL, NULL, NULL, NULL},
        {HAP_CHARACTER_NAME, (void*)ACCESSORY_NAME, NULL, NULL, NULL, NULL},
        {HAP_CHARACTER_SERIAL_NUMBER, (void*)"0123456789", NULL, NULL, NULL, NULL},
        {HAP_CHARACTER_FIRMWARE_REVISION, (void*)"1.0", NULL, NULL, NULL, NULL},
    };
    hap_service_and_characteristics_add(a, accessory_object, HAP_SERVICE_ACCESSORY_INFORMATION, cs, ARRAY_SIZE(cs));
    
//    struct hap_characteristic cc[] = {
//        {HAP_CHARACTER_ON, (void*)state, NULL, switch_read, switch_write, switch_notify},
//    };
//    hap_service_and_characteristics_add(a, accessory_object, HAP_SERVICE_SWITCHS, cc, ARRAY_SIZE(cc));
    
    struct hap_characteristic humidity_sensor[] = {
        {HAP_CHARACTER_CURRENT_RELATIVE_HUMIDITY, (void*)humidity, NULL, _humidity_read, NULL, _humidity_notify},
        {HAP_CHARACTER_NAME, (void*)"HYGROMETER" , NULL, NULL, NULL, NULL},
    };
    hap_service_and_characteristics_add(a, accessory_object, HAP_SERVICE_HUMIDITY_SENSOR, humidity_sensor, ARRAY_SIZE(humidity_sensor));
    
    struct hap_characteristic temperature_sensor[] = {
        {HAP_CHARACTER_CURRENT_TEMPERATURE, (void*)temperature, NULL, _temperature_read, NULL, _temperature_notify},
        {HAP_CHARACTER_NAME, (void*)"THERMOMETER" , NULL, NULL, NULL, NULL},
    };
    hap_service_and_characteristics_add(a, accessory_object, HAP_SERVICE_TEMPERATURE_SENSOR, temperature_sensor, ARRAY_SIZE(temperature_sensor));
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "got ip:%s",
                     ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        {
            hap_init();
            
            uint8_t mac[6];
            esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
            char accessory_id[32] = {0,};
            sprintf(accessory_id, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            hap_accessory_callback_t callback;
            callback.hap_object_init = hap_object_init;
            a = hap_accessory_register((char*)ACCESSORY_NAME, accessory_id, (char*)"053-58-197", (char*)MANUFACTURER_NAME, HAP_ACCESSORY_CATEGORY_OTHER, 811, 1, NULL, &callback);
        }
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

void wifi_init_sta()
{
    wifi_event_group = xEventGroupCreate();
    
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) );
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    
    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
}

void i2c_master_init()
{
	i2c_config_t i2c_config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = SDA_PIN,
		.scl_io_num = SCL_PIN,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 1000000
	};
	i2c_param_config(I2C_NUM_0, &i2c_config);
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

s8 BME280_I2C_bus_write(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    s32 iError = BME280_INIT_VALUE;
    
    esp_err_t espRc;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write(cmd, reg_data, cnt, true);
    i2c_master_stop(cmd);
    
    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
    if (espRc == ESP_OK) {
        iError = SUCCESS;
    } else {
        iError = FAIL;
    }
    i2c_cmd_link_delete(cmd);
    
    return (s8)iError;
}

s8 BME280_I2C_bus_read(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
    s32 iError = BME280_INIT_VALUE;
    esp_err_t espRc;
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    
    if (cnt > 1) {
        i2c_master_read(cmd, reg_data, cnt-1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, reg_data+cnt-1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    
    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
    if (espRc == ESP_OK) {
        iError = SUCCESS;
    } else {
        iError = FAIL;
    }
    
    i2c_cmd_link_delete(cmd);
    
    return (s8)iError;
}

void BME280_delay_msek(u32 msek)
{
    vTaskDelay(msek/portTICK_PERIOD_MS);
}

void task_bme280_normal_mode(void *ignore)
{
    struct bme280_t bme280 = {
        .bus_write = BME280_I2C_bus_write,
        .bus_read = BME280_I2C_bus_read,
        .dev_addr = BME280_I2C_ADDRESS1,
        .delay_msec = BME280_delay_msek
    };
    
    s32 com_rslt;
    
    com_rslt = bme280_init(&bme280);
    
    com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
    com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_2X);
    com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_1X);
    
    com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1_MS);
    com_rslt += bme280_set_filter(BME280_FILTER_COEFF_16);
    
    com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);
    if (com_rslt == SUCCESS) {
        while(true) {
            vTaskDelay(1000/portTICK_PERIOD_MS);
            
            com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
                                                                        &v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);
            
            if (com_rslt == SUCCESS) {
                // sensors_updated = true;
                // ESP_LOGI(TAG_BME280, "%.2f degC / %.3f hPa / %.3f %%",
                //          bme280_compensate_temperature_double(v_uncomp_temperature_s32),
                //          bme280_compensate_pressure_double(v_uncomp_pressure_s32)/100, // Pa -> hPa
                //          bme280_compensate_humidity_double(v_uncomp_humidity_s32));

                if (_humidity_ev_handle) {
                    int hum = bme280_compensate_humidity_double(v_uncomp_humidity_s32) * 100;
                    hap_event_response(a, _humidity_ev_handle, (void*)hum);
                }

                if (_temperature_ev_handle) {
                    int temp = bme280_compensate_temperature_double(v_uncomp_temperature_s32) * 100;
                    hap_event_response(a, _temperature_ev_handle, (void*)temp);
                }
            } else {
                ESP_LOGE(TAG_BME280, "measure error. code: %d", com_rslt);
            }
        }
    } else {
        ESP_LOGE(TAG_BME280, "init or setting error. code: %d", com_rslt);
    }
    
    vTaskDelete(NULL);
}


void ssd1306_init() {
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);

	i2c_master_write_byte(cmd, OLED_CMD_SET_CHARGE_PUMP, true);
	i2c_master_write_byte(cmd, 0x14, true);

	i2c_master_write_byte(cmd, OLED_CMD_SET_SEGMENT_REMAP, true); // reverse left-right mapping
	i2c_master_write_byte(cmd, OLED_CMD_SET_COM_SCAN_MODE, true); // reverse up-bottom mapping

	i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_ON, true);
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
	if (espRc == ESP_OK) {
		ESP_LOGI(tag, "OLED configured successfully");
	} else {
		ESP_LOGE(tag, "OLED configuration failed. code: 0x%.2X", espRc);
	}
	i2c_cmd_link_delete(cmd);
}

void task_ssd1306_display_clear(void *ignore) {
	i2c_cmd_handle_t cmd;

	uint8_t zero[128];
	for (uint8_t i = 0; i < 8; i++) {
		cmd = i2c_cmd_link_create();
		i2c_master_start(cmd);
		i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_SINGLE, true);
		i2c_master_write_byte(cmd, 0xB0 | i, true);

		i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_DATA_STREAM, true);
		i2c_master_write(cmd, zero, 128, true);
		i2c_master_stop(cmd);
		i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
		i2c_cmd_link_delete(cmd);
	}

	vTaskDelete(NULL);
}

void task_ssd1306_display_text(const void *arg_text) {
//	char *text = (char*)arg_text;
    while (1) {
        char text[20];
        sprintf(text,
                "%.1f C\n%.1f%%",
                bme280_compensate_temperature_double(v_uncomp_temperature_s32),
                bme280_compensate_humidity_double(v_uncomp_humidity_s32));
        uint8_t text_len = strlen(text);

        i2c_cmd_handle_t cmd;

        uint8_t cur_page = 0;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

        i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
        i2c_master_write_byte(cmd, 0x00, true); // reset column
        i2c_master_write_byte(cmd, 0x10, true);
        i2c_master_write_byte(cmd, 0xB0 | cur_page, true); // reset page

        i2c_master_stop(cmd);
        i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        for (uint8_t i = 0; i < text_len; i++) {
            if (text[i] == '\n') {
                cmd = i2c_cmd_link_create();
                i2c_master_start(cmd);
                i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

                i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
                i2c_master_write_byte(cmd, 0x00, true); // reset column
                i2c_master_write_byte(cmd, 0x10, true);
                i2c_master_write_byte(cmd, 0xB0 | ++cur_page, true); // increment page

                i2c_master_stop(cmd);
                i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
                i2c_cmd_link_delete(cmd);
            } else {
                cmd = i2c_cmd_link_create();
                i2c_master_start(cmd);
                i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

                i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_DATA_STREAM, true);
                i2c_master_write(cmd, font8x8_basic_tr[(uint8_t)text[i]], 8, true);

                i2c_master_stop(cmd);
                i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
                i2c_cmd_link_delete(cmd);
            }
        }

        vTaskDelay(100/portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
	i2c_master_init();
    
    xTaskCreate(&task_bme280_normal_mode, "bme280_normal_mode",  8192, NULL, 6, NULL);
    
	ssd1306_init();

	xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear",  2048, NULL, 6, NULL);
	vTaskDelay(200/portTICK_PERIOD_MS);
    //xTaskCreate(&task_ssd1306_display_clear, "ssd1306_display_clear",  2048, NULL, 6, NULL);
    
    //xTaskCreate(&task_ssd1306_display_pattern, "ssd1306_display_pattern",  2048, NULL, 6, NULL);
    
	xTaskCreate(&task_ssd1306_display_text, "ssd1306_display_text",  2048,
		(void *)"", 6, NULL);
	//xTaskCreate(&task_ssd1306_contrast, "ssid1306_contrast", 2048, NULL, 6, NULL);
	//xTaskCreate(&task_ssd1306_scroll, "ssid1306_scroll", 2048, NULL, 6, NULL);
    //xTaskCreate(&task_bme280_forced_mode, "bme280_forced_mode",  2048, NULL, 6, NULL);
    
    wifi_init_sta();
}
