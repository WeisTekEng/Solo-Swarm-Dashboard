// lib/BitcoinMiner/BitcoinMiner.h
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "sha256.h"
#include "MiningCore.h"

#define ULTRA_FAST_MODE
#ifdef ULTRA_FAST_MODE
    const uint32_t BATCH = 100000;  // Even bigger batches
    #define CHECK_CONNECTION_INTERVAL 1000000  // Check less often
#endif

class BitcoinMiner {
public:
    BitcoinMiner(const char* name, uint8_t core);
    void start();

private:
    const char* workerName;
    uint8_t coreId;
    WiFiClient client;

    String job_id, extranonce1, ntime, extranonce2;
    
    // Task handle for async share submission
    TaskHandle_t shareTaskHandle = nullptr;

    void connectToPool(const char* host, uint16_t port);
    void subscribeAndAuth();
    bool getNewJob();
    void mineWithMidstate(uint8_t* target);
    void shareSubmissionTask();  // New method for async share submission

    uint8_t blockheader[80] __attribute__((aligned(4)));
    uint8_t target[32] __attribute__((aligned(4)));
    SHA256_CTX midstate;
};