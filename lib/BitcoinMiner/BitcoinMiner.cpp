// BitcoinMiner.cpp - ULTRA OPTIMIZED
#include "BitcoinMiner.h"
#include "configs.h"
#include <M5Core2.h>

// Queue for submitting shares without blocking mining
struct ShareSubmission {
    String job_id;
    String extranonce2;
    String ntime;
    uint32_t nonce;
    bool valid;
};

static QueueHandle_t shareQueue = nullptr;

BitcoinMiner::BitcoinMiner(const char* name, uint8_t core)
    : workerName(name), coreId(core) {
    if (shareQueue == nullptr) {
        shareQueue = xQueueCreate(10, sizeof(ShareSubmission));
    }
}

void BitcoinMiner::start() {
    setCpuFrequencyMhz(240);
    
    while (true) {
        connectToPool(POOL_URL, POOL_PORT);
        subscribeAndAuth();
        
        // Start share submission task
        xTaskCreatePinnedToCore(
            [](void* param) {
                BitcoinMiner* miner = (BitcoinMiner*)param;
                miner->shareSubmissionTask();
            },
            "ShareSubmit",
            4096,
            this,
            1,
            &shareTaskHandle,
            coreId
        );

        while (client.connected()) {
            if (getNewJob()) {
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                templates++;
                xSemaphoreGive(statsMutex);
                mineWithMidstate(target);
            }
        }
        
        if (shareTaskHandle) {
            vTaskDelete(shareTaskHandle);
            shareTaskHandle = nullptr;
        }
        
        client.stop();
        delay(3000);
    }
}

void BitcoinMiner::shareSubmissionTask() {
    ShareSubmission sub;
    while (true) {
        if (xQueueReceive(shareQueue, &sub, portMAX_DELAY)) {
            char nonceHex[9];
            snprintf(nonceHex, 9, "%08x", sub.nonce);
            String payload = "{\"id\":9,\"method\":\"mining.submit\",\"params\":[\"";
            payload += String(ADDRESS) + "\",\"" + sub.job_id + "\",\"";
            payload += sub.extranonce2 + "\",\"" + sub.ntime + "\",\"" + nonceHex + "\"]}\n";
            client.print(payload);
            
            if (sub.valid) {
                // Block found celebration
                for (int f = 0; f < 15; f++) {
                    M5.Lcd.fillScreen(GREEN); 
                    vTaskDelay(80 / portTICK_PERIOD_MS);
                    M5.Lcd.fillScreen(BLACK); 
                    vTaskDelay(80 / portTICK_PERIOD_MS);
                }
            }
        }
    }
}

void BitcoinMiner::connectToPool(const char* host, uint16_t port) {
    client.setTimeout(10000);
    client.setNoDelay(true);
    while (!client.connect(host, port)) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void BitcoinMiner::subscribeAndAuth() {
    DynamicJsonDocument doc(4096);
    String line;

    client.print("{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}\n");
    line = client.readStringUntil('\n');
    deserializeJson(doc, line);
    extranonce1 = doc["result"][1].as<String>();

    client.readStringUntil('\n');
    client.print("{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\"" + String(ADDRESS) + "\",\"x\"]}\n");
    client.readStringUntil('\n');
}

bool BitcoinMiner::getNewJob() {
    String line = client.readStringUntil('\n');
    if (line.indexOf("mining.notify") == -1) return false;

    DynamicJsonDocument doc(4096);
    deserializeJson(doc, line);
    JsonArray params = doc["params"];

    job_id      = params[0].as<String>();
    String prevhash = params[1].as<String>();
    String coinb1   = params[2].as<String>();
    String coinb2   = params[3].as<String>();
    JsonArray merkle_branch = params[4];
    String version  = params[5].as<String>();
    String nbits    = params[6].as<String>();
    ntime           = params[7].as<String>();

    uint32_t r1 = esp_random(), r2 = esp_random();
    char en2[17];
    snprintf(en2, sizeof(en2), "%08x%08x", r1, r2);
    extranonce2 = en2;

    String coinbase_hex = coinb1 + extranonce1 + extranonce2 + coinb2;
    uint8_t coinbase_bin[512];
    size_t cb_len = to_byte_array(coinbase_hex.c_str(), coinbase_hex.length(), coinbase_bin);

    uint8_t merkle_root[32];
    sha256_bitcoin_double(coinbase_bin, cb_len, merkle_root);

    for (size_t i = 0; i < merkle_branch.size(); i++) {
        uint8_t branch[32];
        to_byte_array(merkle_branch[i].as<const char*>(), 64, branch);
        uint8_t tmp[64];
        memcpy(tmp, merkle_root, 32);
        memcpy(tmp + 32, branch, 32);
        sha256_bitcoin_double(tmp, 64, merkle_root);
    }

    String header_hex = version + prevhash;
    for (int i = 0; i < 32; i++) {
        char buf[3];
        sprintf(buf, "%02x", merkle_root[i]);
        header_hex += buf;
    }
    header_hex += nbits + ntime + "00000000";

    to_byte_array(header_hex.c_str(), 160, blockheader);

    for (int j = 0; j < 2; j++) std::swap(blockheader[j], blockheader[3-j]);
    for (int j = 0; j < 16; j++) std::swap(blockheader[36+j], blockheader[67-j]);
    for (int j = 0; j < 2; j++) std::swap(blockheader[72+j], blockheader[75-j]);

    String t = nbits.substring(2);
    int zeros = (int)strtol(nbits.substring(0,2).c_str(), nullptr, 16) - 3;
    for (int k = 0; k < zeros; k++) t += "00";
    while (t.length() < 64) t = "0" + t;
    to_byte_array(t.c_str(), 64, target);
    for (int j = 0; j < 16; j++) std::swap(target[j], target[31-j]);

    sha256_midstate_init(midstate.state, blockheader);
    return true;
}

void BitcoinMiner::mineWithMidstate(uint8_t* target) {
    uint32_t nonce = 0;
    uint32_t local_hashes = 0;
    uint8_t hash[32] __attribute__((aligned(4)));
    
    // CRITICAL: Much larger batch for maximum speed
    //const uint32_t BATCH = 50000;
    const uint32_t REPORT_INTERVAL = 500000;
    
    // Cache connection status - only check occasionally
    bool is_connected = true;
    uint32_t last_connection_check = 0;

    while (nonce < 0xFFFFFFFF && is_connected) {
        // TIGHT INNER LOOP - NO BRANCHES
        for (uint32_t i = 0; i < BATCH && nonce < 0xFFFFFFFF; i++, nonce++) {
            sha256_final_rounds_with_nonce(midstate.state, nonce, hash);
            local_hashes++;

            // Fast path: most hashes fail all checks
            // Only check if last bytes are promising
            uint32_t tail = *(uint32_t*)(hash + 28);
            
            if (tail == 0) {  // 32-bit share or better!
                // Update stats WITHOUT network I/O
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                halfshares++;
                shares++;
                xSemaphoreGive(statsMutex);
                
                // Queue share for async submission
                ShareSubmission sub;
                sub.job_id = job_id;
                sub.extranonce2 = extranonce2;
                sub.ntime = ntime;
                sub.nonce = nonce;
                sub.valid = checkValid(hash, target);
                
                if (sub.valid) {
                    xSemaphoreTake(statsMutex, portMAX_DELAY);
                    valids++;
                    blockFound = true;
                    blockFoundTime = millis();
                    xSemaphoreGive(statsMutex);
                }
                
                xQueueSend(shareQueue, &sub, 0);  // Non-blocking
                
                if (sub.valid) {
                    return;  // Exit on valid block
                }
            } else if ((tail & 0x0000FFFF) == 0) {  // 16-bit half-share
                xSemaphoreTake(statsMutex, portMAX_DELAY);
                halfshares++;
                xSemaphoreGive(statsMutex);
            }
        }

        // Update global counter
        xSemaphoreTake(statsMutex, portMAX_DELAY);
        hashes += local_hashes;
        xSemaphoreGive(statsMutex);
        local_hashes = 0;
        
        // Check connection only every 500k hashes (~15 seconds)
        if (nonce - last_connection_check > REPORT_INTERVAL) {
            is_connected = client.connected();
            last_connection_check = nonce;
            taskYIELD();  // Brief yield for other tasks
        }
    }
}