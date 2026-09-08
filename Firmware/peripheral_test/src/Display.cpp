#include "Display.h"

Display::Display()
    : panel_(GxEPD2_290_GDEY029T94(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY))
{
}

void Display::begin()
{
    pinMode(PIN_SD_ON, OUTPUT);
    digitalWrite(PIN_SD_ON, LOW); // TPS22929D load switch off - SD unpowered

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH); // deselected

    panel_.init(0); // 0 = don't let GxEPD2 re-run Serial.begin()
    panel_.setRotation(3); // landscape

    fonts_.begin(panel_);
    fonts_.setFontMode(1); // transparent - don't paint the glyph background
    fonts_.setForegroundColor(GxEPD_BLACK);
    fonts_.setBackgroundColor(GxEPD_WHITE);
}
