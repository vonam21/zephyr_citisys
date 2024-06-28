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

static const struct modem_cmd unsol_cmds[] = {
    MODEM_CMD("ATREADY", on_cmd_unsol_atready, 0U, ""),
};

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

/* Func: modem_setup
 * Desc: This function is used to setup the modem from zero. The idea
 * is that this function will be called right after the modem is
 * powered on to do the stuff necessary to talk to the modem.
 */
static int modem_setup(void)
{
	int err = 0;
	int at_retry = 0;

	/* Setup the pins to ensure that Modem is enabled. */
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
	    MDM_CMD_TIMEOUT);
	return err;
}

static int modem_init(const struct device *dev)
{
	LOG_INF("modem init");

	int err;
	ARG_UNUSED(dev);

	k_sem_init(&mdata.sem_response, 0, 1);

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

	return modem_setup();

error:
	return err;
}
static struct offloaded_if_api api_funcs = {
    .iface_api.init = modem_net_iface_init,
};
/* Register the device with the Networking stack. */
NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, modem_init, NULL, &mdata, NULL,
    CONFIG_MODEM_LYNQ_L5XX_INIT_PRIORITY, &api_funcs, MDM_MAX_DATA_LENGTH);
