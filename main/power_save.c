#include "driver/i2c.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mqtt_client.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tcpip_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// My own files
#include "sntp_helper.h"

/*set the ssid and password via "idf.py menuconfig"*/
#define DEFAULT_SSID CONFIG_EXAMPLE_WIFI_SSID
#define DEFAULT_PWD CONFIG_EXAMPLE_WIFI_PASSWORD

#define DEFAULT_LISTEN_INTERVAL CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL

#if CONFIG_EXAMPLE_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_EXAMPLE_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_EXAMPLE_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

#define _I2C_NUMBER(num) I2C_NUM_##num
#define I2C_NUMBER(num) _I2C_NUMBER(num)

#define DATA_LENGTH 512                  /*!< Data buffer length of test buffer */
#define DELAY_TIME_BETWEEN_ITEMS_MS 1000 /*!< delay time between different test items */

#define I2C_SLAVE_SCL_IO CONFIG_I2C_SLAVE_SCL /*!< gpio number for i2c slave clock */
#define I2C_SLAVE_SDA_IO CONFIG_I2C_SLAVE_SDA /*!< gpio number for i2c slave data */
#define I2C_SLAVE_NUM I2C_NUMBER(CONFIG_I2C_SLAVE_PORT_NUM) /*!< I2C port number for slave dev \
                                                */
#define I2C_SLAVE_TX_BUF_LEN (2 * DATA_LENGTH) /*!< I2C slave tx buffer size */
#define I2C_SLAVE_RX_BUF_LEN (2 * DATA_LENGTH) /*!< I2C slave rx buffer size */

#define I2C_MASTER_SCL_IO CONFIG_I2C_MASTER_SCL               /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO CONFIG_I2C_MASTER_SDA               /*!< gpio number for I2C master data  */
#define I2C_MASTER_NUM I2C_NUMBER(CONFIG_I2C_MASTER_PORT_NUM) /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ CONFIG_I2C_MASTER_FREQUENCY        /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0                           /*!< I2C master doesn't need buffer */

#define MCP9808_SENSOR_0_ADDRESS CONFIG_MCP9808_0_ADDRESS
#define MCP9808_SENSOR_1_ADDRESS CONFIG_MCP9808_1_ADDRESS
#define MCP9808_SENSOR_2_ADDRESS CONFIG_MCP9808_2_ADDRESS
#define MCP9808_SENSOR_3_ADDRESS CONFIG_MCP9808_3_ADDRESS

#define ESP_SLAVE_ADDR CONFIG_I2C_SLAVE_ADDRESS /*!< ESP32 slave address, you can set any 7bit value */
#define WRITE_BIT I2C_MASTER_WRITE              /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ                /*!< I2C master read */
#define ACK_CHECK_ENABLE 0x1                    /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DISABLE 0x0                   /*!< I2C master will not check ack from slave */
#define ACK_VALUE 0x0                           /*!< I2C ack value */
#define NACK_VALUE 0x1                          /*!< I2C nack value */

enum mqtt_qos { AT_MOST_ONCE, AT_LEAST_ONCE, EXACTLY_ONCE };

/**
 * Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static const char *TAG = "power_save";
static esp_mqtt_client_handle_t client;
const static int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;

static EventGroupHandle_t wifi_event_group;

SemaphoreHandle_t print_mux = NULL;

static float convert_temperature_reading_to_temperature(uint8_t *data_h, uint8_t *data_l) {
  //	First Check flag bits
  if ((*data_h & 0x80) == 0x80) {  // TA ³ TCRIT
  }
  if ((*data_h & 0x40) == 0x40) {  // TA > TUPPER
  }
  if ((*data_h & 0x20) == 0x20) {  // TA < TLOWER
  }
  *data_h = *data_h & 0x1F;        // Clear flag bits
  if ((*data_h & 0x10) == 0x10) {  // Is the temperature lower than 0°C
    *data_h = *data_h & 0x0F;      // Clear SIGN
    return 256 - (*data_h * 16 + *data_l / 16);
  } else {
    return *data_h * 16.0 + *data_l / 16.0;
  }
}

static esp_err_t read_temperature_sensor(int sensor_address, i2c_port_t i2c_num, uint8_t *data_h, uint8_t *data_l,
                                         float *temperature) {
  i2c_cmd_handle_t command = i2c_cmd_link_create();
  i2c_master_start(command);
  i2c_master_write_byte(command, sensor_address << 1 & 0xFE, ACK_CHECK_ENABLE);
  i2c_master_write_byte(command, 0x05, ACK_CHECK_ENABLE);
  i2c_master_stop(command);
  int return_value = i2c_master_cmd_begin(i2c_num, command, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(command);
  // ESP_LOGI(TAG, "First return value is %d", ret);
  if (return_value != ESP_OK) {
    return return_value;
  }

  vTaskDelay(30 / portTICK_RATE_MS);
  command = i2c_cmd_link_create();
  i2c_master_start(command);
  i2c_master_write_byte(command, sensor_address << 1 | READ_BIT, ACK_CHECK_ENABLE);

  i2c_master_read_byte(command, data_h, ACK_VALUE);
  i2c_master_read_byte(command, data_l, NACK_VALUE);
  i2c_master_stop(command);

  return_value = i2c_master_cmd_begin(i2c_num, command, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(command);

  *temperature = convert_temperature_reading_to_temperature(data_h, data_l);
  return return_value;
}

static esp_err_t i2c_master_init() {
  int i2c_master_port = I2C_MASTER_NUM;
  i2c_config_t conf;
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = I2C_MASTER_SDA_IO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = I2C_MASTER_SCL_IO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
  i2c_param_config(i2c_master_port, &conf);
  return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

static float read_sensor(int address, uint8_t *data_h, uint8_t *data_l) {
  float temperature;
  int ret = read_temperature_sensor(address, I2C_MASTER_NUM, data_h, data_l, &temperature);
  xSemaphoreTake(print_mux, portMAX_DELAY);
  if (ret == ESP_ERR_TIMEOUT) {
    ESP_LOGE(TAG, "I2C Timeout");
  } else if (ret == ESP_OK) {
    ESP_LOGI(TAG, "MCP9808 @ %d: temperature: %.2f\n", address,
             convert_temperature_reading_to_temperature(data_h, data_l));
  } else {
    ESP_LOGW(TAG, "%s: No ack, sensor not connected...skip...", esp_err_to_name(ret));
  }
  xSemaphoreGive(print_mux);
  vTaskDelay((DELAY_TIME_BETWEEN_ITEMS_MS) / portTICK_RATE_MS);

  return temperature;
}

static float min_value(float values[]) {
  float minimum = values[0];
  for (int i = 1; i < 4; i++) {
    if (values[i] < minimum) {
      minimum = values[i];
    }
  }
  return minimum;
}

static float max_value(float values[]) {
  float maximum = values[0];
  for (int i = 1; i < 4; i++) {
    if (values[i] > maximum) maximum = values[i];
  }
  return maximum;
}

static void read_all_sensors(void *arg) {
  uint8_t sensor_data_h;
  uint8_t sensor_data_l;
  int count = 0;
  float temp[4];
  char temperature_buffer[32];

  // Initialise the xLastWakeTime variable with the current time.
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xDelay = 15000 / portTICK_PERIOD_MS;

  while (1) {
    // Wait for the next cycle.
    vTaskDelayUntil(&xLastWakeTime, xDelay);

    ESP_LOGI(TAG, "Number of sensor read loops: %d", count++);
    // Read first sensor
    temp[0] = read_sensor(MCP9808_SENSOR_0_ADDRESS, &sensor_data_h, &sensor_data_l);
    temp[1] = read_sensor(MCP9808_SENSOR_1_ADDRESS, &sensor_data_h, &sensor_data_l);
    temp[2] = read_sensor(MCP9808_SENSOR_2_ADDRESS, &sensor_data_h, &sensor_data_l);
    temp[3] = read_sensor(MCP9808_SENSOR_3_ADDRESS, &sensor_data_h, &sensor_data_l);

    float min = min_value(temp);
    float max = max_value(temp);
    float variance = max - min;
    printf("Variance between min %.2f and max %.2f was %.2f\n", min, max, variance);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    if ((bits & CONNECTED_BIT) == 0) {
      ESP_LOGE(TAG, "Wi-Fi is not connected, not publishing temperatures");
    } else {
      sprintf(temperature_buffer, "%.2f", temp[0]);
      esp_mqtt_client_publish(client, "/temperature/0/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", temp[1]);
      esp_mqtt_client_publish(client, "/temperature/1/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", temp[2]);
      esp_mqtt_client_publish(client, "/temperature/2/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", temp[3]);
      esp_mqtt_client_publish(client, "/temperature/3/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", min);
      esp_mqtt_client_publish(client, "/temperature/min/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", max);
      esp_mqtt_client_publish(client, "/temperature/max/", temperature_buffer, 0, 1, 0);

      sprintf(temperature_buffer, "%.2f", variance);
      esp_mqtt_client_publish(client, "/temperature/variance/", temperature_buffer, 0, 1, 0);
    }

    memset(temp, 0, sizeof temp);
  }
  vSemaphoreDelete(print_mux);
  vTaskDelete(NULL);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  uint8_t mac[6];
  char mac_as_text[18];

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
      sprintf(mac_as_text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      msg_id = esp_mqtt_client_publish(client, "/announce", mac_as_text, 0, EXACTLY_ONCE, 0);
      ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
      ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_PUBLISHED:
      ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      break;
    default:
      ESP_LOGI(TAG, "Other event id:%d", event->event_id);
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
      break;
  }
  return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
  mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = CONFIG_BROKER_URL,
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);

  EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
  if ((bits & CONNECTED_BIT) == 0) {
    ESP_LOGE(TAG, "Wi-Fi is not connected, failed to initialize MQTT connection");
  } else {
    ESP_LOGI(TAG, "Starting MQTT client");
    esp_mqtt_client_start(client);
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip: %s", ip4addr_ntoa(&event->ip_info.ip));
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
  }
}

static void on_got_ip_address(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    mqtt_app_start();
  } else {
    ESP_LOGE(TAG, "Received unexpected event in on_got_ip_address: %s", event_base);
  }
}

/*init wifi as sta and set power save mode*/
static void wifi_power_save(void) {
  wifi_event_group = xEventGroupCreate();
  xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);

  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Set up listeners for Wi-Fi events, this tries to re-connect after disconnection
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

  // Set up listener to launch when IP is obtained
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip_address, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = DEFAULT_SSID,
              .password = DEFAULT_PWD,
              .listen_interval = DEFAULT_LISTEN_INTERVAL,
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_wifi_set_ps(DEFAULT_PS_MODE);
}

void print_elapsed_time(time_t now, time_t first_time, struct tm timeinfo) {
  time_t elapsed_time = now - first_time;

  int hours_elapsed = elapsed_time / 3600;
  int minutes_elapsed = (elapsed_time % 3600) / 60;
  int seconds_elapsed = elapsed_time % 60;

  char duration[32];
  snprintf(duration, sizeof(duration), "%02i:%02i:%02i", hours_elapsed, minutes_elapsed, seconds_elapsed);

  ESP_LOGI(TAG, "The total elapsed time so far is %s", duration);
  esp_mqtt_client_publish(client, "/topic/time", duration, 0, 1, 0);
}

void sync_time_via_sntp() {
  time_t first_time = 0;
  time_t now;
  struct tm timeinfo;

  time(&now);
  localtime_r(&now, &timeinfo);
  // Is time set? If not, tm_year will be (1970 - 1900).
  if (timeinfo.tm_year < (2016 - 1900)) {
    ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
    obtain_time();
    // update 'now' variable with current time
    time(&now);
  }

  char strftime_buf[64];
  // Set timezone to Eastern Standard Time and print local time
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
  tzset();

  if (first_time == 0) {
    ESP_LOGI(TAG, "This is the first point the time is known, saving it");
    time(&first_time);

    localtime_r(&first_time, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The very first date/time is: %s", strftime_buf);
    esp_mqtt_client_publish(client, "/topic/time", strftime_buf, 0, 1, 0);
  }

  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "The current date/time in New York is: %s", strftime_buf);
  esp_mqtt_client_publish(client, "/topic/time", strftime_buf, 0, 1, 0);

  print_elapsed_time(now, first_time, timeinfo);
}

void vTaskPublishTime(void *pvParameters) {
  /* Block for 15 minutes. */
  const TickType_t xDelay = 15 * 60 * 1000 / portTICK_PERIOD_MS;

  // Initialise the xLastWakeTime variable with the current time.
  TickType_t xLastWakeTime = xTaskGetTickCount();

  EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
  if ((bits & CONNECTED_BIT) == 0) {
    ESP_LOGE(TAG, "Wi-Fi is not connected, failed to publish time");
  } else {
    sync_time_via_sntp();
  }

  while (1) {
    // Wait for the next cycle.
    vTaskDelayUntil(&xLastWakeTime, xDelay);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    if ((bits & CONNECTED_BIT) == 0) {
      ESP_LOGE(TAG, "Wi-Fi is not connected, failed to publish time");
      continue;
    }

    sync_time_via_sntp();
  }
}

void boot(void) {
  ++boot_count;
  ESP_LOGI(TAG, "Boot count: %d", boot_count);
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

#if CONFIG_PM_ENABLE
  // Configure dynamic frequency scaling:
  // maximum and minimum frequencies are set in sdkconfig,
  // automatic light sleep is enabled if tickless idle support is enabled.
  esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = CONFIG_EXAMPLE_MAX_CPU_FREQ_MHZ,
    .min_freq_mhz = CONFIG_EXAMPLE_MIN_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    .light_sleep_enable = true
#endif
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif  // CONFIG_PM_ENABLE

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
  esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
}

void app_main(void) {
  boot();

  // Try to connect to Wi-Fi
  wifi_power_save();

  xTaskCreate(vTaskPublishTime, "PUBLISH_TIME", 3072, NULL, 1, NULL);

  print_mux = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(i2c_master_init());
  xTaskCreate(read_all_sensors, "i2c_test_task_0", 1024 * 2, (void *)0, 10, NULL);
}
