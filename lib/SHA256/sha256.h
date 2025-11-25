// sha256_optimized.h
#pragma once
#include <Arduino.h>

typedef struct {
    uint32_t state[8];
} SHA256_CTX;

// Optimized SHA256 for Bitcoin mining
void IRAM_ATTR sha256_init_state(uint32_t* state);
void IRAM_ATTR sha256_transform_first64(uint32_t* state, const uint8_t* data);
void IRAM_ATTR sha256_bitcoin_double(const uint8_t* data, size_t len, uint8_t* hash);

// Ultra-fast midstate mining
void IRAM_ATTR sha256_midstate_init(uint32_t* midstate, const uint8_t* header64);
bool IRAM_ATTR sha256_final_rounds_with_nonce(const uint32_t* midstate, uint32_t nonce, uint8_t* hash);