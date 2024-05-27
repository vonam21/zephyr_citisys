# Modem initialization test

## How to build

Minimal build options:

```bash
west build -b urban_power_4g@1.0.1 -Smodem -p
```

## Other options

Build with other options:

```bash
west build -b urban_power_4g@1.0.1 -Smodem -Slogging -Sdebug -p
```

- logging: Enable logging via Segger RTT (must use Segger Jlink or other
  debugger with RTT capabilities).
- debug: Enable heavy debug logging.
- modem: Use Lynq L5xx modem driver.
