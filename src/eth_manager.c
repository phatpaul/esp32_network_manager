/*
 * This file is part of the ESP WiFi Manager project.
 * Copyright (C) 2019  Tido Klaassen <tido_wmngr@4gh.eu>
 * Ethernet Manager added by Paul Abbott 2025
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include "eth_manager.h"

#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"

#include "lwip/ip4.h"
#include "lwip/ip_addr.h"

#include "kutils.h"
#include "kref.h"

static const char *TAG = "eth_manager";

#define WMNGR_NAMESPACE "eth_manager"
#define NVS_CFG_VER     1
#define ETH_USE_IPV6 (CONFIG_LWIP_IPV6)

/* For keeping track of system events. */
#define BIT_ETH_START           BIT1
#define BIT_ETH_CONNECTED       BIT2
#define BIT_ETH_GOT_IP          BIT3

struct eth_manager_handle_s {
    struct kref ref_cnt; /*!< Reference count for the handle. */
    esp_netif_t *eth_netif;
    esp_eth_handle_t eth_handle;
    EventGroupHandle_t eth_events;
};
static struct eth_manager_handle_s *handle = NULL;

/** Set configuration from compiled-in defaults.
 */
static void set_defaults(struct eth_cfg *cfg)
{
    eth_cfg_init(cfg);
    cfg->is_default = true;
    cfg->is_valid = true;
    cfg->is_static = false;
    cfg->is_disabled = false;
}

/** Read saved configuration from NVS.
 *
 * Read configuration from NVS and store it in the struct wifi_cfg.
 * @param[out] cfg Configuration read from NVS.
 * @return ESP_OK if valid configuration was found in NVS, ESP_ERR_* otherwise.
 */
static esp_err_t get_saved_config(struct eth_cfg *cfg)
{
    nvs_handle handle;
    size_t len;
    uint32_t tmp;
    esp_err_t result;

    result = ESP_OK;

    eth_cfg_init(cfg);

    result = nvs_open(WMNGR_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "[%s] nvs_open() failed.", __func__);
        return result;
    }

    /* Make sure we know how to handle the stored configuration. */
    result = nvs_get_u32(handle, "version", &tmp);
    if (result != ESP_OK) {
        goto on_exit;
    }

    if (tmp > NVS_CFG_VER) {
        result = ESP_ERR_INVALID_VERSION;
        goto on_exit;
    }

    result = nvs_get_u32(handle, "eth_static", &tmp);
    if (result != ESP_OK) {
        goto on_exit;
    }
    cfg->is_static = (bool)tmp;

    result = nvs_get_u32(handle, "eth_disable", &tmp);
    if (result != ESP_OK) {
        goto on_exit;
    }
    cfg->is_disabled = (bool)tmp;

    len = sizeof(cfg->ip_info);
    result = nvs_get_blob(handle, "eth_ip", &(cfg->ip_info), &len);
    if (result != ESP_OK || len != sizeof(cfg->ip_info)) {
        result = (result != ESP_OK) ? result : ESP_ERR_NOT_FOUND;
        goto on_exit;
    }

    len = sizeof(cfg->dns_info);
    result = nvs_get_blob(handle, "eth_dns", &(cfg->dns_info), &len);
    if (result != ESP_OK || len != sizeof(cfg->dns_info)) {
        result = (result != ESP_OK) ? result : ESP_ERR_NOT_FOUND;
        goto on_exit;
    }

on_exit:
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "[%s] Reading config failed.", __func__);
    }

    nvs_close(handle);
    return result;
}

static esp_err_t clear_config(void)
{
    nvs_handle handle;
    esp_err_t result;

    result = nvs_open(WMNGR_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "[%s] nvs_open() failed.", __func__);
        return result;
    }

    result = nvs_erase_all(handle);
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_commit(handle);
    if (result != ESP_OK) {
        goto on_exit;
    }

on_exit:
    nvs_close(handle);

    return result;
}

/** Save configuration to NVS.
 *
 * Store the wifi_cfg in NVS. The previously stored configuration will be
 * erased and not be recovered on error, so on return there will either be
 * a valid config or no config at all stored in the NVS.
 * This guarantees that the device is either reachable by the last valid
 * configuration or recoverable by the factory default settings.
 *
 * @param[in] cfg Configuration to be saved.
 * @return ESP_OK if configuration was saved, ESP_ERR_* otherwise.
 */
static esp_err_t save_config(struct eth_cfg *cfg)
{
    nvs_handle handle;
    esp_err_t result;

    result = nvs_open(WMNGR_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "[%s] nvs_open() failed.", __func__);
        return result;
    }

    /*
     * Erase the previous config so that we can be sure that we do not end up
     * with a mix of the old and new in case of a power-fail.
     *
     * FIXME: We should use a two slot mechanism so that the old config will
     *        not be touched until the new one has been written successfully.
     */
    result = clear_config();
    if (result != ESP_OK) {
        goto on_exit;
    }

    /* No point in saving the factory default settings. */
    if (cfg->is_default) {
        result = ESP_OK;
        goto on_exit;
    }

    /*
     * Write all elements of the struct wifi_cfg individually. This gives
     * us a chance to extend it later without forcing the user into a
     * "factory reset" after a firmware update.
     */

    result = nvs_set_u32(handle, "version", NVS_CFG_VER);
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_set_u32(handle, "eth_static", cfg->is_static);
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_set_u32(handle, "eth_disable", cfg->is_disabled);
    if (result != ESP_OK) {
        goto on_exit;
    }

    /* Store the esp-idf types as blobs. */
    /* FIXME: we should also store them component-wise. */
    result = nvs_set_blob(handle, "eth_ip", &(cfg->ip_info),
        sizeof(cfg->ip_info));
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_set_blob(handle, "eth_dns", &(cfg->dns_info),
        sizeof(cfg->dns_info));
    if (result != ESP_OK) {
        goto on_exit;
    }

on_exit:
    if (result != ESP_OK) {
        /* we do not want to leave a half-written config lying around. */
        ESP_LOGE(TAG, "[%s] Writing config failed.", __func__);
        (void)nvs_erase_all(handle);
    }

    (void)nvs_commit(handle);
    nvs_close(handle);

    return result;
}

/* Helper function to check if WiFi is connected in station mode. */
static bool eth_connected(void)
{
    EventBits_t events;
    if (NULL == handle || NULL == handle->eth_events) {
        ESP_LOGE(TAG, "not initialized");
        return false;
    }
    events = xEventGroupGetBits(handle->eth_events);

    return !!(events & BIT_ETH_CONNECTED);
}

static esp_err_t start_eth(void)
{
    esp_err_t result = ESP_OK;

    if (NULL == handle || NULL == handle->eth_handle || NULL == handle->eth_events) {
        ESP_LOGE(TAG, "Ethernet Manager not initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    if (xEventGroupGetBits(handle->eth_events) & BIT_ETH_START) {
        ESP_LOGI(TAG, "Ethernet already started.");
        return ESP_OK;
    }

    // Start the Ethernet driver state machine
    result = esp_eth_start(handle->eth_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Ethernet. %s", esp_err_to_name(result));
    }

    return result;
}

esp_err_t stop_eth(void)
{
    esp_err_t result = ESP_OK;

    if (NULL == handle || NULL == handle->eth_handle || NULL == handle->eth_events) {
        ESP_LOGE(TAG, "Ethernet Manager not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    if (!(xEventGroupGetBits(handle->eth_events) & BIT_ETH_START)) {
        ESP_LOGI(TAG, "Ethernet already stopped.");
        return ESP_OK;
    }

    // Stop the Ethernet driver state machine
    result = esp_eth_stop(handle->eth_handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop Ethernet. %s", esp_err_to_name(result));
    }

    return result;
}

/* Helper function to set Ethernet configuration from struct eth_cfg. */
static esp_err_t set_eth_cfg(struct eth_cfg *cfg)
{
    unsigned int idx;
    esp_err_t result;

    ESP_LOGD(TAG, "[%s] Called.", __FUNCTION__);

    if (cfg->is_disabled)
    {
        // Todo disable ethernet interface
        ESP_LOGI(TAG, "Disabling Ethernet interface.");
        result = stop_eth();
        return result; // no need to continue
    }

    if (cfg->is_static) {
        (void)esp_netif_dhcpc_stop(handle->eth_netif);

        result = esp_netif_set_ip_info(handle->eth_netif, &cfg->ip_info);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_set_ip_info() STA: %s", esp_err_to_name(result));
        }

        for (idx = 0; idx < ARRAY_SIZE(cfg->dns_info); ++idx) {
            if (ip_addr_isany_val(cfg->dns_info[idx].ip)) {
                continue;
            }

            result = esp_netif_set_dns_info(handle->eth_netif,
                idx,
                &(cfg->dns_info[idx]));
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Setting DNS server IP failed.");
            }
        }
    }
    else {
        result = esp_netif_dhcpc_start(handle->eth_netif);
        if (ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED == result) {
            result = ESP_OK;
            ESP_LOGD(TAG, "DHCP already started.");
        }
    }

    // Enable ethernet interface
    ESP_LOGI(TAG, "Enabling Ethernet interface.");
    result = start_eth();

    return result;
}

bool cfgs_are_equal(struct eth_cfg *a, struct eth_cfg *b)
{
    unsigned int idx;
    bool result;

    result = false;

    /*
     * Do some naive checks to see if the new configuration is an actual
     * change. Should be more thorough by actually comparing the elements.
     */
    if (a->is_disabled != b->is_disabled) {
        goto on_exit;
    }

    if (a->is_static != b->is_static) {
        goto on_exit;
    }


    if (a->is_static) {
        if (!ip4_addr_cmp(&(a->ip_info.ip), &(b->ip_info.ip))) {
            goto on_exit;
        }

        if (!ip4_addr_cmp(&(a->ip_info.netmask), &(b->ip_info.netmask))) {
            goto on_exit;
        }

        if (!ip4_addr_cmp(&(a->ip_info.gw), &(b->ip_info.gw))) {
            goto on_exit;
        }

        for (idx = 0; idx < ARRAY_SIZE(a->dns_info); ++idx) {
            if (!ip_addr_cmp(&(a->dns_info[idx].ip),
                &(b->dns_info[idx].ip)))
            {
                goto on_exit;
            }
        }
    }

    result = true;

on_exit:
    return result;
}


/**
 * * @brief Load the configuration from NVS or provide defaults.
 * @param cfg[out] Pointer to the configuration structure to be filled.
 * @return ESP_OK if config was set, ESP_ERR_* otherwise.
 */
static esp_err_t get_saved_or_default_config(struct eth_cfg *cfg_state)
{
    esp_err_t result = ESP_OK;
    /*
     * Restore saved WiFi config or fall back to compiled-in defaults.
     * Setting state to update will trigger applying this config.
     */
    result = get_saved_config(cfg_state);
    if (result != ESP_OK) {
        ESP_LOGI(TAG, "No saved config found, setting defaults");
        set_defaults(cfg_state);
        result = ESP_OK;
    }

    /* Any config read from NVS or restored from defaults should be valid. */
    cfg_state->is_valid = true;

    return result;
}

/*
 * Helper to fetch current Ethernet state from the system and return it in
 * a eth_cfg struct.
 */
static esp_err_t get_eth_state(struct eth_cfg *cfg)
{
    esp_netif_dhcp_status_t dhcp_status;
    unsigned int idx;
    esp_err_t result;

    result = ESP_OK;
    eth_cfg_init(cfg);

    bool connected = eth_connected();
    if (!connected) {
        // not currently connected, so just return default config settings
        result = get_saved_or_default_config(cfg);
        cfg->is_connected = false;
        goto on_exit;
    }

    // Else we are connected, so get the current state
    cfg->is_connected = true;

    // See if DHCP is enabled on the Ethernet interface
    result = esp_netif_dhcpc_get_status(handle->eth_netif, &dhcp_status);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "[%s] Error fetching DHCP status.", __func__);
        goto on_exit;
    }

    // If DHCP is stopped on Ethernet interface, assume static IP
    if (ESP_NETIF_DHCP_STOPPED == dhcp_status) {
        cfg->is_static = 1;
    }

    result = esp_netif_get_ip_info(handle->eth_netif, &cfg->ip_info);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "[%s] esp_netif_get_ip_info() STA: %d %s",
            __func__, result, esp_err_to_name(result));
        goto on_exit;
    }

    for (idx = 0; idx < ARRAY_SIZE(cfg->dns_info); ++idx) {
        result = esp_netif_get_dns_info(handle->eth_netif,
            idx,
            &(cfg->dns_info[idx]));
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "[%s] Getting DNS server IP failed.",
                __func__);
            goto on_exit;
        }
    }

    cfg->is_valid = true;

on_exit:
    return result;
}

/** Event handler for Ethernet events */
static void eth_event_handler(void *esp_netif, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{

    if (ETH_EVENT == event_base) {
        switch (event_id)
        {
        case ETHERNET_EVENT_CONNECTED:
            xEventGroupSetBits(handle->eth_events, BIT_ETH_CONNECTED);
#if ETH_USE_IPV6
            esp_netif_create_ip6_linklocal(esp_netif);
#endif // ETH_USE_IPV6
            if (LOG_LOCAL_LEVEL >= ESP_LOG_INFO)
            {
                uint8_t mac_addr[6] = {0};
                /* we can get the ethernet driver handle from event data */
                esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
                esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &mac_addr[0]);
                ESP_LOGI(TAG, "Ethernet Link Up. HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                    mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            xEventGroupClearBits(handle->eth_events, BIT_ETH_CONNECTED);
            break;
        case ETHERNET_EVENT_START:
            xEventGroupSetBits(handle->eth_events, BIT_ETH_START);
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            xEventGroupClearBits(handle->eth_events, BIT_ETH_START);
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
        }
    }

    // if (IP_EVENT == event_base) {
    //     switch (event_id) {
    //     case IP_EVENT_STA_GOT_IP:
    //         xEventGroupSetBits(handle->eth_events, BIT_ETH_GOT_IP);
    //         break;
    //     case IP_EVENT_STA_LOST_IP:
    //         xEventGroupClearBits(handle->eth_events, BIT_ETH_GOT_IP);
    //         break;
    //     default:
    //         break;
    //     }
    // }
}


/*****************************************************************************\
 *  API functions                                                            *
\*****************************************************************************/

/** Initialize the Ethernet Manager.
 *
 * Calling this function will initialise the Ethernet Manger. It must be called
 * after initialising the NVS, default event loop, Ethernet Driver, and TCP adapter and before
 * calling any other esp_wmngr function.
 *
 * @param eth_handle Pointer to the Ethernet handle which was created by esp_eth_driver_install()
 * @return int 0: success, other: error code
 */
esp_err_t eth_manager_init(esp_eth_handle_t eth_handle)
{
    esp_err_t result = ESP_OK;

    if (NULL != handle) {
        ESP_LOGE(TAG, "Ethernet Manager already initialized.");
        return ESP_ERR_INVALID_STATE;
    }
    handle = calloc(1, sizeof(*handle));
    if (NULL == handle) {
        result = ESP_ERR_NO_MEM;
        goto on_exit;
    }

    kref_init(&(handle->ref_cnt)); // initialises ref_cnt to 1
    handle->eth_handle = eth_handle;

    handle->eth_events = xEventGroupCreate();
    if (NULL == handle->eth_events) {
        result = ESP_ERR_NO_MEM;
        goto on_exit;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    handle->eth_netif = esp_netif_new(&cfg);
    if (NULL == handle->eth_netif)
    {
        goto on_exit;
    }
    // Set default handlers to process TCP/IP stuffs -- NOT NEEDED in IDF v4+
    // err = esp_eth_set_default_handlers(handle->eth_netif);
    // if (err != 0)
    // {
    //     goto on_exit;
    // }

    /* attach Ethernet driver to TCP/IP stack */
    result = esp_netif_attach(handle->eth_netif, esp_eth_new_netif_glue(handle->eth_handle));
    if (ESP_OK != result)
    {
        goto on_exit;
    }
    // Register user defined event handers
    result = esp_event_handler_instance_register(ETH_EVENT,
        ESP_EVENT_ANY_ID,
        &eth_event_handler,
        handle->eth_netif,
        NULL);
    if (ESP_OK != result)
    {
        goto on_exit;
    }

    // Load the saved configuration from NVS
    struct eth_cfg cfg_state;
    result = get_saved_or_default_config(&cfg_state);
    if (ESP_OK != result)
    {
        goto on_exit;
    }
    // Set the configuration to the Ethernet interface
    result = set_eth_cfg(&cfg_state);
    if (ESP_OK != result)
    {
        goto on_exit;
    }

on_exit:
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Ethernet. %s", esp_err_to_name(result));
        if (NULL != handle) {
            if (handle->eth_events != NULL) {
                vEventGroupDelete(handle->eth_events);
                handle->eth_events = NULL;
            }
            free(handle);
            handle = NULL;
        }
    }
    return result;
}

/** Set a new Network Manager configuration.
 *
 * @param[in] new New WiFi Manager configuration to be set.
 * @return ESP_OK if config was set, ESP_ERR_* otherwise.
 */
esp_err_t eth_manager_set_eth_cfg(struct eth_cfg *new_cfg)
{
    // TODO: make this use the asynchronous mechanism, just hacked for now
    save_config(new_cfg);
    return set_eth_cfg(new_cfg);
}

/**
 * @brief Get the current Network Manager configuration.
 * @param[out] new_cfg Pointer to the configuration structure to be filled.
 * @return ESP_OK if config was set, ESP_ERR_* otherwise.
 */
esp_err_t eth_manager_get_eth_cfg(struct eth_cfg *new_cfg)
{
    return get_saved_or_default_config(new_cfg);
}

/** Query current connection status.
 * @return
 */
esp_err_t eth_manager_get_eth_state(struct eth_cfg *get_state)
{
    return get_eth_state(get_state);
}

// Set the hostname on the Ethernet interface
esp_err_t eth_manager_set_hostname(const char *hostname)
{
    esp_err_t err = ESP_OK;

    if (NULL == handle || NULL == handle->eth_netif) {
        ESP_LOGE(TAG, "Ethernet Manager not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_netif_set_hostname(handle->eth_netif, hostname);
    if (err != 0)
    {
        ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
