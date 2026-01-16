/*
  Cat Shelter Monitor - RAM DEMO Logger (NO FRAM)
  PoC demo: BLE download works even without FRAM hardware.

  CSV is intentionally SHORT to avoid BLE terminal truncation.
*/

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <bluefruit.h>

// -------------------- Pins --------------------
static const uint8_t PIN_DS18B20 = A2;

// -------------------- Logging config --------------------
static const uint32_t LOG_INTERVAL_MS = 60UL * 1000UL; // 1 minute

// -------------------- RAM storage (DEMO) --------------------
static const uint32_t RAM_MAX_RECORDS = 1440; // 24 hours @ 1-min

struct __attribute__((packed)) Record {
  uint32_t ts;          // Unix time (seconds); 0 if RTC missing
  int16_t  t0_c_x100;   // probe0 in C * 100; -32768 if invalid
  int16_t  t1_c_x100;   // probe1 in C * 100; -32768 if invalid
};

static Record records[RAM_MAX_RECORDS];

static uint32_t write_index = 0;
static uint32_t last_sent_index = 0xFFFFFFFFUL;

// -------------------- Globals --------------------
RTC_DS3231 rtc;
static bool rtc_ok = false;

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

BLEUart bleuart;

static uint32_t last_log_ms = 0;

// -------------------- Transfer pacing --------------------
static const uint16_t CSV_LINE_DELAY_MS = 3;
static const uint16_t CSV_YIELD_EVERY_N_LINES = 25;
static const uint16_t CSV_YIELD_DELAY_MS = 20;

// -------------------- Utility --------------------
static void disableOnboardNeoPixel() {
#if defined(PIN_NEOPIXEL_POWER)
  pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_POWER, LOW);
#endif
}

static float cFromX100(int16_t v) { return ((float)v) / 100.0f; }

static void blePrintln(const char* s) { bleuart.println(s); }

static void printWelcome() {
  blePrintln("Cat Shelter Logger (RAM DEMO)");
  blePrintln("Type: DOWNLOAD");
  blePrintln("Also: STATUS, HELP");
  blePrintln("NOTE: RAM ONLY (data lost on reset)");
}

static void printHelp() {
  blePrintln("Commands:");
  blePrintln("  DOWNLOAD      (CSV since last download)");
  blePrintln("  DOWNLOADALL   (CSV all records)");
  blePrintln("  STATUS");
  blePrintln("  CLEAR         (erase RAM buffer)");
  blePrintln("  HELP");
}

// SHORT CSV header to avoid truncation in apps
static void sendCsvHeaderShort() {
  blePrintln("ts_unix,t0C,t1C");
  delay(50);
}

static void sendCsvRecordLineShort(const Record& r) {
  float t0c = cFromX100(r.t0_c_x100);
  float t1c = cFromX100(r.t1_c_x100);

  bleuart.print((uint32_t)r.ts);
  bleuart.print(",");
  bleuart.print(t0c, 2);
  bleuart.print(",");
  bleuart.println(t1c, 2);
}

static uint32_t sendCsvRangeChunked(uint32_t startIndex, uint32_t endIndexInclusive) {
  if (endIndexInclusive < startIndex) return 0;

  uint32_t sent = 0;

  sendCsvHeaderShort();

  for (uint32_t i = startIndex; i <= endIndexInclusive; i++) {
    if (i >= write_index) break;
    sendCsvRecordLineShort(records[i]);
    sent++;

    delay(CSV_LINE_DELAY_MS);
    if ((sent % CSV_YIELD_EVERY_N_LINES) == 0) {
      delay(CSV_YIELD_DELAY_MS);
    }
  }

  return sent;
}

// -------------------- BLE callbacks --------------------
void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  printWelcome();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;
}

// -------------------- Logging --------------------
static void doLogOnce() {
  if (write_index >= RAM_MAX_RECORDS) return;

  ds18b20.requestTemperatures();

  float t0 = ds18b20.getTempCByIndex(0);
  float t1 = ds18b20.getTempCByIndex(1);

  bool ok0 = (t0 > -100.0f);
  bool ok1 = (t1 > -100.0f);

  uint32_t ts = 0;
  if (rtc_ok) {
    DateTime now = rtc.now();
    ts = (uint32_t)now.unixtime();
  }

  Record r;
  r.ts = ts;
  r.t0_c_x100 = ok0 ? (int16_t)lroundf(t0 * 100.0f) : (int16_t)-32768;
  r.t1_c_x100 = ok1 ? (int16_t)lroundf(t1 * 100.0f) : (int16_t)-32768;

  records[write_index] = r;

  Serial.print("Logged #");
  Serial.print(write_index);
  Serial.print("  t0C=");
  Serial.print(ok0 ? t0 : NAN);
  Serial.print("  t1C=");
  Serial.println(ok1 ? t1 : NAN);

  write_index++;
}

// -------------------- DOWNLOAD helpers --------------------
static void printDownloadComplete(uint32_t sent, uint32_t remaining) {
  bleuart.print("DOWNLOAD_COMPLETE,");
  bleuart.print(sent);
  bleuart.print(",");
  bleuart.println(remaining);
}

static void doDownloadNew() {
  if (write_index == 0) {
    printDownloadComplete(0, 0);
    return;
  }

  uint32_t start = (last_sent_index == 0xFFFFFFFFUL) ? 0 : (last_sent_index + 1);

  if (start >= write_index) {
    printDownloadComplete(0, 0);
    return;
  }

  uint32_t endIndex = write_index - 1;
  uint32_t sent = sendCsvRangeChunked(start, endIndex);

  if (sent > 0) last_sent_index = endIndex;

  uint32_t remaining = 0;
  if (last_sent_index == 0xFFFFFFFFUL) remaining = write_index;
  else if (last_sent_index + 1 < write_index) remaining = write_index - (last_sent_index + 1);
  else remaining = 0;

  printDownloadComplete(sent, remaining);
}

static void doDownloadAll() {
  if (write_index == 0) {
    printDownloadComplete(0, 0);
    return;
  }

  uint32_t sent = sendCsvRangeChunked(0, write_index - 1);
  uint32_t remaining = (sent < write_index) ? (write_index - sent) : 0;

  printDownloadComplete(sent, remaining);
}

// -------------------- BLE command handling --------------------
static void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "HELP") { printHelp(); return; }

  if (cmd == "STATUS") {
    bleuart.print("write_index=");     bleuart.println((uint32_t)write_index);
    bleuart.print("last_sent_index="); bleuart.println((uint32_t)last_sent_index);
    bleuart.print("max_records=");     bleuart.println((uint32_t)RAM_MAX_RECORDS);

    bleuart.print("rtc_unix=");
    if (rtc_ok) {
      DateTime now = rtc.now();
      bleuart.println((uint32_t)now.unixtime());
    } else {
      bleuart.println((uint32_t)0);
    }

    bleuart.println("fram_ok=0");
    bleuart.print("rtc_ok=");   bleuart.println(rtc_ok ? 1 : 0);
    bleuart.println("ram_only=1");
    return;
  }

  if (cmd == "CLEAR") {
    write_index = 0;
    last_sent_index = 0xFFFFFFFFUL;
    blePrintln("OK: cleared RAM buffer.");
    return;
  }

  if (cmd == "DOWNLOAD")    { doDownloadNew(); return; }
  if (cmd == "DOWNLOADALL") { doDownloadAll(); return; }

  blePrintln("Unknown command. Type HELP");
}

// -------------------- Setup --------------------
void setup() {
  disableOnboardNeoPixel();

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Cat Shelter Monitor - RAM DEMO (short CSV)");

  Wire.begin();
  rtc_ok = rtc.begin();
  if (rtc_ok) {
    Serial.println("RTC OK");
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC lost power - set to compile time");
    }
  } else {
    Serial.println("RTC NOT FOUND (OK for demo).");
  }

  ds18b20.begin();
  ds18b20.setWaitForConversion(true);

  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("CatShelterLogger");

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  bleuart.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.Advertising.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);

  Serial.println("BLE advertising as: CatShelterLogger");
  Serial.println("Ready.");
}

// -------------------- Main loop --------------------
void loop() {
  uint32_t now_ms = millis();
  if ((uint32_t)(now_ms - last_log_ms) >= LOG_INTERVAL_MS) {
    last_log_ms = now_ms;
    doLogOnce();
  }

  while (bleuart.available()) {
    String cmd = bleuart.readStringUntil('\n');
    handleCommand(cmd);
  }

  delay(5);
}
