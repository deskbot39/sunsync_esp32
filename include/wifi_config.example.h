#pragma once

#define SUNSYNC_WIFI_SSID ""
#define SUNSYNC_WIFI_PASSWORD ""
#define SUNSYNC_AP_SSID "SunSync"
#define SUNSYNC_AP_PASSWORD ""
#define SUNSYNC_AP_MAX_CLIENTS 8

#define SUNSYNC_I2C_SDA 8
#define SUNSYNC_I2C_SCL 9

#define SUNSYNC_INA219_BATTERY_ADDRESS 0x40
#define SUNSYNC_INA219_PANEL_ADDRESS 0x41

#define SUNSYNC_LCD_ADDRESS 0x27
#define SUNSYNC_LCD_COLUMNS 16
#define SUNSYNC_LCD_ROWS 2

#define SUNSYNC_ROUTER_RELAY_PIN 16
#define SUNSYNC_CHARGER_RELAY_PIN 17
#define SUNSYNC_RELAY_ACTIVE_LEVEL LOW

#define SUNSYNC_COIN_SIGNAL_ENABLED 1
#define SUNSYNC_COIN_SIGNAL_PIN 5
#define SUNSYNC_COIN_PULSE_DEBOUNCE_US 30000

#define SUNSYNC_MQTT_ENABLED 0
#define SUNSYNC_MQTT_HOST ""
#define SUNSYNC_MQTT_PORT 8883
// 1 = TLS (WiFiClientSecure). Use port 8883 with TLS, 1883 without.
#define SUNSYNC_MQTT_TLS 1
// PEM root CA of the broker (e.g. ISRG Root X1 for Let's Encrypt brokers).
// When empty, the connection is encrypted but the broker is NOT verified.
#define SUNSYNC_MQTT_CA_CERT ""
#define SUNSYNC_MQTT_USERNAME ""
#define SUNSYNC_MQTT_PASSWORD ""
#define SUNSYNC_MQTT_TOPIC_PREFIX "sunsync"
#define SUNSYNC_MQTT_PUBLISH_INTERVAL_MS 2000

// Battery pack: 3 x 18650 Li-ion cells in series (3S).
#define SUNSYNC_BATTERY_CELLS 3
#define SUNSYNC_BATTERY_CELL_EMPTY_V 3.0f
#define SUNSYNC_BATTERY_CELL_FULL_V 4.2f

// Fly.io backend. The ESP32 forwards the phone's API calls to it and adds the
// device key, so the key never reaches the browser. Leave the URL empty to disable.
#define SUNSYNC_BACKEND_URL ""
#define SUNSYNC_DEVICE_API_KEY ""
// PEM root CA for the backend (e.g. ISRG Root X1). When empty, TLS is not verified.
#define SUNSYNC_BACKEND_CA_CERT ""
