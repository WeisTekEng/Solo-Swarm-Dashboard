# Solo-Swarm-Dashboard
A Lottery ticket M5Stack S2 Dashboard for the Solo Swarm repo
The Solo Swarm Dashboard keeps tabs on all your Solo Swarm Miners while
also providing its own hashrate.

### M5Stack S2 SOLO SWARM Dashboard
### **Cluster View**
```
┌─────────────────────────────────┐
│  SOLO SWARM CLUSTER             │
│  168.5 KH/s | 6 miners          │
├─────────────────────────────────┤
│ S3-1  28.1 ████████████  72°C   │
│ S3-2  28.3 ████████████  71°C   │
│ S3-3  27.9 ████████████  73°C   │
│ S3-4  28.0 ████████████  70°C   │
│ S3-5  28.2 ████████████  72°C   │
├─────────────────────────────────┤
│ Core2 23.8 ███████████   68°C   │
├─────────────────────────────────┤
│ Shares: 42 | Valid: 0           │
│ Pool: solo.ckpool.org:3333      │
└─────────────────────────────────┘
  BtnA: Stats
```
Features:

Real-time view of all miners
Individual hashrate bars (green for S3, yellow for Core2)
Temperature color-coded (cyan/yellow/red)
Shows "OFFLINE" if miner stops
Total cluster hashrate at top
Perfect for monitoring cluster health

### **Cluster Statistics**
```
┌─────────────────────────────────┐
│  CLUSTER STATISTICS             │
├─────────────────────────────────┤
│ Total: 168.50 KH/s              │
│ Core2: 25m 34s                  │
│ Hashes: 35.8M                   │
│ Templates: 12                   │
│ Total Shares: 42                │
│ Valid: 0                        │
├─────────────────────────────────┤
│ Bal: 0 BTC                      │
│ BTC: $95432                     │
│ IP: 192.168.1.100               │
│ Pool: solo.ckpool.org:3333      │
└─────────────────────────────────┘
  BtnA: Detailed View
```
Features:

Aggregate cluster statistics
Core2 uptime & total hashes
Template count
Combined shares from all miners
Valid blocks found (cluster-wide)
Bitcoin balance & current price
Network info
Clean, text-focused layout

### **Detailed View**
```
┌─────────────────────────────────┐
│  HAN SOLO   68°C    ▂▄▆█ -62dB│
├─────────────────────────────────┤
│ [████████████████░░░░░░] 84%    │
│                                 │
│ Cluster: 168.50 KH/s    56K H/J │
│ Core2: 25m 34s                  │
│ Hashes: 35.8M           54.2us  │
│ Templates: 12          CHG 85%  │
│ 16bit: 2 | 32bit: 0             │
│ Valid: 0                        │
├─────────────────────────────────┤
│ Bal: 0 BTC                      │
│ BTC: $95432                     │
│ solo.ckpool.org:3333            │
└─────────────────────────────────┘
  BtnA: Cluster View
```

```
**Features:**
- **Top right:** WiFi signal bars + signal strength (dBm)
- **Top left:** CPU temperature (color-coded)
- **Progress bar:** Visual cluster hashrate indicator
- **Cluster hashrate** with power efficiency (H/J)
- **Core2 runtime** and uptime
- **Hash statistics** with average hash time (μs)
- **Battery status:** CHG/BAT with percentage (color-coded)
- **Share breakdown:** 16-bit and 32-bit shares
- **Valid blocks:** Cluster-wide valid block count
- **Balance & Price:** BTC balance and current USD price
- **Pool info:** Current mining pool
- All metrics with secondary info on the right

---
```

## **Button Flow:**
```
Press A → Press A → Press A → (loops)
Cluster   Stats     Detailed     Cluster...
```

### When to Use Each Mode:
Mode | Best For | Update Speed |
Cluster | Quick health check of all miners | Every 2s
Stats |  Tracking overall progress & earnings |Every 2s
Detailed | Deep dive with all sensors active | Every 2s

### Color Coding:
## **Temperature:**

🔵 Cyan: Cool (<60°C)
🟡 Yellow: Warm (60-70°C)
🔴 Red: Hot (>70°C)

## **WiFi Signal:**

🟢 Green: Excellent (4 bars, >-55dBm)
🟡 Yellow: Good (2-3 bars, -65 to -75dBm)
🔴 Red: Poor (1 bar, <-85dBm)

## **Battery:**

🟢 Green: >50% or Charging
🟡 Yellow: 20-50%
🔴 Red: <20%

All three modes update in real-time and show live data from your mining cluster
