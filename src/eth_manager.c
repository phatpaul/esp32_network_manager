#include "eth_manager.h"

#include <string.h>
// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "lwip/ip4.h"
#include "lwip/ip_addr.h"

#define ETH_USE_IPV6 (CONFIG_LWIP_IPV6)
#define WMNGR_NAMESPACE "esp_netman"
#define NVS_CFG_VER     1


static const char *TAG = "eth_manager";

static esp_netif_t *eth_netif = NULL;


/** Set configuration from compiled-in defaults.
 */
static void set_defaults(struct eth_cfg *cfg)
{
    memset(cfg, 0x0, sizeof(*cfg));
    cfg->is_default = true;
    cfg->is_valid = true;
    cfg->eth_static = false;
    cfg->eth_connect = true;
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

    memset(cfg, 0x0, sizeof(*cfg));

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
    cfg->eth_static = (bool)tmp;

    result = nvs_get_u32(handle, "eth_connect", &tmp);
    if (result != ESP_OK) {
        goto on_exit;
    }
    cfg->eth_connect = (bool)tmp;

    len = sizeof(cfg->eth_ip_info);
    result = nvs_get_blob(handle, "eth_ip", &(cfg->eth_ip_info), &len);
    if (result != ESP_OK || len != sizeof(cfg->eth_ip_info)) {
        result = (result != ESP_OK) ? result : ESP_ERR_NOT_FOUND;
        goto on_exit;
    }

    len = sizeof(cfg->eth_dns_info);
    result = nvs_get_blob(handle, "eth_dns", &(cfg->eth_dns_info), &len);
    if (result != ESP_OK || len != sizeof(cfg->eth_dns_info)) {
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

    result = nvs_set_u32(handle, "eth_static", cfg->eth_static);
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_set_u32(handle, "eth_connect", cfg->eth_connect);
    if (result != ESP_OK) {
        goto on_exit;
    }

    /* Store the esp-idf types as blobs. */
    /* FIXME: we should also store them component-wise. */
    result = nvs_set_blob(handle, "eth_ip", &(cfg->eth_ip_info),
        sizeof(cfg->eth_ip_info));
    if (result != ESP_OK) {
        goto on_exit;
    }

    result = nvs_set_blob(handle, "eth_dns", &(cfg->eth_dns_info),
        sizeof(cfg->eth_dns_info));
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

#if !defined(ARRAY_SIZE)
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#endif

static esp_err_t set_eth_cfg(struct eth_cfg *cfg)
{
    unsigned int idx;
    esp_err_t result;

    ESP_LOGD(TAG, "[%s] Called.", __FUNCTION__);

    if (cfg->eth_static) {
        (void)esp_netif_dhcpc_stop(eth_netif);

        result = esp_netif_set_ip_info(eth_netif, &cfg->eth_ip_info);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "[%s] esp_netif_set_ip_info() STA: %d %s",
                __func__, result, esp_err_to_name(result));
        }

        for (idx = 0; idx < ARRAY_SIZE(cfg->eth_dns_info); ++idx) {
            if (ip_addr_isany_val(cfg->eth_dns_info[idx].ip)) {
                continue;
            }

            result = esp_netif_set_dns_info(eth_netif,
                idx,
                &(cfg->eth_dns_info[idx]));
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "[%s] Setting DNS server IP failed.",
                    __func__);
            }
        }
    }
    else {
        result = esp_netif_dhcpc_start(eth_netif);
        if (ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED == result) {
            result = ESP_OK;
            ESP_LOGD(TAG, "[%s] DHCP already started.", __FUNCTION__);
        }
    }

    if (cfg->eth_connect)
    {
        // Todo enable ethernet interface
    }
    else {
        // Todo disable ethernet interface
    }

    return result;
}


/*
 * Helper to fetch current Ethernet configuration from the system and store it in
 * a eth_cfg struct.
 */
static esp_err_t get_eth_cfg(struct eth_cfg *cfg)
{
    esp_netif_dhcp_status_t dhcp_status;
    unsigned int idx;
    esp_err_t result;

    result = ESP_OK;
    memset(cfg, 0x0, sizeof(*cfg));

    result = esp_netif_dhcpc_get_status(eth_netif, &dhcp_status);
    if(result != ESP_OK){
        ESP_LOGE(TAG, "[%s] Error fetching DHCP status.", __func__);
        goto on_exit;
    }

    if(dhcp_status == ESP_NETIF_DHCP_STOPPED){
        cfg->eth_static = 1;

        result = esp_netif_get_ip_info(eth_netif, &cfg->eth_ip_info);
        if(result != ESP_OK){
            ESP_LOGE(TAG, "[%s] esp_netif_get_ip_info() STA: %d %s",
                    __func__, result, esp_err_to_name(result));
            goto on_exit;
        }

        for(idx = 0; idx < ARRAY_SIZE(cfg->eth_dns_info); ++idx){
            result = esp_netif_get_dns_info(eth_netif,
                                            idx,
                                            &(cfg->eth_dns_info[idx]));
            if(result != ESP_OK){
                ESP_LOGE(TAG, "[%s] Getting DNS server IP failed.",
                         __func__);
                goto on_exit;
            }

        }
    }

    cfg->is_valid = true;

on_exit:
    return result;
}

/**
 * * @brief Load the configuration from NVS or provide defaults.
 * @param cfg[out] Pointer to the configuration structure to be filled.
 * @return ESP_OK if config was set, ESP_ERR_* otherwise.
 */
static esp_err_t get_effective_config(struct eth_cfg *cfg)
{
    esp_err_t result = ESP_OK;
    struct eth_cfg cfg_state;
    /*
     * Restore saved WiFi config or fall back to compiled-in defaults.
     * Setting state to update will trigger applying this config.
     */
    result = get_saved_config(&cfg_state);
    if (result != ESP_OK) {
        ESP_LOGI(TAG, "[%s] No saved config found, setting defaults",
            __func__);
        set_defaults(&cfg_state);
    }

    /* Any config read from NVS or restored from defaults should be valid. */
    cfg_state.is_valid = true;

    return result;
}

/** Set a new Network Manager configuration.
 *
 * @param[in] new New WiFi Manager configuration to be set.
 * @return ESP_OK if config was set, ESP_ERR_* otherwise.
 */
esp_err_t netman_set_eth_cfg(struct eth_cfg *new_cfg)
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
esp_err_t netman_get_eth_cfg(struct eth_cfg *new_cfg)
{
    return get_effective_config(new_cfg);
}

/** Event handler for Ethernet events */
static void eth_event_handler(void *esp_netif, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, &mac_addr[0]);
        ESP_LOGI(TAG, "Ethernet Link Up. HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
            mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        // Set static IP address
        //example_set_static_ip(esp_netif);

#if ETH_USE_IPV6
        esp_netif_create_ip6_linklocal(esp_netif);
        ////esp_netif_create_ip6_linklocal(get_esp_interface_netif(ESP_IF_ETH));
#endif // ETH_USE_IPV6
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

// Set the hostname on the Ethernet interface
esp_err_t eth_manager_set_hostname(const char *hostname)
{
    esp_err_t err = ESP_OK;

    if (eth_netif == NULL)
    {
        ESP_LOGE(TAG, "Ethernet interface not initialized");
        return ESP_FAIL;
    }

    err = esp_netif_set_hostname(eth_netif, hostname);
    if (err != 0)
    {
        ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Initialize the Ethernet manager.
 * Do this after initializing the Ethernet driver and Initialize TCP/IP network interface.
 *
 * @param eth_handle Pointer to the Ethernet handle which was created by esp_eth_driver_install()
 * @return int 0: success, other: error code
 */
esp_err_t eth_manager_init(esp_eth_handle_t *eth_handle)
{
    esp_err_t err = ESP_OK;

    if (NULL == eth_handle)
    {
        ESP_LOGE(TAG, "eth_handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);
    if (NULL == eth_netif)
    {
        goto error;
    }
    // Set default handlers to process TCP/IP stuffs -- NOT NEEDED in IDF v4+
    // err = esp_eth_set_default_handlers(eth_netif);
    // if (err != 0)
    // {
    //     goto error;
    // }

    /* attach Ethernet driver to TCP/IP stack */
    err = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    if (err != 0)
    {
        goto error;
    }
    // Register user defined event handers
    err = esp_event_handler_instance_register(ETH_EVENT,
        ESP_EVENT_ANY_ID,
        &eth_event_handler,
        eth_netif,
        NULL);
    if (err != 0)
    {
        goto error;
    }
    /* start Ethernet driver state machine */
    err = esp_eth_start(eth_handle);
    if (err != 0)
    {
        goto error;
    }

    // Load the saved configuration from NVS
    struct eth_cfg cfg_state;
    err = get_effective_config(&cfg_state);
    if (err != 0)
    {
        goto error;
    }
    // Set the configuration to the Ethernet interface
    err = set_eth_cfg(&cfg_state);
    if (err != 0)
    {
        goto error;
    }


    return 0;
error:
    ESP_LOGW(TAG, "Failed to start Ethernet. code:%d", err);
    return err;
}


