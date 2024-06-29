#define DT_DRV_COMPAT lynq_l5xx

#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/types.h>
LOG_MODULE_REGISTER(modem_lynq_l5xx, CONFIG_MODEM_LOG_LEVEL);

#include "lynq-l5xx.h"

static int modem_setup(void);

static struct k_thread modem_rx_thread;
static struct modem_data mdata;
static struct modem_context mctx;
static struct k_work_q modem_workq;
static K_KERNEL_STACK_DEFINE(
    modem_workq_stack, CONFIG_MODEM_LYNQ_L5XX_RX_WORKQ_STACK_SIZE);
int count_reset_mdm = 0;
static K_KERNEL_STACK_DEFINE(
    modem_rx_stack, CONFIG_MODEM_LYNQ_L5XX_RX_STACK_SIZE);
NET_BUF_POOL_DEFINE(
    mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE, 0, NULL);

static const struct gpio_dt_spec power_gpio =
    GPIO_DT_SPEC_INST_GET(0, mdm_power_gpios);
#if DT_INST_NODE_HAS_PROP(0, mdm_reset_gpios)
static const struct gpio_dt_spec reset_gpio =
    GPIO_DT_SPEC_INST_GET(0, mdm_reset_gpios);
#endif
#if DT_INST_NODE_HAS_PROP(0, mdm_dtr_gpios)
static const struct gpio_dt_spec dtr_gpio =
    GPIO_DT_SPEC_INST_GET(0, mdm_dtr_gpios);
#endif

/* Func: modem_atoi
 * Desc: Convert string to long integer, but handle errors
 */
static int modem_atoi(
    const char *s, const int err_value, const char *desc, const char *func)
{
	int ret;
	char *endptr;

	ret = (int)strtol(s, &endptr, 10);
	if (!endptr || *endptr != '\0') {
		LOG_ERR("bad %s '%s' in %s", s, desc, func);
		return err_value;
	}

	return ret;
}

static const struct sim_info sim_infos[] = {
    SIM_INFO("Viettel", "4520499", "v-internet", "", ""),
    SIM_INFO("Viettel-Truphone", "45204", "iot.truphone.comt", "", ""),
    SIM_INFO("Mobifone", "4520199", "m-wap", "mms", "mms"),
    SIM_INFO("Mobifone-Truphone", "45201", "iot.truphone.comt", "", ""),
    SIM_INFO("Vinaphone", "4520299", "m3-world", "mms", "mms"),
    SIM_INFO("Vinaphone-Truphone", "45202", "iot.truphone.comt", "", ""),
    SIM_INFO("Vietnammobile", "4520599", "internet", "", ""),
};

/* Handler: OK */
MODEM_CMD_DEFINE(on_cmd_ok)
{
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/* Handler: ERROR */
MODEM_CMD_DEFINE(on_cmd_error)
{
	modem_cmd_handler_set_error(data, -EIO);
	LOG_ERR("[Data error] data %p", data);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/* Handler: +CME Error: <err>[0] */
MODEM_CMD_DEFINE(on_cmd_exterror)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/* Handler: +CESQ: <rssi>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp> */
/* Or handler: +CSQ: <rssi>,<ber> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_rssi_cesq)
{
	int rssi = ATOI(argv[0], 0, "signal_power");
	if (rssi == 99 || rssi > 63) {
		mdata.mdm_rssi = -1000;
	} else {
		mdata.mdm_rssi = rssi * 2 - 111;
	}

	LOG_INF("RSSI: %d", mdata.mdm_rssi);
	return 0;
}

/* Handler: "RDY" unsol command */
MODEM_CMD_DEFINE(on_cmd_unsol_atready)
{
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

static const struct modem_cmd response_cmds[] = {
    MODEM_CMD("OK", on_cmd_ok, 0U, ""),
    MODEM_CMD("ERROR", on_cmd_error, 0U, ""),
};

/* Handler: +NETOPEN:SUCCESS, +NETOPEN:FAIL */
MODEM_CMD_DEFINE(on_cmd_net_open_pdp)
{
	if (strcmp(argv[0], "SUCCESS") == 0) {
		mdata.is_net_open = 1;
	} else {
		mdata.is_net_open = 0;
	}
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_net_rdy);
	return 0;
}

/* Handler: +NETOPEN:<state> */
MODEM_CMD_DEFINE(on_cmd_net_status)
{
	mdata.is_net_open = ATOI(argv[0], 0, "state");
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_net_rdy);
	return 0;
}

/* Handler: "RDY" unsol command */
MODEM_CMD_DEFINE(on_cmd_unsol_cereg)
{
	mdata.is_net_open = ATOI(argv[0], 0, "status network");
	if (mdata.is_net_open == 0 && mdata.net_open_ignore_cereg == 0) {
		modem_restart();
		sys_reboot(SYS_REBOOT_COLD);
		return 0;
	}
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

const struct sim_info *find_sim_info(
    const struct sim_info *si, char *imsi, size_t si_len)
{
	for (size_t i = 0; i < si_len; i++) {
		if (strncmp(si[i].imsi, imsi, strlen(imsi)) == 0) {
			return &si[i];
		}
	}
	return NULL;
}

/* Handler: <COPS>: <mode>,<format>,<oper>,<Act> */
MODEM_CMD_DEFINE(on_cmd_atcmdinfo_cops)
{
	size_t sim_info_len = ARRAY_SIZE(sim_infos);
	char imsi[6];
	memcpy(imsi, &argv[2][1], 5);
	imsi[5] = '\0';
	const struct sim_info *match_sim_info =
	    find_sim_info(sim_infos, imsi, sim_info_len);

	if (match_sim_info) {
		mdata.mdm_providername = (char *)match_sim_info->pname;
		mdata.mdm_apn = (char *)match_sim_info->apn;
		mdata.mdm_username = (char *)match_sim_info->username;
		mdata.mdm_password = (char *)match_sim_info->password;
	}

	/* Log the received information. */
	LOG_INF("Provider name: %s", mdata.mdm_providername);
	return 0;
}

/* Commands sent to the modem to set it up at boot time. */
static const struct setup_cmd setup_cmds[] = {
    SETUP_CMD_NOHANDLE("AT"),
    SETUP_CMD_NOHANDLE("ATE0"),
    SETUP_CMD_NOHANDLE("AT+IPR=230400"),
    SETUP_CMD_NOHANDLE("AT+CFUN=1"),
    SETUP_CMD_NOHANDLE("AT+CGNETLED=1"),
    SETUP_CMD_NOHANDLE("AT+CTZR=1"),
    SETUP_CMD_NOHANDLE("AT+CTZU=1"),
};

static const struct modem_cmd unsol_cmds[] = {
    MODEM_CMD("ATREADY", on_cmd_unsol_atready, 0U, ""),
    MODEM_CMD("+CEREG:", on_cmd_unsol_cereg, 1, ""),
};

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
/* Handler: +MDNSGIP:<domain name>,<ip address> */
MODEM_CMD_DEFINE(on_cmd_dns)
{
	/* FIXME: Hard coded to return IPv4 addresses */
	result.ai_family = AF_INET;

	/* skip beginning quote when parsing */
	(void)net_addr_pton(result.ai_family, &argv[1][0],
	    &((struct sockaddr_in *)&result_addr)->sin_addr);

	((struct sockaddr_in *)&result_addr)->sin_family = AF_INET;
	k_sem_give(&mdata.sem_dns);
	k_sem_give(&mdata.sem_response);
	return 0;
}
#endif

static inline uint32_t hash32(char *str, int len)
{
#define HASH_MULTIPLIER 37

	uint32_t h = 0;
	int i;

	for (i = 0; i < len; ++i) {
		h = (h * HASH_MULTIPLIER) + str[i];
	}

	return h;
}

static inline uint8_t *modem_get_mac(const struct device *dev)
{
	struct modem_data *data = dev->data;
	uint32_t hash_value;

	data->mac_addr[0] = 0x00;
	data->mac_addr[1] = 0x10;

	/* use IMEI for mac_addr */
	hash_value = hash32(mdata.mdm_imei, strlen(mdata.mdm_imei));

	UNALIGNED_PUT(hash_value, (uint32_t *)(data->mac_addr + 2));

	return data->mac_addr;
}

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
static int offload_getaddrinfo(const char *node, const char *service,
    const struct zsock_addrinfo *hints, struct zsock_addrinfo **res)
{
	static const struct modem_cmd cmd =
	    MODEM_CMD("+MDNSGIP:", on_cmd_dns, 2U, ",");
	uint32_t port = 0U;
	int ret;
	/* DNS command + 128 bytes for domain name parameter */
	char sendbuf[sizeof("AT+MDNSGIP='[]'\r") + 128];

	/* Init result */
	(void)memset(&result, 0, sizeof(result));
	(void)memset(&result_addr, 0, sizeof(result_addr));

	result.ai_family = hints->ai_family;
	result_addr.sa_family = result.ai_family;
	result.ai_addr = &result_addr;
	result.ai_addrlen = sizeof(result_addr);
	result.ai_canonname = result_canonname;
	result_canonname[0] = '\0';
	result.ai_protocol = IPPROTO_TCP;
	result.ai_socktype = hints->ai_socktype;

	if (service) {
		port = ATOI(service, 0U, "port");
		if (port < 1 || port > USHRT_MAX) {
			return DNS_EAI_SERVICE;
		}
	}

	if (port > 0U) {
		if (result.ai_family == AF_INET) {
			net_sin(&result_addr)->sin_port = htons(port);
		}
	}

	/* Check to see if node is an IP address */
	if (net_addr_pton(result.ai_family, node,
	        &((struct sockaddr_in *)&result_addr)->sin_addr) == 0) {
		*res = &result;
		return 0;
	}

	/* User flagged node as numeric host, but we failed net_addr_pton */
	if (hints && hints->ai_flags && AI_NUMERICHOST) {
		return DNS_EAI_NONAME;
	}

	int retry_count = 0;

retry_dns:
	snprintk(sendbuf, sizeof(sendbuf), "AT+MDNSGIP=\"%s\"", node);
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, &cmd, 1U, sendbuf,
	    &mdata.sem_response, MDM_DNS_TIMEOUT);
	if (ret == -ETIMEDOUT && retry_count < 5) {
		retry_count++;
		k_msleep(1000);
		goto retry_dns;
	} else if (ret < 0) {
		return ret;
	}

	ret = modem_cmd_handler_get_error(&mdata.cmd_handler_data);

	if (ret != 0) {
		return ret;
	}
	k_sem_take(&mdata.sem_dns, K_NO_WAIT);

	*res = (struct zsock_addrinfo *)&result;
	return 0;
}
#endif

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
static void offload_freeaddrinfo(struct zsock_addrinfo *res)
{
	/* using static result from offload_getaddrinfo() -- no need to free */
	res = NULL;
}
#endif

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
const struct socket_dns_offload offload_dns_ops = {
    .getaddrinfo = offload_getaddrinfo,
    .freeaddrinfo = offload_freeaddrinfo,
};
#endif

static int offload_socket(int family, int type, int proto);

/* Setup the Modem NET Interface. */
static void modem_net_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct modem_data *data = dev->data;

	/* Direct socket offload used instead of net offload: */
	net_if_set_link_addr(iface, modem_get_mac(dev), sizeof(data->mac_addr),
	    NET_LINK_ETHERNET);
	data->net_iface = iface;

#if defined(CONFIG_DNS_RESOLVER) || defined(CONFIG_MODEM_LYNQ_L5XX_DNS_RESOLVER)
	socket_offload_dns_register(&offload_dns_ops);
#endif

	net_if_socket_offload_set(iface, offload_socket);
}

/* Func: modem_rx
 * Desc: Thread to process all messages received from the Modem.
 */
static void modem_rx(void)
{
	while (true) {
		/* Wait for incoming data */
		k_sem_take(&mdata.iface_data.rx_sem, K_FOREVER);

		mctx.cmd_handler.process(&mctx.cmd_handler, &mctx.iface);
	}
}

void modem_restart(void)
{
	gpio_pin_set_dt(&reset_gpio, 0);
	k_sleep(K_MSEC(1500));

	gpio_pin_set_dt(&reset_gpio, 1);
	k_sleep(K_MSEC(500));
	LOG_INF("... Modem restarted!");
}

/* Func: pin_init
 * Desc: Boot up the Modem.
 */
static void pin_init(void)
{
	modem_restart();
	LOG_INF("Setting Modem Pins");

	/* MDM_POWER -> 1 for 500-1000 msec. */
	gpio_pin_set_dt(&power_gpio, 0);
	k_sleep(K_MSEC(750));

	/* MDM_POWER -> 0 and wait for ~500msecs as UART remains inl"inactive"
	 * state for some time after the power signal is enabled.
	 */
	gpio_pin_set_dt(&power_gpio, 1);
	k_sleep(K_MSEC(500));

	LOG_INF("... Done!");
}

int modem_pdp_active_impl(void)
{
	int ret = 0;
	int count_mdm_pdp_active = 0;
	char send_buf[sizeof("AT+QICSGP=#,#,###############,#####,#####")] = {
	    0};
	snprintk(send_buf, sizeof(send_buf),
	    "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\"", mdata.mdm_apn,
	    mdata.mdm_username, mdata.mdm_password);

	struct modem_cmd open_cmd =
	    MODEM_CMD("+NETOPEN:", on_cmd_net_open_pdp, 1U, "");
	struct modem_cmd net_status =
	    MODEM_CMD("+NETOPEN:", on_cmd_net_status, 1U, "");

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, &net_status, 1U,
	    "AT+NETOPEN?", &mdata.sem_net_rdy, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("check network open failed, err = %d", ret);
		goto error;
	}
	if (mdata.is_net_open == 1) {
		LOG_ERR("net work is opening");
		ret = 0;
		goto error;
	}

	/*set apn and active pdp*/
	do {
		/*if net_open_ignore_cereg is set,URC +CEREG ignore */
		mdata.net_open_ignore_cereg = 1;
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U,
		    send_buf, &mdata.sem_response, MDM_CMD_TIMEOUT);
		if (ret < 0) {
			LOG_ERR("modem configure apn failed err = %d", ret);
			continue;
		}
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, &open_cmd,
		    1U, "AT+NETOPEN", &mdata.sem_net_rdy, MDM_CMD_CONN_TIMEOUT);
	} while (count_mdm_pdp_active++ < MDM_PDP_ACT_RETRY_COUNT && ret < 0);
	/*allow check URC to track network registration*/
	mdata.net_open_ignore_cereg = 0;

	if (ret < 0) {
		LOG_ERR("failed active many times");
		goto error;
	}
	// It appears that after a successful netopen, the first AT command will
	// return bad values This line is to clear the chip off that state by
	// waiting on a cpin command swiftly
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U,
	    "AT+CPIN?", &mdata.sem_response, K_MSEC(200));
	if (ret < 0) {
		LOG_ERR("first cmd after NETOPEN failed");
		goto error;
	}
error:
	return ret;
}

static int modem_find_sim_card_info(void)
{
	int ret = 0;

	struct modem_cmd success_cmd =
	    MODEM_CMD("+COPS: ", on_cmd_atcmdinfo_cops, 4U, ",");

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, &success_cmd, 1U,
	    "AT+COPS?", &mdata.sem_response, MDM_REGISTRATION_TIMEOUT);
	if (ret < 0) {
		LOG_WRN("SIM card not found info");
	}

	return ret;
}

static void modem_query_sim_persecond(struct k_work *work)
{
	int ret = 0;
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U,
	    "AT+CCID", &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		count_reset_mdm++;
		mdata.is_sim_inserted = 0;
		LOG_WRN("SIM card not found query per sec");
	}

	if (ret == 0) {
		count_reset_mdm = 0;
		mdata.is_sim_inserted = 1;
	}

	if (count_reset_mdm >= 10) {
		count_reset_mdm = 0;
		modem_restart();
		sys_reboot(SYS_REBOOT_COLD);
	}
	k_work_reschedule_for_queue(
	    &modem_workq, &mdata.sim_info_query_work, K_SECONDS(2));
}

static void modem_rssi_query_work(struct k_work *work)
{
	struct modem_cmd cmd[] = {
	    MODEM_CMD("+CSQ: ", on_cmd_atcmdinfo_rssi_cesq, 2U, ",")};
	static char *send_cmd = "AT+CSQ";
	int ret;

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, cmd,
	    ARRAY_SIZE(cmd), send_cmd, &mdata.sem_response, MDM_CMD_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("AT+CSQ ret:%d", ret);
	}

	if (work) {
		k_work_reschedule_for_queue(&modem_workq,
		    &mdata.rssi_query_work, K_SECONDS(RSSI_TIMEOUT_SECS));
	}
}

/* Func: modem_setup
 * Desc: This function is used to setup the modem from zero. The idea
 * is that this function will be called right after the modem is
 * powered on to do the stuff necessary to talk to the modem.
 */
static int modem_setup(void)
{
	int err = 0;
	int at_retry = 0;
	int count_mdm_query_sim_card_info = 0;
	int count_mdm_query_rssi = 0;
	/* stop sim info query work */
	k_work_cancel_delayable(&mdata.sim_info_query_work);
	/* stop RSSI delay work */
	k_work_cancel_delayable(&mdata.rssi_query_work);
	/* Setup the pins to ensure that Modem is enabled. */
	/*allow check URC to track network registration*/
	mdata.net_open_ignore_cereg = 1;
	pin_init();

	/* Let the modem respond. */
	LOG_INF("Waiting for modem to respond");

	do {
		err = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U,
		    "AT", &mdata.sem_response, MDM_MAX_AT_BOOT_TIME);
		if (err) {
			LOG_WRN("Timeout waiting for AT OK");
		}
	} while (at_retry < MDM_MAX_AT_RETRY && err);

	/* Run setup commands on the modem. */
	err = modem_cmd_handler_setup_cmds(&mctx.iface, &mctx.cmd_handler,
	    setup_cmds, ARRAY_SIZE(setup_cmds), &mdata.sem_response,
	    MDM_REGISTRATION_TIMEOUT);
	if (err) {
		LOG_DBG("error setup cmds");
		goto restart_system;
	}

	k_sleep(K_SECONDS(3));
	mdata.net_open_ignore_cereg = 0;
	while (count_mdm_query_sim_card_info++ <
	       MDM_WAIT_FOR_SIM_CARD_INFO_COUNT) {
		err = modem_find_sim_card_info();
		if (err == 0) {
			break;
		}
		k_msleep(MDM_WAIT_FOR_SIM_CARD_INFO_DELAY);
	}
	if (err < 0) {
		LOG_ERR("Failed simcardinfo init");
		goto restart_system;
	}

	do {
		modem_rssi_query_work(NULL);
		if (mdata.mdm_rssi < RSSI_MAX_VAL ||
		    mdata.mdm_rssi > RSSI_MIN_VAL) {
			break;
		}
		k_sleep(MDM_WAIT_FOR_RSSI_DELAY);
	} while (
	    count_mdm_query_rssi++ < MDM_WAIT_FOR_RSSI_COUNT &&
	    (mdata.mdm_rssi >= RSSI_MAX_VAL || mdata.mdm_rssi <= RSSI_MIN_VAL));

	if (mdata.mdm_rssi >= RSSI_MAX_VAL || mdata.mdm_rssi <= RSSI_MIN_VAL) {
		LOG_ERR(" Signal CSQ bad and restart system");
		goto restart_system;
	}

	err = modem_pdp_active_impl();
	if (err < 0) {
		goto restart_system;
	}

	k_work_reschedule_for_queue(
	    &modem_workq, &mdata.rssi_query_work, K_SECONDS(15));
	k_work_reschedule_for_queue(
	    &modem_workq, &mdata.sim_info_query_work, K_SECONDS(2));
	return err;
restart_system:
	modem_restart();
	sys_reboot(SYS_REBOOT_COLD);
	return err;
}

static int modem_init(const struct device *dev)
{
	LOG_INF("modem init");

	int err;
	ARG_UNUSED(dev);
	k_sem_init(&mdata.sem_net_rdy, 0, 1);
	k_sem_init(&mdata.sem_response, 0, 1);
	k_sem_init(&mdata.sem_dns, 0, 1);
	mdata.is_sim_inserted = 0;
	k_work_queue_start(&modem_workq, modem_workq_stack,
	    K_KERNEL_STACK_SIZEOF(modem_workq_stack), K_PRIO_COOP(7), NULL);
	/* cmd handler setup */
	const struct modem_cmd_handler_config cmd_handler_config = {
	    .match_buf = &mdata.cmd_match_buf[0],
	    .match_buf_len = sizeof(mdata.cmd_match_buf),
	    .buf_pool = &mdm_recv_pool,
	    .alloc_timeout = BUF_ALLOC_TIMEOUT,
	    .eol = "\r\n",
	    .user_data = NULL,
	    .response_cmds = response_cmds,
	    .response_cmds_len = ARRAY_SIZE(response_cmds),
	    .unsol_cmds = unsol_cmds,
	    .unsol_cmds_len = ARRAY_SIZE(unsol_cmds),
	};

	err = modem_cmd_handler_init(
	    &mctx.cmd_handler, &mdata.cmd_handler_data, &cmd_handler_config);
	if (err < 0) {
		goto error;
	}

	/* modem interface */
	const struct modem_iface_uart_config uart_config = {
	    .rx_rb_buf = &mdata.iface_rb_buf[0],
	    .rx_rb_buf_len = sizeof(mdata.iface_rb_buf),
	    .dev = MDM_UART_DEV,
	    .hw_flow_control = DT_PROP(MDM_UART_NODE, hw_flow_control),
	};

	err =
	    modem_iface_uart_init(&mctx.iface, &mdata.iface_data, &uart_config);
	if (err < 0) {
		goto error;
	}

	/* pin setup */
	err = gpio_pin_configure_dt(&power_gpio, GPIO_OUTPUT_LOW);
	if (err < 0) {
		LOG_ERR("Failed to configure %s pin", "power");
		goto error;
	}

#if DT_INST_NODE_HAS_PROP(0, mdm_reset_gpios)
	err = gpio_pin_configure_dt(&reset_gpio, GPIO_OUTPUT_LOW);
	if (err < 0) {
		LOG_ERR("Failed to configure %s pin", "reset");
		goto error;
	}
#endif

#if DT_INST_NODE_HAS_PROP(0, mdm_dtr_gpios)
	err = gpio_pin_configure_dt(&dtr_gpio, GPIO_OUTPUT_LOW);
	if (err < 0) {
		LOG_ERR("Failed to configure %s pin", "dtr");
		goto error;
	}
#endif

	mctx.driver_data = &mdata;
	err = modem_context_register(&mctx);
	if (err < 0) {
		LOG_ERR("Error registering modem context: %d", err);
		goto error;
	}

	/* start RX thread */
	k_tid_t thread_id = k_thread_create(&modem_rx_thread, modem_rx_stack,
	    K_KERNEL_STACK_SIZEOF(modem_rx_stack), (k_thread_entry_t)modem_rx,
	    NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(thread_id, "modem_rx");
	k_work_init_delayable(&mdata.rssi_query_work, modem_rssi_query_work);
	k_work_init_delayable(
	    &mdata.sim_info_query_work, modem_query_sim_persecond);
	return modem_setup();

error:
	return err;
}

static int offload_socket(int family, int type, int proto)
{
	int ret;

	/* defer modem's socket create call to bind() */
	ret = modem_socket_get(&mdata.socket_config, family, type, proto);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	errno = 0;
	return ret;
}

static struct offloaded_if_api api_funcs = {
    .iface_api.init = modem_net_iface_init,
};
/* Register the device with the Networking stack. */
NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, modem_init, NULL, &mdata, NULL,
    CONFIG_MODEM_LYNQ_L5XX_INIT_PRIORITY, &api_funcs, MDM_MAX_DATA_LENGTH);
