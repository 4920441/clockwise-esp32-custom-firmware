/*
 * Bridge Firmware for Clockwise ESP32 HUB75 64x64 Clock
 *
 * Purpose: Provide OTA web upload so future firmware can be flashed
 * wirelessly without DNS spoofing tricks.
 *
 * Features:
 *   - Connects to WiFi
 *   - Drives the 64x64 HUB75E panel with a test pattern
 *   - Web OTA at http://<ip>/update
 *   - Shows IP address on the display
 *
 * Pin mapping matches the Clockwise PCB schematic.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// --- WiFi credentials — CHANGE THESE ---
const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASSWORD";

// --- HUB75E pin mapping (from Clockwise PCB schematic) ---
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
#define E_PIN    32   // HUB75E for 64x64
#define LAT_PIN   4
#define OE_PIN   15
#define CLK_PIN  16

// --- Display ---
#define PANEL_WIDTH  64
#define PANEL_HEIGHT 64

MatrixPanel_I2S_DMA* dma_display = nullptr;
WebServer server(80);

unsigned long lastMillis = 0;
int hue = 0;

void setupDisplay() {
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN,
        R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
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

void drawTestPattern() {
    // Rainbow gradient background
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            uint8_t h = (hue + x * 4 + y * 4) % 256;
            uint8_t r, g, b;
            // HSV to RGB (simplified, S=255, V=80)
            uint8_t region = h / 43;
            uint8_t remainder = (h - (region * 43)) * 6;
            uint8_t val = 80;
            switch (region) {
                case 0:  r = val; g = (val * remainder) / 255; b = 0; break;
                case 1:  r = val - (val * remainder) / 255; g = val; b = 0; break;
                case 2:  r = 0; g = val; b = (val * remainder) / 255; break;
                case 3:  r = 0; g = val - (val * remainder) / 255; b = val; break;
                case 4:  r = (val * remainder) / 255; g = 0; b = val; break;
                default: r = val; g = 0; b = val - (val * remainder) / 255; break;
            }
            dma_display->drawPixelRGB888(x, y, r, g, b);
        }
    }
    hue = (hue + 1) % 256;
}

void drawText(const char* text, int x, int y, uint16_t color) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(color);
    dma_display->setCursor(x, y);
    dma_display->print(text);
}

void showStatus() {
    // Overlay status text on the pattern
    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t black = dma_display->color565(0, 0, 0);

    // Draw black background for text area
    dma_display->fillRect(0, 0, 64, 26, black);

    drawText("BRIDGE FW", 2, 2, white);
    drawText("OTA ready", 2, 11, dma_display->color565(0, 255, 0));

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        drawText(ip.c_str(), 2, 20, dma_display->color565(0, 200, 255));
    } else {
        drawText("WiFi...", 2, 20, dma_display->color565(255, 100, 0));
    }
}

void handleRoot() {
    String html = "<!DOCTYPE html><html><head><title>ESP32 Bridge</title></head><body>";
    html += "<h1>ESP32 HUB75 Bridge Firmware</h1>";
    html += "<p>WiFi: " + WiFi.SSID() + "</p>";
    html += "<p>IP: " + WiFi.localIP().toString() + "</p>";
    html += "<p>Free heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
    html += "<p><a href='/update'>Upload New Firmware (OTA)</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== ESP32 HUB75 Bridge Firmware ===");

    // Start display
    setupDisplay();
    dma_display->fillScreenRGB888(0, 0, 40);
    drawText("Booting...", 2, 28, dma_display->color565(255, 255, 255));

    // Connect WiFi
    Serial.printf("Connecting to %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi connection failed - continuing without WiFi");
    }

    // Web server + OTA
    server.on("/", handleRoot);
    ElegantOTA.begin(&server);
    server.begin();
    Serial.println("Web server started. OTA at /update");
}

void loop() {
    server.handleClient();
    ElegantOTA.loop();

    // Update display every 50ms
    if (millis() - lastMillis > 50) {
        lastMillis = millis();
        drawTestPattern();
        showStatus();
    }
}
