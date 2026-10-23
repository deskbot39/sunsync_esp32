#include <Adafruit_INA219.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <esp_netif.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <LiquidCrystal_PCF8574.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <lwip/opt.h>
#if IP_NAPT && IP_FORWARD
#include <lwip/lwip_napt.h>
#include <lwip/tcpip.h>
#include <lwip/netif.h>
#include <driver/gpio.h>
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
Adafruit_INA219 batteryIna219(SUNSYNC_INA219_BATTERY_ADDRESS);
Adafruit_INA219 panelIna219(SUNSYNC_INA219_PANEL_ADDRESS);
LiquidCrystal_PCF8574 lcd(SUNSYNC_LCD_ADDRESS);
bool captivePortalActive = false;
bool upstreamConnected = false;
bool internetSharingEnabled = false;
String accessPointSsid;
#if SUNSYNC_MQTT_TLS
WiFiClientSecure mqttNetClient;
#else
WiFiClient mqttNetClient;
#endif
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
bool lcdAvailable = false;
String lcdLastLines[2];
uint32_t lastSensorRefreshMs = 0;
portMUX_TYPE coinSignalMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool coinPayWindowOpen = false;
volatile uint32_t queuedCoinPulses = 0;
volatile uint32_t lastCoinPulseUs = 0;
bool coinInputEnabled = false;
uint32_t coinCredits = 0;
uint32_t coinPulseRemainder = 0;
uint32_t payWindowEndsAt = 0;
String payWindowOwner;
bool serviceSessionActive = false;
bool serviceSessionPaused = false;
bool serviceSessionIsCharging = false;
uint32_t serviceSessionEndsAt = 0;
uint32_t serviceSessionRemainingMs = 0;
uint32_t serviceSessionCredits = 0;
String serviceSessionOwner;
uint32_t serviceSessionClientIp = 0;
volatile int32_t serviceSessionBackendId = 0;  // backend row for the running session
volatile bool usageBaselinePending = false;
volatile uint32_t walledGardenIp = 0;  // backend address every client may reach (admin pages)
String walledGardenHost;
volatile uint32_t gateAllowedIp = 0;  // client IP allowed to resolve real DNS

void setRelay(uint8_t pin, uint8_t onLevel, bool active) {
    const uint32_t level = active ? onLevel : !onLevel;
    gpio_set_level(static_cast<gpio_num_t>(pin), level);
    Serial0.printf("Relay GPIO %u -> %u (readback %d)\n", pin, static_cast<unsigned>(level),
                   gpio_get_level(static_cast<gpio_num_t>(pin)));
}

// Router idle/held level is HIGH and charger idle level is LOW (HIGH while charging).
void initializeRelays() {
    gpio_set_level(static_cast<gpio_num_t>(SUNSYNC_ROUTER_RELAY_PIN), SUNSYNC_ROUTER_RELAY_ON_LEVEL);
    gpio_set_level(static_cast<gpio_num_t>(SUNSYNC_CHARGER_RELAY_PIN), !SUNSYNC_CHARGER_RELAY_ON_LEVEL);
    pinMode(SUNSYNC_ROUTER_RELAY_PIN, OUTPUT);
    pinMode(SUNSYNC_CHARGER_RELAY_PIN, OUTPUT);
    Serial0.println("Router relay held ON; charger relay initialized OFF.");
}
void setChargingRelay(bool active) {
    setRelay(SUNSYNC_CHARGER_RELAY_PIN, SUNSYNC_CHARGER_RELAY_ON_LEVEL, active);
}

void endLocalServiceSession() {
    serviceSessionActive = false;
    serviceSessionPaused = false;
    serviceSessionIsCharging = false;
    serviceSessionRemainingMs = 0;
    serviceSessionCredits = 0;
    serviceSessionOwner = "";
    setChargingRelay(false);
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

String formatLcdClock(uint32_t totalSeconds) {
    char line[SUNSYNC_LCD_COLUMNS + 1];
    snprintf(line, sizeof(line), "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(totalSeconds / 3600),
             static_cast<unsigned long>((totalSeconds / 60) % 60),
             static_cast<unsigned long>(totalSeconds % 60));
    return String(line);
}

uint32_t serviceSessionRemainingSeconds() {
    uint32_t remainingMs = serviceSessionRemainingMs;
    if (!serviceSessionPaused) {
        const int32_t left = static_cast<int32_t>(serviceSessionEndsAt - millis());
        remainingMs = left > 0 ? static_cast<uint32_t>(left) : 0;
    }
    return (remainingMs + 999) / 1000;
}

uint32_t paymentSecondsRemaining();

void updateLcd() {
    if (!lcdAvailable) return;

    String lines[2];
    if (serviceSessionActive && serviceSessionIsCharging) {
        lines[0] = "CHARGING TIME:";
        lines[1] = formatLcdClock(serviceSessionRemainingSeconds());
    } else if (serviceSessionActive) {
        lines[0] = "CURRENT CREDITS:";
        lines[1] = String(serviceSessionCredits);
    } else {
        // Pulses are only counted while the pay window is open (see onCoinSignalPulse).
        if (coinPayWindowOpen) {
            char header[SUNSYNC_LCD_COLUMNS + 1];
            snprintf(header, sizeof(header), "INSERT COIN %2lus",
                     static_cast<unsigned long>(paymentSecondsRemaining()));
            lines[0] = header;
        } else {
            lines[0] = "PRESS PAY BUTTON";
        }
        lines[1] = "CREDITS: " + String(coinCredits);
    }

    for (uint8_t row = 0; row < 2; ++row) {
        if (lines[row] != lcdLastLines[row]) {
            lcd.setCursor(0, row);
            lcd.print(lcdLine(lines[row]));
            lcdLastLines[row] = lines[row];
        }
    }
}

void initializeDisplays() {
    lcd.begin(SUNSYNC_LCD_COLUMNS, SUNSYNC_LCD_ROWS, Wire);
    lcd.setBacklight(255);
    lcdAvailable = lcd.isConnected();
    if (lcdAvailable) {
        lcd.clear();
        updateLcd();
    } else {
        Serial0.printf("WARNING: LCD not detected at I2C address 0x%02X.\n",
                       SUNSYNC_LCD_ADDRESS);
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

volatile uint32_t dbgRawEdges = 0;  // TEMP-DIAG
volatile uint32_t dbgAcceptedPulses = 0;  // TEMP-DIAG
void IRAM_ATTR onCoinSignalPulse() {
#if SUNSYNC_COIN_SIGNAL_ENABLED
    const uint32_t now = micros();
    dbgRawEdges = dbgRawEdges + 1;  // TEMP-DIAG
    portENTER_CRITICAL_ISR(&coinSignalMux);
    // Real pulses are still LOW here; noise spikes have already returned HIGH.
    if (coinPayWindowOpen && digitalRead(SUNSYNC_COIN_SIGNAL_PIN) == LOW &&
        static_cast<uint32_t>(now - lastCoinPulseUs) >= SUNSYNC_COIN_PULSE_DEBOUNCE_US) {
        ++queuedCoinPulses;
        dbgAcceptedPulses = dbgAcceptedPulses + 1;  // TEMP-DIAG
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

    // The acceptor sends several pulses per peso; keep leftover pulses for the next coin.
    coinPulseRemainder += pulses;
    coinCredits += coinPulseRemainder / SUNSYNC_COIN_PULSES_PER_PESO;
    coinPulseRemainder %= SUNSYNC_COIN_PULSES_PER_PESO;
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

// Unspent credits are released if their owner's Wi-Fi client stays away this long.
const uint32_t CREDIT_ORPHAN_GRACE_MS = 30000;
uint8_t payOwnerMac[6] = {};
bool payOwnerMacKnown = false;
volatile uint32_t creditClearAtMs = 0;

bool lookupStationMacByIp(uint32_t ip, uint8_t mac[6]) {
    wifi_sta_list_t stations = {};
    wifi_sta_mac_ip_list_t withIp = {};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK ||
        esp_wifi_ap_get_sta_list_with_ip(&stations, &withIp) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < withIp.num; ++i) {
        if (withIp.sta[i].ip.addr == ip) {
            memcpy(mac, withIp.sta[i].mac, 6);
            return true;
        }
    }
    return false;
}

void releaseOrphanedCredits() {
    if (creditClearAtMs == 0 || static_cast<int32_t>(millis() - creditClearAtMs) < 0) {
        return;
    }
    creditClearAtMs = 0;
    portENTER_CRITICAL(&coinSignalMux);
    coinPayWindowOpen = false;
    queuedCoinPulses = 0;
    portEXIT_CRITICAL(&coinSignalMux);
    coinCredits = 0;
    payWindowOwner = "";
    payOwnerMacKnown = false;
}

void handlePaymentStatus(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    const String owner = requestedPaymentOwner(request);
    const bool ownsCredits = ownsPaymentCredits(owner);
    const bool ownsService = serviceSessionActive && owner.length() > 0 &&
                             owner == serviceSessionOwner;
    const uint32_t clientIp = static_cast<uint32_t>(request->client()->remoteIP());
    // A returning device (new IP/MAC after forgetting the network) re-binds by its device id.
    if (ownsService) {
        serviceSessionClientIp = clientIp;
    }
    if (ownsCredits) {
        creditClearAtMs = 0;
        payOwnerMacKnown = lookupStationMacByIp(clientIp, payOwnerMac);
    }
    String response;
    response.reserve(260);
    response += "{\"coin_input_enabled\":";
    response += coinInputEnabled ? "true" : "false";
    response += ",\"pay_window_active\":";
    response += coinPayWindowOpen && ownsCredits ? "true" : "false";
    response += ",\"seconds_remaining\":";
    response += coinPayWindowOpen && ownsCredits ? String(paymentSecondsRemaining()) : "0";
    response += ",\"credits\":";
    response += ownsCredits ? String(coinCredits) : "0";
    response += ",\"service_active\":";
    response += ownsService ? "true" : "false";
    response += ",\"service_paused\":";
    response += ownsService && serviceSessionPaused ? "true" : "false";
    response += ",\"service_remaining_seconds\":";
    response += ownsService ? String(serviceSessionRemainingSeconds()) : "0";
    response += ",\"dbg_raw_edges\":" + String(dbgRawEdges) +  // TEMP-DIAG
                ",\"dbg_accepted_pulses\":" + String(dbgAcceptedPulses);
    response += ",\"credit_unit\":\"coin_pulse\"}";
    sendResponse(request, 200, "application/json; charset=utf-8", response);
}

void closePaymentWindow() {
    portENTER_CRITICAL(&coinSignalMux);
    coinPayWindowOpen = false;
    queuedCoinPulses = 0;
    portEXIT_CRITICAL(&coinSignalMux);
}

void handleClosePaymentWindow(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    const String owner = requestedPaymentOwner(request);
    if (coinPayWindowOpen && ownsPaymentCredits(owner)) {
        closePaymentWindow();
    }
    handlePaymentStatus(request);
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
    payOwnerMacKnown = lookupStationMacByIp(
        static_cast<uint32_t>(request->client()->remoteIP()), payOwnerMac);
    creditClearAtMs = 0;
    coinPulseRemainder = 0;
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
    updatePaymentWindow();
    closePaymentWindow();
    serviceSessionActive = true;
    serviceSessionPaused = false;
    serviceSessionIsCharging = sessionType == "charging";
    serviceSessionOwner = owner;
    serviceSessionClientIp = static_cast<uint32_t>(request->client()->remoteIP());
    usageBaselinePending = true;
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

// Per-client traffic that crosses the AP (forwarded internet traffic only; the portal's own
// pages to 192.168.4.1 are excluded). Counted by wrapping the AP netif input/output.
struct ClientTraffic {
    uint8_t mac[6];
    uint64_t downBytes;
    uint64_t upBytes;
    bool used;
};
constexpr size_t MAX_TRAFFIC_CLIENTS = 8;
ClientTraffic clientTraffic[MAX_TRAFFIC_CLIENTS];
portMUX_TYPE trafficMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t trafficApIp[4] = {192, 168, 4, 1};
netif_input_fn originalApInput = nullptr;
netif_linkoutput_fn originalApLinkOutput = nullptr;

void addClientTraffic(const uint8_t* mac, uint32_t bytes, bool download) {
    portENTER_CRITICAL(&trafficMux);
    ClientTraffic* slot = nullptr;
    for (auto& entry : clientTraffic) {
        if (entry.used && memcmp(entry.mac, mac, 6) == 0) { slot = &entry; break; }
    }
    if (!slot) {
        for (auto& entry : clientTraffic) {
            if (!entry.used) {
                entry.used = true;
                memcpy(entry.mac, mac, 6);
                entry.downBytes = entry.upBytes = 0;
                slot = &entry;
                break;
            }
        }
    }
    if (slot) {
        if (download) slot->downBytes += bytes; else slot->upBytes += bytes;
    }
    portEXIT_CRITICAL(&trafficMux);
}

bool getClientTraffic(const uint8_t* mac, uint64_t& down, uint64_t& up) {
    bool found = false;
    portENTER_CRITICAL(&trafficMux);
    for (const auto& entry : clientTraffic) {
        if (entry.used && memcmp(entry.mac, mac, 6) == 0) {
            down = entry.downBytes;
            up = entry.upBytes;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&trafficMux);
    return found;
}

err_t countingApInput(struct pbuf* p, struct netif* inp) {
    if (p && p->len >= 34) {
        const uint8_t* frame = static_cast<const uint8_t*>(p->payload);
        const bool ipv4 = frame[12] == 0x08 && frame[13] == 0x00;
        if (ipv4 && (frame[0] & 1) == 0 && memcmp(frame + 30, trafficApIp, 4) != 0) {
            // Traffic leaving the AP is only forwarded for the client with an active, unpaused
            // Wi-Fi session. Broadcast and multicast (DHCP, ARP-adjacent) are left alone.
            uint32_t srcIp;
            memcpy(&srcIp, frame + 26, 4);
            const bool broadcastOrMulticast = frame[30] >= 224 || frame[33] == 255;
            uint32_t dstIp;
            memcpy(&dstIp, frame + 30, 4);
            const bool toWalledGarden = walledGardenIp != 0 && dstIp == walledGardenIp;
            if (!broadcastOrMulticast && !toWalledGarden &&
                (gateAllowedIp == 0 || srcIp != gateAllowedIp)) {
                pbuf_free(p);
                return ERR_OK;
            }
            addClientTraffic(frame + 6, p->tot_len, false);
        }
    }
    return originalApInput(p, inp);
}

err_t countingApLinkOutput(struct netif* netif, struct pbuf* p) {
    if (p && p->len >= 34) {
        const uint8_t* frame = static_cast<const uint8_t*>(p->payload);
        const bool ipv4 = frame[12] == 0x08 && frame[13] == 0x00;
        if (ipv4 && (frame[0] & 1) == 0 && memcmp(frame + 26, trafficApIp, 4) != 0) {
            addClientTraffic(frame, p->tot_len, true);
        }
    }
    return originalApLinkOutput(netif, p);
}

void installTrafficCounters() {
    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!apNetif) return;
    const IPAddress apIp = WiFi.softAPIP();
    for (int i = 0; i < 4; ++i) trafficApIp[i] = apIp[i];
    LOCK_TCPIP_CORE();
    struct netif* apLwipNetif = netif_get_by_index(esp_netif_get_netif_impl_index(apNetif));
    if (apLwipNetif && apLwipNetif->input != countingApInput) {
        originalApInput = apLwipNetif->input;
        originalApLinkOutput = apLwipNetif->linkoutput;
        apLwipNetif->input = countingApInput;
        apLwipNetif->linkoutput = countingApLinkOutput;
    }
    UNLOCK_TCPIP_CORE();
}

// Appends the access-point clients as a JSON array for the telemetry report.
void appendClientsJson(String& response) {
    wifi_sta_list_t associatedStations = {};
    wifi_sta_mac_ip_list_t stationsWithIp = {};
    if (esp_wifi_ap_get_sta_list(&associatedStations) != ESP_OK) {
        response += "[]";
        return;
    }
    const esp_err_t ipResult = esp_wifi_ap_get_sta_list_with_ip(&associatedStations, &stationsWithIp);
    bool first = true;
    response += "[";
    for (int index = 0; index < associatedStations.num; ++index) {
        const wifi_sta_info_t& station = associatedStations.sta[index];
        if (!first) response += ",";
        first = false;
        response += "{\"mac\":\"";
        response += formatMacAddress(station.mac);
        response += "\",\"rssi_dbm\":";
        response += String(static_cast<int>(station.rssi));
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
        uint64_t downBytes = 0, upBytes = 0;
        getClientTraffic(station.mac, downBytes, upBytes);
        response += "\",\"hostname\":null,\"downloaded_bytes\":";
        response += String(static_cast<unsigned long long>(downBytes));
        response += ",\"uploaded_bytes\":";
        response += String(static_cast<unsigned long long>(upBytes));
        response += "}";
    }
    response += "]";
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

// ---- Backend proxy: phones call /api/backend/*; only the ESP32 holds the device key.
struct BackendJob {
    AsyncWebServerRequestPtr request;
    String method;
    String path;
    String body;
};

constexpr size_t BACKEND_QUEUE_LENGTH = 8;
constexpr size_t BACKEND_MAX_RESPONSE_BYTES = 8192;
QueueHandle_t backendQueue = nullptr;

bool backendConfigured() {
    return strlen(SUNSYNC_BACKEND_URL) > 0 && strlen(SUNSYNC_DEVICE_API_KEY) > 0;
}

bool isSafeIdentifier(const String& value, size_t maxLength) {
    if (value.length() == 0 || value.length() > maxLength) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

// The telemetry reply lists every session the backend still considers active. If the admin
// stopped ours from the cloud dashboard, the kiosk ends its local session as well.
void endLocalSessionIfBackendClosed(const String& response) {
    const int32_t id = serviceSessionBackendId;
    if (!serviceSessionActive || id <= 0) return;
    static const char key[] = "\"active_session_ids\":[";
    const int listAt = response.indexOf(key);
    if (listAt < 0) return;
    const int listEnd = response.indexOf(']', listAt);
    if (listEnd < 0) return;
    const String list = "," + response.substring(listAt + strlen(key), listEnd) + ",";
    if (list.indexOf("," + String(id) + ",") < 0) endLocalServiceSession();
}

void backendWorker(void*) {
    for (;;) {
        BackendJob* job = nullptr;
        if (xQueueReceive(backendQueue, &job, portMAX_DELAY) != pdTRUE || !job) continue;

        int status = 502;
        String payload = "{\"detail\":\"The backend is unreachable.\"}";
        String responseType = "application/json; charset=utf-8";
        if (upstreamConnected) {
            static uint32_t lastResolveMs = 0;
            if (walledGardenHost.length() > 0 &&
                millis() - lastResolveMs > (walledGardenIp == 0 ? 5000u : 60000u)) {
                IPAddress resolved;
                if (WiFi.hostByName(walledGardenHost.c_str(), resolved) == 1) {
                    walledGardenIp = static_cast<uint32_t>(resolved);
                }
                lastResolveMs = millis();
            }
            WiFiClientSecure client;
            if (strlen(SUNSYNC_BACKEND_CA_CERT) > 0) {
                client.setCACert(SUNSYNC_BACKEND_CA_CERT);
            } else {
                client.setInsecure();
            }
            HTTPClient http;
            const uint16_t timeoutMs = 15000;
            http.setConnectTimeout(timeoutMs);
            http.setTimeout(timeoutMs);
            if (http.begin(client, String(SUNSYNC_BACKEND_URL) + job->path)) {
                http.addHeader("X-Device-Key", SUNSYNC_DEVICE_API_KEY);
                if (job->body.length() > 0) {
                    http.addHeader("Content-Type", "application/json");
                }
                const int code = http.sendRequest(job->method.c_str(), job->body);
                if (code > 0) {
                    String received = http.getString();
                    if (received.length() <= BACKEND_MAX_RESPONSE_BYTES) {
                        status = code;
                        if (code >= 200 && code < 300 && job->method == "POST" &&
                            job->path == "/sessions/start") {
                            const int idAt = received.indexOf("\"id\":");
                            if (idAt >= 0) {
                                serviceSessionBackendId = received.substring(idAt + 5).toInt();
                            }
                        }
                        if (code >= 200 && code < 300 && job->path == "/devices/telemetry") {
                            endLocalSessionIfBackendClosed(received);
                        }
                        payload = received.length() > 0 ? received : String("null");
                    }
                } else {
                    Serial0.printf("Backend request failed: %s\n",
                                   HTTPClient::errorToString(code).c_str());
                }
                http.end();
            }
        }
        if (auto request = job->request.lock()) {
            sendResponse(request.get(), status, responseType.c_str(), payload);
        }
        delete job;
    }
}

void initWalledGarden() {
    String host = SUNSYNC_BACKEND_URL;
    const int scheme = host.indexOf("://");
    if (scheme >= 0) host = host.substring(scheme + 3);
    const int slash = host.indexOf('/');
    if (slash >= 0) host = host.substring(0, slash);
    const int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    host.toLowerCase();
    walledGardenHost = host;
}

void startBackendWorker() {
    if (backendQueue) return;
    initWalledGarden();
    backendQueue = xQueueCreate(BACKEND_QUEUE_LENGTH, sizeof(BackendJob*));
    xTaskCreate(backendWorker, "backend", 12288, nullptr, 1, nullptr);
}

void forwardToBackend(AsyncWebServerRequest* request, const char* method,
                      const String& path, const String& body = String()) {
    if (!backendConfigured() || !backendQueue) {
        sendResponse(request, 503, "application/json; charset=utf-8",
                     "{\"detail\":\"Backend is not configured on this device.\"}");
        return;
    }
    if (uxQueueSpacesAvailable(backendQueue) == 0) {
        sendResponse(request, 503, "application/json; charset=utf-8",
                     "{\"detail\":\"The device is busy. Try again shortly.\"}");
        return;
    }
    BackendJob* job = new BackendJob{request->pause(), method, path, body};
    if (xQueueSend(backendQueue, &job, 0) != pdTRUE) {
        if (auto paused = job->request.lock()) {
            sendResponse(paused.get(), 503, "application/json; charset=utf-8",
                         "{\"detail\":\"The device is busy. Try again shortly.\"}");
        }
        delete job;
    }
}

// Reports the running Wi-Fi session's downloaded data to the backend every 30 s and once more
// after it ends, so the dashboard can show usage per session.
void reportSessionUsage() {
    static uint8_t mac[6];
    static bool macKnown = false;
    static uint64_t baseline = 0, carry = 0;
    static uint32_t lastReportMs = 0;
    static String owner;

    if (usageBaselinePending) {
        usageBaselinePending = false;
        carry = 0;
        macKnown = false;
        lastReportMs = millis();
    }
    const int32_t id = serviceSessionBackendId;
    if (id <= 0) return;
    const bool active = serviceSessionActive;
    if (active && serviceSessionIsCharging) return;
    if (!active && serviceSessionOwner.length() == 0 && owner.length() == 0) {
        serviceSessionBackendId = 0;
        return;
    }
    if (active) {
        if (serviceSessionOwner.length() > 0) owner = serviceSessionOwner;
        uint8_t current[6];
        uint64_t down = 0, up = 0;
        if (lookupStationMacByIp(serviceSessionClientIp, current) &&
            (!macKnown || memcmp(current, mac, 6) != 0)) {
            if (macKnown && getClientTraffic(mac, down, up) && down > baseline) {
                carry += down - baseline;
            }
            memcpy(mac, current, 6);
            baseline = getClientTraffic(mac, down, up) ? down : 0;
            macKnown = true;
        }
        if (millis() - lastReportMs < 30000) return;
    }
    uint64_t total = carry, down = 0, up = 0;
    if (macKnown && getClientTraffic(mac, down, up) && down > baseline) total += down - baseline;
    const String path = "/sessions/" + String(id) + "/usage?device_id=" + owner +
                        "&data_download_kb=" + String(static_cast<unsigned long>(total / 1024));
    BackendJob* job = new BackendJob{AsyncWebServerRequestPtr(), "PATCH", path, String()};
    if (xQueueSend(backendQueue, &job, 0) != pdTRUE) {
        delete job;
        return;
    }
    lastReportMs = millis();
    if (!active) {
        serviceSessionBackendId = 0;
        owner = "";
        macKnown = false;
    }
}

// Pushes the kiosk's live state (health and connected clients) to the backend every 15 s so the
// cloud dashboard can show it without reaching this device.
void reportTelemetry() {
    static uint32_t lastReportMs = 0;
    const uint32_t now = millis();
    if (!backendConfigured() || !backendQueue || !upstreamConnected) return;
    if (now - lastReportMs < 15000 || uxQueueSpacesAvailable(backendQueue) < 2) return;
    lastReportMs = now;

    String deviceId = WiFi.macAddress();
    deviceId.replace(":", "");
    String body;
    body.reserve(1024);
    body = "{\"device_id\":\"sunsync-" + deviceId + "\",\"uptime_ms\":" + String(now) +
           ",\"upstream_connected\":" + (upstreamConnected ? "true" : "false") +
           ",\"internet_sharing_enabled\":" + (internetSharingEnabled ? "true" : "false") +
           ",\"internet_sharing_supported\":" +
#if SUNSYNC_NAPT_AVAILABLE
           "true" +
#else
           "false" +
#endif
           ",\"clients\":";
    appendClientsJson(body);
    body += "}";
    BackendJob* job = new BackendJob{AsyncWebServerRequestPtr(), "POST", "/devices/telemetry", body};
    if (xQueueSend(backendQueue, &job, 0) != pdTRUE) delete job;
}

bool requireValidDevice(AsyncWebServerRequest* request, String& owner) {
    owner = requestedPaymentOwner(request);
    if (isSafeIdentifier(owner, 100)) return true;
    sendResponse(request, 400, "application/json; charset=utf-8",
                 "{\"detail\":\"A valid device_id is required.\"}");
    return false;
}

void handleBackendRegister(AsyncWebServerRequest* request) {
    String owner;
    if (!requireValidDevice(request, owner)) return;
    forwardToBackend(request, "POST", "/devices/register?device_id=" + owner);
}

void handleBackendRates(AsyncWebServerRequest* request) {
    forwardToBackend(request, "GET", "/settings/public/1");
}

void handleBackendActive(AsyncWebServerRequest* request) {
    String owner;
    if (!requireValidDevice(request, owner)) return;
    forwardToBackend(request, "GET", "/sessions/device/" + owner + "/active");
}

void handleBackendStart(AsyncWebServerRequest* request) {
    updatePaymentWindow();
    String owner;
    if (!requireValidDevice(request, owner)) return;
    const AsyncWebParameter* typeParameter = request->getParam("session_type");
    uint32_t credits = 0;
    if (!typeParameter || !readUnsignedParameter(request, "credits", credits) ||
        (typeParameter->value() != "wifi" && typeParameter->value() != "charging")) {
        sendResponse(request, 400, "application/json; charset=utf-8",
                     "{\"detail\":\"Valid session_type and credits are required.\"}");
        return;
    }
    if (!ownsPaymentCredits(owner)) {
        sendResponse(request, 403, "application/json; charset=utf-8",
                     "{\"detail\":\"These credits do not belong to this device.\"}");
        return;
    }
    // Pressing Connect/Charge ends coin collection; count pulses already queued first.
    updatePaymentWindow();
    closePaymentWindow();
    if (credits > coinCredits) {
        sendResponse(request, 402, "application/json; charset=utf-8",
                     "{\"detail\":\"There are not enough credits to start this session.\"}");
        return;
    }
    String body = "{\"device_id\":\"" + owner + "\",\"session_type\":\"" +
                  typeParameter->value() + "\",\"credits\":" + String(credits) + "}";
    forwardToBackend(request, "POST", "/sessions/start", body);
}

void handleBackendSession(AsyncWebServerRequest* request) {
    String owner;
    if (!requireValidDevice(request, owner)) return;
    const AsyncWebParameter* idParameter = request->getParam("id");
    const AsyncWebParameter* actionParameter = request->getParam("action");
    if (!idParameter || !actionParameter || idParameter->value().length() > 10 ||
        !isSafeIdentifier(idParameter->value(), 10) ||
        strspn(idParameter->value().c_str(), "0123456789") != idParameter->value().length() ||
        (actionParameter->value() != "pause" && actionParameter->value() != "resume" &&
         actionParameter->value() != "end")) {
        sendResponse(request, 400, "application/json; charset=utf-8",
                     "{\"detail\":\"A numeric id and a valid action are required.\"}");
        return;
    }
    forwardToBackend(request, "PATCH",
                     "/sessions/" + idParameter->value() + "/" + actionParameter->value() +
                         "?device_id=" + owner);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        if (payOwnerMacKnown && memcmp(info.wifi_ap_stadisconnected.mac, payOwnerMac, 6) == 0 &&
            (coinCredits > 0 || coinPayWindowOpen)) {
            creditClearAtMs = millis() + CREDIT_ORPHAN_GRACE_MS;
        }
    } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
        if (payOwnerMacKnown && memcmp(info.wifi_ap_staconnected.mac, payOwnerMac, 6) == 0) {
            creditClearAtMs = 0;
        }
    }
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        upstreamConnected = true;
        configTime(0, 0, "pool.ntp.org", "time.google.com");
#if SUNSYNC_NAPT_AVAILABLE
        esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t apInfo;
        bool apDnsConfigured = false;
        if (apNetif && esp_netif_get_ip_info(apNetif, &apInfo) == ESP_OK &&
            esp_netif_dhcps_stop(apNetif) == ESP_OK) {
            // Clients use the ESP32 as DNS so the session gate decides who can resolve names.
            esp_netif_dns_info_t gateDns = {};
            gateDns.ip.type = ESP_IPADDR_TYPE_V4;
            gateDns.ip.u_addr.ip4.addr = apInfo.ip.addr;
            const esp_err_t dnsResult =
                esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &gateDns);
            uint8_t offerDns = 2;
            esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
            const esp_err_t dhcpResult = esp_netif_dhcps_start(apNetif);
            apDnsConfigured = dnsResult == ESP_OK && dhcpResult == ESP_OK;
        }
        if (apDnsConfigured) {
            LOCK_TCPIP_CORE();
            ip_napt_enable(apInfo.ip.addr, 1);
            UNLOCK_TCPIP_CORE();
            internetSharingEnabled = true;
            Serial0.println("IPv4 NAT enabled; internet is gated by the session DNS.");
        } else {
            Serial0.println("ERROR: Could not configure DNS/NAT for AP clients.");
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
            LOCK_TCPIP_CORE();
            ip_napt_enable(apInfo.ip.addr, 0);
            UNLOCK_TCPIP_CORE();
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
        Serial0.printf("Upstream Wi-Fi disconnected (reason %u).\n", info.wifi_sta_disconnected.reason);
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
// DNS for AP clients: only the client with an active Wi-Fi session gets real answers;
// everyone else is pointed at the portal so browsing lands on the payment page.
size_t buildPortalDnsReply(const uint8_t* query, size_t length, uint8_t* reply) {
    if (length < 17 || (query[2] & 0x80) || query[4] != 0 || query[5] != 1) return 0;
    size_t offset = 12;
    while (offset < length && query[offset] != 0) offset += query[offset] + 1;
    const size_t questionEnd = offset + 5;
    if (questionEnd > length) return 0;
    memcpy(reply, query, questionEnd);
    const uint16_t queryType = (query[offset + 1] << 8) | query[offset + 2];

    // Only the OS captive-portal probes get the portal address; other names fail fast.
    String name;
    for (size_t pos = 12; pos < offset && query[pos] != 0; pos += query[pos] + 1) {
        if (name.length()) name += '.';
        for (uint8_t i = 1; i <= query[pos] && pos + i < offset; ++i) {
            name += static_cast<char>(tolower(query[pos + i]));
        }
    }
    static const char* const probeNames[] = {
        "connectivitycheck.gstatic.com", "connectivitycheck.android.com",
        "clients3.google.com", "www.google.com", "captive.apple.com",
        "www.msftconnecttest.com", "msftconnecttest.com", "dns.msftncsi.com",
        "detectportal.firefox.com", "nmcheck.gnome.org", "network-test.debian.org"};
    const uint32_t gardenIp = walledGardenIp;
    const bool isGarden = gardenIp != 0 && name == walledGardenHost;
    bool isProbe = false;
    for (const char* probe : probeNames) {
        if (name == probe) { isProbe = true; break; }
    }
    if (!isProbe && !isGarden) {
        reply[2] = 0x81;
        reply[3] = 0x83;  // NXDOMAIN
        reply[6] = reply[7] = reply[8] = reply[9] = reply[10] = reply[11] = 0;
        return questionEnd;
    }

    reply[2] = 0x81;
    reply[3] = 0x80;
    reply[6] = 0;
    reply[7] = queryType == 1 ? 1 : 0;
    reply[8] = reply[9] = reply[10] = reply[11] = 0;
    size_t size = questionEnd;
    if (queryType == 1) {
        const IPAddress portalIp = isGarden ? IPAddress(gardenIp) : WiFi.softAPIP();
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, static_cast<uint8_t>(isGarden ? 60 : 1), 0, 4,
                                  portalIp[0], portalIp[1], portalIp[2], portalIp[3]};
        memcpy(reply + size, answer, sizeof(answer));
        size += sizeof(answer);
    }
    return size;
}

void gateDnsTask(void*) {
    static uint8_t query[1232];
    static uint8_t reply[1232];
    WiFiUDP clientSocket;
    WiFiUDP upstreamSocket;
    clientSocket.begin(DNS_PORT);
    upstreamSocket.begin(54053);
    for (;;) {
        const int length = clientSocket.parsePacket();
        if (length <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const IPAddress clientIp = clientSocket.remoteIP();
        const uint16_t clientPort = clientSocket.remotePort();
        if (length > static_cast<int>(sizeof(query))) {
            clientSocket.flush();
            continue;
        }
        clientSocket.read(query, length);

        const uint32_t allowedIp = gateAllowedIp;
        const IPAddress upstreamDns = WiFi.dnsIP();
        size_t replySize = 0;
        if (allowedIp != 0 && static_cast<uint32_t>(clientIp) == allowedIp &&
            upstreamDns != IPAddress(0, 0, 0, 0)) {
            upstreamSocket.beginPacket(upstreamDns, 53);
            upstreamSocket.write(query, length);
            upstreamSocket.endPacket();
            const uint32_t deadline = millis() + 1500;
            while (replySize == 0 && static_cast<int32_t>(deadline - millis()) > 0) {
                const int answerLength = upstreamSocket.parsePacket();
                if (answerLength <= 0) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (answerLength <= static_cast<int>(sizeof(reply))) {
                    upstreamSocket.read(reply, answerLength);
                    if (answerLength >= 2 && reply[0] == query[0] && reply[1] == query[1]) {
                        replySize = answerLength;
                    }
                } else {
                    upstreamSocket.flush();
                }
            }
        } else {
            replySize = buildPortalDnsReply(query, length, reply);
        }
        if (replySize > 0) {
            clientSocket.beginPacket(clientIp, clientPort);
            clientSocket.write(reply, replySize);
            clientSocket.endPacket();
        }
    }
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
    xTaskCreate(gateDnsTask, "gateDns", 6144, nullptr, 1, nullptr);
    installTrafficCounters();
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
    server.on("/api/payment/status", HTTP_GET, handlePaymentStatus);
    server.on("/api/payment/open", HTTP_POST, handleOpenPaymentWindow);
    server.on("/api/payment/close", HTTP_POST, handleClosePaymentWindow);
    server.on("/api/service/start", HTTP_POST, handleStartService);
    server.on("/api/service/pause", HTTP_POST, handlePauseService);
    server.on("/api/service/resume", HTTP_POST, handleResumeService);
    server.on("/api/service/end", HTTP_POST, handleEndService);
    server.on("/api/health", HTTP_GET, handleHealth);
    startBackendWorker();
    server.on("/api/backend/register", HTTP_POST, handleBackendRegister);
    server.on("/api/backend/rates", HTTP_GET, handleBackendRates);
    server.on("/api/backend/active", HTTP_GET, handleBackendActive);
    server.on("/api/backend/start", HTTP_POST, handleBackendStart);
    server.on("/api/backend/session", HTTP_POST, handleBackendSession);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/index.html");
    });
    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveLittleFsFile(request, "/index.html");
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
#if SUNSYNC_COIN_SIGNAL_ACTIVE_HIGH
    pinMode(SUNSYNC_COIN_SIGNAL_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(SUNSYNC_COIN_SIGNAL_PIN),
                    onCoinSignalPulse, RISING);
#else
    pinMode(SUNSYNC_COIN_SIGNAL_PIN, INPUT);  // TEMP-TEST: no internal pull, external circuit drives the line
    attachInterrupt(digitalPinToInterrupt(SUNSYNC_COIN_SIGNAL_PIN),
                    onCoinSignalPulse, FALLING);
#endif
    coinInputEnabled = true;
#else
    Serial0.println("Coin signal input disabled; configure a safe 3.3 V interface first.");
#endif

    refreshMeasurements();
    lastSensorRefreshMs = millis();
    startNetwork();
#if SUNSYNC_MQTT_ENABLED
#if SUNSYNC_MQTT_TLS
    if (strlen(SUNSYNC_MQTT_CA_CERT) > 0) {
        mqttNetClient.setCACert(SUNSYNC_MQTT_CA_CERT);
    } else {
        Serial0.println("WARNING: MQTT TLS broker certificate is not verified (no CA set).");
        mqttNetClient.setInsecure();
    }
#endif
    mqttClient.setServer(SUNSYNC_MQTT_HOST, SUNSYNC_MQTT_PORT);
#endif
    startWebServer();
}

void loop() {
    {  // TEMP-DIAG
        static uint32_t lastRaw = 0, lastAcc = 0, lastChange = 0;
        if (dbgRawEdges != lastRaw || dbgAcceptedPulses != lastAcc) {
            lastRaw = dbgRawEdges; lastAcc = dbgAcceptedPulses; lastChange = millis();
        } else if (lastChange && millis() - lastChange > 1500) {
            Serial0.printf("COIN raw_edges=%u accepted=%u credits=%u\n",
                           (unsigned)lastRaw, (unsigned)lastAcc, (unsigned)coinCredits);
            lastChange = 0;
        }
    }
    releaseOrphanedCredits();
    updatePaymentWindow();
    updateServiceSession();
    reportSessionUsage();
    reportTelemetry();
    gateAllowedIp = (serviceSessionActive && !serviceSessionPaused && !serviceSessionIsCharging)
        ? serviceSessionClientIp : 0;
    updateLcd();
    serviceMqtt();

    const uint32_t now = millis();
    if (now - lastSensorRefreshMs >= SENSOR_REFRESH_INTERVAL_MS) {
        refreshMeasurements();
        lastSensorRefreshMs = millis();
    }
    delay(2);
}
