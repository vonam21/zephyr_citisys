/**
 * @copyright Quasal Technologies 2023
 *
 * author: Quasal engineers
 * 
 */

#ifndef __UTILS_H_
#define __UTILS_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

#ifdef CONFIG_LIBS_MQTT_LIB
#include "mqtt/mqtt.h"

/* Dump mqtt connection param for debugging purposes */
void dump_connection_params(struct mqtt_connection_params *params);
#endif

/* Get the string length without the null character */
#define STRING_NO_NULL_LENGTH(s) (sizeof(s) - 1)

void hexchar2uint16(uint16_t *dst, uint8_t *src, size_t len);
void hexchar2uint8(uint8_t *dst, uint8_t *src, size_t len);
#ifdef CONFIG_NET_SOCKET
void dump_addrinfo(const struct addrinfo *ai);
#endif

struct thread_safe_resource {
    void *data;
    struct k_mutex *access_mutex;
};
#define THREAD_SAFE_RESOURCE_STATIC(type, name)  \
    type data_##name = {0x00};           \
    K_MUTEX_DEFINE(access_##name##_mutex);  \
    struct thread_safe_resource resource_##name = {     \
        .data = &data_##name,       \
        .access_mutex = &access_##name##_mutex,         \
    };

#define THREAD_SAFE_RESOURCE_IMPORT(type, name) \
    extern struct thread_safe_resource resource_##name

#define THREAD_SAFE_RESOUCE_REF(name) &resource_##name
#define THREAD_UNSAFE_RESOUCE_REF(type, name) (type *)resource_##name.data

typedef void (*resource_acquired_cb_t)(void *data);

int resource_acquire(struct thread_safe_resource *resource, resource_acquired_cb_t cb, k_timeout_t timeout);

#endif /* __UTILS_H_ */

