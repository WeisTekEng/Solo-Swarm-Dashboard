
#ifndef CONFIGS_H
#define CONFIGS_H

#include <cstdint>

// Wifi
static const char* WIFI_SSID = "Lanyard-WRT"; // 2.5Ghz
static const char* WIFI_PASSWORD = "aS137946285!";

// Mining
static int NUMBER_OF_MINERS = 5;
static int CORES = 2;
static int THREADS = 4;
static const int THREAD_PRIORITY = 3; // High priority for mining threads
static const int MONITOR_PRIORITY = 3; // Medium priority for monitor
static const int BACKGROUND_PRIORITY = 1; // Low priority for background tasks
static const int UDP_LISTENER_PRIORITY = 4; // High priority for UDP listener
static const int MONITOR_UPDATE_INTERVAL_MS = 5000; // Monitor update interval
static const unsigned long MAX_NONCE = 0xFFFFFFFFUL;
static const char* ADDRESS = "bc1qpe8gjgfs5hh0aw7veusxqppycyz0ea0nvjxr3k";

// Web Server
static const bool ENABLE_WEB_SERVER = false;
static const int WEB_SERVER_PRIORITY = 4; // highest priority for web server
static bool WEB_SERVER_ONLY = false;

// Pool
static const char* POOL_URL = "solo.ckpool.org";
static uint16_t POOL_PORT = 3333;
static bool DEBUG = true;

// Variables
// Small epsilon for float comparisons
static constexpr float DEFAULT_EPS = 0.01f;
static constexpr int MAX_MINERS = 8; // compile-time capacity (ensure >= expected runtime NUMBER_OF_MINERS)

#endif // CONFIGS_H
