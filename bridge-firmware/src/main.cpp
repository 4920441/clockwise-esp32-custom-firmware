/*
 * Bridge Firmware for Clockwise ESP32 HUB75 64x64 Clock
 * E-pin test version: cycles through safe E pin candidates via web UI
 *
 * Upload via browser at http://<ip>/update
 * Cycle E pin at http://<ip>/next or http://<ip>/set?e=<gpio>
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Preferences.h>

// --- WiFi credentials — CHANGE THESE ---
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// Pin mapping from Clockwise firmware binary (confirmed at offset 0x19c6)
#define R1_PIN   25
#define G1_PIN   26
#define B1_PIN   27
#define R2_PIN   14
#define G2_PIN   12
#define B2_PIN   13
#define A_PIN    23
#define B_PIN    19
#define C_PIN     5
#define D_PIN    17
#define LAT_PIN   4
#define OE_PIN   15
#define CLK_PIN  16

// Safe E pin candidates to try (GPIO 6-11 are SPI flash — NEVER use them)
const int E_CANDIDATES[] = { -1, 32, 33, 18, 22 };
const int NUM_CANDIDATES = sizeof(E_CANDIDATES) / sizeof(E_CANDIDATES[0]);

#define PANEL_WIDTH  64
#define PANEL_HEIGHT 64

MatrixPanel_I2S_DMA* dma_display = nullptr;
WebServer server(80);
Preferences prefs;
int currentEIndex = 0;
int currentEPin = -1;

void setupDisplay(int ePin) {
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN,
        R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, (int8_t)ePin,
        LAT_PIN, OE_PIN, CLK_PIN
    };

    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, 1);
    mxconfig.gpio = pins;
    mxconfig.clkphase = false;
    mxconfig.driver = HUB75_I2S_CFG::FM6126A;

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    dma_display->begin();
    dma_display->setBrightness8(60);
    dma_display->clearScreen();
}

void drawInfo() {
    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t green = dma_display->color565(0, 255, 0);
    uint16_t cyan  = dma_display->color565(0, 200, 255);
    uint16_t yellow = dma_display->color565(255, 255, 0);

    dma_display->fillScreen(0);
    dma_display->setTextSize(1);

    dma_display->setCursor(2, 2);
    dma_display->setTextColor(yellow);
    dma_display->printf("E=%d", currentEPin);

    dma_display->setCursor(2, 11);
    dma_display->setTextColor(white);
    dma_display->printf("(%d/%d)", currentEIndex + 1, NUM_CANDIDATES);

    dma_display->setCursor(2, 20);
    dma_display->setTextColor(green);
    dma_display->print("OTA ready");

    if (WiFi.status() == WL_CONNECTED) {
        dma_display->setCursor(2, 29);
        dma_display->setTextColor(cyan);
        dma_display->print(WiFi.localIP().toString().c_str());
    }

    // Color bars to verify row mapping
    for (int y = 40; y < 48; y++)
        for (int x = 0; x < 64; x++)
            dma_display->drawPixelRGB888(x, y, 255, 0, 0);

    for (int y = 48; y < 56; y++)
        for (int x = 0; x < 64; x++)
            dma_display->drawPixelRGB888(x, y, 0, 255, 0);

    for (int y = 56; y < 64; y++)
        for (int x = 0; x < 64; x++)
            dma_display->drawPixelRGB888(x, y, 0, 0, 255);
}

bool isSafeGPIO(int pin) {
    // GPIO 6-11 are SPI flash pins — using them WILL brick the device
    if (pin >= 6 && pin <= 11) return false;
    if (pin > 39) return false;
    return true;
}

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>ESP32 Bridge - E Pin Test</title></head><body>";
    html += "<h1>ESP32 HUB75 Bridge Firmware</h1>";
    html += "<h2>E Pin: GPIO " + String(currentEPin) + " (" + String(currentEIndex + 1) + "/" + String(NUM_CANDIDATES) + ")</h2>";
    html += "<p>Candidates: ";
    for (int i = 0; i < NUM_CANDIDATES; i++) {
        if (i == currentEIndex) html += "<b>[";
        html += String(E_CANDIDATES[i]);
        if (i == currentEIndex) html += "]</b>";
        if (i < NUM_CANDIDATES - 1) html += ", ";
    }
    html += "</p>";
    html += "<p><a href='/next'>Try NEXT E pin (saves &amp; reboots)</a></p>";
    html += "<p><b>WARNING:</b> GPIO 6-11 are SPI flash pins. Setting them WILL brick the device!</p>";
    html += "<hr><p>WiFi: " + WiFi.SSID() + " | IP: " + WiFi.localIP().toString() + "</p>";
    html += "<p><a href='/update'>Upload New Firmware (OTA)</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleNext() {
    currentEIndex = (currentEIndex + 1) % NUM_CANDIDATES;
    prefs.begin("bridge", false);
    prefs.putInt("eIdx", currentEIndex);
    prefs.end();
    server.send(200, "text/html", "<html><body><h1>Rebooting with E=" + String(E_CANDIDATES[currentEIndex]) + "...</h1></body></html>");
    delay(500);
    ESP.restart();
}

void handleSetE() {
    if (server.hasArg("e")) {
        int eVal = server.arg("e").toInt();
        if (!isSafeGPIO(eVal) && eVal != -1) {
            server.send(400, "text/html", "<html><body><h1>REFUSED: GPIO " + String(eVal) + " is not safe!</h1></body></html>");
            return;
        }
        for (int i = 0; i < NUM_CANDIDATES; i++) {
            if (E_CANDIDATES[i] == eVal) { currentEIndex = i; break; }
        }
        prefs.begin("bridge", false);
        prefs.putInt("eIdx", currentEIndex);
        prefs.end();
        server.send(200, "text/html", "<html><body><h1>Rebooting with E=" + String(eVal) + "...</h1></body></html>");
        delay(500);
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== E-Pin Test Bridge Firmware ===");

    prefs.begin("bridge", true);
    currentEIndex = prefs.getInt("eIdx", 0);
    prefs.end();
    if (currentEIndex >= NUM_CANDIDATES) currentEIndex = 0;
    currentEPin = E_CANDIDATES[currentEIndex];

    Serial.printf("Using E pin: GPIO %d (candidate %d/%d)\n", currentEPin, currentEIndex + 1, NUM_CANDIDATES);

    setupDisplay(currentEPin);
    drawInfo();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); attempts++; }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        drawInfo();
    }

    server.on("/", handleRoot);
    server.on("/next", handleNext);
    server.on("/set", handleSetE);
    ElegantOTA.begin(&server);
    server.begin();
}

void loop() {
    server.handleClient();
    ElegantOTA.loop();
}
