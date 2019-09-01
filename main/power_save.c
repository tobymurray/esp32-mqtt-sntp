#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mqtt_client.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

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

/**
 * Variable holding number of times ESP32 restarted since first boot.
 * It is placed into RTC memory using RTC_DATA_ATTR and
 * maintains its value when ESP32 wakes from deep sleep.
 */
RTC_DATA_ATTR static int boot_count = 0;

static void obtain_time(void);
static void initialize_sntp(void);

static const char *TAG = "power_save";
static esp_mqtt_client_handle_t client;

void time_sync_notification_cb(struct timeval *tv)
{
  ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    esp_wifi_connect();
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip: %s", ip4addr_ntoa(&event->ip_info.ip));
  }
}

/*init wifi as sta and set power save mode*/
static void wifi_power_save(void)
{
  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = DEFAULT_SSID,
          .password = DEFAULT_PWD,
          .listen_interval = DEFAULT_LISTEN_INTERVAL,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "esp_wifi_set_ps().");
  esp_wifi_set_ps(DEFAULT_PS_MODE);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  // your_context_t *context = event->context;
  switch (event->event_id)
  {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

    msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
    ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

    msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
    ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

    msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
    ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
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

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
  mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = CONFIG_BROKER_URL,
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
  esp_mqtt_client_start(client);
}

void print_elapsed_time(time_t now, time_t first_time, struct tm timeinfo)
{
  char duration[32];

  time_t elapsed_time = now - first_time;

  int hours_elapsed = elapsed_time / 3600;
  int minutes_elapsed = (elapsed_time % 3600) / 60;
  int seconds_elapsed = elapsed_time % 60;
  snprintf(duration, sizeof(duration), "%02i:%02i:%02i", hours_elapsed, minutes_elapsed, seconds_elapsed);

  ESP_LOGI(TAG, "The total elapsed time so far is %s", duration);
  esp_mqtt_client_publish(client, "/topic/time", duration, 0, 1, 0);
}

void vTaskPublishTime(void *pvParameters)
{
  time_t first_time = 0;
  time_t now;
  struct tm timeinfo;
  unsigned long currentMillis;
  unsigned long lastReport = 0;

  while (1)
  {
    currentMillis = millis();
    if (currentMillis - lastReport <= 60 * 1000)
    {
      // Not time yet!
      continue;
    }
    lastReport = currentMillis;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    if (timeinfo.tm_year < (2016 - 1900))
    {
      ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
      obtain_time();
      // update 'now' variable with current time
      time(&now);
    }

    char strftime_buf[64];
    // Set timezone to Eastern Standard Time and print local time
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
    tzset();

    if (first_time == 0)
    {
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
}

void app_main(void)
{
  ++boot_count;
  ESP_LOGI(TAG, "Boot count: %d", boot_count);
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
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
#endif // CONFIG_PM_ENABLE

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
  esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
  esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
  esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

  wifi_power_save();
  mqtt_app_start();

  xTaskCreate(vTaskPublishTime, "PUBLISH_TIME", 3072, NULL, 1, NULL);
}

static void obtain_time(void)
{
  initialize_sntp();

  // wait for time to be set
  time_t now = 0;
  struct tm timeinfo = {
      0};
  int retry = 0;
  const int retry_count = 10;
  while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
  {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  time(&now);
  localtime_r(&now, &timeinfo);
}

static void initialize_sntp(void)
{
  ESP_LOGI(TAG, "Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_set_time_sync_notification_cb(time_sync_notification_cb);
  sntp_init();
}

unsigned long IRAM_ATTR millis()
{
  return (unsigned long)(esp_timer_get_time() / 1000ULL);
}