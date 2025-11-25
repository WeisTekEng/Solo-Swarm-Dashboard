#include "Arduino.h"
#include "configs.h"


struct ClusterUICache {
    float totalHashrate = -1;
    int onlineCount = -1;
    float core2Hashrate = -1;
    float cachedTemp = -1;
    int totalShares = -1;
    int totalValids = -1;
    bool initialized = false;  // Track if static UI is drawn
    float btcPrice = -1;
    char btcBalance[32] = "";
    unsigned long lastUpdate = -1;
    unsigned long lastTempRead = -1;
    unsigned long elapsed = -1;
};

struct StatsUICache {
    float totalHashrate = -1;
    unsigned long elapsed = 0;
    float totalHashes = -1;
    long templates = -1;
    int totalShares = -1;
    int totalValids = -1;
    float btcPrice = -1;
    bool initialized = false;
    bool ipDrawn;
};

struct FancyUICache {
    int rssi = 0;
    int bars = -1;
    float cachedTemp = -1;
    int progress = -1;
    float totalHashrate = -1;
    unsigned long elapsed = 0;
    float totalHashes = -1;
    float usPerHash = -1;
    long templates = -1;
    int battLevel = -1;
    bool charging = false;
    int halfshares = -1;
    int shares = -1;
    int totalValids = -1;
    float btcPrice = -1;
    bool initialized = false;
};

struct MinerUICache {
    float hashrate = -1;
    float temp = -1;
    bool online = false;
    bool lastOnlineState = false;  // Track state changes
};

volatile int displayMode = 0;
volatile bool displayDirty = true;

