#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_matter.h"
#include "esp_matter_console.h"

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;

namespace {

constexpr size_t kMagicPacketSize = 102;
constexpr auto kCommissioningTimeout = chip::System::Clock::Seconds16(300);

const char *TAG = "alexa_wol";
uint16_t wol_endpoint_id = 0;
uint8_t target_mac[6] = {};
std::atomic_bool wol_task_running{false};

bool parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int values[6];
    char tail;

    if (text == nullptr ||
        std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c",
                    &values[0], &values[1], &values[2],
                    &values[3], &values[4], &values[5], &tail) != 6) {
        return false;
    }

    bool all_zero = true;
    for (size_t index = 0; index < 6; ++index) {
        mac[index] = static_cast<uint8_t>(values[index]);
        all_zero &= mac[index] == 0;
    }
    return !all_zero;
}

esp_err_t send_magic_packet()
{
    esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi_netif == nullptr) {
        ESP_LOGW(TAG, "La interfaz Wi-Fi todavía no está disponible");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info = {};
    esp_err_t err = esp_netif_get_ip_info(wifi_netif, &ip_info);
    if (err != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "El ESP32 todavía no tiene una dirección IPv4");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t packet[kMagicPacketSize];
    std::memset(packet, 0xFF, 6);
    for (size_t offset = 6; offset < sizeof(packet); offset += 6) {
        std::memcpy(packet + offset, target_mac, 6);
    }

    const uint32_t broadcast = (ip_info.ip.addr & ip_info.netmask.addr) |
                               ~ip_info.netmask.addr;
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(CONFIG_WOL_UDP_PORT);
    destination.sin_addr.s_addr = broadcast;

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "No se pudo crear el socket UDP: errno=%d", errno);
        return ESP_FAIL;
    }

    int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                   &enabled, sizeof(enabled)) < 0) {
        ESP_LOGE(TAG, "No se pudo habilitar SO_BROADCAST: errno=%d", errno);
        close(socket_fd);
        return ESP_FAIL;
    }

    for (int attempt = 0; attempt < CONFIG_WOL_REPEAT_COUNT; ++attempt) {
        const ssize_t sent = sendto(socket_fd, packet, sizeof(packet), 0,
                                    reinterpret_cast<sockaddr *>(&destination),
                                    sizeof(destination));
        if (sent != static_cast<ssize_t>(sizeof(packet))) {
            ESP_LOGE(TAG, "Falló el envío del Magic Packet: errno=%d", errno);
            close(socket_fd);
            return ESP_FAIL;
        }

        if (attempt + 1 < CONFIG_WOL_REPEAT_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_WOL_REPEAT_DELAY_MS));
        }
    }

    close(socket_fd);

    in_addr broadcast_address = {.s_addr = broadcast};
    ESP_LOGI(TAG,
             "Magic Packet enviado %d veces a %02X:%02X:%02X:%02X:%02X:%02X mediante %s:%d",
             CONFIG_WOL_REPEAT_COUNT,
             target_mac[0], target_mac[1], target_mac[2],
             target_mac[3], target_mac[4], target_mac[5],
             inet_ntoa(broadcast_address), CONFIG_WOL_UDP_PORT);
    return ESP_OK;
}

void return_switch_to_off(intptr_t)
{
    do {
        attribute_t *on_off_attribute =
            attribute::get(wol_endpoint_id, OnOff::Id,
                           OnOff::Attributes::OnOff::Id);
        if (on_off_attribute == nullptr) {
            ESP_LOGE(TAG, "No se encontró el atributo OnOff de Matter");
            break;
        }

        esp_matter_attr_val_t value;
        if (attribute::get_val(on_off_attribute, &value) != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo leer el estado del interruptor Matter");
            break;
        }

        value.val.b = false;
        esp_err_t err = attribute::update(
            wol_endpoint_id, OnOff::Id,
            OnOff::Attributes::OnOff::Id, &value);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo devolver el interruptor a OFF: %s",
                     esp_err_to_name(err));
        }
    } while (false);

    wol_task_running.store(false);
}

void wol_task(void *)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;

    for (int attempt = 0;
         attempt < CONFIG_WOL_NETWORK_WAIT_SECONDS * 2;
         ++attempt) {
        result = send_magic_packet();
        if (result != ESP_ERR_INVALID_STATE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "No fue posible enviar el Magic Packet: %s",
                 esp_err_to_name(result));
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_WOL_MOMENTARY_RESET_MS));
    CHIP_ERROR schedule_result =
        chip::DeviceLayer::PlatformMgr().ScheduleWork(
            return_switch_to_off, static_cast<intptr_t>(0));
    if (schedule_result != CHIP_NO_ERROR) {
        ESP_LOGE(TAG,
                 "No se pudo programar el retorno a OFF en el hilo Matter: %s",
                 schedule_result.AsString());
        wol_task_running.store(false);
    }
    vTaskDelete(nullptr);
}

esp_err_t start_wol_task()
{
    bool expected = false;
    if (!wol_task_running.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Ya hay un envío Wake-on-LAN en curso");
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(wol_task, "wol_sender", 4096,
                                     nullptr, 5, nullptr);
    if (created != pdPASS) {
        wol_task_running.store(false);
        ESP_LOGE(TAG, "No se pudo crear la tarea Wake-on-LAN");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t attribute_update_cb(attribute::callback_type_t type,
                              uint16_t endpoint_id,
                              uint32_t cluster_id,
                              uint32_t attribute_id,
                              esp_matter_attr_val_t *value,
                              void *)
{
    if (type == PRE_UPDATE &&
        endpoint_id == wol_endpoint_id &&
        cluster_id == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id &&
        value->val.b) {
        ESP_LOGI(TAG, "Alexa/Matter solicitó encender la computadora");
        return start_wol_task();
    }
    return ESP_OK;
}

esp_err_t identification_cb(identification::callback_type_t type,
                            uint16_t endpoint_id,
                            uint8_t effect_id,
                            uint8_t effect_variant,
                            void *)
{
    ESP_LOGI(TAG,
             "Identificación Matter: tipo=%u endpoint=%u efecto=%u variante=%u",
             type, endpoint_id, effect_id, effect_variant);
    return ESP_OK;
}

void matter_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "La dirección IP de la interfaz cambió");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Emparejamiento Matter completado");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "El emparejamiento Matter expiró");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Inició la sesión de emparejamiento Matter");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Terminó la sesión de emparejamiento Matter");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Se eliminó una red Matter");
        auto &manager =
            chip::Server::GetInstance().GetCommissioningWindowManager();
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0 &&
            !manager.IsCommissioningWindowOpen()) {
            CHIP_ERROR err = manager.OpenBasicCommissioningWindow(
                kCommissioningTimeout,
                chip::CommissioningWindowAdvertisement::kDnssdOnly);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG,
                         "No se pudo reabrir la ventana de emparejamiento: %s",
                         err.AsString());
            }
        }
        break;
    }
    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE liberado después del emparejamiento");
        break;
    default:
        break;
    }
}

} // namespace

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (!parse_mac(CONFIG_WOL_TARGET_MAC, target_mac)) {
        ESP_LOGE(TAG,
                 "CONFIG_WOL_TARGET_MAC no es una MAC válida. "
                 "Configúrala como AA:BB:CC:DD:EE:FF.");
        return;
    }

    node::config_t node_config;
    strlcpy(node_config.root_node.basic_information.node_label,
            CONFIG_WOL_DEVICE_NAME,
            sizeof(node_config.root_node.basic_information.node_label));
    node_t *node = node::create(&node_config, attribute_update_cb,
                                identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "No se pudo crear el nodo Matter");
        return;
    }

    on_off_plug_in_unit::config_t plug_config;
    plug_config.on_off.on_off = false;
    endpoint_t *endpoint = on_off_plug_in_unit::create(
        node, &plug_config, ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        ESP_LOGE(TAG, "No se pudo crear el interruptor Matter");
        return;
    }

    wol_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG,
             "Interruptor Matter '%s' creado en endpoint %u para "
             "%02X:%02X:%02X:%02X:%02X:%02X",
             CONFIG_WOL_DEVICE_NAME, wol_endpoint_id,
             target_mac[0], target_mac[1], target_mac[2],
             target_mac[3], target_mac[4], target_mac[5]);

    err = esp_matter::start(matter_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar Matter: %s",
                 esp_err_to_name(err));
        return;
    }

#if CONFIG_ENABLE_CHIP_SHELL
    ESP_ERROR_CHECK(
        esp_matter::console::diagnostics_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::wifi_register_commands());
    ESP_ERROR_CHECK(
        esp_matter::console::factoryreset_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::init());
#endif
}
