/*
 * eth_manager.h
 */

#ifndef ETH_MANAGER_H_
#define ETH_MANAGER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
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

                             //wifi_config_t sta;  /*!< Configuration of the STA component. */
    bool eth_static;    /*!< True if ETH interface should use static IP and DNS
                             configuration. When false, DHCP will be used. */
    esp_netif_ip_info_t eth_ip_info;
    /*!< The IP address of the STA interface in static mode.*/
    esp_netif_dns_info_t eth_dns_info[ESP_NETIF_DNS_MAX];
    /*!< IP addresses of DNS servers to use in static IP mode. */
    bool eth_connect;   /*!< True if device should connect to Ethernet (false to disable Ethernet interface). */
};

esp_err_t eth_manager_set_eth_cfg(struct eth_cfg *new_cfg);
esp_err_t eth_manager_get_eth_cfg(struct eth_cfg *get_cfg);
bool eth_manager_eth_is_connected(void);
esp_err_t eth_manager_set_hostname(const char *hostname);
esp_err_t eth_manager_init(esp_eth_handle_t *eth_handle);

#ifdef __cplusplus
}
#endif 

#endif /* ETH_MANAGER_H_ */
