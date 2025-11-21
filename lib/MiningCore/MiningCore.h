// lib/MiningCore/MiningCore.h
#pragma once
#include <Arduino.h>

// Global statistics
extern volatile long templates;
extern volatile long hashes;
extern volatile int halfshares;
extern volatile int shares;
extern volatile int valids;
extern volatile bool blockFound;
extern volatile unsigned long blockFoundTime;
extern SemaphoreHandle_t statsMutex;

// Helper functions
int to_byte_array(const char* in, size_t in_size, uint8_t* out);

inline bool checkHalfShare(unsigned char* hash) {
    return (*(uint16_t*)(hash + 30) == 0);
}

inline bool checkShare(unsigned char* hash) {
    return (*(uint32_t*)(hash + 28) == 0);
}

inline bool checkValid(unsigned char* hash, unsigned char* target) {
    uint32_t* h = (uint32_t*)hash;
    uint32_t* t = (uint32_t*)target;
    for (int8_t i = 7; i >= 0; i--) {
        if (h[i] > t[i]) return false;
        if (h[i] < t[i]) return true;
    }
    return true;
}