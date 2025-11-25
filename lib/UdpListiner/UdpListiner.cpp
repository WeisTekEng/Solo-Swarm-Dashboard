#include "configs.h"
#include "MiningCore.h"
#include <WiFiUDP.h>

WiFiUDP udp;

// Optimized: Reduced buffer checks and increased yield
void runUDPListener(void *name, volatile bool &displayDirty, MinerStats (&miners)[MAX_MINERS + 1]) {
    Serial.println("UDP Listener started on port 8888");
    udp.begin(8888);
    
    char buffer[64];
    unsigned long lastOfflineCheck = 0;
    
    while(1) {
        int packetSize = udp.parsePacket();
        if (packetSize) {
            int len = udp.read(buffer, sizeof(buffer) - 1);
            buffer[len] = 0;
            
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
                    displayDirty = true; // Mark display for update
                }
            }
        }
        #if false // this gets checked in the UI update.
        // Check offline status less frequently
        unsigned long now = millis();
        if (now - lastOfflineCheck > 5000) {
            lastOfflineCheck = now;
            for (int i = 0; i < 5; i++) {
                if (now - miners[i].lastUpdate > 10000) {
                    if (miners[i].online) {
                        miners[i].online = false;
                        displayDirty = true;
                    }
                }
            }
        }
        #endif
        
        vTaskDelay(250 / portTICK_PERIOD_MS); // Increased from 100ms
    }
}