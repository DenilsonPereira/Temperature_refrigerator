#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"

// Drivers
#include "ssd1306.h"
#include "max31865.h"
#include "driver/spi_master.h"
#include "mqtt_client.h"

//Faixa de temperatura
#define TEMP_MIN_IDEAL -6.0f
#define TEMP_MAX_IDEAL 5.0f
#define TEMP_OFFSET -5.70f

// Configurações de Wi-Fi
#define WIFI_SSID      "Nome de sua rede wifi"
#define WIFI_PASSWORD  "Sua senha"

// Configurações do Broker MQTT
#define MQTT_BROKER_URL "url do seu broker / ip"
#define MQTT_TOPIC      "sensor/pt100/temperatura"
#define MQTT_TOPIC_STATUS "sensor/pt100/status"

// Definições dos pinos
#define SENSOR_SPI_HOST SPI3_HOST
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define LED_ALERTA   GPIO_NUM_2
#define BOTAO_MODO   GPIO_NUM_15

static const char *TAG = "PT100_MONITOR";

// Variáveis globais
static max31865_t sensor_dev;
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;

// Controle de modos do display
volatile int modo_display = 0;
float temp_max = -999.0f;
float temp_min = 999.0f;

// Event Group para sincronizar eventos de Wi-Fi
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Interrupção do botão
static void IRAM_ATTR botao_isr_handler(void* arg) {
    modo_display = (modo_display + 1) % 3;
}

// Inicialização do botão
void botao_init() {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOTAO_MODO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOTAO_MODO, botao_isr_handler, NULL);
}

// LÓGICA DE WI-FI
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) { esp_wifi_connect(); } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) { ESP_LOGI(TAG, "Falha ao conectar ao Wi-Fi. Tentando novamente..."); esp_wifi_connect(); }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) { ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data; ESP_LOGI(TAG, "Conectado ao Wi-Fi! IP: " IPSTR, IP2STR(&event->ip_info.ip)); xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); }
}
void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default()); esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD }};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Inicialização do Wi-Fi completa.");
}

// LÓGICA DE MQTT 
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: ESP_LOGI(TAG, "MQTT: Conectado ao broker"); mqtt_connected = true; break;
        case MQTT_EVENT_DISCONNECTED: ESP_LOGI(TAG, "MQTT: Desconectado do broker"); mqtt_connected = false; break;
        case MQTT_EVENT_PUBLISHED: ESP_LOGI(TAG, "MQTT: Mensagem publicada, msg_id=%d", event->msg_id); break;
        default: break;
    }
}
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URL, };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Inicialização do GPIO do LED
void gpio_init_led() {
    gpio_set_direction(LED_ALERTA, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_ALERTA, 0);
}

// Task Principal
void main_task(void *pvParameters) {
    char buffer_display[20];
    char buffer_mqtt[20];
    const char *status_str;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        float temp = 0.0f;
        esp_err_t res = max31865_measure(&sensor_dev, &temp);

        ssd1306_clear();
        ssd1306_print_str(10, 10, "Temp. PT100", false);

        if (res == ESP_OK) {
            temp += TEMP_OFFSET;

            if (temp > temp_max) temp_max = temp;
            if (temp < temp_min) temp_min = temp;

            if (temp < TEMP_MIN_IDEAL) {
                status_str = "ABAIXO";
                gpio_set_level(LED_ALERTA, 1);
            } else if (temp > TEMP_MAX_IDEAL) {
                status_str = "ACIMA";
                gpio_set_level(LED_ALERTA, 1);
            } else {
                status_str = "OK";
                gpio_set_level(LED_ALERTA, 0);
            }

            if (modo_display == 0) {
                snprintf(buffer_display, sizeof(buffer_display), "Atual: %.2fC", temp);
            } else if (modo_display == 1) {
                snprintf(buffer_display, sizeof(buffer_display), "Max: %.2fC", temp_max);
            } else {
                snprintf(buffer_display, sizeof(buffer_display), "Min: %.2fC", temp_min);
            }

        } else {
            snprintf(buffer_display, sizeof(buffer_display), "Falha Leitura");
            status_str = "ERRO";
            gpio_set_level(LED_ALERTA, 1);
            ESP_LOGE(TAG, "Falha ao ler o sensor.");
        }
        
        ssd1306_print_str(22, 30, buffer_display, false);
        ssd1306_print_str(30, 50, status_str, false);
        ssd1306_display();
        
        if (res == ESP_OK && mqtt_connected) {
            snprintf(buffer_mqtt, sizeof(buffer_mqtt), "%.2f", temp);
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, buffer_mqtt, 0, 1, 0);
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, status_str, 0, 1, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// App_main
void app_main(void) {
    ESP_LOGI(TAG, "Iniciando Monitor de Temperatura PT100 com MQTT...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_init_led();
    botao_init();
    init_ssd1306();

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SENSOR_SPI_HOST, &buscfg, 0));
    ESP_ERROR_CHECK(max31865_init_desc(&sensor_dev, SENSOR_SPI_HOST, MAX31865_MAX_CLOCK_SPEED_HZ, PIN_NUM_CS));
    sensor_dev.r_ref = 430.0f;
    sensor_dev.rtd_nominal = 100.0f;
    sensor_dev.standard = MAX31865_ITS90;
    max31865_config_t config = {
        .mode = MAX31865_MODE_AUTO,
        .connection = MAX31865_2WIRE,
        .v_bias = true,
        .filter = MAX31865_FILTER_50HZ
    };
    ESP_ERROR_CHECK(max31865_set_config(&sensor_dev, &config));
    
    xTaskCreate(main_task, "main_task", 4096, &sensor_dev, 5, NULL);

    wifi_init_sta();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi conectado, iniciando MQTT...");
        mqtt_app_start();
    } else {
        ESP_LOGE(TAG, "Falha ao conectar ao Wi-Fi. A publicação MQTT não funcionará.");
    }
}
