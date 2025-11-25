
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include <WebServer.h>
#include "Server.h"
#include "configs.h"
#include "BitcoinMiner.h"
#include "UiManagement.h"
#include "UdpListiner.h"
#include <Preferences.h>
#define MININGCORE_IMPLEMENTATION
#include "MiningCore.h"
#define UPDATED_DISPLAY 1
#define NEW_CLUSTER_UI 1
#define NEW_STATS_UI 1
#define NEW_FANCY_UI 1

// ============== OPTIMIZATIONS APPLIED ==============
// 1. Reduced WiFi client timeout from 10s to 5s
// 2. Reduced JSON document sizes where possible
// 3. Increased UDP listener yield time (100ms -> 250ms)
// 4. Reduced monitor update rate to save CPU (2s -> 3s)
// 5. Increased background task sleep to 15 minutes
// 6. Pre-allocated strings to reduce heap fragmentation
// 7. Added task stack monitoring capability
// 8. Optimized display updates with dirty flag
// 9. Reduced temperature check frequency
// 10. Cached expensive calculations
// ===================================================

// Cached data
String btcBalance = "Loading...";
float btcPrice = 0.0;
unsigned long lastBalanceCheck = 0;
const unsigned long BALANCE_INTERVAL = 10UL * 60UL * 1000UL; // 10 minutes

// Fixed-size arrays must use compile-time constants; NUMBER_OF_MINERS can be changed at runtime

MinerStats miners[MAX_MINERS + 1];
ClusterUICache clusterUI;
StatsUICache statsUI;
FancyUICache fancyUI;
MinerUICache minerUI[MAX_MINERS + 1]; // +1 for Core2


WebServer server(80);
Preferences prefs;
unsigned long startTime = 0;

void loadConfig() {
    prefs.begin("miner", false);
    POOL_URL = prefs.getString("pool", "solo.ckpool.org").c_str();
    POOL_PORT = prefs.getUShort("port", 3333);
    ADDRESS = prefs.getString("addr", "").c_str();
    CORES = prefs.getUInt("cores", 2);
    THREADS = prefs.getUInt("threads", 1);
    NUMBER_OF_MINERS = prefs.getUInt("miners", 5);
    prefs.end();
}

void saveConfig() {
    prefs.begin("miner", false);
    prefs.putString("pool", POOL_URL);
    prefs.putUShort("port", POOL_PORT);
    prefs.putString("addr", ADDRESS);
    prefs.putUInt("cores", (int)CORES);
    prefs.putUInt("threads", (int)THREADS);
    prefs.putUInt("miners", (int)NUMBER_OF_MINERS);
    prefs.end();
}

// HTML template in Flash (PROGMEM)
static const char index_html[] PROGMEM = R"=====( 
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solo Swarm</title>
<style>body{font-family:Arial;background:#000;color:#0f0;margin:0;padding:10px}h1{text-align:center;margin:10px;color:#0f0}.tab button{background:#222;color:#0f0;border:2px solid #0f0;padding:12px;margin:5px;font-size:18px;width:48%}.tab button.active{background:#0f0;color:#000}.tabcontent{display:none;padding:20px;border:2px solid #0f0;margin-top:10px;background:#111}#Stats{display:block}.stat{font-size:22px;margin:15px 0}input,button{width:100%;padding:14px;margin:10px 0;font-size:18px;border:none}button{background:#0f0;color:#000;font-weight:bold}.small{font-size:14px;color:#888}</style>
</head><body>
<h1>SOLO SWARM</h1>
<div class="tab">
  <button onclick="openTab(event,'Stats')" class="active">STATS</button>
  <button onclick="openTab(event,'Settings')">SETTINGS</button>
</div>
<div id="Stats" class="tabcontent">
  <div class="stat">Hashrate: <span id="hr">0.00</span> KH/s</div>
  <div class="stat">Shares: <span id="sh">0</span> | Valid: <span id="va">0</span></div>
  <div class="stat">Templates: <span id="tp">0</span></div>
  <div class="stat">Uptime: <span id="up">0</span>m</div>
  <div class="stat">Temp: <span id="temp">--</span>°C</div>
  <div class="stat">Pool: <span id="pool">--</span></div>
  <div class="stat small">IP: <span id="ip">--</span></div>
</div>
<div id="Settings" class="tabcontent">
  <form action="/save" method="POST">
    <input type="text" name="pool" placeholder="Pool URL" value="%POOL%">
    <input type="number" name="port" placeholder="Port" value="%PORT%">
    <input type="text" name="addr" placeholder="BTC Address" value="%ADDR%">
    <input type="number" name="cores" placeholder="Cores" value="%CORES%">
    <input type="number" name="threads" placeholder="Threads" value="%THREADS%">
    <input type="number" name="miners" placeholder="Miners" value="%MINERS%">
    <button type="submit">SAVE & REBOOT</button>
  </form>
  <form action="/reboot" method="POST">
    <button style="background:#f00;color:#fff">REBOOT NOW</button>
  </form>
</div>
<script>
function openTab(e,n){document.querySelectorAll(".tabcontent").forEach(t=>t.style.display="none");document.querySelectorAll(".tab button").forEach(b=>b.className=b.className.replace(" active",""));document.getElementById(n).style.display="block";e.currentTarget.className+=" active";}
function update(){fetch("/data").then(r=>r.json()).then(d=>{document.getElementById("hr").innerText=d.hr;document.getElementById("sh").innerText=d.shares;document.getElementById("va").innerText=d.valids;document.getElementById("tp").innerText=d.templates;document.getElementById("up").innerText=d.uptime;document.getElementById("temp").innerText=d.temp;document.getElementById("pool").innerText=d.pool;document.getElementById("ip").innerText=d.ip;});}
setInterval(update,5000);update();
</script>
</body></html>
)=====";

// Helper: safe replace/send of template from a RAM copy (no heap Strings)
void handleRoot() {
    // NOTE: static so buffer doesn't live on stack; size chosen conservative
    static char pageBuf[8192];
    // copy PROGMEM template into RAM buffer
    strncpy_P(pageBuf, index_html, sizeof(pageBuf) - 1);
    pageBuf[sizeof(pageBuf) - 1] = '\0';

    // Build response in another static buffer to avoid temporary heap allocations
    static char outBuf[9216];
    char *dst = outBuf;
    size_t remain = sizeof(outBuf);

    const char *ptr = pageBuf;
    while (*ptr && remain > 1) {
        // find next placeholder among three keys
        const char *pPool = strstr(ptr, "%POOL%");
        const char *pPort = strstr(ptr, "%PORT%");
        const char *pAddr = strstr(ptr, "%ADDR%");
        const char *pCores = strstr(ptr, "%CORES%");
        const char *pThreads = strstr(ptr, "%THREADS%");
        const char *pMiners = strstr(ptr, "%MINERS%");

        // pick nearest placeholder
        const char *next = NULL;
        if (pPool) next = pPool;
        if (pPort && (!next || pPort < next)) next = pPort;
        if (pCores && (!next || pCores < next)) next = pCores;
        if (pAddr && (!next || pAddr < next)) next = pAddr;
        if (pThreads && (!next || pThreads < next)) next = pThreads;
        if (pMiners && (!next || pMiners < next)) next = pMiners;
        
        if (!next) {
            // copy rest
            size_t n = strlen(ptr);
            if (n >= remain) n = remain - 1;
            memcpy(dst, ptr, n);
            dst += n;
            remain -= n;
            *dst = '\0';
            break;
        } else {
            // copy chunk before placeholder
            size_t chunk = next - ptr;
            if (chunk >= remain) chunk = remain - 1;
            memcpy(dst, ptr, chunk);
            dst += chunk;
            remain -= chunk;
            *dst = '\0';

            // insert replacement
            if (next == pPool) {
                int w = snprintf(dst, remain, "%s", POOL_URL);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 6;
            } else if (next == pPort) {
                int w = snprintf(dst, remain, "%u", (unsigned)POOL_PORT);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 6;
            } else if (next == pCores) {
                int w = snprintf(dst, remain, "%d", (int)CORES);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 6;
            } else if (next == pThreads) {
                int w = snprintf(dst, remain, "%d", (int)THREADS);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 9;
            } else if (next == pMiners) {
                int w = snprintf(dst, remain, "%d", (int)NUMBER_OF_MINERS);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 8;
            } else if (next == pAddr) {
                int w = snprintf(dst, remain, "%s", ADDRESS);
                if (w < 0) w = 0;
                if ((size_t)w >= remain) w = remain - 1;
                dst += w; remain -= w; *dst = '\0';
                ptr = next + 6;
            }
        }
    }

    // send final constructed page
    server.send(200, "text/html", outBuf);
}

// Safe JSON for /data using fixed buffer
void handleData() {
    // try to take mutex but protect against deadlock — short wait
    if (xSemaphoreTake(statsMutex, 50 / portTICK_PERIOD_MS) == pdFALSE) {
        server.send(503, "application/json", "{\"error\":\"busy\"}");
        return;
    }

    // copy needed fields quickly
    unsigned long now = millis();
    float hashrate = (now > startTime) ? ((hashes / ((now - startTime) / 1000.0f)) / 1000.0f) : 0.0f;
    unsigned long uptimeMin = (now - startTime) / 60000UL;
    unsigned int u_shares = shares;
    unsigned int u_valids = valids;
    unsigned int u_templates = templates;
    float temp = temperatureRead();
    xSemaphoreGive(statsMutex);

    // build JSON into fixed buffer
    char jsonBuf[512];
    snprintf(jsonBuf, sizeof(jsonBuf),
             "{\"hr\":%.2f,\"shares\":%u,\"valids\":%u,\"templates\":%u,"
             "\"uptime\":%lu,\"temp\":%.1f,\"pool\":\"%s:%u\",\"ip\":\"%s\"}",
             hashrate, u_shares, u_valids, u_templates,
             uptimeMin, temp,
             POOL_URL, (unsigned)POOL_PORT, WiFi.localIP().toString().c_str());

    server.send(200, "application/json", jsonBuf);
}

void handleSave() {
    // read form args (these are small transient Strings provided by server.arg)
    if (server.hasArg("pool")) POOL_URL = server.arg("pool").c_str();
    if (server.hasArg("port")) POOL_PORT = (uint16_t)server.arg("port").toInt();
    if (server.hasArg("addr")) ADDRESS = server.arg("addr").c_str();

    saveConfig();

    // minimal HTML response — no large allocations
    server.send(200, "text/html",
                "<h1 style='color:#0f0;background:#000;text-align:center;padding:100px'>Saved!<br>Rebooting...</h1>");
    delay(1000);
    ESP.restart();
}

void handleReboot() {
    server.send(200, "text/html",
                "<h1 style='color:#f00;background:#000;text-align:center;padding:100px'>Rebooting...</h1>");
    delay(1000);
    ESP.restart();
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reboot", HTTP_POST, handleReboot);

    server.begin();
    Serial.println("Web dashboard initialized");
}

// Task handle
TaskHandle_t webTaskHandle = NULL;

void webServerTask(void *pvParameters) {
    Serial.println("Web server task running on core: " + String(xPortGetCoreID()));
    for (;;) {
        server.handleClient();
        vTaskDelay(2 / portTICK_PERIOD_MS); // small yield
    }
}

// call this after WiFi is connected and after setupWebServer()
void startWebTask() {
    if (webTaskHandle) return;
    // use larger stack if you add more features; 8k is safe
    BaseType_t res = xTaskCreatePinnedToCore(
        webServerTask, "WebServer", 8192, NULL, WEB_SERVER_PRIORITY, &webTaskHandle, 0); // CORE 0
    if (res != pdPASS) {
        Serial.println("Failed to start web server task");
    } else {
        Serial.println("Web server started on core 0");
    }
}



// Optimized: Increased sleep time, reduced timeout
void runBackgroundTasks(void *name) {
    Serial.println("Background task started - waiting 10s...");
    vTaskDelay(10000 / portTICK_PERIOD_MS);
    
    // Pre-allocate strings
    String response;
    response.reserve(512);
    
    while(1) {
        // BTC Price check - with connection pooling consideration
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(5000);
        
        if (client.connect("api.coinbase.com", 443)) {
            client.print("GET /v2/prices/BTC-USD/spot HTTP/1.1\r\n"
                        "Host: api.coinbase.com\r\n"
                        "Connection: close\r\n\r\n");
            
            unsigned long timeout = millis();
            response = "";
            while (millis() - timeout < 3000) {
                if (client.available()) {
                    response += (char)client.read();
                }
                if (response.indexOf("\r\n\r\n") > 0 && response.indexOf("}") > 0) break;
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            
            int jsonStart = response.indexOf("{");
            if (jsonStart > 0) {
                String jsonStr = response.substring(jsonStart);
                StaticJsonDocument<512> doc; // Reduced from 1024
                if (!deserializeJson(doc, jsonStr)) {
                    float newPrice = doc["data"]["amount"].as<float>();
                    if (newPrice != btcPrice) {
                        btcPrice = newPrice;
                        displayDirty = true;
                    }
                }
            }
            client.stop();
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        // Balance check
        client.setInsecure();
        client.setTimeout(5000);
        
        if (client.connect("blockchain.info", 443)) {
            String addr = String(ADDRESS);
            String request = "GET /q/addressbalance/" + addr + " HTTP/1.1\r\n"
                            "Host: blockchain.info\r\n"
                            "User-Agent: M5Stack\r\n"
                            "Connection: close\r\n\r\n";
            
            client.print(request);
            
            unsigned long timeout = millis();
            response = "";
            while (millis() - timeout < 3000) {
                if (client.available()) {
                    response += (char)client.read();
                }
                if (response.indexOf("\r\n\r\n") > 0 && response.length() > 100) break;
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
            
            int bodyStart = response.indexOf("\r\n\r\n");
            if (bodyStart > 0) {
                String body = response.substring(bodyStart + 4);
                body.trim();
                
                String newBalance;
                if (body.indexOf("error") > 0 || body.indexOf("invalid") > 0) {
                    newBalance = "Invalid Addr";
                } else {
                    long satoshis = body.toInt();
                    float btc = satoshis / 100000000.0;
                    
                    if (satoshis == 0 && body != "0") {
                        newBalance = "Check Addr";
                    } else if (btc > 0) {
                        newBalance = String(btc, 8) + " BTC";
                    } else {
                        newBalance = "0 BTC";
                    }
                }
                
                if (newBalance != btcBalance) {
                    btcBalance = newBalance;
                    displayDirty = true;
                }
            }
            client.stop();
        }
        
        // Increased from 10 minutes to 15 minutes
        Serial.println("Background: Done, sleeping 15 min...");
        vTaskDelay(900000 / portTICK_PERIOD_MS);
    }
}

void updateBTCBalance() {
    if (millis() - lastBalanceCheck < 5000) return; // rate limit
    lastBalanceCheck = millis();

    WiFiClientSecure client;
    client.setInsecure(); // We use HTTPS but skip cert verify (ESP32 limitation)

    String payload;
    bool success = false;

    #if false
    // METHOD 1: solo.ckpool.org API (FASTEST + MOST RELIABLE for solo miners)
    if (client.connect("solo.ckpool.org", 443)) {
        client.print(String("GET /user/") + ADDRESS + " HTTP/1.1\r\n" +
                     "Host: solo.ckpool.org\r\n" +
                     "User-Agent: SoloSwarm/1.0\r\n" +
                     "Connection: close\r\n\r\n");

        unsigned long timeout = millis() + 8000;
        while (client.connected() && millis() < timeout) {
            String line = client.readStringUntil('\n');
            if (line.indexOf("\"balance\"") != -1) {
                int start = line.indexOf(':') + 1;
                int end = line.indexOf(',');
                String balStr = line.substring(start, end);
                balStr.trim();
                long satoshis = balStr.toInt();
                btcBalance = String(satoshis / 100000000.0, 8) + " BTC";
                success = true;
                break;
            }
        }
        client.stop();
    }
    #endif

    // METHOD 2: Fallback to mempool.space (if ckpool down)
    if (!success && client.connect("mempool.space", 443)) {
        client.print(String("GET /api/address/") + ADDRESS + " HTTP/1.1\r\n" +
                     "Host: mempool.space\r\n" +
                     "Connection: close\r\n\r\n");

        unsigned long timeout = millis() + 10000;
        while (client.connected() && millis() < timeout) {
            String line = client.readStringUntil('\n');
            if (line.indexOf("\"balance\"") != -1) {
                int start = line.indexOf(':') + 1;
                int end = line.indexOf(',');
                String balStr = line.substring(start, end);
                long satoshis = balStr.toInt();
                btcBalance = String(satoshis / 100000000.0, 8) + " BTC";
                success = true;
                break;
            }
        }
        client.stop();
    }

    // UI UPDATE (only if changed)
    static String lastDisplayed = "";
    String displayText = btcBalance;
    if (btcPrice > 0) displayText += " ($" + String((btcBalance.substring(0, btcBalance.indexOf(' ')).toFloat() * btcPrice), 0) + ")";

    if (displayText != lastDisplayed) {
        lastDisplayed = displayText;
        Serial.printf("Balance: %s", displayText.c_str());
        if (btcPrice > 0) Serial.printf(" ≈ $%.0f\n", btcBalance.substring(0, btcBalance.indexOf(' ')).toFloat() * btcPrice);
        else Serial.println();
    }
}
// Use the same 16-bit color type as M5.Lcd (color565 / constants like BLACK)
void DrawRectangle(int x, int y, int w, int h, uint16_t color) {
    // Protect against nonsense sizes
    if (w <= 0 || h <= 0) return;
    M5.Lcd.fillRect(x, y, w, h, color);
}

void drawIfChanged(float &cached, float value, int size, int x, int y, 
                   uint16_t color, const char* fmt = "%.2f", float eps = 0.01f)
{
    if (isnan(value)) return;
    
    if (fabsf(cached - value) > eps) {
        cached = value;
        
        // ✅ FIXED: Precise clearing based on actual text size
        int clearW = size * 6 * 6;  // Assume ~6 chars max (e.g., "123.45")
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        // Draw new value
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf(fmt, value);
    }
}

void drawIfChangedInt(int &cached, int value, int size, int x, int y, uint16_t color)
{
    if (cached != value) {
        cached = value;
        
        // ✅ FIXED: Precise clearing for integers
        int clearW = size * 6 * 4;  // Assume ~4 chars max (e.g., "9999")
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf("%d", value);
    }
}

void drawIfChangedTemp(float &cached, float value, int size, int x, int y, uint16_t color)
{
    if (isnan(value)) return;
    
    if (fabsf(cached - value) > 0.5f) {  // 0.5°C threshold
        cached = value;
        
        // ✅ Clear area for "XXC" (3 chars)
        int clearW = size * 6 * 3;
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf("%.0fC", value);
    }
}

void drawBar(int x, int y, int width, int height, float percent, uint16_t color) {
    if (width <= 2 || height <= 2) return;
    
    percent = constrain(percent, 0.0f, 1.0f);
    
    M5.Lcd.drawRect(x, y, width, height, DARKGREY);
    
    int innerW = width - 2;
    int innerH = height - 2;
    int fillWidth = (int)(innerW * percent);
    
    if (fillWidth <= 0) return;
    if (fillWidth > innerW) fillWidth = innerW;
    
    // Clear old bar completely
    M5.Lcd.fillRect(x + 1, y + 1, innerW, innerH, BLACK);
    
    // Draw new bar
    M5.Lcd.fillRect(x + 1, y + 1, fillWidth, innerH, color);
}

void drawClusterStaticUI() {
    // Header
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(5, 5);
    M5.Lcd.println("SOLO SWARM CLUSTER");
    
    M5.Lcd.drawLine(0, 40, 320, 40, GREEN);
    
    // ✅ FIXED: Add "miners" label here (static)
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.setCursor(165, 25);
    M5.Lcd.print("miners");
    
    // Miner labels
    int y = 48;
    M5.Lcd.setTextSize(1);
    
    #if false
    for (int i = 0; i < 5; i++) {
        M5.Lcd.setCursor(5, y);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.printf("S3-%d", i + 1);
        
        // Draw empty bar placeholder
        M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
        
        y += 20;
    }
    #endif
    
    for (int i = 0; i < 5; i++) {
        M5.Lcd.setCursor(5, y);
        
        if (miners[i].online) {
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.printf("S3-%d", i + 1);
            
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.setCursor(50, y);
            M5.Lcd.printf("%.1f", miners[i].hashrate);
            
            int barWidth = (int)(miners[i].hashrate / 45.0 * 150);
            M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
            
            uint16_t tempColor = (miners[i].temp > 70) ? RED : (miners[i].temp > 60) ? YELLOW : CYAN;
            
            M5.Lcd.setTextColor(tempColor);
            M5.Lcd.setCursor(260, y);
            M5.Lcd.printf("%.0fC", miners[i].temp);
        } else {
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.printf("S3-%d OFFLINE", i + 1);
        }
        
        y += 20;
    }
    
    y += 5;
    M5.Lcd.drawLine(0, y, 320, y, DARKGREY);
    y += 5;
    
    // Core2 label
    M5.Lcd.setCursor(5, y);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.print("Core2");
    
    // Draw empty bar placeholder
    M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
    
    y += 20;
    M5.Lcd.drawLine(0, y, 320, y, GREEN);
    y += 5;
    
    // ✅ FIXED: Static labels with better positioning
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Shares:");
    
    M5.Lcd.setCursor(110, y);
    M5.Lcd.print("| Valid:");
    
    y += 15;
    
    // Pool info (static)
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.printf("Pool: %s:%d", POOL_URL, POOL_PORT);
    
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(5, 225);
    M5.Lcd.print("BtnA: Stats");
}

void drawFancyStaticUI() {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(5, 5);
    M5.Lcd.println("SOLO SWARM CLUSTER");
    
    M5.Lcd.drawLine(0, 28, 320, 28, GREEN);
    
    // Progress bar outline
    M5.Lcd.drawRect(5, 33, 310, 15, WHITE);
    
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(WHITE);
    
    int y = 55;
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Cluster:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Core2:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Hashes:");
    y += 16;
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Templates:");
    M5.Lcd.setTextColor(WHITE);
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("16bit:");
    M5.Lcd.setCursor(95, y);
    M5.Lcd.print("| 32bit:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Valid:");
    y += 18;
    
    M5.Lcd.drawLine(0, y, 320, y, GREEN);
    y += 6;
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Bal:");
    y += 16;
    
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("BTC:");
    y += 16;
    
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.printf("%s:%d", POOL_URL, POOL_PORT);
    
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(5, 225);
    M5.Lcd.print("BtnA: Cluster");
}

void drawStatsStaticUI() {
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(WHITE);
    
    int y = 5;
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("CLUSTER STATISTICS");
    y += 15;
    
    M5.Lcd.drawLine(0, y, 320, y, GREEN);
    y += 5;
    
    // Static labels
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Total:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Core2:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Hashes:");
    y += 16;
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Templates:");
    M5.Lcd.setTextColor(WHITE);
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Total Shares:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Valid:");
    y += 18;
    
    M5.Lcd.drawLine(0, y, 320, y, GREEN);
    y += 5;
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("Bal:");
    y += 16;
    
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("BTC:");
    y += 16;
    
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.setCursor(5, y);
    M5.Lcd.print("IP:");
    y += 16;
    
    M5.Lcd.setCursor(5, y);
    M5.Lcd.printf("Pool: %s:%d", POOL_URL, POOL_PORT);
    
    M5.Lcd.setTextColor(DARKGREY);
    M5.Lcd.setCursor(5, 225);
    M5.Lcd.print("BtnA: Fancy");
}

// Optimized: Reduced update frequency, dirty flag, cached calculations
void runMonitor(void *name) {
    vTaskDelay(20 / portTICK_PERIOD_MS); // prevent millis()==start
 
    unsigned long start = millis();
    unsigned long lastUpdate = 0;
    unsigned long lastTempRead = 0;
    float cachedTemp = 0;
    
    Serial.println("Monitor task started");
    
    while (1) {
        unsigned long now = millis();
        
        M5.update();
        if (M5.BtnA.wasPressed()) {
            displayMode = (displayMode + 1) % 3;
            displayDirty = true;
        }
        
        // Increased from 2s to 3s
        int updateInterval = MONITOR_UPDATE_INTERVAL_MS;
        
        // Only update if dirty flag set or interval passed
        if (!displayDirty && (now - lastUpdate < updateInterval)) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }
        
        lastUpdate = now;
        
        taskYIELD();
        vTaskDelay(10 / portTICK_PERIOD_MS);
        
        unsigned long elapsed = now - start;
    
        xSemaphoreTake(statsMutex, portMAX_DELAY);
        if (hashes == 65536000) {
            // Prevent overflow
            start = now;
            hashes = 0;
        }
        if (templates == 65536000) {
            // Prevent overflow
            start = now;
            templates = 0;
        }
        if (shares == 65536000) {
            // Prevent overflow
            start = now;
            shares = 0;
        }
        if (valids == 65536000) {
            // Prevent overflow
            start = now;
            valids = 0;
        }
        if (halfshares == 65536000) {
            // Prevent overflow
            start = now;
            halfshares = 0;
        }
        if (blockFoundTime == 65536000) {
            // Prevent overflow
            start = now;
            blockFoundTime = 0;
        }
        if (blockFound == 65536000) {
            // Prevent overflow
            start = now;
            blockFound = 0;
        }
        if (now - start > 3600000) {
            // Reset every hour to prevent overflows
            start = now;
            hashes = 0;
            blockFoundTime = 0;
            blockFound = 0;
        }
        for (int i = 0; i < sizeof(miners) / sizeof(miners[0]) -1; i++) {
            if (miners[i].hashrate == 65536000) {
                // Prevent overflow
                miners[i].hashrate = 0;
            }
            if (miners[i].shares == 65536000) {
                // Prevent overflow
                miners[i].shares = 0;
            }
            if (miners[i].valids == 65536000) {
                // Prevent overflow
                miners[i].valids = 0;
            }
            if (miners[i].temp == 65536000) {
                // Prevent overflow
                miners[i].temp = 0;
            }
        }
        
        long local_hashes = hashes;
        long local_templates = templates;
        int local_halfshares = halfshares;
        int local_shares = shares;
        int local_valids = valids;
        bool local_blockFound = blockFound;
        unsigned long local_blockTime = blockFoundTime;
        
        
        // Cache temperature reading (expensive operation)
        if (now - lastTempRead > 5000) {
            cachedTemp = temperatureRead();
            lastTempRead = now;
        }
        
        float core2_hashrate = 0;
        if (elapsed > 0) {
            core2_hashrate = (float)local_hashes / (elapsed / 1000.0f) / 1000.0f;
        }
        xSemaphoreGive(statsMutex);
        
        
        miners[5].hashrate = core2_hashrate;
        miners[5].shares = local_shares;
        miners[5].valids = local_valids;
        miners[5].temp = cachedTemp;
        miners[5].online = true;

        // Calculate totals once
        float totalHashrate = 0;
        int totalShares = 0;
        int totalValids = 0;
        int onlineCount = 0;
        
        for (int i = 0; i < 6; i++) {
            if (miners[i].online) {
                totalHashrate += miners[i].hashrate;
                totalShares += miners[i].shares;
                totalValids += miners[i].valids;
                onlineCount++;
            }
        }

        // Block found celebration
        if (local_blockFound && (now - local_blockTime < 60000)) {
            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.setTextSize(3);
            M5.Lcd.setCursor(20, 60);
            M5.Lcd.println("BLOCK FOUND!");
            M5.Lcd.setTextSize(2);
            M5.Lcd.setCursor(40, 100);
            M5.Lcd.println("YOU WIN!");
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(10, 140);
            M5.Lcd.printf("Cluster: %d blocks", totalValids);
            M5.Lcd.setCursor(10, 160);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.println("Check your wallet!");
            M5.Lcd.setCursor(10, 180);
            M5.Lcd.printf("%.2f BTC reward!", 3.125);
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }
        
        if (displayDirty) {
            M5.Lcd.fillScreen(BLACK);

            if (displayMode == 0) {
                drawClusterStaticUI();   // NEW
            }
            else if (displayMode == 1) {
                drawStatsStaticUI();     // NEW
            }
            else if (displayMode == 2) {
                drawFancyStaticUI();     // NEW
            }

            displayDirty = false;
        }
        
        M5.Lcd.setTextDatum(TL_DATUM);
        
        if (displayMode == 0) {
            statsUI.initialized = false;
            fancyUI.initialized = false;
            statsUI.ipDrawn = false;
            // ✅ Draw static UI once when switching modes or first time
            if (displayDirty || !clusterUI.initialized) {
                M5.Lcd.fillScreen(BLACK);
                drawClusterStaticUI();
                clusterUI.initialized = true;
                displayDirty = false;
                
                // Reset all cached values to force redraw
                clusterUI.totalHashrate = -1;
                clusterUI.onlineCount = -1;
                clusterUI.core2Hashrate = -1;
                clusterUI.cachedTemp = -1;
                clusterUI.totalShares = -1;
                clusterUI.totalValids = -1;
                
                for (int i = 0; i < 6; i++) {
                    minerUI[i].hashrate = -1;
                    minerUI[i].temp = -1;
                    minerUI[i].lastOnlineState = false;
                }
            }
            
            // Top line: hashrate and miner count
            // Position: "XX.XX KH/s | N miners"
            //            5,25        140,25  165,25(static)
            if (!totalHashrate == 0)
            {
                DrawRectangle(5, 25, 135, 10, BLACK);  // Clear area for hashrate
            }
            drawIfChanged(clusterUI.totalHashrate, totalHashrate, 1, 5, 25, GREEN, "%.2f KH/s |");
            drawIfChangedInt(clusterUI.onlineCount, onlineCount, 1, 140, 25, GREEN);  // ✅ FIXED: Position after " | "
            
            // Individual miners
            int y = 48;
            
            for (int i = 0; i < 5; i++) {
                // ✅ Check if miner state changed (online/offline)
                if (minerUI[i].lastOnlineState != miners[i].online) {
                    // State changed - redraw entire miner line
                    M5.Lcd.fillRect(40, y, 275, 12, BLACK);  // ✅ FIXED: Clear only data area, not label
                    
                    M5.Lcd.setTextSize(1);
                    
                    if (miners[i].online) {
                        // Just redraw bar outline
                        M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
                    } else {
                        // Draw OFFLINE text
                        M5.Lcd.setTextColor(DARKGREY);
                        M5.Lcd.setCursor(50, y);
                        M5.Lcd.print("OFFLINE");
                    }
                    
                    minerUI[i].lastOnlineState = miners[i].online;
                    minerUI[i].hashrate = -1;  // Force redraw
                    minerUI[i].temp = -1;      // Force redraw
                }
                
                // ✅ Update values only if online
                if (miners[i].online) {
                    
                    
                    // Hashrate at position 50
                    drawIfChanged(minerUI[i].hashrate, miners[i].hashrate, 1, 50, y, GREEN, "%.1f");
                    
                    // Bar
                    drawBar(100, y, 150, 10, miners[i].hashrate / 45.0, GREEN);
                    
                    // Temperature at position 260
                    uint16_t tempColor = (miners[i].temp > 70) ? RED : 
                                        (miners[i].temp > 60) ? YELLOW : CYAN;
                    drawIfChangedTemp(minerUI[i].temp, miners[i].temp, 1, 260, y, tempColor);  // ✅ Use special temp function
                }
                
                y += 20;
            }
            
            // Skip separator lines
            y += 10;
            
            // Core2 values
            // Position: "Core2" at 5, hashrate at 50, bar at 100, temp at 260
            drawIfChanged(clusterUI.core2Hashrate, core2_hashrate, 1, 50, y, YELLOW, "%.2f");
            drawBar(100, y, 150, 10, core2_hashrate / 45.0, YELLOW);
            
            uint16_t tempColor = (cachedTemp > 70) ? RED : 
                                (cachedTemp > 60) ? YELLOW : CYAN;
            drawIfChangedTemp(clusterUI.cachedTemp, cachedTemp, 1, 260, y, tempColor);  // ✅ Use special temp function
            
            y += 25;  // Skip separator
            
            // Shares and valids
            // Position: "Shares:" at 5, value at 60, "| Valid:" at 110, value at 165
            drawIfChangedInt(clusterUI.totalShares, totalShares, 1, 60, y, YELLOW);
            drawIfChangedInt(clusterUI.totalValids, totalValids, 1, 165, y, 
                            totalValids > 0 ? GREEN : RED);
        }
        else if (displayMode == 1) {
          // ✅ Draw static UI once
          if (displayDirty || !statsUI.initialized) {
              M5.Lcd.fillScreen(BLACK);
              drawStatsStaticUI();
              statsUI.initialized = true;
              displayDirty = false;
              
              // Reset cache
              statsUI.totalHashrate = -1;
              statsUI.elapsed = 0;
              statsUI.totalHashes = -1;
              statsUI.templates = -1;
              statsUI.totalShares = -1;
              statsUI.totalValids = -1;
              statsUI.btcPrice = -1;
          }
          
          // ✅ Update dynamic values only
          int y = 25;
          
          // Total hashrate
          drawIfChanged(statsUI.totalHashrate, totalHashrate, 1, 60, y, GREEN, "%.2f KH/s");
          y += 16;
          
          // Core2 time
          if (statsUI.elapsed != elapsed) {
              statsUI.elapsed = elapsed;
              M5.Lcd.fillRect(60, y, 100, 8, BLACK);
              M5.Lcd.setTextSize(1);
              M5.Lcd.setTextColor(WHITE);
              M5.Lcd.setCursor(60, y);
              M5.Lcd.printf("%ldm %lds", elapsed/60000, (elapsed/1000)%60);
          }
          y += 16;
          
          // Hashes
          float hashesM = local_hashes / 1000000.0;
          drawIfChanged(statsUI.totalHashes, hashesM, 1, 80, y, WHITE, "%.2fM");
          y += 16;
          
          // Templates
          if (statsUI.templates != local_templates) {
              statsUI.templates = local_templates;
              M5.Lcd.fillRect(95, y, 60, 8, BLACK);
              M5.Lcd.setTextSize(1);
              M5.Lcd.setTextColor(YELLOW);
              M5.Lcd.setCursor(95, y);
              M5.Lcd.printf("%ld", local_templates);
          }
          y += 16;
          
          // Total Shares
          drawIfChangedInt(statsUI.totalShares, totalShares, 1, 115, y, WHITE);
          y += 16;
          
          // Valid
          drawIfChangedInt(statsUI.totalValids, totalValids, 1, 60, y, 
                          totalValids > 0 ? GREEN : RED);
          y += 23;  // Skip line
          
          // Balance
          //updateBTCBalance();
          M5.Lcd.setTextSize(1);
          M5.Lcd.setTextColor(YELLOW);
          M5.Lcd.setCursor(40, y);
          M5.Lcd.print(btcBalance.c_str());
          y += 16;
          
          // BTC Price
          if (btcPrice > 0) {
              drawIfChanged(statsUI.btcPrice, btcPrice, 1, 45, y, GREEN, "$%.0f");
              y += 16;
          } else {
              y += 16;
          }
          
          // IP (static after first draw)
          statsUI.ipDrawn  = false;
          if (!statsUI.ipDrawn) {
              M5.Lcd.setTextColor(CYAN);
              M5.Lcd.setCursor(30, y);
              M5.Lcd.print(WiFi.localIP().toString().c_str());
              statsUI.ipDrawn = true;
          }
        }
        else {
        clusterUI.initialized = false;  // Reset cluster for when we go back
        statsUI.initialized = false;     // Reset stats too
        statsUI.ipDrawn = false;
        // ✅ Draw static UI once
        if (displayDirty || !fancyUI.initialized) {
            M5.Lcd.fillScreen(BLACK);
            drawFancyStaticUI();
            fancyUI.initialized = true;
            displayDirty = false;
            
            // Reset cache
            fancyUI.rssi = 0;
            fancyUI.bars = -1;
            fancyUI.cachedTemp = -1;
            fancyUI.progress = -1;
            fancyUI.totalHashrate = -1;
            fancyUI.elapsed = 0;
            fancyUI.totalHashes = -1;
            fancyUI.usPerHash = -1;
            fancyUI.templates = -1;
            fancyUI.battLevel = -1;
            fancyUI.charging = false;
            fancyUI.halfshares = -1;
            fancyUI.shares = -1;
            fancyUI.totalValids = -1;
            fancyUI.btcPrice = -1;
        }
        
        // ✅ Update WiFi indicator
        int rssi = WiFi.RSSI();
        int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
        
        if (fancyUI.bars != bars || abs(fancyUI.rssi - rssi) > 5) {
            fancyUI.bars = bars;
            fancyUI.rssi = rssi;
            
            uint16_t wifiColor = (bars > 2) ? GREEN : (bars > 1) ? YELLOW : RED;
            int wifiX = 290;
            int wifiY = 5;
            
            // Clear area
            M5.Lcd.fillRect(wifiX, wifiY, 30, 20, BLACK);
            
            // Draw bars
            for (int i = 0; i < 4; i++) {
                if (i < bars) {
                    M5.Lcd.fillRect(wifiX + (i * 6), wifiY + (12 - (i * 3)), 4, i * 3 + 3, wifiColor);
                } else {
                    M5.Lcd.drawRect(wifiX + (i * 6), wifiY + (12 - (i * 3)), 4, i * 3 + 3, DARKGREY);
                }
            }
            
            // Draw RSSI value
            M5.Lcd.fillRect(255, 14, 35, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(wifiColor);
            M5.Lcd.setCursor(255, 14);
            M5.Lcd.printf("%ddB", rssi);
        }
        
        // ✅ Update temperature
        uint16_t tempColor = (cachedTemp > 70) ? RED : (cachedTemp > 60) ? YELLOW : CYAN;
        drawIfChangedTemp(fancyUI.cachedTemp, cachedTemp, 1, 225, 14, tempColor);
        
        // ✅ Update progress bar
        int progress = min(100, (int)((totalHashrate / 270.0) * 100));
        if (fancyUI.progress != progress) {
            fancyUI.progress = progress;
            
            // Clear and redraw bar
            M5.Lcd.fillRect(7, 35, 306, 11, BLACK);
            M5.Lcd.fillRect(7, 35, 306 * progress / 100, 11, GREEN);
            
            // Update percentage
            M5.Lcd.fillRect(270, 37, 40, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(270, 37);
            M5.Lcd.printf("%d%%", progress);
        }
        
        int y = 55;
        
        // Cluster hashrate
        drawIfChanged(fancyUI.totalHashrate, totalHashrate, 1, 75, y, WHITE, "%.2f KH/s");
        
        // H/J calculation
        if (totalHashrate > 0) {
            M5.Lcd.fillRect(200, y, 100, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(200, y);
            M5.Lcd.printf("%.0f H/J", (totalHashrate * 1000) / 3.0);
        }
        y += 16;
        
        // Core2 time
        if (fancyUI.elapsed != elapsed) {
            fancyUI.elapsed = elapsed;
            M5.Lcd.fillRect(60, y, 100, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(60, y);
            M5.Lcd.printf("%ldm %lds", elapsed/60000, (elapsed/1000)%60);
        }
        y += 16;
        
        // Hashes
        float hashesM = local_hashes / 1000000.0;
        drawIfChanged(fancyUI.totalHashes, hashesM, 1, 80, y, WHITE, "%.2fM");
        
        // us/hash
        if (local_hashes > 0 && elapsed > 0) {
            float usPerHash = (elapsed * 1000.0) / local_hashes;
            if (abs(fancyUI.usPerHash - usPerHash) > 0.1f) {
                fancyUI.usPerHash = usPerHash;
                M5.Lcd.fillRect(200, y, 50, 8, BLACK);
                M5.Lcd.setTextSize(1);
                M5.Lcd.setTextColor(DARKGREY);
                M5.Lcd.setCursor(200, y);
                M5.Lcd.printf("%.1fus", usPerHash);
            }
        }
        y += 16;
        
        // Templates
        if (fancyUI.templates != local_templates) {
            fancyUI.templates = local_templates;
            M5.Lcd.fillRect(95, y, 60, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(95, y);
            M5.Lcd.printf("%ld", local_templates);
        }
        
        // Battery status
        int battLevel = M5.Axp.GetBatteryLevel();
        bool charging = M5.Axp.isCharging();
        if (fancyUI.battLevel != battLevel || fancyUI.charging != charging) {
            fancyUI.battLevel = battLevel;
            fancyUI.charging = charging;
            
            M5.Lcd.fillRect(200, y, 80, 8, BLACK);
            M5.Lcd.setTextSize(1);
            M5.Lcd.setCursor(200, y);
            
            if (charging) {
                M5.Lcd.setTextColor(GREEN);
                M5.Lcd.printf("CHG %d%%", battLevel);
            } else {
                M5.Lcd.setTextColor((battLevel > 50) ? GREEN : (battLevel > 20) ? YELLOW : RED);
                M5.Lcd.printf("BAT %d%%", battLevel);
            }
        }
        y += 16;
        
        // 16bit / 32bit shares
        drawIfChangedInt(fancyUI.halfshares, local_halfshares, 1, 50, y, WHITE);
        drawIfChangedInt(fancyUI.shares, local_shares, 1, 145, y, WHITE);
        y += 16;
        
        // Valid
        drawIfChangedInt(fancyUI.totalValids, totalValids, 1, 60, y, 
                        totalValids > 0 ? GREEN : RED);
        y += 24;  // Skip line
        
        // Balance
        //updateBTCBalance();
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(YELLOW);
        M5.Lcd.setCursor(40, y);
        M5.Lcd.print(btcBalance.c_str());
        y += 16;
        
        // BTC Price
        if (btcPrice > 0) {
            drawIfChanged(fancyUI.btcPrice, btcPrice, 1, 45, y, GREEN, "$%.0f");
        }
      }
        taskYIELD();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void setup(){
    M5.begin(true, true, true, true);

    if (DEBUG) {
        Serial.begin(115200);
        delay(100);
    }
    
    

    for (int i = 0; i < 6; i++) {
        miners[i].hashrate = 0;
        miners[i].shares = 0;
        miners[i].valids = 0;
        miners[i].temp = 0;
        miners[i].lastUpdate = 0;
        miners[i].online = false;
    }

    setCpuFrequencyMhz(240);
    
    statsMutex = xSemaphoreCreateMutex();

    disableCore0WDT();
    disableCore1WDT();

    
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("SOLO SWARM CLUSTER");
    M5.Lcd.drawLine(0, 35, 320, 35, GREEN);
    
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(10, 50);
    M5.Lcd.println("Cluster Mining Rig");
    M5.Lcd.setCursor(10, 70);
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.println("Core2 + 5x ESP32-S3");
    
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(10, 100);
    M5.Lcd.println("Connecting WiFi...");
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        M5.Lcd.print(".");
        attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        M5.Lcd.setCursor(10, 130);
        M5.Lcd.setTextColor(RED);
        M5.Lcd.println("WiFi FAILED!");
        while(1) delay(1000);
    }
    
    M5.Lcd.setCursor(10, 130);
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.println("Connected!");
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(10, 150);
    M5.Lcd.print("IP: ");
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.println(WiFi.localIP());
    
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(10, 170);
    M5.Lcd.print("Pool: ");
    M5.Lcd.setTextColor(CYAN);
    M5.Lcd.printf("%s:%d", POOL_URL, POOL_PORT);
    
    M5.Lcd.setTextColor(YELLOW);
    M5.Lcd.setCursor(10, 190);
    M5.Lcd.println("Listening for S3 miners...");

    delay(2000);
    if (ENABLE_WEB_SERVER) {
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(10, 210);
        M5.Lcd.println("Web server starting...");
        startTime = millis();
        //prefs.clear();
        //prefs.end();
        loadConfig();
        setupWebServer();
        startWebTask();
    }
    
    // Create background tasks with optimized priorities
    xTaskCreatePinnedToCore([](void* pvParameters){ runUDPListener(pvParameters, displayDirty, miners); }, "UDP", 8000, NULL, UDP_LISTENER_PRIORITY, NULL, 0);
    
    xTaskCreatePinnedToCore(runBackgroundTasks, "Background", 10000, NULL, BACKGROUND_PRIORITY, NULL, 0);
    
    if (WEB_SERVER_ONLY) {
      
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setTextColor(WHITE);
      M5.Lcd.setTextSize(2);
      M5.Lcd.setCursor(10, 10);
      M5.Lcd.println("SOLO SWARM CLUSTER");
      M5.Lcd.drawLine(0, 35, 320, 35, GREEN);
      
      M5.Lcd.setTextSize(1);
      M5.Lcd.setCursor(10, 50);
      M5.Lcd.println("Cluster Mining Rig");
      M5.Lcd.setCursor(10, 70);
      M5.Lcd.setTextColor(YELLOW);
      M5.Lcd.println("Core2 + 5x ESP32-S3");
      
      M5.Lcd.setTextColor(WHITE);
      M5.Lcd.setCursor(10, 100);
      M5.Lcd.println("Web Server Only Mode  Enabled");
      M5.Lcd.print("IP: " + WiFi.localIP().toString());
      M5.Lcd.setCursor(10, 120);
    }
    xTaskCreatePinnedToCore(runMonitor, "Monitor", 10000, NULL, MONITOR_PRIORITY, NULL, 0);
    
    delay(2000);
    
    // Mining tasks - highest priority for maximum hashrate
    if (CORES == 1) {
        if (THREADS == 1) {
            static BitcoinMiner miner1("M1", 0);
            xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
        }
        if (THREADS == 2) {
            static BitcoinMiner miner1("M1", 0);
            static BitcoinMiner miner2("M2", 0);
            xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
            xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
        }
        
    }else if (CORES == 2)
    {
        if (THREADS == 2) {
        static BitcoinMiner miner1("M1", 0);
        static BitcoinMiner miner2("M2", 1);
        xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, THREAD_PRIORITY, nullptr, 0);
        xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
      }
      if (THREADS == 3){
          static BitcoinMiner miner1("M1", 0);
          static BitcoinMiner miner2("M2", 1);
          static BitcoinMiner miner3("M3", 1);
          xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, THREAD_PRIORITY, nullptr, 0);
          xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
          xTaskCreatePinnedToCore([](void*){ miner3.start(); }, "M3", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
      }
      if (THREADS == 4) {
          static BitcoinMiner miner1("M1", 0);
          static BitcoinMiner miner2("M2", 1);
          static BitcoinMiner miner3("M3", 0);
          static BitcoinMiner miner4("M4", 1);
          xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, THREAD_PRIORITY, nullptr, 0);
          xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
          xTaskCreatePinnedToCore([](void*){ miner3.start(); }, "M3", 35000, nullptr, THREAD_PRIORITY, nullptr, 0);
          xTaskCreatePinnedToCore([](void*){ miner4.start(); }, "M4", 35000, nullptr, THREAD_PRIORITY, nullptr, 1);
      }
    }
    
    Serial.println("Monitor task created");
}

void loop(){
  delay(10000);
}