#include <Adafruit_INA219.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <LiquidCrystal_PCF8574.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <lwip/opt.h>
#if IP_NAPT && IP_FORWARD && ESP_L2_TO_L3_COPY
#include <lwip/lwip_napt.h>
#define SUNSYNC_NAPT_AVAILABLE 1
#else
#define SUNSYNC_NAPT_AVAILABLE 0
#endif

#include "wifi_config.h"

namespace {

constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t SENSOR_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t PAY_WINDOW_MS = 60000;
AsyncWebServer server(HTTP_PORT);
DNSServer dnsServer;
Adafruit_INA219 batteryIna219(SUNSYNC_INA219_BATTERY_ADDRESS);
Adafruit_INA219 panelIna219(SUNSYNC_INA219_PANEL_ADDRESS);
LiquidCrystal_PCF8574 panelLcd(SUNSYNC_LCD_PANEL_ADDRESS);
LiquidCrystal_PCF8574 batteryLcd(SUNSYNC_LCD_BATTERY_ADDRESS);
bool captivePortalActive = false;
bool upstreamConnected = false;
bool internetSharingEnabled = false;
String accessPointSsid;
WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);
uint32_t lastMqttAttemptMs = 0;
uint32_t lastMqttPublishMs = 0;

struct Ina219Readings {
    bool available = false;
    float busVoltageV = NAN;
    float shuntVoltageMv = NAN;
    float currentMa = NAN;
    float powerW = NAN;
};

Ina219Readings batteryReadings;
Ina219Readings panelReadings;
bool panelLcdAvailable = false;
bool batteryLcdAvailable = false;
uint32_t lastSensorRefreshMs = 0;
portMUX_TYPE coinSignalMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool coinPayWindowOpen = false;
volatile uint32_t queuedCoinPulses = 0;
volatile uint32_t lastCoinPulseUs = 0;
bool coinInputEnabled = false;
uint32_t coinCredits = 0;
uint32_t payWindowEndsAt = 0;
String payWindowOwner;
bool serviceSessionActive = false;
bool serviceSessionPaused = false;
bool serviceSessionIsCharging = false;
uint32_t serviceSessionEndsAt = 0;
uint32_t serviceSessionRemainingMs = 0;
uint32_t serviceSessionCredits = 0;
String serviceSessionOwner;

void setRelay(uint8_t pin, bool active) {
    digitalWrite(pin, active ? SUNSYNC_RELAY_ACTIVE_LEVEL
                             : !SUNSYNC_RELAY_ACTIVE_LEVEL);
}

void initializeRelays() {
    const uint8_t inactiveLevel = !SUNSYNC_RELAY_ACTIVE_LEVEL;
    digitalWrite(SUNSYNC_ROUTER_RELAY_PIN, SUNSYNC_RELAY_ACTIVE_LEVEL);
    digitalWrite(SUNSYNC_CHARGER_RELAY_PIN, inactiveLevel);
    pinMode(SUNSYNC_ROUTER_RELAY_PIN, OUTPUT);
    pinMode(SUNSYNC_CHARGER_RELAY_PIN, OUTPUT);
    setRelay(SUNSYNC_ROUTER_RELAY_PIN, true);
    setRelay(SUNSYNC_CHARGER_RELAY_PIN, false);
    Serial0.println("Router relay held ON; charger relay initialized OFF.");
}

void setChargingRelay(bool active) {
    setRelay(SUNSYNC_CHARGER_RELAY_PIN, active);
}

void updateServiceSession() {
    if (serviceSessionActive && !serviceSessionPaused &&
        static_cast<int32_t>(millis() - serviceSessionEndsAt) >= 0) {
        serviceSessionActive = false;
        serviceSessionIsCharging = false;
        serviceSessionRemainingMs = 0;
        serviceSessionCredits = 0;
        serviceSessionOwner = "";
        setChargingRelay(false);
        Serial0.println("Service session expired; charger relay turned OFF.");
    }
}

String jsonNumber(float value, uint8_t decimals) {
    if (!isfinite(value)) {
        return "null";
    }
    return String(value, static_cast<unsigned int>(decimals));
}

float batteryPercentage(const Ina219Readings& readings) {
    if (!readings.available || !isfinite(readings.busVoltageV)) return NAN;
    const float emptyV = SUNSYNC_BATTERY_CELLS * SUNSYNC_BATTERY_CELL_EMPTY_V;
    const float fullV = SUNSYNC_BATTERY_CELLS * SUNSYNC_BATTERY_CELL_FULL_V;
    return constrain((readings.busVoltageV - emptyV) / (fullV - emptyV) * 100.0f,
                     0.0f, 100.0f);
}
void appendIna219Json(String& response, const Ina219Readings& readings) {
    response += "{\"available\":";
    response += readings.available ? "true" : "false";
    response += ",\"bus_voltage_v\":";
    response += jsonNumber(readings.busVoltageV, 4);
    response += ",\"shunt_voltage_mv\":";
    response += jsonNumber(readings.shuntVoltageMv, 4);
    response += ",\"current_ma\":";
    response += jsonNumber(readings.currentMa, 3);
    response += ",\"power_w\":";
    response += jsonNumber(readings.powerW, 4);
    response += "}";
}

String lcdLine(const String& value) {
    String line = value.substring(0, SUNSYNC_LCD_COLUMNS);
    while (line.length() < SUNSYNC_LCD_COLUMNS) {
        line += ' ';
    }
    return line;
}

String formatLcdReading(const char* label, const Ina219Readings& readings) {
    if (!readings.available || !isfinite(readings.busVoltageV) ||
        !isfinite(readings.powerW)) {
        return "Sensor unavailable";
    }

    char line[SUNSYNC_LCD_COLUMNS + 1];
    snprintf(line, sizeof(line), "%s:%5.2fV", label, readings.busVoltageV);
    return String(line);
}

String formatLcdCurrent(const Ina219Readings& readings) {
    if (!readings.available || !isfinite(readings.currentMa) ||
        !isfinite(readings.powerW)) {
        return "Check I2C sensor";
    }

    char line[SUNSYNC_LCD_COLUMNS + 1];
    snprintf(line, sizeof(line), "I:%4.2fA P:%4.1fW",
             readings.currentMa / 1000.0f, readings.powerW);
    return String(line);
}

void writeLcd(LiquidCrystal_PCF8574& lcd, const char* label,
              const Ina219Readings& readings) {
    lcd.setCursor(0, 0);
    lcd.print(lcdLine(formatLcdReading(label, readings)));
    lcd.setCursor(0, 1);
    lcd.print(lcdLine(formatLcdCurrent(readings)));
}

void initializeDisplays() {
    panelLcd.begin(SUNSYNC_LCD_COLUMNS, SUNSYNC_LCD_ROWS, Wire);
    panelLcd.setBacklight(255);
    panelLcdAvailable = panelLcd.isConnected();
    if (panelLcdAvailable) {
        panelLcd.clear();
        writeLcd(panelLcd, "Panel", panelReadings);
    } else {
        Serial0.printf("WARNING: Panel LCD not detected at I2C address 0x%02X.\n",
                       SUNSYNC_LCD_PANEL_ADDRESS);
    }

    batteryLcd.begin(SUNSYNC_LCD_COLUMNS, SUNSYNC_LCD_ROWS, Wire);
    batteryLcd.setBacklight(255);
    batteryLcdAvailable = batteryLcd.isConnected();
    if (batteryLcdAvailable) {
        batteryLcd.clear();
        writeLcd(batteryLcd, "Battery", batteryReadings);
    } else {
        Serial0.printf("WARNING: Battery LCD not detected at I2C address 0x%02X.\n",
                       SUNSYNC_LCD_BATTERY_ADDRESS);
    }
}

String contentTypeForPath(const String& path) {
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".json")) return "application/json; charset=utf-8";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".ico")) return "image/x-icon";
    if (path.endsWith(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

void addSecurityHeaders(AsyncWebServerResponse* response) {
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("Referrer-Policy", "same-origin");
    response->addHeader("Cache-Control", "no-store");
}

void sendResponse(AsyncWebServerRequest* request, int status,
                  const char* contentType, const String& body) {
    AsyncWebServerResponse* response =
        request->beginResponse(status, contentType, body);
    addSecurityHeaders(response);
    request->send(response);
}

void IRAM_ATTR onCoinSignalPulse() {
#if SUNSYNC_COIN_SIGNAL_ENABLED
    const uint32_t now = micros();
    portENTER_CRITICAL_ISR(&coinSignalMux);
    if (coinPayWindowOpen &&
        static_cast<uint32_t>(now - lastCoinPulseUs) >= SUNSYNC_COIN_PULSE_DEBOUNCE_US) {
        ++queuedCoinPulses;
        lastCoinPulseUs = now;
    }
    portEXIT_CRITICAL_ISR(&coinSignalMux);
#endif
}

void updatePaymentWindow() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&coinSignalMux);
    if (coinPayWindowOpen && static_cast<int32_t>(now - payWindowEndsAt) >= 0) {
        coinPayWindowOpen = false;
    }
    const uint32_t pulses = queuedCoinPulses;
    queuedCoinPulses = 0;
    portEXIT_CRITICAL(&coinSignalMux);

    coinCredits += pulses;
}

uint32_t paymentSecondsRemaining() {
    if (!coinPayWindowOpen) {
        return 0;
    }
    const int32_t remaining = static_cast<int32_t>(payWindowEndsAt - millis());
    return remaining > 0 ? (static_cast<uint32_t>(remaining) + 999) / 1000 : 0;
}

String requestedPaymentOwner(AsyncWebServerRequest* request) {
    const AsyncWebParameter* parameter = request->getParam("device_id");
    return parameter ? parameter->value() : String();
}

bool ownsPaymentCredits(const String& owner) {
    return owner.length() > 0 && owner == payWindowOwner;
}

void handlePaymentStatus(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    const String owner = requestedPaymentOwner(request);
    const bool ownsCredits = ownsPaymentCredits(owner);
    String response;
    response.reserve(160);
    response += "{\"coin_input_enabled\":";
    response += coinInputEnabled ? "true" : "false";
    response += ",\"pay_window_active\":";
    response += coinPayWindowOpen && ownsCredits ? "true" : "false";
    response += ",\"seconds_remaining\":";
    response += coinPayWindowOpen && ownsCredits ? String(paymentSecondsRemaining()) : "0";
    response += ",\"credits\":";
    response += ownsCredits ? String(coinCredits) : "0";
    response += ",\"credit_unit\":\"coin_pulse\"}";
    sendResponse(request, 200, "application/json; charset=utf-8", response);
}

void handleOpenPaymentWindow(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    if (!coinInputEnabled) {
        sendResponse(request, 503, "application/json; charset=utf-8",
                     "{\"detail\":\"Coin signal input is not configured.\"}");
        return;
    }

    const String owner = requestedPaymentOwner(request);
    if (owner.length() == 0 || owner.length() > 100) {
        sendResponse(request, 400, "application/json; charset=utf-8",
                     "{\"detail\":\"A valid device_id is required.\"}");
        return;
    }
    if (coinPayWindowOpen && owner != payWindowOwner) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"Another payment window is already active.\"}");
        return;
    }
    if (!coinPayWindowOpen && coinCredits > 0 && owner != payWindowOwner) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"Unspent credits belong to another device.\"}");
        return;
    }

    payWindowOwner = owner;
    payWindowEndsAt = millis() + PAY_WINDOW_MS;
    portENTER_CRITICAL(&coinSignalMux);
    queuedCoinPulses = 0;
    coinPayWindowOpen = true;
    portEXIT_CRITICAL(&coinSignalMux);
    handlePaymentStatus(request);
}

bool readUnsignedParameter(AsyncWebServerRequest* request, const char* name,
                           uint32_t& value) {
    const AsyncWebParameter* parameter = request->getParam(name);
    if (!parameter || parameter->value().isEmpty()) {
        return false;
    }
    const String text = parameter->value();
    char* end = nullptr;
    const unsigned long parsed = strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ownsActiveService(const String& owner) {
    return owner.length() > 0 && owner == serviceSessionOwner &&
           serviceSessionActive;
}

void handleStartService(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    updateServiceSession();

    const String owner = requestedPaymentOwner(request);
    const AsyncWebParameter* typeParameter = request->getParam("session_type");
    uint32_t creditsToSpend = 0;
    uint32_t durationSeconds = 0;
    if (owner.length() == 0 || owner.length() > 100 ||
        !typeParameter || !readUnsignedParameter(request, "credits", creditsToSpend) ||
        !readUnsignedParameter(request, "duration_seconds", durationSeconds) ||
        durationSeconds > 86400) {
        sendResponse(request, 400, "application/json; charset=utf-8",
                     "{\"detail\":\"Valid device, session type, credits, and duration are required.\"}");
        return;
    }

    const String sessionType = typeParameter->value();
    if (sessionType != "wifi" && sessionType != "charging") {
        sendResponse(request, 400, "application/json; charset=utf-8",
                     "{\"detail\":\"session_type must be wifi or charging.\"}");
        return;
    }
    if (serviceSessionActive) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"A service session is already active.\"}");
        return;
    }
    if (!ownsPaymentCredits(owner)) {
        sendResponse(request, 403, "application/json; charset=utf-8",
                     "{\"detail\":\"These credits do not belong to this device.\"}");
        return;
    }
    if (creditsToSpend > coinCredits) {
        sendResponse(request, 402, "application/json; charset=utf-8",
                     "{\"detail\":\"There are not enough credits to start this session.\"}");
        return;
    }

    coinCredits -= creditsToSpend;
    serviceSessionActive = true;
    serviceSessionPaused = false;
    serviceSessionIsCharging = sessionType == "charging";
    serviceSessionOwner = owner;
    serviceSessionCredits = creditsToSpend;
    serviceSessionRemainingMs = durationSeconds * 1000UL;
    serviceSessionEndsAt = millis() + serviceSessionRemainingMs;
    setChargingRelay(serviceSessionIsCharging);
    if (serviceSessionIsCharging) {
        Serial0.printf("Charging session started for %lu seconds; charger relay ON.\n",
                       static_cast<unsigned long>(durationSeconds));
    } else {
        Serial0.printf("Wi-Fi session started for %lu seconds; router relay remains ON.\n",
                       static_cast<unsigned long>(durationSeconds));
    }
    handlePaymentStatus(request);
}

void handlePauseService(AsyncWebServerRequest* request) {
    updateServiceSession();
    if (!ownsActiveService(requestedPaymentOwner(request))) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"No active service session belongs to this device.\"}");
        return;
    }
    if (serviceSessionPaused) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"Service session is already paused.\"}");
        return;
    }

    const int32_t remaining =
        static_cast<int32_t>(serviceSessionEndsAt - millis());
    if (remaining <= 0) {
        updateServiceSession();
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"Service session has expired.\"}");
        return;
    }
    serviceSessionRemainingMs = static_cast<uint32_t>(remaining);
    serviceSessionPaused = true;
    setChargingRelay(false);
    sendResponse(request, 200, "application/json; charset=utf-8",
                 "{\"paused\":true}");
}

void handleResumeService(AsyncWebServerRequest* request) {
    if (!ownsActiveService(requestedPaymentOwner(request)) ||
        !serviceSessionPaused) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"No paused service session belongs to this device.\"}");
        return;
    }
    serviceSessionPaused = false;
    serviceSessionEndsAt = millis() + serviceSessionRemainingMs;
    setChargingRelay(serviceSessionIsCharging);
    sendResponse(request, 200, "application/json; charset=utf-8",
                 "{\"paused\":false}");
}

void handleEndService(AsyncWebServerRequest* request) {
    updateServiceSession();
    if (!ownsActiveService(requestedPaymentOwner(request))) {
        sendResponse(request, 409, "application/json; charset=utf-8",
                     "{\"detail\":\"No active service session belongs to this device.\"}");
        return;
    }
    serviceSessionActive = false;
    serviceSessionPaused = false;
    serviceSessionIsCharging = false;
    serviceSessionRemainingMs = 0;
    serviceSessionCredits = 0;
    serviceSessionOwner = "";
    setChargingRelay(false);
    sendResponse(request, 200, "application/json; charset=utf-8",
                 "{\"ended\":true}");
}

void serveLittleFsFile(AsyncWebServerRequest* request, String path) {
    if (path == "/") path = "/index.html";
    if (path == "/login") path = "/login.html";
    if (path == "/dashboard") path = "/dashboard.html";

    if (path.indexOf("..") >= 0 || path.indexOf('\\') >= 0) {
        sendResponse(request, 400, "text/plain; charset=utf-8", "Invalid path");
        return;
    }

    String servedPath = path;
    bool compressed = false;
    const AsyncWebHeader* encoding = request->getHeader("Accept-Encoding");
    const bool acceptsGzip = encoding && encoding->value().indexOf("gzip") >= 0;
    if (acceptsGzip && (path.endsWith(".html") || path.endsWith(".css") ||
                       path.endsWith(".js"))) {
        const String gzipPath = path + ".gz";
        if (LittleFS.exists(gzipPath)) {
            servedPath = gzipPath;
            compressed = true;
        }
    }

    File file = LittleFS.open(servedPath, "r");
    if (!file || file.isDirectory()) {
        sendResponse(request, 404, "text/plain; charset=utf-8", "File not found");
        return;
    }

    AsyncWebServerResponse* response =
        request->beginResponse(file, servedPath, contentTypeForPath(path));
    addSecurityHeaders(response);
    response->addHeader("Vary", "Accept-Encoding");
    response->addHeader(
        "Cache-Control",
        path.endsWith(".html") ? "no-cache" : "public, max-age=3600"
    );
    if (compressed) {
        response->addHeader("Content-Encoding", "gzip");
    }
    request->send(response);
}

void handleNotFound(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_GET && request->method() != HTTP_HEAD) {
        sendResponse(request, 405, "text/plain; charset=utf-8", "Method not allowed");
        return;
    }
    const String uri = request->url();
    if (LittleFS.exists(uri)) {
        serveLittleFsFile(request, uri);
        return;
    }
    if (captivePortalActive) {
        AsyncWebServerResponse* response =
            request->beginResponse(302, "text/plain; charset=utf-8", "Open the Sun Sync portal");
        response->addHeader("Location", "http://192.168.4.1/");
        addSecurityHeaders(response);
        request->send(response);
        return;
    }
    serveLittleFsFile(request, uri);
}

void redirectToPortal(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
        request->beginResponse(302, "text/plain; charset=utf-8", "Open the Sun Sync portal");
    response->addHeader("Location", "http://192.168.4.1/");
    addSecurityHeaders(response);
    request->send(response);
}

void handleSensorData(AsyncWebServerRequest* request) {
    String response;
    response.reserve(384);
    response += "{\"timestamp_ms\":";
    response += String(millis());
    response += ",\"ina219\":";
    appendIna219Json(response, batteryReadings);
    response += ",\"battery_pct\":";
    response += jsonNumber(batteryPercentage(batteryReadings), 1);
    response += ",\"ina219_panel\":";
    appendIna219Json(response, panelReadings);
    response += "}";

    sendResponse(request, 200, "application/json; charset=utf-8", response);
}

String formatMacAddress(const uint8_t* mac) {
    char formatted[18];
    snprintf(formatted, sizeof(formatted), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(formatted);
}

void handleWifiClients(AsyncWebServerRequest* request) {
    wifi_sta_list_t associatedStations = {};
    esp_netif_sta_list_t stationsWithIp = {};
    if (esp_wifi_ap_get_sta_list(&associatedStations) != ESP_OK) {
        sendResponse(request, 503, "application/json; charset=utf-8",
                     "{\"detail\":\"Could not read AP station list.\"}");
        return;
    }
    const esp_err_t ipResult = esp_netif_get_sta_list(&associatedStations, &stationsWithIp);
    String response;
    response.reserve(128 + associatedStations.num * 128);
    response = "{\"clients\":[";
    bool first = true;
    for (int index = 0; index < associatedStations.num; ++index) {
        const wifi_sta_info_t& station = associatedStations.sta[index];
        if (!first) response += ",";
        first = false;
        response += "{\"mac\":\"";
        response += formatMacAddress(station.mac);
        response += "\",\"rssi_dbm\":";
        response += String(station.rssi);
        response += ",\"ip\":\"";
        bool hasIp = false;
        if (ipResult == ESP_OK) {
            for (int ipIndex = 0; ipIndex < stationsWithIp.num; ++ipIndex) {
                if (memcmp(station.mac, stationsWithIp.sta[ipIndex].mac, 6) == 0) {
                    response += IPAddress(stationsWithIp.sta[ipIndex].ip.addr).toString();
                    hasIp = true;
                    break;
                }
            }
        }
        if (!hasIp) response += "unknown";
        response += "\",\"hostname\":null,\"downloaded_bytes\":null}";
    }
    response += "],\"client_count\":";
    response += String(associatedStations.num);
    response += ",\"traffic_counters_available\":false}";
    sendResponse(request, 200, "application/json; charset=utf-8", response);
}

void handleHealth(AsyncWebServerRequest* request) {
    String response = "{\"status\":\"ok\",\"uptime_ms\":";
    response += String(millis());
    response += ",\"upstream_connected\":";
    response += upstreamConnected ? "true" : "false";
    response += ",\"internet_sharing_enabled\":";
    response += internetSharingEnabled ? "true" : "false";
    response += ",\"internet_sharing_supported\":";
#if SUNSYNC_NAPT_AVAILABLE
    response += "true";
#else
    response += "false";
#endif
    response += ",\"ap_clients\":";
    response += String(WiFi.softAPgetStationNum());
    response += "}";
    sendResponse(request, 200, "application/json; charset=utf-8", response);
}

void onWiFiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        upstreamConnected = true;
#if SUNSYNC_NAPT_AVAILABLE
        esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_t* staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t apInfo;
        esp_netif_dns_info_t upstreamDns;
        bool apDnsConfigured = false;
        if (apNetif && staNetif &&
            esp_netif_get_ip_info(apNetif, &apInfo) == ESP_OK &&
            esp_netif_get_dns_info(staNetif, ESP_NETIF_DNS_MAIN, &upstreamDns) == ESP_OK &&
            esp_netif_dhcps_stop(apNetif) == ESP_OK) {
            const esp_err_t dnsResult =
                esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &upstreamDns);
            const esp_err_t dhcpResult = esp_netif_dhcps_start(apNetif);
            apDnsConfigured = dnsResult == ESP_OK && dhcpResult == ESP_OK;
        }
        if (apDnsConfigured) {
            ip_napt_enable(apInfo.ip.addr, 1);
            dnsServer.stop();
            captivePortalActive = false;
            internetSharingEnabled = true;
            Serial0.println("IPv4 NAT enabled for AP clients.");
        } else {
            Serial0.println("ERROR: Could not configure upstream DNS/NAT for AP clients.");
        }
#else
        internetSharingEnabled = false;
        Serial0.println("Upstream connected, but this ESP-IDF build has IP forwarding/NAPT disabled.");
#endif
        Serial0.printf("Upstream Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
#if SUNSYNC_NAPT_AVAILABLE
        esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t apInfo;
        if (apNetif && esp_netif_get_ip_info(apNetif, &apInfo) == ESP_OK) {
            ip_napt_enable(apInfo.ip.addr, 0);
            esp_netif_dns_info_t apDns = {};
            apDns.ip.type = ESP_IPADDR_TYPE_V4;
            apDns.ip.u_addr.ip4.addr = apInfo.ip.addr;
            if (esp_netif_dhcps_stop(apNetif) == ESP_OK) {
                esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &apDns);
                esp_netif_dhcps_start(apNetif);
            }
        }
#endif
        upstreamConnected = false;
        internetSharingEnabled = false;
        const IPAddress accessPointIp = WiFi.softAPIP();
        if (accessPointIp) {
            dnsServer.stop();
            dnsServer.start(DNS_PORT, "*", accessPointIp);
            captivePortalActive = true;
        }
        Serial0.println("Upstream Wi-Fi disconnected.");
    }
}

void refreshMeasurements() {
    if (batteryReadings.available) {
        batteryReadings.busVoltageV = batteryIna219.getBusVoltage_V();
        batteryReadings.shuntVoltageMv = batteryIna219.getShuntVoltage_mV();
        batteryReadings.currentMa = batteryIna219.getCurrent_mA();
        batteryReadings.powerW = batteryIna219.getPower_mW() / 1000.0f;
    } else {
        batteryReadings.busVoltageV = NAN;
        batteryReadings.shuntVoltageMv = NAN;
        batteryReadings.currentMa = NAN;
        batteryReadings.powerW = NAN;
    }

    if (panelReadings.available) {
        panelReadings.busVoltageV = panelIna219.getBusVoltage_V();
        panelReadings.shuntVoltageMv = panelIna219.getShuntVoltage_mV();
        panelReadings.currentMa = panelIna219.getCurrent_mA();
        panelReadings.powerW = panelIna219.getPower_mW() / 1000.0f;
    } else {
        panelReadings.busVoltageV = NAN;
        panelReadings.shuntVoltageMv = NAN;
        panelReadings.currentMa = NAN;
        panelReadings.powerW = NAN;
    }

    if (panelLcdAvailable) writeLcd(panelLcd, "Panel", panelReadings);
    if (batteryLcdAvailable) writeLcd(batteryLcd, "Battery", batteryReadings);
}

// Sensor 1 is the solar panel input and sensor 2 is the battery, matching the backend.
void publishIna219(uint8_t sensorId, const Ina219Readings& readings) {
    if (!readings.available || isnan(readings.busVoltageV) ||
        isnan(readings.currentMa)) {
        return;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/ina219/%u", SUNSYNC_MQTT_TOPIC_PREFIX, sensorId);
    char payload[80];
    snprintf(payload, sizeof(payload), "{\"voltage_v\":%.3f,\"current_a\":%.4f}",
             readings.busVoltageV, readings.currentMa / 1000.0f);
    mqttClient.publish(topic, payload);
}

void serviceMqtt() {
#if SUNSYNC_MQTT_ENABLED
    if (!upstreamConnected || strlen(SUNSYNC_MQTT_HOST) == 0) return;
    const uint32_t now = millis();
    if (!mqttClient.connected()) {
        if (now - lastMqttAttemptMs < 5000) return;
        lastMqttAttemptMs = now;
        const String clientId = "sunsync-" + WiFi.macAddress();
        const bool hasCredentials = strlen(SUNSYNC_MQTT_USERNAME) > 0;
        const bool connected = hasCredentials
            ? mqttClient.connect(clientId.c_str(), SUNSYNC_MQTT_USERNAME,
                                 SUNSYNC_MQTT_PASSWORD)
            : mqttClient.connect(clientId.c_str());
        Serial0.println(connected ? "MQTT connected." : "MQTT connection failed.");
        return;
    }
    mqttClient.loop();
    if (now - lastMqttPublishMs >= SUNSYNC_MQTT_PUBLISH_INTERVAL_MS) {
        lastMqttPublishMs = now;
        publishIna219(1, panelReadings);
        publishIna219(2, batteryReadings);
    }
#endif
}
void startAccessPoint() {
    const String macAddress = WiFi.macAddress();
    String suffix = macAddress.substring(macAddress.length() - 8);
    suffix.replace(":", "");
    accessPointSsid = String(SUNSYNC_AP_SSID) + "-" + suffix;
    const char* password = strlen(SUNSYNC_AP_PASSWORD) >= 8
        ? SUNSYNC_AP_PASSWORD
        : nullptr;
    if (!WiFi.softAP(accessPointSsid.c_str(), password, 1, false,
                     SUNSYNC_AP_MAX_CLIENTS)) {
        Serial0.println("ERROR: Could not start the Sun Sync access point.");
        return;
    }

    const IPAddress accessPointIp = WiFi.softAPIP();
    if (!dnsServer.start(DNS_PORT, "*", accessPointIp)) {
        Serial0.println("ERROR: Could not start captive portal DNS.");
        return;
    }
    captivePortalActive = true;
    Serial0.printf("Sun Sync access point: %s (%s)\n", accessPointSsid.c_str(),
                   password ? "password protected" : "open");
    Serial0.printf("Open http://%s/ to load the Sun Sync portal.\n",
                   accessPointIp.toString().c_str());
}

void startNetwork() {
    WiFi.setSleep(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.onEvent(onWiFiEvent);
    startAccessPoint();
    if (strlen(SUNSYNC_WIFI_SSID) > 0) {
        Serial0.printf("Connecting to upstream Wi-Fi network: %s\n", SUNSYNC_WIFI_SSID);
        WiFi.begin(SUNSYNC_WIFI_SSID, SUNSYNC_WIFI_PASSWORD);
    } else {
        Serial0.println("No upstream Wi-Fi configured; AP provides local access only.");
    }
}

void startWebServer() {
    server.on("/api/sensors", HTTP_GET, handleSensorData);
    server.on("/api/wifi/clients", HTTP_GET, handleWifiClients);
    server.on("/api/payment/status", HTTP_GET, handlePaymentStatus);
    server.on("/api/payment/open", HTTP_POST, handleOpenPaymentWindow);
    server.on("/api/service/start", HTTP_POST, handleStartService);
    server.on("/api/service/pause", HTTP_POST, handlePauseService);
    server.on("/api/service/resume", HTTP_POST, handleResumeService);
    server.on("/api/service/end", HTTP_POST, handleEndService);
    server.on("/api/health", HTTP_GET, handleHealth);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/index.html");
    });
    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/index.html");
    });
    server.on("/login", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/login.html");
    });
    server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/login.html");
    });
    server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/dashboard.html");
    });
    server.on("/dashboard.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/dashboard.html");
    });
    server.on("/generate_204", HTTP_GET, redirectToPortal);
    server.on("/gen_204", HTTP_GET, redirectToPortal);
    server.on("/hotspot-detect.html", HTTP_GET, redirectToPortal);
    server.on("/library/test/success.html", HTTP_GET, redirectToPortal);
    server.on("/connecttest.txt", HTTP_GET, redirectToPortal);
    server.on("/ncsi.txt", HTTP_GET, redirectToPortal);
    server.on("/canonical.html", HTTP_GET, redirectToPortal);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial0.printf("Asynchronous HTTP server listening on port %u.\n", HTTP_PORT);
}

}  // namespace

void setup() {
    Serial0.begin(115200);
    initializeRelays();
    delay(250);
    Serial0.println();
    Serial0.println("Starting Sun Sync ESP32-S3...");

    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
        Serial0.println("ERROR: LittleFS mount failed. Upload the filesystem image first.");
        while (true) delay(1000);
    }

    Wire.begin(SUNSYNC_I2C_SDA, SUNSYNC_I2C_SCL);
    batteryReadings.available = batteryIna219.begin(&Wire);
    if (!batteryReadings.available) {
        Serial0.printf("WARNING: Battery INA219 was not detected at I2C address 0x%02X.\n",
                       SUNSYNC_INA219_BATTERY_ADDRESS);
    }
    panelReadings.available = panelIna219.begin(&Wire);
    if (!panelReadings.available) {
        Serial0.printf("WARNING: Panel INA219 was not detected at I2C address 0x%02X.\n",
                       SUNSYNC_INA219_PANEL_ADDRESS);
    }
    initializeDisplays();

#if SUNSYNC_COIN_SIGNAL_ENABLED
    pinMode(SUNSYNC_COIN_SIGNAL_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SUNSYNC_COIN_SIGNAL_PIN),
                    onCoinSignalPulse, FALLING);
    coinInputEnabled = true;
#else
    Serial0.println("Coin signal input disabled; configure a safe 3.3 V interface first.");
#endif

    refreshMeasurements();
    lastSensorRefreshMs = millis();
    startNetwork();
#if SUNSYNC_MQTT_ENABLED
    mqttClient.setServer(SUNSYNC_MQTT_HOST, SUNSYNC_MQTT_PORT);
#endif
    startWebServer();
}

void loop() {
    if (captivePortalActive) {
        dnsServer.processNextRequest();
    }
    updatePaymentWindow();
    updateServiceSession();
    serviceMqtt();

    const uint32_t now = millis();
    if (now - lastSensorRefreshMs >= SENSOR_REFRESH_INTERVAL_MS) {
        refreshMeasurements();
        lastSensorRefreshMs = millis();
    }
    delay(2);
}
