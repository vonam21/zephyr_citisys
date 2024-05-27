if (CONFIG_MODEM_LYNQ_L5XX)
	zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/modem)
	zephyr_library_sources(lynq-l5xx.c)
endif ()
