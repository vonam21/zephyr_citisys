#ifndef _LIBS_MQTT_H__
#define _LIBS_MQTT_H__

#include <zephyr/net/mqtt.h>

#define APP_CONNECT_TIMEOUT_MS 5000
#define APP_SLEEP_MSECS 50
#define APP_CONNECT_TRIES 10
#define APP_MQTT_BUFFER_SIZE 256
#define APP_DATA_REFRESH_TIMEOUT_MSECS 1000

#define MQTT_PING_TIMEOUT_SECS 10
#define MQTT_PROCESS_TIMEOUT_MSECS 150
#define MQTT_SEND_STATUS_TIMEOUT_SECS 2
#define MQTT_SERVER_PING_MAX_COUNTER 3

#define MDM_NETWORK_RETRY_COUNT 10
#define MDM_WAIT_FOR_MQTT_PING K_SECONDS(2)
#define MDM_WAIT_FOR_MQTT_PING_COUNT 10
#define MQTT_MAX_TOPIC_LEN 128
#define MQTT_MAX_PAYLOAD_LEN 512

#define APP_CA_CERT_TAG 1

/* @brief Callback to various event 
 *
 * on_topic_cb      Refer that application do not process the packet in this
 *                  callback but instead create a separate thread and pass the 
 *                  data to that thread instead
 * */
struct mqtt_callback {
    void (*on_topic_cb)(struct mqtt_client *const client, const uint8_t *topic, size_t payload_len);
    void (*on_disconnect)(struct mqtt_client *const client, int reason);
    void (*on_connected)(struct mqtt_client *const client);
    void (*on_subscribed)(struct mqtt_client *const client);
};

/* Client connection params */
struct mqtt_connection_params {
    char host[CONFIG_LIBS_MQTT_MAX_HOST_LENGTH];
    char client_id[CONFIG_LIBS_MQTT_MAX_CLIENT_ID_LENGTH];
    char user_name[CONFIG_LIBS_MQTT_MAX_USERNAME_LENGTH];
    char password[CONFIG_LIBS_MQTT_MAX_PASSWORD_LENGTH];
    char will_topic[CONFIG_LIBS_MQTT_MAX_TOPIC_LENGTH];
    char will_message[64];
    char port[6];
};

/* Client params to add new client */
struct mqtt_client_params {
    struct mqtt_connection_params connection_params;
    struct mqtt_callback callbacks;
};

/* @brief Initialize the mqtt library
 *
 **/
int libs_mqtt_init();

/* @brief Subscribe to mqtt topic with client_fd
 *
 * @params client_fd        Client file descriptor
 * @params mqtt_topic       Topic to subscribe to 
 * @params qos              The specified quality-of-service
 *
 * @return 0                if the call was successful
 *         < 0              if the call was unsuccessful
 * */
int libs_mqtt_subscribe(int client_fd, uint8_t *mqtt_topic, int qos);

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
                        uint8_t *payload, size_t payload_len);

/* @brief Disconnect from mqtt with client_fd
 *
 * @params client_fd        Client file descriptor
 *
 * @return 0                if the call was successful
 *         < 0              if the call was unsuccessful
 * */
int libs_mqtt_disconnect(int client_fd);

/* @brief Connect to mqtt client with specified params
 *
 * This call is non-blocking and will execute the on_connect callback
 *
 * @params params           Client connection params
 *
 * @return client_fd        A file descriptor to the client
 *         < 0              If the function was unsuccessful
 **/
int libs_mqtt_connect(const struct mqtt_client_params *params);

#endif // _LIBS_MQTT_H__

