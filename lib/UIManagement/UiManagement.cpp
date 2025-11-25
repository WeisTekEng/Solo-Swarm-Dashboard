#include <M5Core2.h>
#include <cmath>

void drawIfChanged(float &cached, float value, int size, int x, int y, 
                   uint16_t color, const char* fmt = "%.2f", float eps = 0.01f)
{
    if (isnan(value)) return;
    
    if (fabsf(cached - value) > eps) {
        cached = value;
        
        // ✅ FIXED: Precise clearing based on actual text size
        int clearW = size * 6 * 6;  // Assume ~6 chars max (e.g., "123.45")
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        // Draw new value
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf(fmt, value);
    }
}

void drawIfChangedInt(int &cached, int value, int size, int x, int y, uint16_t color)
{
    if (cached != value) {
        cached = value;
        
        // ✅ FIXED: Precise clearing for integers
        int clearW = size * 6 * 4;  // Assume ~4 chars max (e.g., "9999")
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf("%d", value);
    }
}

// ✅ NEW: Special version for temperature (includes "C" suffix)
void drawIfChangedTemp(float &cached, float value, int size, int x, int y, uint16_t color)
{
    if (isnan(value)) return;
    
    if (fabsf(cached - value) > 0.5f) {  // 0.5°C threshold
        cached = value;
        
        // ✅ Clear area for "XXC" (3 chars)
        int clearW = size * 6 * 3;
        int clearH = size * 8;
        
        M5.Lcd.fillRect(x, y, clearW, clearH, BLACK);
        
        M5.Lcd.setTextSize(size);
        M5.Lcd.setTextColor(color);
        M5.Lcd.setCursor(x, y);
        M5.Lcd.printf("%.0fC", value);
    }
}

void drawBar(int x, int y, int width, int height, float percent, uint16_t color) {
    if (width <= 2 || height <= 2) return;
    
    percent = constrain(percent, 0.0f, 1.0f);
    
    M5.Lcd.drawRect(x, y, width, height, DARKGREY);
    
    int innerW = width - 2;
    int innerH = height - 2;
    int fillWidth = (int)(innerW * percent);
    
    if (fillWidth <= 0) return;
    if (fillWidth > innerW) fillWidth = innerW;
    
    // Clear old bar completely
    M5.Lcd.fillRect(x + 1, y + 1, innerW, innerH, BLACK);
    
    // Draw new bar
    M5.Lcd.fillRect(x + 1, y + 1, fillWidth, innerH, color);
}