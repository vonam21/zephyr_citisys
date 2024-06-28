#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(application, CONFIG_APPLICATION_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#define LED0_NODE DT_NODELABEL(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int err;
	const struct gpio_dt_spec *leds[] = {&led};
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
