# Modem shell test

Modem function test with shell commands enabled

## How to build

Build options:

```bash
west build -b urban_power_4g@1.0.1 -Smodem -Smodem_shell -p
```

NOTE: shell cannot be used with logging features such as LOG_MODE_IMMEDIATE.

