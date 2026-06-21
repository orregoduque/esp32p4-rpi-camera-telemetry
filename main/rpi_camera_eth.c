/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ethernet transport for Waveshare ESP32-P4 boards with IP101 PHY.
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "rpi_camera_config.h"
#include "rpi_camera_net.h"
#include "rpi_camera_tcp.h"

static const char *TAG = "rpi_eth";

#define ETH_CONNECTED_BIT    BIT0

/* Waveshare ESP32-P4-Module-DEV-KIT / NANO RMII pins */
#define RPI_ETH_PHY_ADDR        1
#define RPI_ETH_PHY_RST_GPIO    51
#define RPI_ETH_MDC_GPIO        31
#define RPI_ETH_MDIO_GPIO       52
#define RPI_ETH_RMII_CLK_GPIO   50
#define RPI_ETH_RMII_TX_EN      49
#define RPI_ETH_RMII_TXD0       34
#define RPI_ETH_RMII_TXD1       35
#define RPI_ETH_RMII_CRS_DV     28
/* ESP32-P4 IOMUX: RXD0=GPIO29, RXD1=GPIO30 (see emac_periph.c / IDF ethernet/basic) */
#define RPI_ETH_RMII_RXD0       29
#define RPI_ETH_RMII_RXD1       30

static EventGroupHandle_t s_eth_events;
static esp_netif_t *s_eth_netif;
static char s_ip_str[16];
static bool s_ready;

#if RPI_ETH_USE_STATIC_IP
static esp_err_t apply_static_ip(esp_netif_t *netif)
{
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcpc_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_ip_info_t ip = {0};
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(RPI_ETH_STATIC_IP, &ip.ip), TAG, "bad static IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(RPI_ETH_STATIC_NETMASK, &ip.netmask), TAG, "bad netmask");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(RPI_ETH_STATIC_GW, &ip.gw), TAG, "bad gateway");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "set_ip_info failed");

    snprintf(s_ip_str, sizeof(s_ip_str), RPI_ETH_STATIC_IP);
    s_ready = true;
    xEventGroupSetBits(s_eth_events, ETH_CONNECTED_BIT);
    ESP_LOGI(TAG, "static IP %s gw %s mask %s", RPI_ETH_STATIC_IP, RPI_ETH_STATIC_GW, RPI_ETH_STATIC_NETMASK);
    return ESP_OK;
}
#endif

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
    s_ready = true;
    ESP_LOGI(TAG, "Ethernet IP: %s", s_ip_str);
    xEventGroupSetBits(s_eth_events, ETH_CONNECTED_BIT);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
#if RPI_ETH_USE_STATIC_IP
        if (netif) {
            if (apply_static_ip(netif) != ESP_OK) {
                ESP_LOGW(TAG, "static IP reapply failed after link up");
            }
        }
#endif
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_ready = false;
        rpi_camera_tcp_on_link_down();
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    default:
        break;
    }
}

static esp_err_t eth_driver_init(esp_eth_handle_t *eth_handle_out)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = RPI_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = RPI_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = RPI_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = RPI_ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = RPI_ETH_RMII_CLK_GPIO;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = RPI_ETH_RMII_TX_EN;
    emac_config.emac_dataif_gpio.rmii.txd0_num = RPI_ETH_RMII_TXD0;
    emac_config.emac_dataif_gpio.rmii.txd1_num = RPI_ETH_RMII_TXD1;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = RPI_ETH_RMII_CRS_DV;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = RPI_ETH_RMII_RXD0;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = RPI_ETH_RMII_RXD1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "MAC init failed");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "PHY init failed");
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, eth_handle_out), TAG, "eth install failed");
    return ESP_OK;
}

esp_err_t rpi_camera_net_init(void)
{
    s_eth_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_eth_events, ESP_ERR_NO_MEM, TAG, "event group");

    esp_eth_handle_t eth_handle = NULL;
    ESP_RETURN_ON_ERROR(eth_driver_init(&eth_handle), TAG, "eth init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "netif attach");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, s_eth_netif), TAG, "eth event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL), TAG, "ip event");

    ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), TAG, "eth start");
    return ESP_OK;
}

esp_err_t rpi_camera_net_wait_for_ip(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_eth_events, ETH_CONNECTED_BIT, pdFALSE, pdTRUE, ticks);
    return (bits & ETH_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char *rpi_camera_net_get_ip_str(void)
{
    return s_ip_str;
}

bool rpi_camera_net_is_ready(void)
{
    return s_ready;
}
