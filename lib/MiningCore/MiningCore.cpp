// ============================================================================
// lib/MiningCore/MiningCore.cpp - SAFE VERSION
// ============================================================================
#include "MiningCore.h"

// ── ACTUAL DEFINITIONS ─────────────────────────────────────────────────
volatile long templates = 0;
volatile long hashes = 0;
volatile int halfshares = 0;
volatile int shares = 0;
volatile int valids = 0;
volatile bool blockFound = false;
volatile unsigned long blockFoundTime = 0;
SemaphoreHandle_t statsMutex = nullptr;

// ✅ SAFE: Keep your original to_byte_array (it works fine!)
int to_byte_array(const char* in, size_t in_size, uint8_t* out) {
    int count = 0;
    
    // Inline hex converter
    auto hex = [](char ch) -> uint8_t { 
        return (ch > '9') ? (ch - 'A' + 10) : (ch - '0'); 
    };
    
    if (in_size % 2) {
        while (*in && out) {
            *out = hex(*in++);
            if (!*in) break;
            *out = (*out << 4) | hex(*in++);
            out++;
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