/*
 * NanoPage peripheral bring-up test.
 *
 * Exercises every peripheral on the board except the microSD card:
 *   - PCF85063ATL RTC (I2C 0x51)
 *   - MAX17055 fuel gauge (I2C 0x36)
 *   - BQ25170 Li-Po charger (status/power-good GPIOs, no I2C)
 *   - Action / Next / Prev tactile buttons
 *   - GDEY029T94 e-paper panel (SSD1680, SPI)
 *
 * Prints an I2C bus scan and chip identification once at boot. After that,
 * the RTC/fuel-gauge/charger/USB peripherals are only read right before a
 * screen redraw (every DISPLAY_UPDATE_MS in loop()), never on some faster
 * background cadence, and loop() spends the time between redraws blocked
 * in delay(), which puts the MCU into FreeRTOS's tickless-idle low-power
 * sleep (WFE / sd_app_evt_wait under the hood) instead of busy-polling.
 * Buttons and the RTC-INT/fuel-ALERT lines are wired as GPIOTE interrupts
 * (deferred to task context) so their edges are still debounced and
 * reported immediately over the USB CDC serial monitor at 115200 baud,
 * independent of whatever loop() is doing.
 *
 * Also does one-time bring-up init, skipped on later boots once each
 * chip reports it's already configured:
 *   - Seeds the RTC from this firmware's build time if its OS
 *     (oscillator-stop) flag is set, i.e. it's never been set before.
 *   - Runs the MAX17055's EZ Config sequence if its POR flag is set.
 *     FUEL_DESIGN_CAPACITY_MAH below must match the battery in use.
 *
 * The e-paper panel shows a live RTC/fuel-gauge/charger/USB snapshot,
 * redrawn (full refresh, ~1.75s) every DISPLAY_UPDATE_MS in loop() -
 * deliberately slow, since e-paper shouldn't be driven with frequent
 * full refreshes; that same cadence now doubles as the peripheral
 * sampling rate.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <string.h>

// ---- PCF85063ATL RTC -------------------------------------------------
static const uint8_t RTC_ADDR = 0x51;
static const uint8_t RTC_REG_CONTROL1 = 0x00;
static const uint8_t RTC_REG_SECONDS = 0x04; // .. 0x0A = Years

// ---- MAX17055 fuel gauge ----------------------------------------------
static const uint8_t FUEL_ADDR = 0x36;
static const uint8_t FUEL_REG_STATUS = 0x00;    // bit1 = POR
static const uint8_t FUEL_REG_REPSOC = 0x06;    // 1/256 %/LSB
static const uint8_t FUEL_REG_VCELL = 0x09;     // 78.125uV/LSB
static const uint8_t FUEL_REG_CURRENT = 0x0A;   // signed, 1.5625uV/RSENSE per LSB
static const uint8_t FUEL_REG_DESIGNCAP = 0x18; // 5.0uVh/RSENSE per LSB
static const uint8_t FUEL_REG_MODELCFG = 0x1D;
static const uint8_t FUEL_REG_ICHGTERM = 0x1E; // same LSB as Current
static const uint8_t FUEL_REG_DEVNAME = 0x21;  // expected ~0x4010
static const uint8_t FUEL_REG_VEMPTY = 0x3A;
static const uint8_t FUEL_REG_FSTAT = 0x3D;   // bit0 = DNR
static const uint8_t FUEL_REG_HIBCFG = 0xBA;
static const uint8_t FUEL_REG_COMMAND = 0x60;

// Current-sense resistor on the fuel gauge's CSP/CSN inputs.
static const float FUEL_RSENSE_OHMS = 0.010f;
static const float FUEL_CURRENT_MA_PER_LSB = 1.5625f / FUEL_RSENSE_OHMS / 1000.0f;
static const float FUEL_CAPACITY_MAH_PER_LSB = 5.0f / (FUEL_RSENSE_OHMS * 1000.0f);

// EZ Config inputs - the board has a generic 2-pin JST battery connector,
// so this can't be read from the schematic. Set to whatever pack is
// actually plugged in for the test.
static const float FUEL_DESIGN_CAPACITY_MAH = 320.0f;
static const uint16_t FUEL_VEMPTY_MV = 3300; // empty-under-load target
static const uint16_t FUEL_VRECOVERY_MV = 3880;

// nRF52840 UICR->NFCPINS, see docs/bootloader.md step 4. Bit0 = 1 means
// P0.09/P0.10 are still latched as NFC antenna pins, not GPIO, which
// would make the Action/Next buttons below read garbage.
#define UICR_NFCPINS (*(volatile uint32_t *)0x1000120CUL)

// ---- GDEY029T94 e-paper panel (SSD1680) --------------------------------
// Full-height single buffer (128x296/8 = 4736 bytes) - plenty of RAM on
// the nRF52840, no need for GxEPD2's paged/partial-buffer drawing.
GxEPD2_BW<GxEPD2_290_GDEY029T94, GxEPD2_290_GDEY029T94::HEIGHT> display(
    GxEPD2_290_GDEY029T94(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

struct DebouncedInput
{
  uint8_t pin;
  const char *name;
  bool activeLow;
  bool state;
};

// Order matters: setup() wires edgeCallbacks[i] to inputs[i] by index.
static DebouncedInput inputs[] = {
    {PIN_BTN_PREV, "PREV button", true, false},
    {PIN_BTN_ACTION, "ACTION button", true, false},
    {PIN_BTN_NEXT, "NEXT button", true, false},
    {PIN_RTC_INT, "RTC ~INT", true, false},
    {PIN_FUEL_ALERT, "Fuel gauge ALRT", true, false},
    {PIN_USB_DETECT, "USB detect (power mux ST)", false, false},
};
static const size_t NUM_INPUTS = sizeof(inputs) / sizeof(inputs[0]);
static const uint32_t DEBOUNCE_MS = 25;

static uint8_t bcd2bin(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }

static bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(addr, n) != n)
    return false;
  for (uint8_t i = 0; i < n; i++)
    buf[i] = Wire.read();
  return true;
}

static bool i2cWriteRegs(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t n)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(buf, n);
  return Wire.endTransmission() == 0;
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value)
{
  return i2cWriteRegs(addr, reg, &value, 1);
}

static bool max17055ReadReg(uint8_t reg, uint16_t *out)
{
  uint8_t buf[2];
  if (!i2cReadRegs(FUEL_ADDR, reg, buf, 2))
    return false;
  *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8); // LSB first
  return true;
}

static bool max17055WriteReg(uint8_t reg, uint16_t value)
{
  uint8_t buf[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)}; // LSB first
  return i2cWriteRegs(FUEL_ADDR, reg, buf, 2);
}

static void scanI2CBus()
{
  Serial.println(F("I2C scan:"));
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++)
  {
    if (i2cPresent(addr))
    {
      Serial.print(F("  found device at 0x"));
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0)
    Serial.println(F("  (nothing responded - check I2C wiring/pull-ups)"));
}

static void identifyRtc()
{
  if (!i2cPresent(RTC_ADDR))
  {
    Serial.println(F("RTC: NOT FOUND at 0x51"));
    return;
  }
  uint8_t control1 = 0;
  i2cReadRegs(RTC_ADDR, RTC_REG_CONTROL1, &control1, 1);
  Serial.print(F("RTC: PCF85063ATL present, Control_1=0x"));
  Serial.println(control1, HEX);
}

static void identifyFuelGauge()
{
  if (!i2cPresent(FUEL_ADDR))
  {
    Serial.println(F("Fuel gauge: NOT FOUND at 0x36"));
    return;
  }
  uint16_t devName = 0;
  max17055ReadReg(FUEL_REG_DEVNAME, &devName);
  Serial.print(F("Fuel gauge: MAX17055 present, DevName=0x"));
  Serial.print(devName, HEX);
  Serial.println(devName == 0x4010 ? F(" (matches datasheet)") : F(" (unexpected, but chip did ACK)"));
}

static uint8_t bin2bcd(uint8_t bin) { return ((bin / 10) << 4) | (bin % 10); }

static uint8_t monthFromCompileDate(const char *date)
{
  static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (uint8_t i = 0; i < 12; i++)
    if (strncmp(date, names + i * 3, 3) == 0)
      return i + 1;
  return 1;
}

// Zeller's congruence, Gregorian calendar. Returns 0=Sunday .. 6=Saturday,
// matching the PCF85063's Weekdays register convention.
static uint8_t dayOfWeek(uint16_t year, uint8_t month, uint8_t day)
{
  static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3)
    year--;
  return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

// If the RTC has never had its time set (OS flag still set from power-on),
// seed it from this firmware's build time so it reads something sane.
// Leaves an already-running clock alone on subsequent boots.
static void seedRtcFromCompileTimeIfNeeded()
{
  uint8_t secondsReg = 0;
  if (!i2cReadRegs(RTC_ADDR, RTC_REG_SECONDS, &secondsReg, 1))
    return;
  if (!(secondsReg & 0x80))
  {
    Serial.println(F("RTC: OS flag already clear, leaving the running clock as-is"));
    return;
  }

  uint8_t month = monthFromCompileDate(__DATE__);
  uint8_t day = (__DATE__[4] == ' ' ? 0 : (__DATE__[4] - '0') * 10) + (__DATE__[5] - '0');
  uint16_t year = (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 +
                   (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
  uint8_t hour = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
  uint8_t minute = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');
  uint8_t second = (__TIME__[6] - '0') * 10 + (__TIME__[7] - '0');
  uint8_t weekday = dayOfWeek(year, month, day);

  uint8_t regs[7] = {
      bin2bcd(second), bin2bcd(minute), bin2bcd(hour),
      bin2bcd(day), weekday, bin2bcd(month), bin2bcd((uint8_t)(year % 100)),
  };

  i2cWriteReg(RTC_ADDR, RTC_REG_CONTROL1, 0x20); // STOP=1 while loading time
  i2cWriteRegs(RTC_ADDR, RTC_REG_SECONDS, regs, sizeof(regs));
  i2cWriteReg(RTC_ADDR, RTC_REG_CONTROL1, 0x00); // STOP=0, resume counting

  Serial.println(F("RTC: OS flag was set, seeded clock from this firmware's "
                    "build time (approximate - doesn't account for "
                    "flashing/reset delay)."));
}

// One-time MAX17055 "EZ Config" init (no OCV characterization table),
// following Maxim's documented POR sequence. Only runs while the
// fuel gauge's Status.POR flag is set, i.e. since the last time it lost
// power - a configured gauge is left alone on subsequent boots.
static void initFuelGaugeIfNeeded()
{
  uint16_t status = 0;
  if (!max17055ReadReg(FUEL_REG_STATUS, &status))
    return;
  if (!(status & 0x0002))
  {
    Serial.println(F("Fuel gauge: POR flag clear, already configured, leaving as-is"));
    return;
  }

  Serial.println(F("Fuel gauge: POR detected, running EZ Config init..."));

  uint16_t fstat = 0;
  uint32_t waitStart = millis();
  do
  {
    max17055ReadReg(FUEL_REG_FSTAT, &fstat);
    if (!(fstat & 0x0001))
      break;
    delay(10);
  } while (millis() - waitStart < 1000);

  uint16_t hibCfg = 0;
  max17055ReadReg(FUEL_REG_HIBCFG, &hibCfg);
  max17055WriteReg(FUEL_REG_COMMAND, 0x90); // exit hibernate, step 1
  max17055WriteReg(FUEL_REG_HIBCFG, 0x0);
  max17055WriteReg(FUEL_REG_COMMAND, 0x0); // exit hibernate, step 3

  uint16_t designCapReg = (uint16_t)(FUEL_DESIGN_CAPACITY_MAH / FUEL_CAPACITY_MAH_PER_LSB + 0.5f);
  float ichgTermMa = FUEL_DESIGN_CAPACITY_MAH * 0.1f; // 0.1C termination current
  uint16_t ichgTermReg = (uint16_t)(ichgTermMa / FUEL_CURRENT_MA_PER_LSB + 0.5f);
  uint16_t vEmptyReg = ((FUEL_VEMPTY_MV / 10) << 7) | (FUEL_VRECOVERY_MV / 40);

  max17055WriteReg(FUEL_REG_DESIGNCAP, designCapReg);
  max17055WriteReg(FUEL_REG_ICHGTERM, ichgTermReg);
  max17055WriteReg(FUEL_REG_VEMPTY, vEmptyReg);
  // Refresh=1, ModelID=0 (default Li-ion), VChg=0 (4.2V charge), R100=0
  // (10mOhm RSENSE is in the normal, non-doubled-LSB range).
  max17055WriteReg(FUEL_REG_MODELCFG, 0x8000);

  uint16_t modelCfg = 0x8000;
  waitStart = millis();
  do
  {
    max17055ReadReg(FUEL_REG_MODELCFG, &modelCfg);
    if (!(modelCfg & 0x8000))
      break;
    delay(10);
  } while (millis() - waitStart < 1000);

  max17055WriteReg(FUEL_REG_HIBCFG, hibCfg);

  max17055ReadReg(FUEL_REG_STATUS, &status);
  max17055WriteReg(FUEL_REG_STATUS, status & ~0x0002); // clear POR

  Serial.print(F("Fuel gauge: EZ Config done - DesignCap=0x"));
  Serial.print(designCapReg, HEX);
  Serial.print(F(" ("));
  Serial.print(FUEL_DESIGN_CAPACITY_MAH, 0);
  Serial.print(F(" mAh) IChgTerm=0x"));
  Serial.print(ichgTermReg, HEX);
  Serial.print(F(" VEmpty=0x"));
  Serial.println(vEmptyReg, HEX);
}

// One-time setup for the e-paper panel. SPI is shared with the microSD
// card, so its CS/power-enable are driven inactive first to keep it off
// the bus.
static void initDisplay()
{
  pinMode(PIN_SD_ON, OUTPUT);
  digitalWrite(PIN_SD_ON, LOW); // TPS22929D load switch off - SD unpowered

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH); // deselected

  Serial.println(F("Display: initializing GDEY029T94..."));
  // 0 = don't let GxEPD2 re-run Serial.begin() for its own diagnostics;
  // Serial here is USB CDC and already set up in setup() above.
  display.init(0);
  display.setRotation(3); // landscape, flipped 180 from the panel's native orientation
}

static void printTwoDigits(Print &out, uint8_t v)
{
  if (v < 10)
    out.print('0');
  out.print(v);
}

// Defined below - forward-declared so updateDisplayStatus() can log the
// same snapshot it draws, without reading any peripheral a second time.
static void printRtcStatus(const uint8_t regs[7], bool ok);
static void printFuelGaugeStatus(uint16_t vcellRaw, uint16_t socRaw, uint16_t currentRaw, bool ok);
static void printChargerStatus(bool powerGood, bool charging);

// Redraws the panel with a fresh snapshot of RTC/fuel-gauge/charger/USB
// state, then hibernates it again. A full refresh takes ~1.75s and
// visibly flashes, so this is called on a slow cadence (see loop()),
// not every time the serial status line updates.
static void updateDisplayStatus()
{
  uint8_t rtcRegs[7];
  bool rtcOk = i2cReadRegs(RTC_ADDR, RTC_REG_SECONDS, rtcRegs, sizeof(rtcRegs));
  uint16_t vcellRaw = 0, socRaw = 0, currentRaw = 0;
  bool fuelOk = max17055ReadReg(FUEL_REG_VCELL, &vcellRaw) &&
                max17055ReadReg(FUEL_REG_REPSOC, &socRaw) &&
                max17055ReadReg(FUEL_REG_CURRENT, &currentRaw);
  bool powerGood = digitalRead(PIN_CHG_PG) == LOW;
  bool charging = digitalRead(PIN_CHG_STAT) == LOW;
  bool usbSelected = digitalRead(PIN_USB_DETECT) == HIGH;

  Serial.println(F("--- status ---"));
  printRtcStatus(rtcRegs, rtcOk);
  printFuelGaugeStatus(vcellRaw, socRaw, currentRaw, fuelOk);
  printChargerStatus(powerGood, charging);

  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(4, 16);
    display.print(F("NanoPage"));

    display.setFont(); // revert to the built-in 6x8 font - more room for data
    display.setTextSize(1);
    int16_t y = 34;
    const int16_t lineHeight = 12;

    display.setCursor(4, y);
    display.print(F("RTC  "));
    if (rtcOk)
    {
      display.print(F("20"));
      printTwoDigits(display, bcd2bin(rtcRegs[6]));
      display.print('-');
      printTwoDigits(display, bcd2bin(rtcRegs[5] & 0x1F));
      display.print('-');
      printTwoDigits(display, bcd2bin(rtcRegs[3] & 0x3F));
      display.print(' ');
      printTwoDigits(display, bcd2bin(rtcRegs[2] & 0x3F));
      display.print(':');
      printTwoDigits(display, bcd2bin(rtcRegs[1] & 0x7F));
      display.print(':');
      printTwoDigits(display, bcd2bin(rtcRegs[0] & 0x7F));
      if (rtcRegs[0] & 0x80)
        display.print(F(" !"));
    }
    else
      display.print(F("not found"));
    y += lineHeight;

    display.setCursor(4, y);
    display.print(F("Batt "));
    if (fuelOk)
    {
      float mv = vcellRaw * 78.125f / 1000.0f;
      float pct = socRaw / 256.0f;
      float ma = (int16_t)currentRaw * FUEL_CURRENT_MA_PER_LSB;
      display.print(mv, 0);
      display.print(F("mV "));
      display.print(pct, 1);
      display.print(F("% "));
      display.print(ma, 0);
      display.print(F("mA"));
    }
    else
      display.print(F("not found"));
    y += lineHeight;

    display.setCursor(4, y);
    display.print(F("Chg  "));
    display.print(powerGood ? F("pwr ") : F("no-pwr "));
    display.print(charging ? F("charging") : F("idle"));
    y += lineHeight;

    display.setCursor(4, y);
    display.print(F("USB  "));
    display.print(usbSelected ? F("connected") : F("on battery"));
  } while (display.nextPage());

  display.hibernate();
}

static void checkNfcPins()
{
  if (UICR_NFCPINS & 0x1)
  {
    Serial.println(F("WARNING: UICR NFCPINS still set to NFC mode - "
                      "ACTION/NEXT buttons (P0.09/P0.10) will not read "
                      "correctly. See docs/bootloader.md step 4."));
  }
}

// regs[7]: Seconds, Minutes, Hours, Days, Weekdays, Months, Years - already
// read by the caller (updateDisplayStatus()), not read again here.
static void printRtcStatus(const uint8_t regs[7], bool ok)
{
  if (!ok)
  {
    Serial.println(F("  RTC: read failed"));
    return;
  }
  bool oscillatorStopped = regs[0] & 0x80;
  uint8_t seconds = bcd2bin(regs[0] & 0x7F);
  uint8_t minutes = bcd2bin(regs[1] & 0x7F);
  uint8_t hours = bcd2bin(regs[2] & 0x3F);
  uint8_t days = bcd2bin(regs[3] & 0x3F);
  uint8_t months = bcd2bin(regs[5] & 0x1F);
  uint8_t years = bcd2bin(regs[6]);

  Serial.print(F("  RTC: 20"));
  if (years < 10)
    Serial.print('0');
  Serial.print(years);
  Serial.print('-');
  if (months < 10)
    Serial.print('0');
  Serial.print(months);
  Serial.print('-');
  if (days < 10)
    Serial.print('0');
  Serial.print(days);
  Serial.print(' ');
  if (hours < 10)
    Serial.print('0');
  Serial.print(hours);
  Serial.print(':');
  if (minutes < 10)
    Serial.print('0');
  Serial.print(minutes);
  Serial.print(':');
  if (seconds < 10)
    Serial.print('0');
  Serial.print(seconds);
  if (oscillatorStopped)
    Serial.print(F("  [OS flag set: clock integrity lost, needs (re)setting]"));
  Serial.println();
}

// vcellRaw/socRaw/currentRaw - already read by the caller, not read again here.
static void printFuelGaugeStatus(uint16_t vcellRaw, uint16_t socRaw, uint16_t currentRaw, bool ok)
{
  if (!ok)
  {
    Serial.println(F("  Fuel gauge: read failed"));
    return;
  }
  float millivolts = vcellRaw * 78.125f / 1000.0f;
  float percent = socRaw / 256.0f;
  float milliamps = (int16_t)currentRaw * FUEL_CURRENT_MA_PER_LSB;
  Serial.print(F("  Fuel gauge: "));
  Serial.print(millivolts, 1);
  Serial.print(F(" mV, "));
  Serial.print(percent, 1);
  Serial.print(F(" %, "));
  Serial.print(milliamps, 1);
  Serial.print(F(" mA ("));
  Serial.print(milliamps > 0 ? F("charging") : (milliamps < 0 ? F("discharging") : F("idle")));
  Serial.println(F(")"));
}

// powerGood/charging - already read by the caller, not read again here.
// Both lines are open-drain, active low, pulled up on the board.
static void printChargerStatus(bool powerGood, bool charging)
{
  Serial.print(F("  Charger: input power "));
  Serial.print(powerGood ? F("PRESENT") : F("absent"));
  Serial.print(F(", STAT "));
  Serial.println(charging ? F("LOW (charging)") : F("HIGH (not charging / done / no battery)"));
}

// Runs off a GPIOTE edge, deferred to task context (see attachInterrupt(...,
// ISR_DEFERRED) in setup()), so delay()/Serial are safe to use here. Waits
// out the switch bounce, then only reports a change if the level held.
static void handleInputEdge(DebouncedInput &in)
{
  delay(DEBOUNCE_MS);
  bool reading = digitalRead(in.pin) == (in.activeLow ? LOW : HIGH);
  if (reading == in.state)
    return;
  in.state = reading;
  Serial.print(F("  >> "));
  Serial.print(in.name);
  Serial.println(in.state ? F(" ASSERTED") : F(" released"));
}

// One trampoline per input - attachInterrupt() callbacks take no argument.
static void onPrevEdge() { handleInputEdge(inputs[0]); }
static void onActionEdge() { handleInputEdge(inputs[1]); }
static void onNextEdge() { handleInputEdge(inputs[2]); }
static void onRtcIntEdge() { handleInputEdge(inputs[3]); }
static void onFuelAlertEdge() { handleInputEdge(inputs[4]); }
static void onUsbDetectEdge() { handleInputEdge(inputs[5]); }

void setup()
{
  Serial.begin(115200);
  uint32_t waitStart = millis();
  while (!Serial && (millis() - waitStart) < 3000)
    delay(10);

  // Order matches inputs[]/edgeCallbacks[] above.
  static void (*const edgeCallbacks[])() = {
      onPrevEdge, onActionEdge, onNextEdge, onRtcIntEdge, onFuelAlertEdge, onUsbDetectEdge};

  for (size_t i = 0; i < NUM_INPUTS; i++)
  {
    pinMode(inputs[i].pin, INPUT_PULLUP);
    // Seed the debounced state from the pin's actual level so a button
    // already held at boot doesn't look like a spurious edge later.
    inputs[i].state = digitalRead(inputs[i].pin) == (inputs[i].activeLow ? LOW : HIGH);
    attachInterrupt(inputs[i].pin, edgeCallbacks[i], CHANGE | ISR_DEFERRED);
  }

  // Charger status lines: read directly (polled, not debounced/edge-logged
  // like the inputs[] pins above), so they need their own pinMode() - an
  // unconfigured nRF52840 pin's input buffer is disconnected by default,
  // which was making digitalRead() on these always come back LOW.
  pinMode(PIN_CHG_PG, INPUT_PULLUP);
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(100000);

  Serial.println();
  Serial.println(F("=== NanoPage peripheral test ==="));
  checkNfcPins();
  scanI2CBus();
  identifyRtc();
  identifyFuelGauge();
  seedRtcFromCompileTimeIfNeeded();
  initFuelGaugeIfNeeded();
  initDisplay();
  updateDisplayStatus();
  Serial.println(F("Display: refresh done, panel hibernated."));
  Serial.println(F("Press PREV/ACTION/NEXT to test buttons - edges report immediately."));
  Serial.println(F("Peripherals are otherwise only read once per screen refresh."));
  Serial.println();
}

void loop()
{
  static const uint32_t DISPLAY_UPDATE_MS = 30000;

  // Buttons/RTC-INT/fuel-ALERT are handled entirely by the GPIOTE interrupts
  // set up in setup(), so there's nothing for loop() to poll in between.
  // Blocking here in delay() lets FreeRTOS's tickless idle put the MCU into
  // low-power sleep for the full period instead of waking on a fast cadence.
  delay(DISPLAY_UPDATE_MS);
  updateDisplayStatus();
  Serial.println(F("Display: updated."));
}
