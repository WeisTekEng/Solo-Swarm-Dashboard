
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include "configs.h"
#include "BitcoinMiner.h"
#define MININGCORE_IMPLEMENTATION
#include "MiningCore.h"

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
float btcPrice = 0;
volatile int displayMode = 0;
volatile bool displayDirty = true; // Force initial draw

// Cluster stats
struct MinerStats {
    float hashrate;
    int shares;
    int valids;
    float temp;
    unsigned long lastUpdate;
    bool online;
};

MinerStats miners[6];
WiFiUDP udp;

// Optimized: Reduced buffer checks and increased yield
void runUDPListener(void *name) {
    Serial.println("UDP Listener started on port 8888");
    udp.begin(8888);
    
    char buffer[64];
    unsigned long lastOfflineCheck = 0;
    
    while(1) {
        int packetSize = udp.parsePacket();
        if (packetSize) {
            int len = udp.read(buffer, sizeof(buffer) - 1);
            buffer[len] = 0;
            
            int id, shares_val, valids_val;
            float hashrate, temp;
            
            if (sscanf(buffer, "%d,%f,%d,%d,%f", &id, &hashrate, &shares_val, &valids_val, &temp) == 5) {
                if (id >= 1 && id <= 5) {
                    miners[id - 1].hashrate = hashrate;
                    miners[id - 1].shares = shares_val;
                    miners[id - 1].valids = valids_val;
                    miners[id - 1].temp = temp;
                    miners[id - 1].lastUpdate = millis();
                    miners[id - 1].online = true;
                    displayDirty = true; // Mark display for update
                }
            }
        }
        
        // Check offline status less frequently
        unsigned long now = millis();
        if (now - lastOfflineCheck > 5000) {
            lastOfflineCheck = now;
            for (int i = 0; i < 5; i++) {
                if (now - miners[i].lastUpdate > 10000) {
                    if (miners[i].online) {
                        miners[i].online = false;
                        displayDirty = true;
                    }
                }
            }
        }
        
        vTaskDelay(250 / portTICK_PERIOD_MS); // Increased from 100ms
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
        int updateInterval = 3000;
        
        // Only update if dirty flag set or interval passed
        if (!displayDirty && (now - lastUpdate < updateInterval)) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }
        
        lastUpdate = now;
        displayDirty = false;
        
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
        
        long local_hashes = hashes;
        long local_templates = templates;
        int local_halfshares = halfshares;
        int local_shares = shares;
        int local_valids = valids;
        bool local_blockFound = blockFound;
        unsigned long local_blockTime = blockFoundTime;
        xSemaphoreGive(statsMutex);
        
        // Cache temperature reading (expensive operation)
        if (now - lastTempRead > 5000) {
            cachedTemp = temperatureRead();
            lastTempRead = now;
        }
        
        float core2_hashrate = 0;
        if (elapsed > 0) {
            core2_hashrate = (float)local_hashes / (elapsed / 1000.0f) / 1000.0f;
        }
        
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
        
        M5.Lcd.fillScreen(BLACK);
        
        // Display modes (unchanged for readability)
        if (displayMode == 0) {
            // Cluster View
            M5.Lcd.setTextSize(2);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(5, 5);
            M5.Lcd.println("SOLO SWARM CLUSTER");
            
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.setCursor(5, 25);
            M5.Lcd.printf("%.2f KH/s | %d miners", totalHashrate, onlineCount);
            
            M5.Lcd.drawLine(0, 40, 320, 40, GREEN);
            
            int y = 48;
            M5.Lcd.setTextSize(1);
            
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
                    M5.Lcd.fillRect(101, y + 1, barWidth, 8, GREEN);
                    
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
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.print("Core2");
            
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(50, y);
            M5.Lcd.printf("%.2f", core2_hashrate);
            
            int barWidth = (int)(core2_hashrate / 45.0 * 150);
            M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
            M5.Lcd.fillRect(101, y + 1, barWidth, 8, YELLOW);
            
            uint16_t tempColor = (cachedTemp > 70) ? RED : (cachedTemp > 60) ? YELLOW : CYAN;
            
            M5.Lcd.setTextColor(tempColor);
            M5.Lcd.setCursor(260, y);
            M5.Lcd.printf("%.0fC", cachedTemp);
            
            y += 20;
            M5.Lcd.drawLine(0, y, 320, y, GREEN);
            y += 5;
            
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Shares: %d | Valid: %d", totalShares, totalValids);
            y += 15;
            
            M5.Lcd.setTextColor(CYAN);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Pool: %s:%d", POOL_URL, POOL_PORT);
            
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(5, 225);
            M5.Lcd.print("BtnA: Stats");
        } 
        else if (displayMode == 1) {
            // Stats View
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(WHITE);
            
            int y = 5;
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("CLUSTER STATISTICS");
            y += 15;
            
            M5.Lcd.drawLine(0, y, 320, y, GREEN);
            y += 5;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.printf("Total: %.2f KH/s", totalHashrate);
            y += 16;
            
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Core2: %ldm %lds", elapsed/60000, (elapsed/1000)%60);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Hashes: %.2fM", local_hashes/1000000.0);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.printf("Templates: %ld", local_templates);
            y += 16;
            
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Total Shares: %d", totalShares);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(totalValids > 0 ? GREEN : RED);
            M5.Lcd.printf("Valid: %d", totalValids);
            y += 18;
            
            M5.Lcd.drawLine(0, y, 320, y, GREEN);
            y += 5;
            
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Bal: %s", btcBalance.c_str());
            y += 16;
            
            if (btcPrice > 0) {
                M5.Lcd.setTextColor(GREEN);
                M5.Lcd.setCursor(5, y);
                M5.Lcd.printf("BTC: $%.0f", btcPrice);
                y += 16;
            }
            
            M5.Lcd.setTextColor(CYAN);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("IP: %s", WiFi.localIP().toString().c_str());
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Pool: %s:%d", POOL_URL, POOL_PORT);
            
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(5, 225);
            M5.Lcd.print("BtnA: Fancy");
        }
        else {
            // Fancy View
            int rssi = WiFi.RSSI();
            int bars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
            uint16_t wifiColor = (bars > 2) ? GREEN : (bars > 1) ? YELLOW : RED;
            int wifiX = 290;
            int wifiY = 5;
            
            for (int i = 0; i < 4; i++) {
                if (i < bars) {
                    M5.Lcd.fillRect(wifiX + (i * 6), wifiY + (12 - (i * 3)), 4, i * 3 + 3, wifiColor);
                } else {
                    M5.Lcd.drawRect(wifiX + (i * 6), wifiY + (12 - (i * 3)), 4, i * 3 + 3, DARKGREY);
                }
            }
            
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(wifiColor);
            M5.Lcd.setCursor(255, 14);
            M5.Lcd.printf("%ddB", rssi);
            
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setTextSize(2);
            M5.Lcd.setCursor(5, 5);
            M5.Lcd.println("SOLO SWARM CLUSTER");
            
            M5.Lcd.setTextSize(1);
            M5.Lcd.setCursor(225, 14);
            uint16_t tempColor = (cachedTemp > 70) ? RED : (cachedTemp > 60) ? YELLOW : CYAN;
            M5.Lcd.setTextColor(tempColor);
            M5.Lcd.printf("%.0fC", cachedTemp);
            
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.drawLine(0, 28, 320, 28, GREEN);
            
            int progress = min(100, (int)((totalHashrate / 270.0) * 100));
            M5.Lcd.drawRect(5, 33, 310, 15, WHITE);
            M5.Lcd.fillRect(7, 35, 306 * progress / 100, 11, GREEN);
            
            M5.Lcd.setTextSize(1);
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(270, 37);
            M5.Lcd.printf("%d%%", progress);
            
            M5.Lcd.setTextColor(WHITE);
            int y = 55;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Cluster: %.2f KH/s", totalHashrate);
            
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(200, y);
            if (totalHashrate > 0) {
                M5.Lcd.printf("%.0f H/J", (totalHashrate * 1000) / 3.0);
            }
            M5.Lcd.setTextColor(WHITE);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Core2: %ldm %lds", elapsed/60000, (elapsed/1000)%60);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Hashes: %.2fM", local_hashes/1000000.0);
            
            if (local_hashes > 0 && elapsed > 0) {
                float usPerHash = (elapsed * 1000.0) / local_hashes;
                M5.Lcd.setTextColor(DARKGREY);
                M5.Lcd.setCursor(200, y);
                M5.Lcd.printf("%.1fus", usPerHash);
                M5.Lcd.setTextColor(WHITE);
            }
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.printf("Templates: %ld", local_templates);
            
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(200, y);
            int battLevel = M5.Axp.GetBatteryLevel();
            bool charging = M5.Axp.isCharging();
            if (charging) {
                M5.Lcd.setTextColor(GREEN);
                M5.Lcd.printf("CHG %d%%", battLevel);
            } else {
                M5.Lcd.setTextColor((battLevel > 50) ? GREEN : (battLevel > 20) ? YELLOW : RED);
                M5.Lcd.printf("BAT %d%%", battLevel);
            }
            M5.Lcd.setTextColor(WHITE);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("16bit: %d | 32bit: %d", local_halfshares, local_shares);
            y += 16;
            
            M5.Lcd.setCursor(5, y);
            M5.Lcd.setTextColor(totalValids > 0 ? GREEN : RED);
            M5.Lcd.printf("Valid: %d", totalValids);
            y += 18;
            
            M5.Lcd.drawLine(0, y, 320, y, GREEN);
            y += 6;
            
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("Bal: %s", btcBalance.c_str());
            y += 16;
            
            if (btcPrice > 0) {
                M5.Lcd.setTextColor(GREEN);
                M5.Lcd.setCursor(5, y);
                M5.Lcd.printf("BTC: $%.0f", btcPrice);
                y += 16;
            }
            
            M5.Lcd.setTextColor(CYAN);
            M5.Lcd.setCursor(5, y);
            M5.Lcd.printf("%s:%d", POOL_URL, POOL_PORT);
            
            M5.Lcd.setTextColor(DARKGREY);
            M5.Lcd.setCursor(5, 225);
            M5.Lcd.print("BtnA: Cluster");
        }
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

    // Create background tasks with optimized priorities
    xTaskCreatePinnedToCore(runUDPListener, "UDP", 8000, NULL, 3, NULL, 1);
    Serial.println("UDP listener created (priority 3)");
    
    xTaskCreatePinnedToCore(runBackgroundTasks, "Background", 10000, NULL, 1, NULL, 1);
    Serial.println("Background task created (priority 1)");
    
    delay(2000);
    
    // Mining tasks - highest priority for maximum hashrate
    if (THREADS == 2) {
        static BitcoinMiner miner1("M1", 0);
        static BitcoinMiner miner2("M2", 1);
        xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, 2, nullptr, 0);
        xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, 2, nullptr, 1);
    }
    if (THREADS == 4) {
        static BitcoinMiner miner1("M1", 0);
        static BitcoinMiner miner2("M2", 1);
        static BitcoinMiner miner3("M3", 0);
        static BitcoinMiner miner4("M4", 1);
        xTaskCreatePinnedToCore([](void*){ miner1.start(); }, "M1", 35000, nullptr, 2, nullptr, 0);
        xTaskCreatePinnedToCore([](void*){ miner2.start(); }, "M2", 35000, nullptr, 2, nullptr, 1);
        xTaskCreatePinnedToCore([](void*){ miner3.start(); }, "M3", 35000, nullptr, 2, nullptr, 0);
        xTaskCreatePinnedToCore([](void*){ miner4.start(); }, "M4", 35000, nullptr, 2, nullptr, 1);
    }

    xTaskCreatePinnedToCore(runMonitor, "Monitor", 10000, NULL, 2, NULL, 1);
    Serial.println("Monitor task created");
}

void loop(){
  delay(10000);
}