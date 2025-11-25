
// ============================================================================
// BitcoinMiner.h - FULLY OPTIMIZED
// ============================================================================
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "sha256.h"
#include "MiningCore.h"

class BitcoinMiner {
public:
    BitcoinMiner(const char* name, uint8_t core);
    void start();

private:
    const char* workerName;
    uint8_t coreId;
    WiFiClient client;

    // ✅ Pre-allocated buffers to avoid String operations in hot path
    char job_id_buf[64];
    char extranonce1_buf[32];
    char extranonce2_buf[32];
    char ntime_buf[16];
    
    String job_id, extranonce1, ntime, extranonce2;  // Keep for compatibility
    
    TaskHandle_t shareTaskHandle = nullptr;

    void connectToPool(String host, uint16_t port);
    void subscribeAndAuth();
    bool getNewJob();
    void mineWithMidstate(uint8_t* target);
    void shareSubmissionTask();

    // ✅ Aligned memory for optimal access
    uint8_t blockheader[80] __attribute__((aligned(4)));
    uint8_t target[32] __attribute__((aligned(4)));
    SHA256_CTX midstate;
};