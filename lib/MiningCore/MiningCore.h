// ============================================================================
// lib/MiningCore/MiningCore.h - SAFE OPTIMIZED VERSION
// ============================================================================
#pragma once
#include <Arduino.h>
#include "configs.h"

// Global statistics
extern volatile long templates;
extern volatile long hashes;
extern volatile int halfshares;
extern volatile int shares;
extern volatile int valids;
extern volatile bool blockFound;
extern volatile unsigned long blockFoundTime;
extern SemaphoreHandle_t statsMutex;

// Cluster stats
struct MinerStats {
    float hashrate;
    int shares;
    int valids;
    float temp;
    unsigned long lastUpdate;
    bool online;
};

// Helper functions
int to_byte_array(const char* in, size_t in_size, uint8_t* out);

// ✅ SAFE: Keep original implementations but with const correctness
inline bool checkHalfShare(const uint8_t* hash) {
    return (*(const uint16_t*)(hash + 30) == 0);
}

inline bool checkShare(const uint8_t* hash) {
    return (*(const uint32_t*)(hash + 28) == 0);
}

// ✅ OPTIMIZED: Only optimize checkValid - it's called most often
inline bool checkValid(const uint8_t* hash, const uint8_t* target) {
    const uint32_t* h = (const uint32_t*)hash;
    const uint32_t* t = (const uint32_t*)target;
    
    // Unrolled comparison from most significant to least
    if (h[7] < t[7]) return true;
    if (h[7] > t[7]) return false;
    if (h[6] < t[6]) return true;
    if (h[6] > t[6]) return false;
    if (h[5] < t[5]) return true;
    if (h[5] > t[5]) return false;
    if (h[4] < t[4]) return true;
    if (h[4] > t[4]) return false;
    if (h[3] < t[3]) return true;
    if (h[3] > t[3]) return false;
    if (h[2] < t[2]) return true;
    if (h[2] > t[2]) return false;
    if (h[1] < t[1]) return true;
    if (h[1] > t[1]) return false;
    if (h[0] < t[0]) return true;
    if (h[0] > t[0]) return false;
    return true;
}