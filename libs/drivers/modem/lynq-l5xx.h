#ifndef _PROJECT_LIBS_LYNQ_L5XX_H_
#define _PROJECT_LIBS_LYNQ_L5XX_H_
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/offloaded_netdev.h>
#include <zephyr/net/socket_offload.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/types.h>
/* Direct "include"s from drivers/modem */
#include <modem_cmd_handler.h>
#include <modem_context.h>
#include <modem_iface_uart.h>
#include <modem_socket.h>

#define MDM_UART_NODE DT_INST_BUS(0)
#define MDM_UART_DEV DEVICE_DT_GET(MDM_UART_NODE)

#define MDM_CMD_TIMEOUT K_SECONDS(2)
#define MDM_CMD_CONN_TIMEOUT K_SECONDS(120)
#define MDM_CMD_TCP_OPEN_TIMEOUT K_SECONDS(15)
#define MDM_REGISTRATION_TIMEOUT K_SECONDS(180)
#define MDM_SENDMSG_SLEEP K_MSEC(1)
#define MDM_MAX_DATA_LENGTH 2048
#define MDM_MAX_RECEIVE_DATA_LENGTH_PER_COMMAND 1500
#define MDM_RECV_MAX_BUF 10
#define MDM_RECV_BUF_SIZE 256
#define MDM_MAX_SOCKETS 5
#define MDM_SOCKET_QUEUE_LIST_SIZE 64
#define MDM_BASE_SOCKET_NUM 0
#define MDM_NETWORK_RETRY_COUNT 10
#define MDM_INIT_RETRY_COUNT 10
#define MDM_PDP_ACT_RETRY_COUNT 3
#define MDM_WAIT_FOR_RSSI_COUNT 10
#define MDM_WAIT_FOR_RSSI_DELAY K_SECONDS(2)
#define BUF_ALLOC_TIMEOUT K_SECONDS(1)
#define MDM_MAX_BOOT_TIME K_SECONDS(30)
#define MDM_MAX_AT_BOOT_TIME K_SECONDS(2)
#define MDM_MAX_BOOT_RETRY 5
#define MDM_MAX_AT_RETRY 20
#define MDM_DNS_TIMEOUT K_SECONDS(10)
#define MDM_PROMPT_CMD_DELAY K_MSEC(50)

/* Default lengths of certain things. */
#define MDM_MANUFACTURER_LENGTH 10
#define MDM_MODEL_LENGTH 16
#define MDM_REVISION_LENGTH 96
#define MDM_IMEI_LENGTH 16
#define MDM_IMSI_LENGTH 16
#define MDM_COPS_LENGTH 16
#define MDM_ICCID_LENGTH 32
#define MDM_APN_LENGTH 32
#define MDM_SIM_FAIL_RETRY 5
#define MDM_DATA_READY_MAX_TIMEOUT_SECS 15
#define RSSI_TIMEOUT_SECS 15
#define RSSI_MAX_VAL -30
#define RSSI_MIN_VAL -110
#define SIM_QUERY_TIMEOUT_MSECS 100

#define MDM_WAIT_FOR_SIM_CARD_INFO_COUNT 10
#define MDM_WAIT_FOR_SIM_CARD_INFO_DELAY 500

/* Modem ATOI routine. */
#define ATOI(s_, value_, desc_) modem_atoi(s_, value_, desc_, __func__)

/* driver data */
struct modem_data {
	/* modem interface */
	struct modem_iface_uart_data iface_data;
	struct net_if *net_iface;
	uint8_t mac_addr[6];
	uint8_t iface_rb_buf[MDM_MAX_DATA_LENGTH];

	/* modem cmds */
	struct modem_cmd_handler_data cmd_handler_data;
	uint8_t cmd_match_buf[MDM_RECV_BUF_SIZE + 1];

	/* modem data */
	char mdm_manufacturer[MDM_MANUFACTURER_LENGTH];
	char mdm_model[MDM_MODEL_LENGTH];
	char mdm_revision[MDM_REVISION_LENGTH];
	char mdm_imei[MDM_IMEI_LENGTH];
	char mdm_imsi[MDM_IMSI_LENGTH];
	char mdm_cops[MDM_COPS_LENGTH];
	char *mdm_providername;
	char *mdm_apn;
	char *mdm_username;
	char *mdm_password;

	/* socket data */
	struct modem_socket_config socket_config;

#if defined(CONFIG_MODEM_SIM_NUMBERS)
	char mdm_iccid[MDM_ICCID_LENGTH];
#endif /* #if defined(CONFIG_MODEM_SIM_NUMBERS) */

	/* RSSI work */
	struct k_work_delayable rssi_query_work;
	int mdm_rssi;
	/* SIM card work */
	struct k_work_delayable sim_info_query_work;
	/* Semaphore(s) */
	struct k_sem sem_response;
	struct k_sem sem_dns;
	/* Modem sim status */
	int is_sim_inserted;
};

#define SIM_INFO(pname_, imsi_, apn_, username_, password_)           \
	{                                                             \
		.pname = pname_, .imsi = imsi_,                       \
		.imsi_len = (uint16_t)sizeof(imsi_) - 1, .apn = apn_, \
		.username = username_, .password = password_,         \
	}

struct sim_info {
	/* Phone provider's name */
	const char *pname;
	/* IMSI of phone provider */
	const char *imsi;
	/* IMSI length of phone provider */
	uint16_t imsi_len;
	/* APN of phone provider */
	const char *apn;
	/* Username of apn */
	const char *username;
	/* Password of apn */
	const char *password;
};

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
static struct zsock_addrinfo result;
static struct sockaddr result_addr;
static char result_canonname[DNS_MAX_NAME_SIZE + 1];
#endif

#endif /* _PROJECT_LIBS_LYNQ_L5XX_H_ */
