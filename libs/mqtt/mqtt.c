#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/drivers/watchdog.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(libs_mqtt, CONFIG_LIBS_MQTT_LOG_LEVEL);

#include "mqtt.h"

#define APP_BMEM
#define APP_DMEM

/* Quasal MQTT client struct to hold all things related to a client connection */
struct libs_mqtt_client {
    struct mqtt_client context;
    struct {
        struct mqtt_utf8 host;
        // Considering removing this client_id
        // Because there is already a struct mqtt_utf8 client_id
        // in struct mqtt_client (weird, since other fields are 
        // ptr to struct mqtt_utf8).
        struct mqtt_utf8 client_id;
        struct mqtt_utf8 user_name;
        struct mqtt_utf8 password;
        struct mqtt_topic will_topic;
        struct mqtt_utf8 will_message;
    };
    struct mqtt_client_params params;
    
    struct sockaddr_storage broker;
    struct mqtt_puback_param puback;
    struct zsock_pollfd fds;

    uint8_t rx_buffer[APP_MQTT_BUFFER_SIZE];
    uint8_t tx_buffer[APP_MQTT_BUFFER_SIZE];
    bool assigned;
    bool connected;
};

/* Contain runtime components for MQTT */
struct mqtt_runtime {
    struct k_work_delayable mqtt_ping_work;
    struct k_work_delayable mqtt_process_work;

    /* The mqtt client struct holding client data */
    struct libs_mqtt_client client[CONFIG_LIBS_MQTT_MAX_CLIENT_SUPPORT];
    int nfds;
};

/* MQTT runtime data */
static APP_BMEM struct mqtt_runtime runtime;

static inline bool is_valid_client_fd(int client_fd)
{
    if (client_fd < 0) {
        return false;
    }

    if (client_fd >= runtime.nfds) {
        return false;
    }

    return true;
}

static int prepare_fds(int client_fd)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    struct mqtt_client *client = &libs_client->context;
    if (client->transport.type == MQTT_TRANSPORT_NON_SECURE)
    {
        libs_client->fds.fd = client->transport.tcp.sock;
    }

    libs_client->fds.events = ZSOCK_POLLIN;
    return 0;
}

static int wait(int client_fd, int timeout)
{
    int err = 0;
    struct libs_mqtt_client *client = &runtime.client[client_fd];

    err = poll(&client->fds, 1, timeout);
    if (err < 0)
    {
        LOG_ERR("poll error: %d", errno);
    }

    return err;
}

static int assign_fd()
{
    if (runtime.nfds >= CONFIG_LIBS_MQTT_MAX_CLIENT_SUPPORT) {
        return -ENOMEM;
    }

    for (int i = 0; i < ARRAY_SIZE(runtime.client); i++) {
        if (!runtime.client[i].assigned) {
            runtime.client[i].assigned = true; 
            runtime.nfds++;
            return i;
        }
    }

    return -EINVAL;
}

/* We won't allow two of the same connection to connect to the same host/port etc
 * This will starve the connection of other possible connections. */
static int is_already_connected(const struct mqtt_connection_params *params)
{
    for (int i = 0; i < CONFIG_LIBS_MQTT_MAX_CLIENT_SUPPORT; i++) {
        if (!runtime.client[i].assigned) {
            continue;
        }
        struct mqtt_connection_params *this_client_params = &runtime.client[i].params.connection_params;
        if (strncmp(params->host, this_client_params->host, strlen(this_client_params->host))) {
            continue;
        }

        if (strncmp(params->port, this_client_params->port, strlen(this_client_params->port))) {
            continue;
        }

        if (strncmp(params->client_id, this_client_params->client_id, strlen(this_client_params->client_id))) {
            continue;
        }

        if (strncmp(params->user_name, this_client_params->user_name, strlen(this_client_params->user_name))) {
            continue;
        }

        return i;
    }

    return -EINVAL;
}

static int decommision_fd(int client_fd)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    runtime.client[client_fd].assigned = false;
    runtime.nfds--;
    return 0;
}

static int get_client_fd(const struct mqtt_client *client)
{
    if (!client) {
        return -EINVAL;
    }

    for (int i = 0; i < runtime.nfds; i++) {
        if (client == &runtime.client[i].context) {
            return i;
        }
    }

    return -ENOENT;
}

void mqtt_evt_handler(struct mqtt_client *const client, const struct mqtt_evt *evt)
{
    int err = 0;
    int len;
    int client_fd = get_client_fd(client);
    
    if (client_fd < 0) {
        LOG_ERR("Invalid client_fd %d", client_fd);
        return;
    }

    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    struct mqtt_client_params *params = &libs_client->params;
    switch (evt->type)
    {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0)
        {
            LOG_ERR("client %d: MQTT connect failed %d", client_fd, evt->result);
            break;
        }

        LOG_DBG("client %d: MQTT client connected!", client_fd);
        libs_client->connected = true;
        if (params->callbacks.on_connected) {
            params->callbacks.on_connected(client);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_DBG("client %d: MQTT client disconnected %d", client_fd, evt->result);

        if (params->callbacks.on_disconnect) {
            params->callbacks.on_disconnect(client, evt->result);
        }
        libs_client->connected = false;
        (void)decommision_fd(client_fd);
        break;

    case MQTT_EVT_PUBLISH:
        len = evt->param.publish.message.payload.len;

        /* This callback needs to return quickly */
        if (params->callbacks.on_topic_cb) {
            params->callbacks.on_topic_cb(client,
                    evt->param.publish.message.topic.topic.utf8, len);
        }

        libs_client->puback.message_id = evt->param.publish.message_id;
        mqtt_publish_qos1_ack(&libs_client->context, &libs_client->puback);
        break;

    case MQTT_EVT_PUBACK:
        if (evt->result != 0)
        {
            LOG_ERR("client %d: MQTT PUBACK error %d", client_fd, evt->result);
            break;
        }
        break;

    case MQTT_EVT_PUBREC:
        if (evt->result != 0)
        {
            LOG_ERR("client %d: MQTT PUBREC error %d", client_fd, evt->result);
            break;
        }

        LOG_INF("client %d: PUBREC packet id: %u", client_fd, evt->param.pubrec.message_id);

        const struct mqtt_pubrel_param rel_param = {
            .message_id = evt->param.pubrec.message_id};

        err = mqtt_publish_qos2_release(client, &rel_param);
        if (err != 0)
        {
            LOG_ERR("client %d: Failed to send MQTT PUBREL: %d", client_fd, err);
        }
        break;

    case MQTT_EVT_PUBCOMP:
        if (evt->result != 0)
        {
            LOG_ERR("client %d: MQTT PUBCOMP error %d", client_fd,  evt->result);
            break;
        }

        LOG_DBG("client %d: PUBCOMP packet id: %u", client_fd,
                evt->param.pubcomp.message_id);
        break;

    case MQTT_EVT_PINGRESP:
        LOG_DBG("client %d: PINGRESP packet", client_fd);
        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT subscribe: %d", evt->result);
            break;
        }
        
        if (params->callbacks.on_subscribed) {
            params->callbacks.on_subscribed(client);
        }
        break;

    default:
        break;
    }
}

static int broker_init(struct libs_mqtt_client *libs_client)
{
    int err;
    struct sockaddr_in *broker_ipv4 = (struct sockaddr_in *)&libs_client->broker;
    struct addrinfo hints = {0x00};
    struct addrinfo *res;
    char *host = libs_client->params.connection_params.host;
    char *port = libs_client->params.connection_params.port;

    /* hint that we only want the IPv4 version of the host IP address */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;

    err = getaddrinfo(host, port, &hints, &res);
    if (err) {
        LOG_DBG("modem_getaddrinfo %s status: %d", libs_client->params.connection_params.host, err);
        return err;
    }

#ifdef CONFIG_LIBS_MQTT_LOG_LEVEL_DBG
    dump_addrinfo(res);
#endif
    (void)memcpy(broker_ipv4, ((struct sockaddr_in *)res->ai_addr), sizeof(struct sockaddr_in));
    return err;
}

static void client_init(struct libs_mqtt_client *libs_client)
{
    struct mqtt_client *client = &libs_client->context;
    struct mqtt_connection_params *connection_params = &libs_client->params.connection_params;
    mqtt_client_init(client);

    /* MQTT client config */
    client->broker = &libs_client->broker;
    client->evt_cb = mqtt_evt_handler;

    libs_client->client_id.utf8 = connection_params->client_id;
    client->client_id.utf8 = connection_params->client_id;
    
    libs_client->password.utf8 = connection_params->password;
    client->password = &libs_client->password;

    libs_client->user_name.utf8 = connection_params->user_name;
    client->user_name = &libs_client->user_name;
    
    libs_client->will_topic.topic.utf8 = connection_params->will_topic;
    libs_client->will_topic.qos = 0;
    client->will_topic = &libs_client->will_topic;

    libs_client->will_message.utf8 = connection_params->will_message;
    client->will_message = &libs_client->will_message;

    client->keepalive = 30;

    /* MQTT buffers configuration */
    client->rx_buf = libs_client->rx_buffer;
    client->rx_buf_size = sizeof(libs_client->rx_buffer);
    client->tx_buf = libs_client->tx_buffer;
    client->tx_buf_size = sizeof(libs_client->tx_buffer);

    // TODO: Support secured TLS connection
    client->transport.type = MQTT_TRANSPORT_NON_SECURE;
}

static int try_to_connect(int client_fd)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    struct mqtt_client *client = &libs_client->context;
    int err;

    err = mqtt_connect(client);
    if (err != 0)
    {
        LOG_WRN("Failed mqtt connect result %d", err);
        err = -ENOTCONN;
        goto error;
    }

    prepare_fds(client_fd);
    if (wait(client_fd, APP_CONNECT_TIMEOUT_MS))
    {
        mqtt_input(client);
    }

    if (!libs_client->connected)
    {
        mqtt_abort(client);
        err = -ENOTCONN;
        goto error;
    }

    return err;

error:
    decommision_fd(client_fd);
    return err;
}

static int process_mqtt_and_sleep(int client_fd, int timeout)
{
    int err;
    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    struct mqtt_client *client = &libs_client->context;

    if (libs_client->connected)
    {
        if (wait(client_fd, timeout))
        {
            err = mqtt_input(client);
            if (err)
            {
                /* This is just to stop the library from complaining about not having any data */
                if (err != -EAGAIN) {
                    LOG_ERR("client %d: mqtt_input err %d", client_fd, err);
                }
                return err;
            }
        }

        err = mqtt_live(client);
        if (err && err != -EAGAIN)
        {
            LOG_ERR("client %d: mqtt_input err %d", client_fd, err);
            return err;
        }

        err = mqtt_input(client);
        if (err != 0)
        {
            LOG_ERR("client %d: mqtt_input err %d", client_fd, err);
            return err;
        }
    }

    return 0;
}

/* @brief mqtt_process_work
 *
 * Asynchronously process mqtt message from network stack. This
 * is functionally similar to mqtt_loop from libmosquitto
 *
 */
static void mqtt_process_work(struct k_work *work)
{
    for (int i = 0; i < ARRAY_SIZE(runtime.client); i++) {
        if (runtime.client[i].assigned && runtime.client[i].connected)
        {
            process_mqtt_and_sleep(i, APP_SLEEP_MSECS);
        }
    }

    if (work)
    {
        k_work_reschedule(&runtime.mqtt_process_work,
                                    K_MSEC(MQTT_PROCESS_TIMEOUT_MSECS));
    }
}

/* @brief Disconnect from mqtt with client_fd
 *
 * @params client_fd        Client file descriptor
 *
 * @return 0                if the call was successful
 *         < 0              if the call was unsuccessful
 * */
int libs_mqtt_disconnect(int client_fd)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    if (libs_client->assigned && libs_client->connected) {
        mqtt_abort(&libs_client->context);
    }
    return 0;
}

/* @brief Connect to mqtt client with specified params
 *
 * This call is non-blocking and will execute the on_connect callback
 *
 * @params params           Client connection params
 *
 * @return client_fd        A file descriptor to the client
 *         < 0              If the function was unsuccessful
 **/
int libs_mqtt_connect(const struct mqtt_client_params *params)
{
    int err;
    int client_fd;
    
    client_fd = is_already_connected(&params->connection_params);
    if (client_fd >= 0) {
        return client_fd;
    }

    client_fd = assign_fd();
    if (client_fd < 0) {
        LOG_ERR("No more connection can be made %d", client_fd);
        return -ENOMEM;
    }

    struct libs_mqtt_client *libs_client = &runtime.client[client_fd];
    struct mqtt_connection_params *connection_params = &libs_client->params.connection_params;
    (void)memcpy(&libs_client->params, params, sizeof(struct mqtt_client_params));
    libs_client->host.size = strlen(connection_params->host);
    libs_client->client_id.size = strlen(connection_params->client_id);
    libs_client->context.client_id.size = libs_client->client_id.size;
    libs_client->password.size = strlen(connection_params->password);
    libs_client->user_name.size = strlen(connection_params->user_name);
    libs_client->will_topic.topic.size = strlen(connection_params->will_topic);
    libs_client->will_message.size = strlen(connection_params->will_message);

    err = broker_init(libs_client);
    if (err) {
        decommision_fd(client_fd);
        return err;
    }

    err = try_to_connect(client_fd);
    if (err) {
        return err;
    }
    return client_fd;
}

/* @brief Publish to mqtt topic with client_fd
 *
 * @params client_fd        Client file descriptor
 * @params mqtt_topic       Topic to subscribe to 
 * @params qos              The specified quality-of-service
 * @params payload          Pointer to the payload
 * @params payload_len      The payload length
 *
 * @return 0                if the call was successful
 *         < 0              if the call was unsuccessful
 * */
int libs_mqtt_publish(int client_fd, uint8_t *mqtt_topic, enum mqtt_qos qos,
        uint8_t *payload, size_t payload_len)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    if (!mqtt_topic) {
        return -EINVAL;
    }

    if (!payload) {
        return -EINVAL;
    }

    struct mqtt_publish_param param;
    param.message.topic.qos = qos;
    param.message.topic.topic.utf8 = mqtt_topic;
    param.message.topic.topic.size =
        strlen(param.message.topic.topic.utf8);
    param.message.payload.data = payload;
    param.message.payload.len = payload_len;
    param.message_id = sys_rand32_get();
    param.dup_flag = 0U;
    param.retain_flag = 0U;

    return mqtt_publish(&runtime.client[client_fd].context, &param);
}

/* @brief Subscribe to mqtt topic with client_fd
 *
 * @params client_fd        Client file descriptor
 * @params mqtt_topic       Topic to subscribe to 
 * @params qos              The specified quality-of-service
 *
 * @return 0                if the call was successful
 *         < 0              if the call was unsuccessful
 * */
int libs_mqtt_subscribe(int client_fd, uint8_t *mqtt_topic, int qos)
{
    if (!is_valid_client_fd(client_fd)) {
        return -EINVAL;
    }

    if (!mqtt_topic) {
        return -EINVAL;
    }

    int err;
    struct mqtt_topic topic;
    struct mqtt_subscription_list sub;

    topic.topic.utf8 = mqtt_topic;
    topic.topic.size = strlen(topic.topic.utf8);
    topic.qos = qos;

    sub.list = &topic;
    sub.list_count = 1U;
    sub.message_id = sys_rand32_get();

    err = mqtt_subscribe(&runtime.client[client_fd].context, &sub);
    if (err != 0)
    {
        return -1;
    }

    wait(client_fd, APP_SLEEP_MSECS);
    err = mqtt_input(&runtime.client[client_fd].context);
    if (err != 0)
    {
        return -1;
    }

    return 0;
}

/* @brief Initialize the mqtt library
 *
 **/
int libs_mqtt_init()
{
    int err = 0;
    struct libs_mqtt_client *libs_client;
    for (int i = 0; i < ARRAY_SIZE(runtime.client); i++) {
        libs_client = &runtime.client[i];
        client_init(libs_client);
    }

    k_work_init_delayable(&runtime.mqtt_process_work, mqtt_process_work);
    k_work_reschedule(&runtime.mqtt_process_work,
            K_MSEC(MQTT_PROCESS_TIMEOUT_MSECS));

    LOG_DBG("MQTT is ready to connect");
    return err;
}

