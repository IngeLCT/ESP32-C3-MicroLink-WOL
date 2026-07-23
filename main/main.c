#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "microlink.h"

static const char *TAG = "tailscale_wol";

#define WIFI_CONNECTED_BIT BIT0
#define MAGIC_PACKET_SIZE 102
#define HTTP_REQUEST_SIZE 1024
#define WOL_REPEAT_COUNT 3

static EventGroupHandle_t wifi_events;
static esp_netif_t *wifi_netif;
static microlink_t *microlink;
static uint8_t target_mac[6];

static bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int values[6];
    char tail;

    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5], &tail) != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        mac[i] = (uint8_t)values[i];
    }
    return true;
}

static bool constant_time_equal(const char *left, size_t left_len,
                                const char *right, size_t right_len)
{
    size_t max_len = left_len > right_len ? left_len : right_len;
    unsigned char difference = (unsigned char)(left_len ^ right_len);

    for (size_t i = 0; i < max_len; ++i) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

static bool bearer_token_is_valid(const char *request)
{
    static const char header_name[] = "Authorization:";
    static const char bearer_prefix[] = "Bearer ";
    const char *line = request;

    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL) {
            break;
        }
        if (line_end == line) {
            break;
        }

        size_t line_len = (size_t)(line_end - line);
        if (line_len > sizeof(header_name) - 1 &&
            strncasecmp(line, header_name, sizeof(header_name) - 1) == 0) {
            const char *value = line + sizeof(header_name) - 1;
            while (value < line_end && isspace((unsigned char)*value)) {
                ++value;
            }
            if ((size_t)(line_end - value) < sizeof(bearer_prefix) - 1 ||
                strncasecmp(value, bearer_prefix, sizeof(bearer_prefix) - 1) != 0) {
                return false;
            }
            value += sizeof(bearer_prefix) - 1;
            return constant_time_equal(value, (size_t)(line_end - value),
                                       CONFIG_WOL_SHARED_SECRET,
                                       strlen(CONFIG_WOL_SHARED_SECRET));
        }
        line = line_end + 2;
    }
    return false;
}

static esp_err_t send_magic_packet(void)
{
    uint8_t packet[MAGIC_PACKET_SIZE];
    memset(packet, 0xFF, 6);
    for (size_t offset = 6; offset < sizeof(packet); offset += 6) {
        memcpy(packet + offset, target_mac, 6);
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(wifi_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t broadcast = (ip_info.ip.addr & ip_info.netmask.addr) |
                         ~ip_info.netmask.addr;
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(9),
        .sin_addr.s_addr = broadcast,
    };

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        return ESP_FAIL;
    }

    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                   &enabled, sizeof(enabled)) < 0) {
        close(socket_fd);
        return ESP_FAIL;
    }

    for (int attempt = 0; attempt < WOL_REPEAT_COUNT; ++attempt) {
        ssize_t sent = sendto(socket_fd, packet, sizeof(packet), 0,
                              (struct sockaddr *)&destination,
                              sizeof(destination));
        if (sent != sizeof(packet)) {
            ESP_LOGE(TAG, "Magic Packet send failed: errno=%d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }
        if (attempt + 1 < WOL_REPEAT_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    close(socket_fd);

    struct in_addr broadcast_addr = {.s_addr = broadcast};
    ESP_LOGI(TAG,
             "Magic Packet sent %d times to " MACSTR " via %s:9",
             WOL_REPEAT_COUNT, MAC2STR(target_mac), inet_ntoa(broadcast_addr));
    return ESP_OK;
}

static void send_http_response(int client, const char *status,
                               const char *body)
{
    char response[384];
    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: no-store\r\n"
                          "\r\n%s",
                          status, (unsigned int)strlen(body), body);
    if (length > 0 && length < (int)sizeof(response)) {
        send(client, response, (size_t)length, 0);
    }
}

static void handle_http_request(int client)
{
    char request[HTTP_REQUEST_SIZE];
    ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    if (received <= 0) {
        return;
    }
    request[received] = '\0';

    if (strncmp(request, "GET /health ", 12) == 0) {
        send_http_response(client, "200 OK",
                           "{\"status\":\"ok\",\"service\":\"wol\"}\n");
        return;
    }

    if (strncmp(request, "POST /wol ", 10) != 0) {
        send_http_response(client, "404 Not Found",
                           "{\"error\":\"not_found\"}\n");
        return;
    }

    if (!bearer_token_is_valid(request)) {
        ESP_LOGW(TAG, "Rejected unauthorized Wake-on-LAN request");
        send_http_response(client, "401 Unauthorized",
                           "{\"error\":\"unauthorized\"}\n");
        return;
    }

    esp_err_t err = send_magic_packet();
    if (err == ESP_OK) {
        send_http_response(client, "200 OK",
                           "{\"status\":\"magic_packet_sent\"}\n");
    } else {
        send_http_response(client, "500 Internal Server Error",
                           "{\"error\":\"magic_packet_failed\"}\n");
    }
}

static void private_http_task(void *argument)
{
    uint32_t vpn_ip = (uint32_t)(uintptr_t)argument;
    int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server < 0) {
        ESP_LOGE(TAG, "Unable to create private HTTP socket: errno=%d", errno);
        vTaskDelete(NULL);
    }

    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_WOL_HTTP_PORT),
        .sin_addr.s_addr = htonl(vpn_ip),
    };

    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, 2) < 0) {
        ESP_LOGE(TAG, "Unable to bind private HTTP endpoint: errno=%d", errno);
        close(server);
        vTaskDelete(NULL);
    }

    char vpn_ip_text[16];
    microlink_ip_to_str(vpn_ip, vpn_ip_text);
    ESP_LOGI(TAG, "Private endpoint ready at http://%s:%d",
             vpn_ip_text, CONFIG_WOL_HTTP_PORT);

    while (true) {
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        int client = accept(server, (struct sockaddr *)&source, &source_length);
        if (client < 0) {
            ESP_LOGW(TAG, "HTTP accept failed: errno=%d", errno);
            continue;
        }

        struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        handle_http_request(client);
        shutdown(client, SHUT_RDWR);
        close(client);
    }
}

static void microlink_state_changed(microlink_t *handle,
                                    microlink_state_t state,
                                    void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, "MicroLink state=%d", state);
    if (state == ML_STATE_CONNECTED) {
        char vpn_ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(handle), vpn_ip);
        ESP_LOGI(TAG, "Connected to tailnet as %s", vpn_ip);
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)config.sta.ssid, CONFIG_ML_WIFI_SSID,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_ML_WIFI_PASSWORD,
            sizeof(config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (strlen(CONFIG_ML_WIFI_SSID) == 0 ||
        strlen(CONFIG_ML_TAILSCALE_AUTH_KEY) == 0 ||
        strlen(CONFIG_WOL_SHARED_SECRET) < 16 ||
        !parse_mac(CONFIG_WOL_TARGET_MAC, target_mac)) {
        ESP_LOGE(TAG,
                 "Invalid configuration. Set Wi-Fi, Tailscale key, target MAC "
                 "and a WOL secret of at least 16 characters.");
        return;
    }

    wifi_start();
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID '%s'", CONFIG_ML_WIFI_SSID);
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    microlink_config_t config = {
        .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
    };

    microlink = microlink_init(&config);
    if (microlink == NULL) {
        ESP_LOGE(TAG, "MicroLink initialization failed");
        return;
    }
    microlink_set_state_callback(microlink, microlink_state_changed, NULL);
    ESP_ERROR_CHECK(microlink_start(microlink));

    while (!microlink_is_connected(microlink)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    uint32_t vpn_ip = microlink_get_vpn_ip(microlink);
    BaseType_t created = xTaskCreate(private_http_task, "private_http",
                                    4096, (void *)(uintptr_t)vpn_ip, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Unable to start private HTTP task");
    }
}
