/*
 * eth_manager.h
 */

#ifndef ETH_MANAGER_H_
#define ETH_MANAGER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <string.h> // for memset
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"
/*
 * Holds complete config for the Ethernet interface.
 */
struct eth_cfg {
    bool is_default;    /*!< True if this is the factory default config. */
    bool is_valid;      /*!< True if this config has been applied successfully
                             before. */
    bool is_connected;   /*!< True if the interface is connected.
    (is_connected is only used for eth_manager_get_eth_state) */
    bool is_disabled;   /*!< True to disable interface. */
    bool is_static;    /*!< True if interface should use static IP and DNS
                             configuration. When false, DHCP will be used. */
    esp_netif_ip_info_t ip_info;
    /*!< The IP address of the interface.*/
    esp_netif_dns_info_t dns_info[ESP_NETIF_DNS_MAX];
};
static inline void eth_cfg_init(struct eth_cfg *cfg)
{
    // Init all values to 0
    memset(cfg, 0x0, sizeof(*cfg));
}

esp_err_t eth_manager_set_eth_cfg(struct eth_cfg *new_cfg);
esp_err_t eth_manager_get_eth_cfg(struct eth_cfg *get_cfg);
esp_err_t eth_manager_get_eth_state(struct eth_cfg *get_state);
esp_err_t eth_manager_set_hostname(const char *hostname);
esp_err_t eth_manager_init(esp_eth_handle_t eth_handle);

#ifdef __cplusplus
}
#endif

#endif /* ETH_MANAGER_H_ */
