#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include "mbedtls/md.h"
#include "configs.h"

// Global statistics
volatile long templates = 0;
volatile long hashes = 0;
volatile int halfshares = 0;
volatile int shares = 0;
volatile int valids = 0;
volatile bool blockFound = false;
volatile unsigned long blockFoundTime = 0;

// Cached data for display (updated by background task)
String btcBalance = "Loading...";
float btcPrice = 0;

// Display mode: 0 = Cluster View, 1 = Stats View, 2 = Fancy View
volatile int displayMode = 0;

// Cluster stats (from S3 miners via UDP)
struct MinerStats {
    float hashrate;
    int shares;
    int valids;
    float temp;
    unsigned long lastUpdate;
    bool online;
};

MinerStats miners[6];  // 0-4 = S3 miners, 5 = Core2 itself
WiFiUDP udp;

// Mutex for thread-safe statistics updates
SemaphoreHandle_t statsMutex;

// Inline functions for speed
inline bool checkHalfShare(unsigned char* hash) {
  return (*(uint16_t*)(hash + 30) == 0);
}

inline bool checkShare(unsigned char* hash) {
  return (*(uint32_t*)(hash + 28) == 0);
}

inline bool checkValid(unsigned char* hash, unsigned char* target) {
  uint32_t* h = (uint32_t*)hash;
  uint32_t* t = (uint32_t*)target;
  
  for(int8_t i = 7; i >= 0; i--) {
    if(h[i] > t[i]) return false;
    if(h[i] < t[i]) return true;
  }
  return true;
}

inline uint8_t hex(char ch) {
    return (ch > 57) ? (ch - 55) : (ch - 48);
}

int to_byte_array(const char *in, size_t in_size, uint8_t *out) {
    int count = 0;
    if (in_size % 2) {
        while (*in && out) {
            *out = hex(*in++);
            if (!*in) return count;
            *out = (*out << 4) | hex(*in++);
            *out++;
            count++;
        }
    } else {
        while (*in && out) {
            *out++ = (hex(*in++) << 4) | hex(*in++);
            count++;
        }
    }
    return count;
}

void runWorker(void *name) {
  Serial.printf("\nRunning %s on core %d\n", (char *)name, xPortGetCoreID());
  
  setCpuFrequencyMhz(240);
  
  byte interResult[32] __attribute__((aligned(4)));
  byte shaResult[32] __attribute__((aligned(4)));
  
  while(true) { 
    WiFiClient client;
    client.setTimeout(10000);
    client.setNoDelay(true);
    
    Serial.printf("%s: Connecting to %s:%d\n", (char*)name, POOL_URL, POOL_PORT);
    if (!client.connect(POOL_URL, POOL_PORT)) {
      Serial.printf("%s: Connection failed!\n", (char*)name);
      delay(5000);
      continue;
    }
    Serial.printf("%s: Connected!\n", (char*)name);

    xSemaphoreTake(statsMutex, portMAX_DELAY);
    templates++;
    xSemaphoreGive(statsMutex);
    
    DynamicJsonDocument doc(4096);
    String payload;
    String line;
    
    payload = "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}\n";
    Serial.printf("%s: Sending subscribe\n", (char*)name);
    client.print(payload);
    line = client.readStringUntil('\n');
    Serial.printf("%s: Received: %s\n", (char*)name, line.c_str());
    
    if (deserializeJson(doc, line)) {
      Serial.printf("%s: JSON parse error on subscribe\n", (char*)name);
      client.stop();
      delay(5000);
      continue;
    }
    
    String sub_details = String((const char*) doc["result"][0][0][1]);
    String extranonce1 = String((const char*) doc["result"][1]);
    int extranonce2_size = doc["result"][2];
    
    Serial.printf("%s: extranonce1=%s, extranonce2_size=%d\n", (char*)name, extranonce1.c_str(), extranonce2_size);
    
    line = client.readStringUntil('\n');
    Serial.printf("%s: Received difficulty: %s\n", (char*)name, line.c_str());
    deserializeJson(doc, line);

    payload = "{\"params\":[\"" + String(ADDRESS) + "\",\"password\"],\"id\":2,\"method\":\"mining.authorize\"}\n";
    Serial.printf("%s: Sending authorize\n", (char*)name);
    client.print(payload);
    line = client.readStringUntil('\n');
    Serial.printf("%s: Auth response: %s\n", (char*)name, line.c_str());
    deserializeJson(doc, line);
    
    String job_id = String((const char*) doc["params"][0]);
    String prevhash = String((const char*) doc["params"][1]);
    String coinb1 = String((const char*) doc["params"][2]);
    String coinb2 = String((const char*) doc["params"][3]);
    JsonArray merkle_branch = doc["params"][4];
    String version = String((const char*) doc["params"][5]);
    String nbits = String((const char*) doc["params"][6]);
    String ntime = String((const char*) doc["params"][7]);
    
    Serial.printf("%s: Job ID=%s, nbits=%s\n", (char*)name, job_id.c_str(), nbits.c_str());
    
    line = client.readStringUntil('\n');
    Serial.printf("%s: Extra line: %s\n", (char*)name, line.c_str());
    line = client.readStringUntil('\n');
    Serial.printf("%s: Extra line: %s\n", (char*)name, line.c_str());

    String target = nbits.substring(2);
    int zeros = (int) strtol(nbits.substring(0, 2).c_str(), 0, 16) - 3;
    for (int k = 0; k < zeros; k++) target += "00";
    while(target.length() < 64) target = "0" + target;
    
    Serial.printf("%s: Target: %s\n", (char*)name, target.c_str());
    Serial.printf("%s: Starting mining...\n", (char*)name);
    
    uint8_t bytearray_target[32] __attribute__((aligned(4)));
    to_byte_array(target.c_str(), 64, bytearray_target);
    
    for (size_t j = 0; j < 16; j++) {
        uint8_t tmp = bytearray_target[j];
        bytearray_target[j] = bytearray_target[31 - j];
        bytearray_target[31 - j] = tmp;
    }

    uint32_t extranonce2_a = esp_random();
    uint32_t extranonce2_b = esp_random();
    char extranonce2[17];
    snprintf(extranonce2, sizeof(extranonce2), "%08x%08x", extranonce2_a, extranonce2_b);

    String coinbase = coinb1 + extranonce1 + String(extranonce2) + coinb2;
    size_t str_len = coinbase.length() / 2;
    uint8_t* bytearray_coinbase = (uint8_t*)malloc(str_len);
    to_byte_array(coinbase.c_str(), str_len * 2, bytearray_coinbase);

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);

    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, bytearray_coinbase, str_len);
    mbedtls_md_finish(&ctx, interResult);

    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, interResult, 32);
    mbedtls_md_finish(&ctx, shaResult);

    free(bytearray_coinbase);

    byte merkle_result[32] __attribute__((aligned(4)));
    memcpy(merkle_result, shaResult, 32);
    
    byte merkle_concatenated[64] __attribute__((aligned(4)));
    for (size_t k = 0; k < merkle_branch.size(); k++) {
        const char* merkle_element = (const char*) merkle_branch[k];
        uint8_t bytearray_merkle[32];
        to_byte_array(merkle_element, 64, bytearray_merkle);

        memcpy(merkle_concatenated, merkle_result, 32);
        memcpy(merkle_concatenated + 32, bytearray_merkle, 32);
            
        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, merkle_concatenated, 64);
        mbedtls_md_finish(&ctx, interResult);

        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, interResult, 32);
        mbedtls_md_finish(&ctx, merkle_result);
    }
    
    char merkle_root[65];
    for (int i = 0; i < 32; i++) {
      snprintf(merkle_root + (i * 2), 3, "%02x", merkle_result[i]);
    }

    String blockheader = version + prevhash + String(merkle_root) + nbits + ntime + "00000000";
    uint8_t bytearray_blockheader[80] __attribute__((aligned(4)));
    to_byte_array(blockheader.c_str(), 160, bytearray_blockheader);
    
    for (size_t j = 0; j < 2; j++) {
        uint8_t tmp = bytearray_blockheader[j];
        bytearray_blockheader[j] = bytearray_blockheader[3 - j];
        bytearray_blockheader[3 - j] = tmp;
    }
    
    for (size_t j = 0; j < 16; j++) {
        uint8_t tmp = bytearray_blockheader[36 + j];
        bytearray_blockheader[36 + j] = bytearray_blockheader[67 - j];
        bytearray_blockheader[67 - j] = tmp;
    }
    
    for (size_t j = 0; j < 2; j++) {
        uint8_t tmp = bytearray_blockheader[72 + j];
        bytearray_blockheader[72 + j] = bytearray_blockheader[75 - j];
        bytearray_blockheader[75 - j] = tmp;
    }

    uint32_t nonce = 0;
    const uint32_t BATCH_SIZE = 1000;
    const uint32_t REPORT_INTERVAL = 100000;
    uint32_t local_hashes = 0;
    
    Serial.printf("%s: Mining started, target difficulty\n", (char*)name);
    
    while(nonce < MAX_NONCE) {
      for(uint32_t i = 0; i < BATCH_SIZE && nonce < MAX_NONCE; i++, nonce++) {
        bytearray_blockheader[76] = nonce & 0xFF;
        bytearray_blockheader[77] = (nonce >> 8) & 0xFF;
        bytearray_blockheader[78] = (nonce >> 16) & 0xFF;
        bytearray_blockheader[79] = (nonce >> 24) & 0xFF;

        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, bytearray_blockheader, 80);
        mbedtls_md_finish(&ctx, interResult);

        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, interResult, 32);
        mbedtls_md_finish(&ctx, shaResult);

        local_hashes++;

        if(checkHalfShare(shaResult)) {
          xSemaphoreTake(statsMutex, portMAX_DELAY);
          halfshares++;
          xSemaphoreGive(statsMutex);
          
          if(checkShare(shaResult)) {
            xSemaphoreTake(statsMutex, portMAX_DELAY);
            shares++;
            xSemaphoreGive(statsMutex);
          }
        }
        
        if(checkValid(shaResult, bytearray_target)) {
          Serial.println("\n\n========================================");
          Serial.println("    VALID BLOCK FOUND!!!");
          Serial.println("========================================");
          Serial.printf("Worker: %s on core %d\n", (char*)name, xPortGetCoreID());
          Serial.printf("Nonce: %u (0x%08x)\n", nonce, nonce);
          Serial.printf("Job ID: %s\n", job_id.c_str());
          Serial.print("Block hash: ");
          for (size_t i = 0; i < 32; i++) Serial.printf("%02x", shaResult[31-i]);
          Serial.println();
          Serial.println("========================================\n");
          
          xSemaphoreTake(statsMutex, portMAX_DELAY);
          valids++;
          blockFound = true;
          blockFoundTime = millis();
          xSemaphoreGive(statsMutex);
          
          char nonceHex[9];
          snprintf(nonceHex, 9, "%08x", nonce);
          payload = "{\"params\":[\"" + String(ADDRESS) + "\",\"" + job_id + 
                    "\",\"" + String(extranonce2) + "\",\"" + ntime + 
                    "\",\"" + String(nonceHex) + "\"],\"id\":1,\"method\":\"mining.submit\"}\n";
          Serial.print("Submitting to pool: "); Serial.println(payload);
          client.print(payload);
          line = client.readStringUntil('\n');
          Serial.print("Pool response: "); Serial.println(line);
          
          for(int flash = 0; flash < 5; flash++) {
            M5.Lcd.fillScreen(GREEN);
            delay(200);
            M5.Lcd.fillScreen(BLACK);
            delay(200);
          }
          
          nonce = MAX_NONCE;
          break;
        }
      }
      
      xSemaphoreTake(statsMutex, portMAX_DELAY);
      hashes += local_hashes;
      xSemaphoreGive(statsMutex);
      
      if (nonce % REPORT_INTERVAL == 0 && nonce > 0) {
        Serial.printf("%s: %u hashes done (%.2f%%)\n", (char*)name, nonce, (nonce * 100.0) / MAX_NONCE);
        taskYIELD();
      }
      
      local_hashes = 0;
    }
    
    if (local_hashes > 0) {
      xSemaphoreTake(statsMutex, portMAX_DELAY);
      hashes += local_hashes;
      xSemaphoreGive(statsMutex);
    }
    
    Serial.printf("%s: Finished mining job, getting new job...\n", (char*)name);
    
    mbedtls_md_free(&ctx);
    client.stop();
  }
}

void runUDPListener(void *name) {
    Serial.println("UDP Listener started on port 8888");
    udp.begin(8888);
    
    while(1) {
        int packetSize = udp.parsePacket();
        if (packetSize) {
            char buffer[64];
            int len = udp.read(buffer, sizeof(buffer) - 1);
            buffer[len] = 0;
            
            // Parse: "ID,HASHRATE,SHARES,VALIDS,TEMP"
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
                    
                    Serial.printf("S3-%d: %.2f KH/s, %d shares, %.1f°C\n", id, hashrate, shares_val, temp);
                }
            }
        }
        
        // Check for offline miners
        unsigned long now = millis();
        for (int i = 0; i < 5; i++) {
            if (now - miners[i].lastUpdate > 10000) {
                miners[i].online = false;
            }
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void runBackgroundTasks(void *name) {
  Serial.println("Background task started - waiting 10s...");
  vTaskDelay(10000 / portTICK_PERIOD_MS);
  
  Serial.println("Background task: Starting first API call");
  
  while(1) {
    // Check BTC price - QUICK operation
    Serial.println("Fetching BTC price...");
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(5000); // 5 second timeout
    
    if (client.connect("api.coinbase.com", 443)) {
      client.print("GET /v2/prices/BTC-USD/spot HTTP/1.1\r\n"
                  "Host: api.coinbase.com\r\n"
                  "Connection: close\r\n\r\n");
      
      unsigned long timeout = millis();
      String response = "";
      while (millis() - timeout < 3000) { // Max 3 seconds
        if (client.available()) {
          response += (char)client.read();
        }
        if (response.indexOf("\r\n\r\n") > 0 && response.indexOf("}") > 0) break;
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to miners
      }
      
      int jsonStart = response.indexOf("{");
      if (jsonStart > 0) {
        String jsonStr = response.substring(jsonStart);
        DynamicJsonDocument doc(1024);
        if (!deserializeJson(doc, jsonStr)) {
          btcPrice = doc["data"]["amount"].as<float>();
          Serial.printf("BTC Price: $%.2f\n", btcPrice);
        } else {
          Serial.println("Price JSON parse failed");
        }
      }
      client.stop();
    } else {
      Serial.println("Failed to connect to Coinbase API");
    }
    
    // Yield heavily to mining
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    // Check balance - QUICK operation
    Serial.println("Fetching balance...");
    Serial.printf("Address: %s\n", ADDRESS);
    
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
      String response = "";
      while (millis() - timeout < 3000) { // Max 3 seconds
        if (client.available()) {
          response += (char)client.read();
        }
        if (response.indexOf("\r\n\r\n") > 0 && response.length() > 100) break;
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to miners
      }
      
      int bodyStart = response.indexOf("\r\n\r\n");
      if (bodyStart > 0) {
        String body = response.substring(bodyStart + 4);
        body.trim();
        
        if (body.indexOf("error") > 0 || body.indexOf("invalid") > 0) {
          Serial.println("Balance API returned error");
          btcBalance = "Invalid Addr";
        } else {
          long satoshis = body.toInt();
          float btc = satoshis / 100000000.0;
          
          if (satoshis == 0 && body != "0") {
            btcBalance = "Check Addr";
          } else if (btc > 0) {
            btcBalance = String(btc, 8) + " BTC";
            Serial.printf("Balance: %s (%ld sats)\n", btcBalance.c_str(), satoshis);
          } else {
            btcBalance = "0 BTC";
            Serial.println("Balance: 0 BTC");
          }
        }
      } else {
        Serial.println("No response body");
        btcBalance = "No Response";
      }
      client.stop();
    } else {
      Serial.println("Failed to connect to blockchain.info");
      btcBalance = "Conn Failed";
    }
    
    // Now SLEEP for 10 minutes - gives CPU back to mining!
    Serial.println("Background: Done, sleeping 10 min...");
    vTaskDelay(600000 / portTICK_PERIOD_MS); // 10 minutes
  }
}

void runMonitor(void *name) {
  unsigned long start = millis();
  unsigned long lastUpdate = 0;
  
  Serial.println("Monitor task started");
  
  while (1) {
    unsigned long now = millis();
    
    M5.update();
    if (M5.BtnA.wasPressed()) {
      displayMode = (displayMode + 1) % 3;
      String modeName = displayMode == 0 ? "Cluster View" : displayMode == 1 ? "Stats View" : "Fancy View";
      Serial.printf("Display mode: %s\n", modeName.c_str());
      lastUpdate = 0;
    }
    
    int updateInterval = 2000;
    
    if (now - lastUpdate < updateInterval) {
      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }
    lastUpdate = now;
    
    unsigned long elapsed = now - start;
    
    xSemaphoreTake(statsMutex, portMAX_DELAY);
    long local_hashes = hashes;
    long local_templates = templates;
    int local_halfshares = halfshares;
    int local_shares = shares;
    int local_valids = valids;
    bool local_blockFound = blockFound;
    unsigned long local_blockTime = blockFoundTime;
    xSemaphoreGive(statsMutex);
    
    float core2_hashrate = (elapsed > 0) ? (1.0 * local_hashes) / elapsed : 0.0;
    
    miners[5].hashrate = core2_hashrate;
    miners[5].shares = local_shares;
    miners[5].valids = local_valids;
    miners[5].temp = temperatureRead();
    miners[5].online = true;
    
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
    
    if (displayMode == 0) {
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
                
                int barWidth = (int)(miners[i].hashrate / 30.0 * 150);
                M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
                M5.Lcd.fillRect(101, y + 1, barWidth, 8, GREEN);
                
                uint16_t tempColor = WHITE;
                if (miners[i].temp > 70) tempColor = RED;
                else if (miners[i].temp > 60) tempColor = YELLOW;
                else tempColor = CYAN;
                
                M5.Lcd.setTextColor(tempColor);
                M5.Lcd.setCursor(260, y);
                M5.Lcd.printf("%.0fC", miners[i].temp);
            } else {
                M5.Lcd.setTextColor(DARKGREY);
                M5.Lcd.printf("S3-%d", i + 1);
                M5.Lcd.setCursor(50, y);
                M5.Lcd.print("OFFLINE");
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
        M5.Lcd.printf("%.1f", core2_hashrate);
        
        int barWidth = (int)(core2_hashrate / 30.0 * 150);
        M5.Lcd.drawRect(100, y, 150, 10, DARKGREY);
        M5.Lcd.fillRect(101, y + 1, barWidth, 8, YELLOW);
        
        uint16_t tempColor = WHITE;
        float temp = miners[5].temp;
        if (temp > 70) tempColor = RED;
        else if (temp > 60) tempColor = YELLOW;
        else tempColor = CYAN;
        
        M5.Lcd.setTextColor(tempColor);
        M5.Lcd.setCursor(260, y);
        M5.Lcd.printf("%.0fC", temp);
        
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
    
    // ========== STATS VIEW ==========
    else if (displayMode == 1) {
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
    
    // ========== FANCY VIEW (Detailed with all features) ==========
    else {
        // Draw WiFi signal strength indicator (top right)
        int rssi = WiFi.RSSI();
        int bars = 0;
        if (rssi > -55) bars = 4;
        else if (rssi > -65) bars = 3;
        else if (rssi > -75) bars = 2;
        else if (rssi > -85) bars = 1;
        
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
        
        // Show signal strength as dBm (small text)
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(wifiColor);
        M5.Lcd.setCursor(255, 14);
        M5.Lcd.printf("%ddB", rssi);
        
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setTextSize(2);
        
        // Title - smaller to save space
        M5.Lcd.setCursor(5, 5);
        M5.Lcd.println("SOLO SWARM CLUSTER");
        int headerWidth = M5.Lcd.textWidth("SOLO SWARM CLUSTER");
        int headerHeight = 16;
        
        // Show temperature if available
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(CYAN);
        M5.Lcd.setCursor(225, 14);
        float temp = temperatureRead();
        if (temp > 70) M5.Lcd.setTextColor(RED);
        else if (temp > 60) M5.Lcd.setTextColor(YELLOW);
        else M5.Lcd.setTextColor(CYAN);
        M5.Lcd.printf("%.0fC", temp);
        
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.drawLine(0, 28, 320, 28, GREEN);
        
        // Progress bar
        int progress = min(100, (int)((totalHashrate / 200.0) * 100));
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
        
        // Power efficiency (estimate ~0.5W per board * 6)
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
        
        // Average hash time
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
        
        // Battery percentage
        M5.Lcd.setTextColor(DARKGREY);
        M5.Lcd.setCursor(200, y);
        int battLevel = M5.Axp.GetBatteryLevel();
        bool charging = M5.Axp.isCharging();
        if (charging) {
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.printf("CHG %d%%", battLevel);
        } else if (battLevel > 50) {
            M5.Lcd.setTextColor(GREEN);
            M5.Lcd.printf("BAT %d%%", battLevel);
        } else if (battLevel > 20) {
            M5.Lcd.setTextColor(YELLOW);
            M5.Lcd.printf("BAT %d%%", battLevel);
        } else {
            M5.Lcd.setTextColor(RED);
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
    
    Serial.printf("Cluster: %.2f KH/s | %d/%d online\n", totalHashrate, onlineCount, 6);
  }
}

void setup(){
  M5.begin(true, true, true, true);
  
  Serial.begin(115200);
  delay(100);

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

  xTaskCreatePinnedToCore(runUDPListener, "UDP", 8000, NULL, 4, NULL, 1);
  Serial.println("UDP listener created");
  
  xTaskCreatePinnedToCore(runBackgroundTasks, "Background", 10000, NULL, 1, NULL, 1);
  Serial.println("Background task created (priority 1)");
  
  for (size_t i = 0; i < THREADS; i++) {
    char *name = (char*) malloc(32);
    sprintf(name, "Worker[%d]", i);
    BaseType_t core = (i % 2);
    xTaskCreatePinnedToCore(runWorker, name, 35000, (void*)name, 1, NULL, core);
    Serial.printf("Starting %s on core %d\n", name, core);
  }

  xTaskCreatePinnedToCore(runMonitor, "Monitor", 10000, NULL, 2, NULL, 1);
  Serial.println("Monitor task created");
}

void loop(){
  delay(10000);
}