/**
 * @copyright Quasal Technologies 2023
 *
 * author: Quasal engineers
 * 
 */
#include "utils.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lib_utils, LOG_LEVEL_DBG);

void hexchar2uint16(uint16_t *dst, uint8_t *src, size_t len)
{
    uint8_t tmp[5] = {0x00};
    for (int i = 0; i < len; i++)
    {
        memcpy(tmp, src, 4);
        uint16_t parsed = strtoul((uint8_t *)tmp, NULL, 16);
        src += 4;
        LOG_ERR("parsed: %d", parsed);
        dst[i] = parsed;
    }
}
void hexchar2uint8(uint8_t *dst, uint8_t *src, size_t len)
{
    uint8_t tmp[3] = {0x00};
    for (int i = 0; i < len; i++)
    {
        memcpy(tmp, src, 2);
        uint16_t parsed = strtoul((uint8_t *)tmp, NULL, 16);
        src += 2;
        LOG_ERR("parsed: %d", parsed);
        dst[i] = parsed;
    }
}

#ifdef CONFIG_NET_SOCKET
void dump_addrinfo(const struct addrinfo *ai)
{
    struct sockaddr_in *sock_addr = ((struct sockaddr_in *)ai->ai_addr);
    LOG_DBG("addrinfo @%p: ai_family=%d, ai_socktype=%d, ai_protocol=%d, "
            "sa_family=%d, sin_port=%x, addr=%d.%d.%d.%d",
            ai, ai->ai_family, ai->ai_socktype, ai->ai_protocol,
            ai->ai_addr->sa_family,
            sock_addr->sin_port, sock_addr->sin_addr.s4_addr[0], sock_addr->sin_addr.s4_addr[1], sock_addr->sin_addr.s4_addr[2], sock_addr->sin_addr.s4_addr[3]);
}
#endif

#ifdef CONFIG_QUASAL_MQTT_LIB
void dump_connection_params(struct mqtt_connection_params *params)
{
    LOG_DBG("\nhostname: %s\nport: %s\nclient_id: %s\nusername: %s",
            params->host, params->port, params->client_id, params->user_name);
    LOG_DBG("\npassword: %s\nwill_topic: %s\nwill_msg: %s",
            params->password, params->will_topic, params->will_message);
}
#endif

int resource_acquire(struct thread_safe_resource *resource, resource_acquired_cb_t cb, k_timeout_t timeout) {
    int err = 0;
    err = k_mutex_lock(resource->access_mutex, timeout);

    cb(resource->data);

    k_mutex_unlock(resource->access_mutex);

    return err;
}

