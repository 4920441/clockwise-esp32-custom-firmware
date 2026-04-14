/*
 * ESP32 HUB75E 64x64 Split-Flap Display + UDP Video
 *
 * Default: Animated Solari/split-flap board showing date, time, temp, solar
 * UDP mode: Receives 64x64 RGB888 frames on port 5005
 * OTA: http://<ip>/update
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <HTTPClient.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <time.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ─── Config ───────────────────────────────────────────────────────
static const char* WIFI_SSID = "YOUR_SSID";
static const char* WIFI_PASS = "YOUR_PASSWORD";
static const char* NTP_SERVER = "pool.ntp.org";  // or your local NTP server
static const char* TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";  // adjust for your timezone
// InfluxDB: 4 queries combined into one multi-query request
static const char* INFLUX_HOST = "192.168.1.10";  // your InfluxDB host
static const int   INFLUX_PORT = 8086;
static const char* INFLUX_DB   = "telegraf";
// Queries joined with semicolons; results come back in same order.
static const char* INFLUX_MULTI_Q =
    // 0: outdoor temperature
    "SELECT last(\"value\") FROM \"autogen\".\"mqtt_consumer\" "
    "WHERE \"topic\" = 'panasonic_heat_pump/main/Outside_Temp' AND time > now() - 1h"
    ";"
    // 1: SolaX today kWh
    "SELECT max(\"value\") FROM \"solaxboth\" WHERE \"dailyall\" = '1' "
    "AND time > now() - 1d GROUP BY time(1d)"
    ";"
    // 2: EcoFlow today kWh
    "SELECT last(\"ENERGY_Today\") FROM \"autogen\".\"mqtt_consumer\" "
    "WHERE \"topic\" = 'tele/tasmota_1F79BA/SENSOR' "
    "AND time > now() - 1d GROUP BY time(1d)"
    ";"
    // 3: grid power (MT175_P), positive = importing, negative = exporting
    "SELECT last(\"MT175_P\") FROM \"autogen\".\"mqtt_consumer\" "
    "WHERE \"topic\" = 'tele/tasmota_1E6CDF/SENSOR' AND time > now() - 10m";

// ─── HUB75E pins ──────────────────────────────────────────────────
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13
#define A_PIN  23
#define B_PIN  19
#define C_PIN   5
#define D_PIN  17
#define E_PIN  18
#define LAT_PIN 4
#define OE_PIN 15
#define CLK_PIN 16

#define PW 64
#define PH 64
#define FRAME_SIZE (PW * PH * 3)
#define UDP_PORT 5005
#define HDR_SIZE 4
#define CHUNK_MAX 1400

// ─── Split-flap layout ───────────────────────────────────────────
// Using default 5x7 font: each char = 6px wide, 8px tall
// We use 9px per row (8px + 1px gap for the "flap split" look)
#define COLS 10
#define ROWS 5
#define CW 6    // char cell width
#define CH 9    // char cell height

// Row Y positions
static const int ROW_Y[] = { 1, 12, 24, 37, 48 };
static const int ROW_X = 2;

// ─── Globals ─────────────────────────────────────────────────────
MatrixPanel_I2S_DMA* dp = nullptr;
WiFiUDP udp;
WebServer server(80);

// Colors
uint16_t cAmber, cGreen, cRed, cWhite, cBg, cCell, cSep, cFlap;

// UDP state
uint8_t frameBuf[FRAME_SIZE];
uint8_t recvBuf[HDR_SIZE + CHUNK_MAX + 64];
uint32_t rxChunks = 0;
uint8_t rxFrameId = 255, rxTotal = 0;
unsigned long lastPktTime = 0;
unsigned long frameCnt = 0, lastFrameT = 0;
float fps = 0;
bool udpMode = false;

// Data
float dTemp = -999, dSolarKwh = -999;
int dSolarW = 0;
bool dValid = false;
unsigned long lastFetch = 0;

// ─── LDR auto-brightness ────────────────────────────────────────
#define LDR_PIN 35
#define LDR_READ_INTERVAL 500    // ms between readings
#define LDR_BRIGHT_MIN 10        // display brightness in dark
#define LDR_BRIGHT_MAX 150       // display brightness in bright light
#define LDR_RAW_MIN 30           // LDR ADC value in darkness
#define LDR_RAW_MAX 2000         // LDR ADC value in bright room
bool autoBrightness = true;
int ldrSmoothed = 500;           // smoothed ADC reading
unsigned long lastLdrRead = 0;
uint8_t currentBrightness = 60;

// ─── MQTT / Rotation config ─────────────────────────────────────
#define MQTT_MAX_ITEMS 8
#define MQTT_TOPIC_MAX 80
#define MQTT_LABEL_MAX 4
#define MQTT_UNIT_MAX 2
#define MQTT_VALUE_MAX 12
#define MQTT_FIELD_MAX 24
#define MQTT_DISCOVER_MAX 32

struct MqttItem {
    char topic[MQTT_TOPIC_MAX + 1];
    char jsonField[MQTT_FIELD_MAX + 1];  // empty = use raw payload
    char label[MQTT_LABEL_MAX + 1];
    char unit[MQTT_UNIT_MAX + 1];
    char value[MQTT_VALUE_MAX + 1];
    bool haveValue;
};

struct DiscoveredEntry {
    String topic;
    String field;  // empty = sub-topic entry; non-empty = JSON field entry
};

MqttItem mqttItems[MQTT_MAX_ITEMS];
int mqttItemCount = 0;

char mqttHost[64] = "";
int mqttPort = 1883;
char mqttUser[32] = "";
char mqttPass[32] = "";
int rotationInterval = 15000;

int rotationIdx = 0;          // 0 = HTTP outdoor temp, 1..mqttItemCount = MQTT items
unsigned long lastRotate = 0;

WiFiClient mqttWifi;
PubSubClient mqtt(mqttWifi);
Preferences prefs;
unsigned long lastMqttReconnect = 0;

// Discovery state
bool discoverActive = false;
char discoverPrefix[MQTT_TOPIC_MAX + 4] = "";  // "<prefix>/#"
char discoverPrefixBase[MQTT_TOPIC_MAX + 1] = "";
DiscoveredEntry discoveredTopics[MQTT_DISCOVER_MAX];
int discoveredCount = 0;

// Debug: ring buffer of last 10 received MQTT topics (for /mqtt/debug)
#define MQTT_DEBUG_BUF 10
String mqttRecent[MQTT_DEBUG_BUF];
int mqttRecentCount = 0;
int mqttRecentHead = 0;
unsigned long mqttTotalMsgs = 0;

// Forward decls
void saveConfig();
void loadConfig();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void mqttConnect();
void mqttResubscribe();

// Split-flap drum sequence (ordered flaps on the physical drum)
static const char DRUM[] = {
    ' ', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    '.', ':', '-', '+', 'k', 'W', 'h', 'o', 'C'
};
static const int DRUM_LEN = sizeof(DRUM) / sizeof(DRUM[0]);

// Find a character's position on the drum, or -1 if not on drum
int drumIndex(char ch) {
    for (int i = 0; i < DRUM_LEN; i++)
        if (DRUM[i] == ch) return i;
    return -1;
}

// Per-cell flap state
struct FlapCell {
    int drumPos;          // current position on the drum (index into DRUM)
    int targetPos;        // where we need to get to (-1 = not on drum)
    unsigned long stepStart; // millis() when current step began
    bool animating;       // is this cell currently rotating?
};

FlapCell sfCells[ROWS][COLS];
char sfTgt[ROWS][COLS + 1];   // what we want to display
uint16_t sfColor[ROWS];
unsigned long sfLastTick = 0;
bool sfFirstDraw = true;

// ─── Display init ────────────────────────────────────────────────
void initDisplay() {
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
        LAT_PIN, OE_PIN, CLK_PIN
    };
    HUB75_I2S_CFG cfg(PW, PH, 1);
    cfg.gpio = pins;
    cfg.clkphase = false;
    cfg.driver = HUB75_I2S_CFG::FM6126A;
    cfg.double_buff = true;

    dp = new MatrixPanel_I2S_DMA(cfg);
    dp->begin();
    dp->setBrightness8(60);
    dp->clearScreen();
    dp->flipDMABuffer();
    dp->clearScreen();

    cAmber = dp->color565(255, 200, 0);
    cGreen = dp->color565(0, 220, 60);
    cRed   = dp->color565(220, 40, 20);
    cWhite = dp->color565(200, 200, 200);
    cBg    = dp->color565(3, 3, 3);
    cCell  = dp->color565(12, 11, 8);
    cSep   = dp->color565(50, 40, 0);
    cFlap  = dp->color565(255, 240, 120);
}

// ─── Draw one character cell (normal, not animating) ─────────────
void drawCellNormal(int x, int y, char ch, uint16_t color) {
    // Cell background
    dp->fillRect(x, y, CW - 1, CH - 1, cCell);

    // Split line at y+4 (gap between flaps)
    dp->drawFastHLine(x, y + 4, CW - 1, cBg);

    // Character
    if (ch != ' ') {
        dp->setTextSize(1);
        dp->setTextColor(color);
        dp->setCursor(x, y + 1);
        dp->print(ch);
    }
}

// Glyph data for drum characters (5x7 Adafruit default font, 5 bytes per char)
// Each byte = one column, bit 0 = top row. Index by drumGlyphIndex().
// Characters: ' ','0','1','2','3','4','5','6','7','8','9','.',':','-','+','k','W','h','o','C'
static const uint8_t DRUM_GLYPHS[][5] PROGMEM = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x72, 0x49, 0x49, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x49, 0x4D, 0x33}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x31}, // '6'
    {0x41, 0x21, 0x11, 0x09, 0x07}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x46, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x00, 0x60, 0x60, 0x00}, // '.'
    {0x00, 0x00, 0x14, 0x00, 0x00}, // ':'
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
};

// Get glyph index for a drum character, or -1 if not found
int drumGlyphIndex(char ch) {
    for (int i = 0; i < DRUM_LEN; i++)
        if (DRUM[i] == ch) return i;
    return -1;
}

// Draw a single character clipped to a Y range using pixel-level rendering
void drawCharClipped(int x, int y, char ch, uint16_t color, int clipYmin, int clipYmax) {
    int gi = drumGlyphIndex(ch);
    if (gi < 0) return; // not a drum character or space
    if (gi == 0) return; // space — nothing to draw

    for (int col = 0; col < 5; col++) {
        uint8_t colData = pgm_read_byte(&DRUM_GLYPHS[gi][col]);
        for (int row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                int py = y + 1 + row;  // char drawn at y+1
                if (py >= clipYmin && py <= clipYmax) {
                    dp->drawPixel(x + col, py, color);
                }
            }
        }
    }
}

// Draw an animating cell — Phase 1: flap falling (first 30ms of step)
// Upper half: new char arriving, lower half: old char still visible
void drawCellPhase1(int x, int y, char oldCh, char newCh, uint16_t color) {
    uint16_t cDimAmber = dp->color565(180, 140, 0);
    int w = CW - 1;

    // Cell background
    dp->fillRect(x, y, w, CH - 1, cCell);

    // Draw NEW character clipped to upper half (y to y+3)
    drawCharClipped(x, y, newCh, color, y, y + 3);

    // Draw OLD character clipped to lower half (y+5 to y+8)
    drawCharClipped(x, y, oldCh, cDimAmber, y + 5, y + 8);

    // Bright highlight line at split position (flap edge catching light)
    dp->drawFastHLine(x, y + 4, w, cFlap);
}

// Draw an animating cell — Phase 2: flap settled (next 30ms of step)
// Shows the new intermediate character fully
void drawCellPhase2(int x, int y, char newCh, uint16_t color) {
    // Cell background
    dp->fillRect(x, y, CW - 1, CH - 1, cCell);

    // Split line (visible but no highlight)
    dp->drawFastHLine(x, y + 4, CW - 1, cBg);

    // Full character
    if (newCh != ' ') {
        dp->setTextSize(1);
        dp->setTextColor(color);
        dp->setCursor(x, y + 1);
        dp->print(newCh);
    }
}

// ─── Draw the complete split-flap screen ─────────────────────────
void drawSplitFlap() {
    dp->fillScreen(cBg);

    // Separator line between temp and solar
    dp->drawFastHLine(3, 35, 58, cSep);

    unsigned long now = millis();

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x = ROW_X + c * CW;
            int y = ROW_Y[r];
            FlapCell& cell = sfCells[r][c];

            if (!cell.animating) {
                // Static cell: if char is on drum, show drum position; otherwise show target directly
                char tch = sfTgt[r][c];
                char ch = (drumIndex(tch) >= 0) ? DRUM[cell.drumPos] : tch;
                drawCellNormal(x, y, ch, sfColor[r]);
            } else {
                // Animating cell: determine phase within current step
                unsigned long elapsed = now - cell.stepStart;
                int nextPos = (cell.drumPos + 1) % DRUM_LEN;
                char oldCh = DRUM[cell.drumPos];
                char newCh = DRUM[nextPos];

                if (elapsed < 30) {
                    // Phase 1: flap falling
                    drawCellPhase1(x, y, oldCh, newCh, sfColor[r]);
                } else {
                    // Phase 2: flap settled, showing intermediate char
                    drawCellPhase2(x, y, newCh, sfColor[r]);
                }
            }
        }
    }

    dp->flipDMABuffer();
}

// Format a raw value string into a 4-char right-aligned field.
// Rules: numeric tries 1 decimal; if too big, show integer; otherwise copy as-is.
// If empty/invalid → "  --"
void formatRowValue(const char* raw, char out[5]) {
    if (!raw || !*raw) { strcpy(out, "  --"); return; }
    // Try to parse as float
    char* endp = nullptr;
    double v = strtod(raw, &endp);
    bool isNum = (endp && endp != raw);
    // Skip trailing whitespace
    if (isNum) {
        while (*endp == ' ' || *endp == '\t' || *endp == '\r' || *endp == '\n') endp++;
        if (*endp != 0) isNum = false;
    }
    if (!isNum) {
        // Copy up to 4 chars, right-align
        char tmp[5] = "    ";
        int n = strlen(raw);
        if (n > 4) n = 4;
        for (int i = 0; i < n; i++) tmp[4 - n + i] = raw[i];
        tmp[4] = 0;
        memcpy(out, tmp, 5);
        return;
    }
    // Numeric: decide format
    double av = v < 0 ? -v : v;
    char buf[16];
    if (av < 10.0) {
        // e.g. "12.3" or " 1.2" or "-1.2"
        snprintf(buf, sizeof(buf), "%.1f", v);
    } else if (av < 100.0) {
        snprintf(buf, sizeof(buf), "%.1f", v);
    } else {
        snprintf(buf, sizeof(buf), "%d", (int)(v >= 0 ? v + 0.5 : v - 0.5));
    }
    int n = strlen(buf);
    if (n > 4) {
        // Too wide even as integer — try integer w/o decimals
        snprintf(buf, sizeof(buf), "%d", (int)(v >= 0 ? v + 0.5 : v - 0.5));
        n = strlen(buf);
        if (n > 4) {
            // Saturate
            strcpy(buf, v >= 0 ? "++++" : "----");
            n = 4;
        }
    }
    char tmp[5] = "    ";
    for (int i = 0; i < n; i++) tmp[4 - n + i] = buf[i];
    tmp[4] = 0;
    memcpy(out, tmp, 5);
}

// Build the 10-char row string: "LABL VVVVU"
void buildRotationRow(const char* label, const char* value, const char* unit, char out[COLS + 1]) {
    char lab[5] = "    ";
    int ln = strlen(label); if (ln > 4) ln = 4;
    for (int i = 0; i < ln; i++) lab[i] = label[i];
    lab[4] = 0;

    char val[5];
    formatRowValue(value, val);

    char un[2] = " ";
    if (unit && *unit) un[0] = unit[0];
    un[1] = 0;

    snprintf(out, COLS + 1, "%s %s%s", lab, val, un);
    // Ensure exactly 10 chars, space-padded
    int n = strlen(out);
    while (n < COLS) { out[n++] = ' '; }
    out[COLS] = 0;
}

// ─── Update target strings ───────────────────────────────────────
void updateTargets() {
    struct tm ti;
    char buf[COLS + 1];

    // Row 0: Date
    if (getLocalTime(&ti, 10))
        snprintf(buf, sizeof(buf), "%02d.%02d.%04d", ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    else
        snprintf(buf, sizeof(buf), "--.--.----");
    snprintf(sfTgt[0], COLS + 1, "%-*s", COLS, buf);
    sfColor[0] = cAmber;

    // Row 1: Time
    if (getLocalTime(&ti, 10))
        snprintf(buf, sizeof(buf), " %02d:%02d:%02d ", ti.tm_hour, ti.tm_min, ti.tm_sec);
    else
        snprintf(buf, sizeof(buf), " --:--:-- ");
    snprintf(sfTgt[1], COLS + 1, "%-*s", COLS, buf);
    sfColor[1] = cAmber;

    // Row 2: Rotating values (outdoor temp + MQTT items)
    unsigned long nowMs = millis();
    int totalItems = 1 + mqttItemCount;  // idx 0 = HTTP temp
    if (lastRotate == 0) lastRotate = nowMs;
    if (nowMs - lastRotate >= (unsigned long)rotationInterval) {
        lastRotate = nowMs;
        rotationIdx = (rotationIdx + 1) % totalItems;
    }
    if (rotationIdx >= totalItems) rotationIdx = 0;

    if (rotationIdx == 0) {
        // Outdoor temp from HTTP
        char vbuf[12];
        if (dValid && dTemp > -900) snprintf(vbuf, sizeof(vbuf), "%.1f", dTemp);
        else vbuf[0] = 0;
        buildRotationRow("OUT", vbuf, "C", sfTgt[2]);
    } else {
        const MqttItem& it = mqttItems[rotationIdx - 1];
        buildRotationRow(it.label, it.haveValue ? it.value : "", it.unit, sfTgt[2]);
    }
    sfColor[2] = cAmber;

    // Row 3: Solar today kWh
    if (dValid && dSolarKwh >= 0)
        snprintf(buf, sizeof(buf), "%5.1f kWh ", dSolarKwh);
    else
        snprintf(buf, sizeof(buf), " --.- kWh ");
    snprintf(sfTgt[3], COLS + 1, "%-*s", COLS, buf);
    sfColor[3] = cAmber;

    // Row 4: Current watts
    if (dValid) {
        if (dSolarW >= 0)
            snprintf(buf, sizeof(buf), " +%4d W  ", dSolarW);
        else
            snprintf(buf, sizeof(buf), " %5d W  ", dSolarW);
        sfColor[4] = (dSolarW > 0) ? cGreen : (dSolarW < 0) ? cRed : cWhite;
    } else {
        snprintf(buf, sizeof(buf), " ---- W   ");
        sfColor[4] = cWhite;
    }
    snprintf(sfTgt[4], COLS + 1, "%-*s", COLS, buf);
}

// ─── Split-flap animation tick (called at ~30fps) ────────────────
void tickSplitFlap() {
    unsigned long now = millis();

    updateTargets();

    // First draw: snap all cells to target positions immediately, no animation
    if (sfFirstDraw) {
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                char tch = sfTgt[r][c];
                int di = drumIndex(tch);
                sfCells[r][c].drumPos = (di >= 0) ? di : 0;
                sfCells[r][c].targetPos = -1;
                sfCells[r][c].stepStart = 0;
                sfCells[r][c].animating = false;
            }
        }
        sfFirstDraw = false;
        drawSplitFlap();
        return;
    }

    // For each cell, check if target character differs from current drum character
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            FlapCell& cell = sfCells[r][c];
            char tch = sfTgt[r][c];
            int tdi = drumIndex(tch);

            if (tdi < 0) {
                // Character not on drum — snap instantly
                cell.animating = false;
                cell.targetPos = -1;
                // drumPos stays as-is; we'll draw tch directly via sfTgt
                // Actually set drumPos to 0 (space) as fallback
                continue;
            }

            // If already animating toward this target, leave it alone
            if (cell.animating && cell.targetPos == tdi) continue;

            // If current drum position already shows the target character
            if (!cell.animating && cell.drumPos == tdi) continue;

            // Need to start or retarget animation
            if (!cell.animating) {
                cell.targetPos = tdi;
                cell.animating = true;
                cell.stepStart = now;
            } else {
                // Retarget (drum keeps spinning forward to new target)
                cell.targetPos = tdi;
            }
        }
    }

    // Advance animating cells: each step = 60ms
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            FlapCell& cell = sfCells[r][c];
            if (!cell.animating) continue;

            unsigned long elapsed = now - cell.stepStart;
            if (elapsed >= 60) {
                // Advance drum by one position (wrapping)
                cell.drumPos = (cell.drumPos + 1) % DRUM_LEN;
                cell.stepStart = now;

                // Check if we've reached the target
                if (cell.drumPos == cell.targetPos) {
                    cell.animating = false;
                }
            }
        }
    }

    // Full screen redraw every frame
    drawSplitFlap();
}

// ─── Data fetch (direct InfluxDB) ────────────────────────────────
// URL-encode only the bare minimum (spaces, quotes, =, %, etc.)
static String urlEncode(const String& s) {
    String o;
    o.reserve(s.length() * 2);
    const char* hex = "0123456789ABCDEF";
    for (unsigned i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// Extract the numeric value from series[0].values[last][1] of a single result
static bool influxLastVal(JsonVariant result, float& out) {
    JsonArray series = result["series"];
    if (series.isNull() || series.size() == 0) return false;
    JsonArray values = series[0]["values"];
    if (values.isNull() || values.size() == 0) return false;
    JsonArray lastRow = values[values.size() - 1];
    if (lastRow.isNull() || lastRow.size() < 2) return false;
    JsonVariant v = lastRow[1];
    if (v.isNull()) return false;
    out = v.as<float>();
    return true;
}

void fetchData() {
    HTTPClient http;
    http.setTimeout(5000);

    String url = "http://" + String(INFLUX_HOST) + ":" + String(INFLUX_PORT)
               + "/query?db=" + urlEncode(INFLUX_DB)
               + "&q=" + urlEncode(INFLUX_MULTI_Q);

    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[INFLUX] HTTP %d\n", code);
        http.end();
        return;
    }

    // Parse the multi-result JSON response
    // { "results": [ {statement_id:0, series:[{values:[[...,val]]}] }, ... ] }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[INFLUX] JSON parse error: %s\n", err.c_str());
        return;
    }

    JsonArray results = doc["results"];
    if (results.isNull() || results.size() < 4) {
        Serial.println("[INFLUX] unexpected response structure");
        return;
    }

    float vTemp, vSolax, vEco, vGrid;
    bool okTemp  = influxLastVal(results[0], vTemp);
    bool okSolax = influxLastVal(results[1], vSolax);
    bool okEco   = influxLastVal(results[2], vEco);
    bool okGrid  = influxLastVal(results[3], vGrid);

    if (okTemp) dTemp = vTemp;
    if (okSolax || okEco) {
        dSolarKwh = (okSolax ? vSolax : 0) + (okEco ? vEco : 0);
    }
    if (okGrid) {
        // Flip sign: positive = exporting = producing (green)
        dSolarW = (int)(-vGrid);
    }
    dValid = (okTemp || okSolax || okEco || okGrid);

    Serial.printf("[INFLUX] temp=%.1f solar=%.1f watts=%d (t=%d s=%d e=%d g=%d)\n",
                  dTemp, dSolarKwh, dSolarW, okTemp, okSolax, okEco, okGrid);
}

// ─── UDP frame handling ──────────────────────────────────────────
bool allRx() {
    if (rxTotal == 0) return false;
    uint32_t m = (rxTotal >= 32) ? 0xFFFFFFFF : ((1U << rxTotal) - 1);
    return (rxChunks & m) == m;
}

void showUdpFrame() {
    for (int y = 0; y < PH; y++) {
        int ro = y * PW * 3;
        for (int x = 0; x < PW; x++) {
            int i = ro + x * 3;
            dp->drawPixelRGB888(x, y, frameBuf[i], frameBuf[i+1], frameBuf[i+2]);
        }
    }
    dp->flipDMABuffer();
}

// ─── Web handlers ────────────────────────────────────────────────
void handleRoot() {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Panel64</title>"
               "<style>body{font:14px monospace;background:#111;color:#fa0;padding:20px}"
               "a{color:#0af}h1{color:#fc0}</style></head><body><h1>64x64 Panel</h1><table>";
    h += "<tr><td>Mode</td><td>" + String(udpMode ? "UDP Video" : "Split-Flap") + "</td></tr>";
    h += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    h += "<tr><td>UDP</td><td>:" + String(UDP_PORT) + "</td></tr>";
    h += "<tr><td>Frames</td><td>" + String(frameCnt) + "</td></tr>";
    h += "<tr><td>FPS</td><td>" + String(fps, 1) + "</td></tr>";
    h += "<tr><td>Heap</td><td>" + String(ESP.getFreeHeap()) + "</td></tr>";
    if (dValid) {
        h += "<tr><td>Temp</td><td>" + String(dTemp, 1) + " C</td></tr>";
        h += "<tr><td>Solar</td><td>" + String(dSolarKwh, 1) + " kWh / " + String(dSolarW) + " W</td></tr>";
    }
    h += "<tr><td>LDR raw</td><td>" + String(ldrSmoothed) + "</td></tr>";
    h += "<tr><td>Brightness</td><td>" + String(currentBrightness) + (autoBrightness ? " (auto)" : " (manual)") + "</td></tr>";
    h += "<tr><td>MQTT</td><td>" + String(mqtt.connected() ? "connected" : (mqttHost[0] ? "disconnected" : "not configured")) + "</td></tr>";
    h += "<tr><td>Rotate</td><td>idx " + String(rotationIdx) + " / " + String(1 + mqttItemCount) + "</td></tr>";
    h += "</table><p><a href='/mqtt'>MQTT Config</a> | <a href='/update'>OTA Update</a></p>";
    h += "<p>Brightness: <a href='/b?v=auto'>Auto</a> | "
         "<a href='/b?v=30'>30</a> <a href='/b?v=60'>60</a> "
         "<a href='/b?v=90'>90</a> <a href='/b?v=150'>150</a></p></body></html>";
    server.send(200, "text/html", h);
}

void handleB() {
    if (server.hasArg("v")) {
        String v = server.arg("v");
        if (v == "auto") {
            autoBrightness = true;
            server.send(200, "text/plain", "Auto brightness enabled");
        } else {
            autoBrightness = false;
            currentBrightness = constrain(v.toInt(), 0, 255);
            dp->setBrightness8(currentBrightness);
            server.send(200, "text/plain", "Manual brightness: " + String(currentBrightness));
        }
    }
}

// ─── Config persistence ─────────────────────────────────────────
void loadConfig() {
    prefs.begin("panel64", true);
    String cfg = prefs.getString("mqtt_cfg", "");
    prefs.end();

    // Defaults
    mqttHost[0] = 0;
    mqttPort = 1883;
    mqttUser[0] = 0;
    mqttPass[0] = 0;
    rotationInterval = 15000;
    mqttItemCount = 0;

    if (cfg.length() == 0) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, cfg);
    if (err) {
        Serial.printf("Config parse err: %s\n", err.c_str());
        return;
    }
    strlcpy(mqttHost, doc["host"] | "", sizeof(mqttHost));
    mqttPort = doc["port"] | 1883;
    strlcpy(mqttUser, doc["user"] | "", sizeof(mqttUser));
    strlcpy(mqttPass, doc["pass"] | "", sizeof(mqttPass));
    rotationInterval = doc["interval"] | 15000;
    if (rotationInterval < 1000) rotationInterval = 1000;

    JsonArray items = doc["items"].as<JsonArray>();
    mqttItemCount = 0;
    for (JsonObject it : items) {
        if (mqttItemCount >= MQTT_MAX_ITEMS) break;
        MqttItem& m = mqttItems[mqttItemCount];
        strlcpy(m.topic, it["topic"] | "", sizeof(m.topic));
        strlcpy(m.jsonField, it["field"] | "", sizeof(m.jsonField));
        strlcpy(m.label, it["label"] | "", sizeof(m.label));
        strlcpy(m.unit, it["unit"] | "", sizeof(m.unit));
        m.value[0] = 0;
        m.haveValue = false;
        if (m.topic[0]) mqttItemCount++;
    }
    Serial.printf("Loaded config: host=%s port=%d items=%d\n", mqttHost, mqttPort, mqttItemCount);
}

void saveConfig() {
    JsonDocument doc;
    doc["host"] = mqttHost;
    doc["port"] = mqttPort;
    doc["user"] = mqttUser;
    doc["pass"] = mqttPass;
    doc["interval"] = rotationInterval;
    JsonArray items = doc["items"].to<JsonArray>();
    for (int i = 0; i < mqttItemCount; i++) {
        JsonObject o = items.add<JsonObject>();
        o["topic"] = mqttItems[i].topic;
        if (mqttItems[i].jsonField[0]) o["field"] = mqttItems[i].jsonField;
        o["label"] = mqttItems[i].label;
        o["unit"] = mqttItems[i].unit;
    }
    String out;
    serializeJson(doc, out);
    prefs.begin("panel64", false);
    prefs.putString("mqtt_cfg", out);
    prefs.end();
    Serial.printf("Saved config (%d bytes)\n", out.length());
}

// ─── MQTT ───────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    // Debug tracking
    mqttTotalMsgs++;
    mqttRecent[mqttRecentHead] = String(topic) + " (" + String(len) + "b)";
    mqttRecentHead = (mqttRecentHead + 1) % MQTT_DEBUG_BUF;
    if (mqttRecentCount < MQTT_DEBUG_BUF) mqttRecentCount++;

    // Copy payload to a null-terminated buffer large enough for zigbee2mqtt JSON
    static char pfull[2048];
    unsigned int pn = len;
    if (pn >= sizeof(pfull)) pn = sizeof(pfull) - 1;
    memcpy(pfull, payload, pn);
    pfull[pn] = 0;

    // Raw-value short buffer (for items without jsonField)
    char pbuf[MQTT_VALUE_MAX + 1];
    unsigned int n = len;
    if (n > MQTT_VALUE_MAX) n = MQTT_VALUE_MAX;
    memcpy(pbuf, payload, n);
    pbuf[n] = 0;

    // Pre-parse JSON once per message if any matching item uses a jsonField
    bool jsonParsed = false;
    bool jsonOk = false;
    JsonDocument jdoc;
    auto ensureJson = [&]() {
        if (jsonParsed) return;
        jsonParsed = true;
        DeserializationError err = deserializeJson(jdoc, pfull);
        jsonOk = (!err && jdoc.is<JsonObject>());
    };

    // Update matching items
    for (int i = 0; i < mqttItemCount; i++) {
        if (strcmp(mqttItems[i].topic, topic) != 0) continue;
        MqttItem& m = mqttItems[i];
        if (m.jsonField[0] == 0) {
            // Raw payload
            strlcpy(m.value, pbuf, sizeof(m.value));
            m.haveValue = true;
        } else {
            ensureJson();
            if (!jsonOk) {
                strlcpy(m.value, "--", sizeof(m.value));
                m.haveValue = true;
                continue;
            }
            JsonVariant v = jdoc[m.jsonField];
            if (v.isNull()) {
                strlcpy(m.value, "--", sizeof(m.value));
                m.haveValue = true;
                continue;
            }
            if (v.is<float>() || v.is<double>()) {
                char b[16];
                snprintf(b, sizeof(b), "%.1f", v.as<float>());
                strlcpy(m.value, b, sizeof(m.value));
            } else if (v.is<int>() || v.is<long>()) {
                // Integer field: format bare integer (no forced decimal)
                char b[16];
                snprintf(b, sizeof(b), "%d", v.as<int>());
                strlcpy(m.value, b, sizeof(m.value));
            } else if (v.is<bool>()) {
                strlcpy(m.value, v.as<bool>() ? "1" : "0", sizeof(m.value));
            } else if (v.is<const char*>()) {
                strlcpy(m.value, v.as<const char*>(), sizeof(m.value));
            } else {
                // Fallback: serialize
                char b[16];
                serializeJson(v, b, sizeof(b));
                strlcpy(m.value, b, sizeof(m.value));
            }
            m.haveValue = true;
        }
    }

    // Discovery
    if (discoverActive && discoveredCount < MQTT_DISCOVER_MAX) {
        size_t plen = strlen(discoverPrefixBase);
        bool isExactPrefix = (strcmp(topic, discoverPrefixBase) == 0);
        bool isSubTopic = (!isExactPrefix && strncmp(topic, discoverPrefixBase, plen) == 0
                           && topic[plen] == '/');

        if (isSubTopic) {
            // Dedup on topic (no field)
            bool found = false;
            for (int i = 0; i < discoveredCount; i++) {
                if (discoveredTopics[i].field.length() == 0 &&
                    discoveredTopics[i].topic == topic) { found = true; break; }
            }
            if (!found && discoveredCount < MQTT_DISCOVER_MAX) {
                discoveredTopics[discoveredCount].topic = String(topic);
                discoveredTopics[discoveredCount].field = "";
                discoveredCount++;
            }
        }

        if (isExactPrefix) {
            // Try to parse as JSON object and enumerate top-level keys
            ensureJson();
            if (jsonOk) {
                JsonObject obj = jdoc.as<JsonObject>();
                for (JsonPair kv : obj) {
                    if (discoveredCount >= MQTT_DISCOVER_MAX) break;
                    const char* key = kv.key().c_str();
                    // Dedup on topic+field
                    bool found = false;
                    for (int i = 0; i < discoveredCount; i++) {
                        if (discoveredTopics[i].topic == topic &&
                            discoveredTopics[i].field == key) { found = true; break; }
                    }
                    if (!found) {
                        discoveredTopics[discoveredCount].topic = String(topic);
                        discoveredTopics[discoveredCount].field = String(key);
                        discoveredCount++;
                    }
                }
            }
        }
    }
}

void mqttResubscribe() {
    if (!mqtt.connected()) return;
    for (int i = 0; i < mqttItemCount; i++) {
        if (mqttItems[i].topic[0]) mqtt.subscribe(mqttItems[i].topic);
    }
}

void mqttConnect() {
    if (mqttHost[0] == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (mqtt.connected()) return;

    mqtt.setServer(mqttHost, mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(2048);

    String cid = "panel64-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok;
    if (mqttUser[0])
        ok = mqtt.connect(cid.c_str(), mqttUser, mqttPass);
    else
        ok = mqtt.connect(cid.c_str());

    if (ok) {
        Serial.println("MQTT connected");
        mqttResubscribe();
    } else {
        Serial.printf("MQTT connect failed, rc=%d\n", mqtt.state());
    }
}

// ─── Web: MQTT config page ──────────────────────────────────────
static String htmlEscape(const String& s) {
    String o;
    o.reserve(s.length());
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

void handleMqttPage() {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>MQTT Config</title>"
               "<style>body{font:14px monospace;background:#111;color:#fa0;padding:20px;max-width:780px}"
               "a{color:#0af}h1,h2{color:#fc0}"
               "input,select,button{font:14px monospace;background:#222;color:#fa0;border:1px solid #555;padding:4px}"
               "table{border-collapse:collapse}td,th{padding:4px 8px;border-bottom:1px solid #333;text-align:left}"
               ".box{border:1px solid #444;padding:12px;margin:10px 0}"
               "#disc{color:#f80}</style></head><body>";
    h += "<h1>MQTT Config</h1><p><a href='/'>&larr; back</a></p>";

    h += "<div class='box'><h2>Broker</h2>"
         "<form action='/mqtt/save' method='get'>"
         "Host: <input name='host' value='" + htmlEscape(mqttHost) + "' size='30'><br>"
         "Port: <input name='port' value='" + String(mqttPort) + "' size='6'><br>"
         "User: <input name='user' value='" + htmlEscape(mqttUser) + "' size='20'><br>"
         "Pass: <input name='pass' type='password' value='" + htmlEscape(mqttPass) + "' size='20'><br>"
         "Rotate interval (s): <input name='interval' value='" + String(rotationInterval / 1000) + "' size='5'><br>"
         "<button type='submit'>Save &amp; reconnect</button></form></div>";

    h += "<div class='box'><h2>Items (" + String(mqttItemCount) + "/" + String(MQTT_MAX_ITEMS) + ")</h2>"
         "<table><tr><th>#</th><th>Label</th><th>Unit</th><th>Topic / field</th><th>Value</th><th></th></tr>";
    h += "<tr><td>0</td><td>OUT</td><td>C</td><td><i>(HTTP outdoor temp)</i></td><td>";
    if (dValid && dTemp > -900) h += String(dTemp, 1); else h += "--";
    h += "</td><td></td></tr>";
    for (int i = 0; i < mqttItemCount; i++) {
        String tcol = htmlEscape(mqttItems[i].topic);
        if (mqttItems[i].jsonField[0]) {
            tcol += " &rarr; " + htmlEscape(mqttItems[i].jsonField);
        }
        h += "<tr><td>" + String(i + 1) + "</td><td>" + htmlEscape(mqttItems[i].label) +
             "</td><td>" + htmlEscape(mqttItems[i].unit) + "</td><td>" + tcol +
             "</td><td>" + htmlEscape(mqttItems[i].haveValue ? mqttItems[i].value : "--") + "</td>"
             "<td><a href='/mqtt/del?idx=" + String(i) + "'>del</a></td></tr>";
    }
    h += "</table></div>";

    h += "<div class='box'><h2>Add item</h2>"
         "<p>Background sniffing: enter a prefix, click <b>Start</b>, and leave it running. "
         "The ESP32 subscribes to <code>&lt;prefix&gt;/#</code> and <code>&lt;prefix&gt;</code> and "
         "collects everything it sees — including sub-topics and JSON fields from leaf payloads. "
         "Battery sensors may take minutes to publish, so just wait or trigger them physically. "
         "The list auto-refreshes every 2 seconds. When you see what you want, pick it from the "
         "dropdown and add it below.</p>"
         "Prefix: <input id='pfx' value='panasonic_heat_pump/main' size='40'> "
         "<button onclick='sniffStart()'>Start</button> "
         "<button onclick='sniffStop()'>Stop</button> "
         "<button onclick='sniffClear()'>Clear</button> "
         "<span id='disc'>idle</span><br><br>"
         "<form action='/mqtt/add' method='get'>"
         "<select id='sel' onchange='applySel(this)' style='width:100%'>"
         "<option value=''>-- 0 entries --</option></select><br>"
         "Topic: <input id='topic' name='topic' size='50'><br>"
         "JSON field (optional): <input id='field' name='field' maxlength='24' size='24'><br>"
         "Label (4): <input name='label' maxlength='4' size='6'> "
         "Unit (1-2): <input name='unit' maxlength='2' size='4'><br>"
         "<button type='submit'>Add</button></form></div>";

    h += "<script>"
         "function applySel(sel){"
         "var opt=sel.options[sel.selectedIndex];if(!opt||!opt.value)return;"
         "document.getElementById('topic').value=opt.dataset.topic||'';"
         "document.getElementById('field').value=opt.dataset.field||'';"
         "}"
         "function renderList(j){"
         "var s=document.getElementById('sel');"
         "var cur=s.value;"
         "var ents=j.entries||[];"
         "var html='<option value=\"\">-- '+ents.length+' entries --</option>';"
         "ents.forEach(e=>{"
         "if(e.type==='json'){"
         "var v=e.name+'|'+e.field;"
         "html+='<option value=\"'+v+'\" data-topic=\"'+e.name+'\" data-field=\"'+e.field+'\">'"
         "+e.name+' \\u2192 '+e.field+'</option>';}"
         "else{html+='<option value=\"'+e.name+'\" data-topic=\"'+e.name+'\" data-field=\"\">'"
         "+e.name+'</option>';}});"
         "s.innerHTML=html;"
         "if(cur){s.value=cur;}"
         "var d=document.getElementById('disc');"
         "d.textContent=(j.active?'sniffing \"'+j.prefix+'\" ':'stopped ')+'('+ents.length+' found)';"
         "}"
         "function sniffStart(){"
         "var p=document.getElementById('pfx').value;"
         "fetch('/mqtt/sniff/start?prefix='+encodeURIComponent(p)).then(r=>r.json()).then(renderList);"
         "}"
         "function sniffStop(){fetch('/mqtt/sniff/stop').then(r=>r.json()).then(renderList);}"
         "function sniffClear(){fetch('/mqtt/sniff/clear').then(r=>r.json()).then(renderList);}"
         "function refresh(){fetch('/mqtt/sniff/list').then(r=>r.json()).then(renderList).catch(()=>{});}"
         "refresh();setInterval(refresh,2000);"
         "</script>";
    h += "<p>MQTT status: " + String(mqtt.connected() ? "connected" : "disconnected") + "</p>";
    h += "</body></html>";
    server.send(200, "text/html", h);
}

void handleMqttSave() {
    if (server.hasArg("host")) strlcpy(mqttHost, server.arg("host").c_str(), sizeof(mqttHost));
    if (server.hasArg("port")) mqttPort = server.arg("port").toInt();
    if (mqttPort <= 0) mqttPort = 1883;
    if (server.hasArg("user")) strlcpy(mqttUser, server.arg("user").c_str(), sizeof(mqttUser));
    if (server.hasArg("pass")) strlcpy(mqttPass, server.arg("pass").c_str(), sizeof(mqttPass));
    if (server.hasArg("interval")) {
        int s = server.arg("interval").toInt();
        if (s < 1) s = 1;
        rotationInterval = s * 1000;
    }
    saveConfig();
    if (mqtt.connected()) mqtt.disconnect();
    mqttConnect();
    server.sendHeader("Location", "/mqtt");
    server.send(302, "text/plain", "");
}

void handleMqttAdd() {
    if (mqttItemCount >= MQTT_MAX_ITEMS) {
        server.send(400, "text/plain", "Max items reached");
        return;
    }
    String topic = server.arg("topic");
    String field = server.arg("field");
    String label = server.arg("label");
    String unit = server.arg("unit");
    if (topic.length() == 0 || label.length() == 0) {
        server.send(400, "text/plain", "topic and label required");
        return;
    }
    MqttItem& m = mqttItems[mqttItemCount];
    strlcpy(m.topic, topic.c_str(), sizeof(m.topic));
    strlcpy(m.jsonField, field.c_str(), sizeof(m.jsonField));
    strlcpy(m.label, label.c_str(), sizeof(m.label));
    strlcpy(m.unit, unit.c_str(), sizeof(m.unit));
    m.value[0] = 0;
    m.haveValue = false;
    mqttItemCount++;
    saveConfig();
    if (mqtt.connected()) mqtt.subscribe(m.topic);
    server.sendHeader("Location", "/mqtt");
    server.send(302, "text/plain", "");
}

void handleMqttDel() {
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= mqttItemCount) {
        server.send(400, "text/plain", "bad idx");
        return;
    }
    char delTopic[MQTT_TOPIC_MAX + 1];
    strlcpy(delTopic, mqttItems[idx].topic, sizeof(delTopic));
    for (int i = idx; i < mqttItemCount - 1; i++) mqttItems[i] = mqttItems[i + 1];
    mqttItemCount--;
    // Only unsubscribe if no other item still uses this topic
    if (mqtt.connected()) {
        bool stillUsed = false;
        for (int i = 0; i < mqttItemCount; i++) {
            if (strcmp(mqttItems[i].topic, delTopic) == 0) { stillUsed = true; break; }
        }
        if (!stillUsed) mqtt.unsubscribe(delTopic);
    }
    saveConfig();
    server.sendHeader("Location", "/mqtt");
    server.send(302, "text/plain", "");
}

// Helper: escape string for JSON
static String jsonEsc(const String& s) {
    String o;
    o.reserve(s.length() + 2);
    for (unsigned j = 0; j < s.length(); j++) {
        char c = s[j];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

// Build JSON array of discovered entries
static String buildDiscoverJson() {
    String out = "{\"active\":";
    out += discoverActive ? "true" : "false";
    out += ",\"prefix\":\"";
    out += jsonEsc(discoverPrefixBase);
    out += "\",\"entries\":[";
    for (int i = 0; i < discoveredCount; i++) {
        if (i) out += ",";
        const DiscoveredEntry& e = discoveredTopics[i];
        if (e.field.length() == 0) {
            out += "{\"type\":\"topic\",\"name\":\"" + jsonEsc(e.topic) + "\"}";
        } else {
            out += "{\"type\":\"json\",\"name\":\"" + jsonEsc(e.topic) +
                   "\",\"field\":\"" + jsonEsc(e.field) + "\"}";
        }
    }
    out += "]}";
    return out;
}

// Unsubscribe from a topic ONLY if no configured MQTT item still uses it.
// This prevents the sniffer from clobbering subscriptions needed by live items.
static void safeUnsubscribe(const char* topic) {
    if (!topic || !topic[0]) return;
    for (int i = 0; i < mqttItemCount; i++) {
        if (strcmp(mqttItems[i].topic, topic) == 0) return; // still needed
    }
    mqtt.unsubscribe(topic);
}

// Start non-blocking sniffing: subscribe to <prefix>/# and <prefix>
// Messages are collected in the background via mqttCallback.
void handleMqttSniffStart() {
    if (!mqtt.connected()) {
        server.send(503, "application/json", "{\"error\":\"mqtt not connected\"}");
        return;
    }
    String prefix = server.arg("prefix");
    if (prefix.length() == 0 || prefix.length() > MQTT_TOPIC_MAX) {
        server.send(400, "application/json", "{\"error\":\"invalid prefix\"}");
        return;
    }

    // If already sniffing a different prefix, stop that first (safely)
    if (discoverActive) {
        safeUnsubscribe(discoverPrefix);
        safeUnsubscribe(discoverPrefixBase);
        discoverActive = false;
    }

    // Clear previous list
    discoveredCount = 0;
    for (int i = 0; i < MQTT_DISCOVER_MAX; i++) {
        discoveredTopics[i].topic = "";
        discoveredTopics[i].field = "";
    }

    strlcpy(discoverPrefixBase, prefix.c_str(), sizeof(discoverPrefixBase));
    snprintf(discoverPrefix, sizeof(discoverPrefix), "%s/#", discoverPrefixBase);
    discoverActive = true;
    mqtt.subscribe(discoverPrefix);
    mqtt.subscribe(discoverPrefixBase);

    server.send(200, "application/json", buildDiscoverJson());
}

// Stop sniffing (unsubscribe safely), but keep the discovered list
void handleMqttSniffStop() {
    if (discoverActive) {
        safeUnsubscribe(discoverPrefix);
        safeUnsubscribe(discoverPrefixBase);
        discoverActive = false;
    }
    // Always re-subscribe to all configured items — defensive repair in case
    // any previous unsubscribe accidentally dropped them
    if (mqtt.connected()) {
        for (int i = 0; i < mqttItemCount; i++) {
            mqtt.subscribe(mqttItems[i].topic);
        }
    }
    server.send(200, "application/json", buildDiscoverJson());
}

// Return current discovered list (for polling from the UI)
void handleMqttSniffList() {
    server.send(200, "application/json", buildDiscoverJson());
}

// Clear the discovered list without stopping the subscription
void handleMqttSniffClear() {
    discoveredCount = 0;
    for (int i = 0; i < MQTT_DISCOVER_MAX; i++) {
        discoveredTopics[i].topic = "";
        discoveredTopics[i].field = "";
    }
    server.send(200, "application/json", buildDiscoverJson());
}

// Backward-compat: /mqtt/discover acts as "start sniffing, return immediately"
void handleMqttDiscover() {
    handleMqttSniffStart();
}

// Diagnostic info: subscriptions, last received topics, item state
void handleMqttDebug() {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>MQTT Debug</title>"
               "<style>body{font:12px monospace;background:#111;color:#fa0;padding:20px}"
               "table{border-collapse:collapse;margin-bottom:20px}td,th{padding:3px 8px;border:1px solid #444}"
               "a{color:#0af}h1,h2{color:#fc0}.ok{color:#0f0}.bad{color:#f44}</style></head><body>";
    h += "<h1>MQTT Debug</h1>";

    h += "<p>Connected: <span class='" + String(mqtt.connected() ? "ok'>YES" : "bad'>NO") + "</span></p>";
    h += "<p>Broker: " + String(mqttHost) + ":" + String(mqttPort) + "</p>";
    h += "<p>Total messages received: " + String(mqttTotalMsgs) + "</p>";
    h += "<p>Buffer size: 2048 bytes</p>";

    h += "<h2>Configured items (subscribed on connect)</h2>";
    h += "<table><tr><th>#</th><th>Topic</th><th>jsonField</th><th>Label</th><th>Unit</th><th>Value</th></tr>";
    for (int i = 0; i < mqttItemCount; i++) {
        h += "<tr><td>" + String(i) + "</td>"
             "<td>" + htmlEscape(mqttItems[i].topic) + "</td>"
             "<td>" + htmlEscape(mqttItems[i].jsonField) + "</td>"
             "<td>" + htmlEscape(mqttItems[i].label) + "</td>"
             "<td>" + htmlEscape(mqttItems[i].unit) + "</td>"
             "<td>" + (mqttItems[i].haveValue ? htmlEscape(mqttItems[i].value) : "<i>no value</i>") + "</td></tr>";
    }
    h += "</table>";

    h += "<h2>Last " + String(MQTT_DEBUG_BUF) + " received topics</h2>";
    h += "<table><tr><th>Topic (size)</th></tr>";
    int n = mqttRecentCount;
    int idx = (mqttRecentHead - 1 + MQTT_DEBUG_BUF) % MQTT_DEBUG_BUF;
    for (int i = 0; i < n; i++) {
        h += "<tr><td>" + htmlEscape(mqttRecent[idx]) + "</td></tr>";
        idx = (idx - 1 + MQTT_DEBUG_BUF) % MQTT_DEBUG_BUF;
    }
    h += "</table>";

    h += "<h2>Actions</h2>"
         "<p><a href='/mqtt/debug/resub'>Force resubscribe all items</a></p>"
         "<p><a href='/mqtt/debug/reconnect'>Force disconnect + reconnect</a></p>"
         "<p><a href='/mqtt'>Back to MQTT config</a></p>";
    h += "</body></html>";
    server.send(200, "text/html", h);
}

void handleMqttDebugResub() {
    if (mqtt.connected()) {
        for (int i = 0; i < mqttItemCount; i++) mqtt.subscribe(mqttItems[i].topic);
    }
    server.sendHeader("Location", "/mqtt/debug");
    server.send(302, "text/plain", "");
}

void handleMqttDebugReconnect() {
    if (mqtt.connected()) mqtt.disconnect();
    mqttConnect();
    server.sendHeader("Location", "/mqtt/debug");
    server.send(302, "text/plain", "");
}

// Publish <topic>/get with '{"state":""}' to force zigbee2mqtt to republish
void handleMqttDebugGet() {
    int idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= mqttItemCount) { server.send(400, "text/plain", "bad idx"); return; }
    if (!mqtt.connected()) { server.send(503, "text/plain", "mqtt not connected"); return; }

    char getTopic[MQTT_TOPIC_MAX + 8];
    snprintf(getTopic, sizeof(getTopic), "%s/get", mqttItems[idx].topic);
    bool ok = mqtt.publish(getTopic, "{\"state\":\"\"}");

    String msg = "Published to " + String(getTopic) + ": " + (ok ? "OK" : "FAIL");
    msg += "<br><br><a href='/mqtt/debug'>Back</a>";
    server.send(200, "text/html", msg);
}

void updateLdr() {
    int raw = analogRead(LDR_PIN);
    // Exponential smoothing (avoid jitter)
    ldrSmoothed = (ldrSmoothed * 7 + raw) / 8;

    if (autoBrightness) {
        int clamped = constrain(ldrSmoothed, LDR_RAW_MIN, LDR_RAW_MAX);
        currentBrightness = map(clamped, LDR_RAW_MIN, LDR_RAW_MAX, LDR_BRIGHT_MIN, LDR_BRIGHT_MAX);
        dp->setBrightness8(currentBrightness);
    }
}

// ─── Setup ───────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Panel64: Split-Flap + UDP ===");

    initDisplay();

    // Boot message on BOTH buffers so no trash
    for (int buf = 0; buf < 2; buf++) {
        dp->fillScreen(0);
        dp->setTextSize(1);
        dp->setTextColor(dp->color565(255, 200, 0));
        dp->setCursor(4, 20);
        dp->print("Connecting");
        dp->setCursor(4, 32);
        dp->print("WiFi...");
        dp->flipDMABuffer();
    }

    // LDR init
    analogSetAttenuation(ADC_11db);  // full range 0-3.3V
    pinMode(LDR_PIN, INPUT);
    ldrSmoothed = analogRead(LDR_PIN);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname("panel64");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    configTzTime(TZ_INFO, NTP_SERVER);
    struct tm ti;
    getLocalTime(&ti, 5000);

    loadConfig();

    udp.begin(UDP_PORT);
    server.on("/", handleRoot);
    server.on("/b", handleB);
    server.on("/mqtt", handleMqttPage);
    server.on("/mqtt/save", handleMqttSave);
    server.on("/mqtt/add", handleMqttAdd);
    server.on("/mqtt/del", handleMqttDel);
    server.on("/mqtt/discover", handleMqttDiscover);
    server.on("/mqtt/sniff/start", handleMqttSniffStart);
    server.on("/mqtt/sniff/stop", handleMqttSniffStop);
    server.on("/mqtt/sniff/list", handleMqttSniffList);
    server.on("/mqtt/sniff/clear", handleMqttSniffClear);
    server.on("/mqtt/debug", handleMqttDebug);
    server.on("/mqtt/debug/resub", handleMqttDebugResub);
    server.on("/mqtt/debug/reconnect", handleMqttDebugReconnect);
    server.on("/mqtt/debug/get", handleMqttDebugGet);
    ElegantOTA.begin(&server);
    server.begin();

    if (WiFi.status() == WL_CONNECTED && mqttHost[0]) mqttConnect();

    // Init split-flap state
    for (int r = 0; r < ROWS; r++) {
        memset(sfTgt[r], ' ', COLS); sfTgt[r][COLS] = 0;
        for (int c = 0; c < COLS; c++) {
            sfCells[r][c].drumPos = 0;   // space (index 0 in DRUM)
            sfCells[r][c].targetPos = -1;
            sfCells[r][c].stepStart = 0;
            sfCells[r][c].animating = false;
        }
    }
    sfFirstDraw = true;

    if (WiFi.status() == WL_CONNECTED) { fetchData(); lastFetch = millis(); }
    Serial.println("Ready.");
}

// ─── Loop ────────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // 1. Process UDP packets
    bool gotFrame = false;
    for (int n = 0; n < 30; n++) {
        int ps = udp.parsePacket();
        if (ps <= HDR_SIZE || ps > (int)sizeof(recvBuf)) break;
        int len = udp.read(recvBuf, ps);
        if (len <= HDR_SIZE) continue;

        uint8_t fid = recvBuf[0], ci = recvBuf[1], tc = recvBuf[2];
        int dl = len - HDR_SIZE, off = ci * CHUNK_MAX;
        if (off < 0 || off >= FRAME_SIZE || dl <= 0) continue;
        if (off + dl > FRAME_SIZE) dl = FRAME_SIZE - off;

        if (fid != rxFrameId) { rxFrameId = fid; rxChunks = 0; rxTotal = tc; }
        memcpy(frameBuf + off, recvBuf + HDR_SIZE, dl);
        if (ci < 32) rxChunks |= (1U << ci);
        if (allRx()) { gotFrame = true; rxChunks = 0; rxFrameId = 255; lastPktTime = now; }
    }

    // 2. Show UDP frame
    if (gotFrame) {
        showUdpFrame();
        frameCnt++;
        if (lastFrameT > 0 && now > lastFrameT)
            fps = fps * 0.9f + 1000.0f / (now - lastFrameT) * 0.1f;
        lastFrameT = now;
        udpMode = true;
    }

    // 3. UDP timeout → split-flap
    if (udpMode && now - lastPktTime > 5000) {
        udpMode = false;
        sfFirstDraw = true;
    }

    // 4. Split-flap display at ~30fps
    if (!udpMode && now - sfLastTick >= 33) {
        sfLastTick = now;
        tickSplitFlap();
    }

    // 5. Fetch data every 30s
    if (WiFi.status() == WL_CONNECTED && now - lastFetch >= 30000) {
        lastFetch = now;
        fetchData();
    }

    // 6. Web + OTA
    server.handleClient();
    ElegantOTA.loop();

    // 6b. MQTT
    if (mqttHost[0] && WiFi.status() == WL_CONNECTED) {
        if (mqtt.connected()) {
            mqtt.loop();
        } else if (now - lastMqttReconnect >= 5000) {
            lastMqttReconnect = now;
            mqttConnect();
        }
    }

    // 7. LDR auto-brightness
    if (now - lastLdrRead >= LDR_READ_INTERVAL) {
        lastLdrRead = now;
        updateLdr();
    }

    // 8. WiFi reconnect
    static unsigned long lastWC = 0;
    if (now - lastWC > 30000) { lastWC = now; if (WiFi.status() != WL_CONNECTED) WiFi.reconnect(); }
}
