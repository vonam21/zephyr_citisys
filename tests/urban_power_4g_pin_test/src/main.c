#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(application, LOG_LEVEL_DBG);

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#define LED0_NODE DT_NODELABEL(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec uart_tx =
    GPIO_DT_SPEC_GET(DT_NODELABEL(uarttx), gpios);
static const struct gpio_dt_spec uart_rx =
    GPIO_DT_SPEC_GET(DT_NODELABEL(uartrx), gpios);
static const struct gpio_dt_spec modbus_tx =
    GPIO_DT_SPEC_GET(DT_NODELABEL(modbustx), gpios);
static const struct gpio_dt_spec modbus_rx =
    GPIO_DT_SPEC_GET(DT_NODELABEL(modbusrx), gpios);

int main(void)
{
	int err;
	const struct gpio_dt_spec *leds[4] = {
	    &led, &uart_tx, &uart_rx, &modbus_tx};
	for (int i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(leds[i])) {
			return 0;
		}

		err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_ACTIVE);
		if (err < 0) {
			LOG_ERR("Failed to initialize gpio");
			return 0;
		}
	}
	while (1) {
		for (int i = 0; i < ARRAY_SIZE(leds); i++) {
			err = gpio_pin_toggle_dt(leds[i]);
			if (err < 0) {
				return 0;
			}
		}
		k_msleep(500);
	}
	return 0;
}
