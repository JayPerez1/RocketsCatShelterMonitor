/*
  Cat Shelter Monitor - v1.1 BLE Usability (RTC + FRAM + 2x DS18B20 + BLE CSV download)

  v1.1 additions:
  - Self-guided BLE output (banner/help on connect)
  - DOWNLOAD / DOWNLOADALL
  - Chunked CSV transfer
  - DOWNLOAD_COMPLETE,<sent>,<remaining>
  - Explicit FRAM/RTC health in STATUS
  - FRAMTEST command to verify FRAM is truly readable/writable

  Hardware (UNCHANGED):
  - Adafruit Feather nRF52840 Express
  - DS3231 RTC (I2C)
  - 256KB SPI FRAM (CS on A1)
  - 2x DS18B20 on A2 with external pull-up to 3V
*/

#include <Arduino.h>
#include <Wire.h>

#include <RTClib.h>

#include <SPI.h>
#include <Adafruit_FRAM_SPI.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <bluefruit.h>

// -------------------- Pins (match your wiring) --------------------
static const uint8_t PIN_FRAM_CS   = 10;
static const uint8_t PIN_DS18B20   = A2;

// -------------------- Logging config --------------------
static const uint32_t LOG_INTERVAL_MS = 60UL * 1000UL; // 1 minute

// -------------------- FRAM layout --------------------
static const uint32_t FRAM_BYTES = 256UL * 1024UL;

// Header area (fixed addresses)
static const uint32_t HDR_ADDR         = 0;
static const uint32_t HDR_SIZE         = 64;
static const uint32_t DATA_BASE_ADDR   = HDR_ADDR + HDR_SIZE;

static const uint32_t MAGIC = 0x43534C47; // 'CSLG'

struct __attribute__((packed)) FramHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t record_size;
  uint32_t write_index;      // next record index to write (0..)
  uint32_t last_sent_index;  // last record index sent via DOWNLOAD/GETNEW, init = 0xFFFFFFFF
  uint32_t reserved[10];
};

struct __attribute__((packed)) Record {
  uint32_t ts;         // Unix time (seconds)
  int16_t  t0_c_x100;  // probe0 in C * 100
  int16_t  t1_c_x100;  // probe1 in C * 100
};

static const uint16_t RECORD_SIZE = sizeof(Record);

// Capacity (stop logging when full, MVP)
static uint32_t max_records = 0;

// -------------------- Globals --------------------
RTC_DS3231 rtc;
Adafruit_FRAM_SPI fram = Adafruit_FRAM_SPI(PIN_FRAM_CS, &SPI, 500000);

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

BLEUart bleuart;

FramHeader hdr;

uint32_t last_log_ms = 0;

// Explicit health flags (v1.1)
static bool rtc_ok  = false;
static bool fram_ok = false;

// -------------------- v1.1 Transfer pacing --------------------
static const uint16_t CSV_LINE_DELAY_MS = 3;
static const uint16_t CSV_YIELD_EVERY_N_LINES = 25;
static const uint16_t CSV_YIELD_DELAY_MS = 20;

// -------------------- Utility helpers --------------------
static void disableOnboardNeoPixel() {
#if defined(PIN_NEOPIXEL_POWER)
  pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_POWER, LOW);
#endif
}

static void framWriteBytes(uint32_t addr, const uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    fram.write8(addr + i, data[i]);
  }
}

static void framReadBytes(uint32_t addr, uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    data[i] = fram.read8(addr + i);
  }
}

static void saveHeader() {
  framWriteBytes(HDR_ADDR, (uint8_t*)&hdr, sizeof(hdr));
}

static bool loadHeader() {
  framReadBytes(HDR_ADDR, (uint8_t*)&hdr, sizeof(hdr));
  if (hdr.magic != MAGIC) return false;
  if (hdr.record_size != RECORD_SIZE) return false;
  if (hdr.version != 1) return false;
  return true;
}

static void initFreshHeader() {
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MAGIC;
  hdr.version = 1;
  hdr.record_size = RECORD_SIZE;
  hdr.write_index = 0;
  hdr.last_sent_index = 0xFFFFFFFFUL;
  saveHeader();
}

static uint32_t recordAddr(uint32_t index) {
  return DATA_BASE_ADDR + (index * RECORD_SIZE);
}

static bool writeRecord(uint32_t index, const Record& r) {
  uint32_t addr = recordAddr(index);
  if (addr + RECORD_SIZE > FRAM_BYTES) return false;
  framWriteBytes(addr, (const uint8_t*)&r, RECORD_SIZE);
  return true;
}

static bool readRecord(uint32_t index, Record& r) {
  uint32_t addr = recordAddr(index);
  if (addr + RECORD_SIZE > FRAM_BYTES) return false;
  framReadBytes(addr, (uint8_t*)&r, RECORD_SIZE);
  return true;
}

static float cFromX100(int16_t v) { return ((float)v) / 100.0f; }
static float fFromC(float c) { return c * 9.0f / 5.0f + 32.0f; }

static void blePrintln(const char* s) { bleuart.println(s); }

// -------------------- v1.1 User-facing text --------------------
static void printWelcome() {
  blePrintln("Cat Shelter Logger");
  blePrintln("Type: DOWNLOAD");
  blePrintln("Also: STATUS, HELP");
  if (!fram_ok) {
    blePrintln("ERROR: FRAM NOT DETECTED");
  }
  if (!rtc_ok) {
    blePrintln("ERROR: RTC NOT DETECTED");
  }
}

static void printHelpV11() {
  blePrintln("Commands:");
  blePrintln("  DOWNLOAD      (CSV since last download)");
  blePrintln("  DOWNLOADALL   (CSV all records)");
  blePrintln("  STATUS");
  blePrintln("  FRAMTEST      (verify FRAM read/write)");
  blePrintln("  CLEAR         (reset indexes)");
  blePrintln("  HELP");
}

// -------------------- CSV helpers --------------------
static void sendCsvHeader() {
  blePrintln("ts_iso,ts_unix,probe0_C,probe0_F,probe1_C,probe1_F");
  // Small pause after header so terminals don’t drop the tail of the header line
  delay(50);
}

static void sendCsvRecordLine(const Record& r) {
  DateTime dt((uint32_t)r.ts);

  char iso[25];
  snprintf(iso, sizeof(iso), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());

  float t0c = cFromX100(r.t0_c_x100);
  float t1c = cFromX100(r.t1_c_x100);
  float t0f = fFromC(t0c);
  float t1f = fFromC(t1c);

  bleuart.print(iso); bleuart.print(",");
  bleuart.print(r.ts); bleuart.print(",");
  bleuart.print(t0c, 2); bleuart.print(",");
  bleuart.print(t0f, 2); bleuart.print(",");
  bleuart.print(t1c, 2); bleuart.print(",");
  bleuart.println(t1f, 2);
}

static uint32_t sendCsvRangeChunked(uint32_t startIndex, uint32_t endIndexInclusive) {
  if (endIndexInclusive < startIndex) return 0;

  uint32_t sent = 0;

  sendCsvHeader();

  for (uint32_t i = startIndex; i <= endIndexInclusive; i++) {
    Record r;
    if (!readRecord(i, r)) {
      break;
    }

    sendCsvRecordLine(r);
    sent++;

    delay(CSV_LINE_DELAY_MS);
    if ((sent % CSV_YIELD_EVERY_N_LINES) == 0) {
      delay(CSV_YIELD_DELAY_MS);
    }
  }

  return sent;
}

// -------------------- FRAM self-test --------------------
// Writes one byte into a RESERVED header byte, reads it back, restores original.
static void doFramTest() {
  if (!fram_ok) {
    blePrintln("FRAMTEST=FAIL (fram_ok=0)");
    return;
  }

  // Use last byte of the header area (HDR_ADDR + HDR_SIZE - 1) which is reserved.
  const uint32_t testAddr = HDR_ADDR + HDR_SIZE - 1;

  uint8_t orig = fram.read8(testAddr);
  const uint8_t pattern = 0xA5;

  fram.write8(testAddr, pattern);
  delay(2);
  uint8_t rd = fram.read8(testAddr);

  // Restore
  fram.write8(testAddr, orig);

  if (rd == pattern) {
    blePrintln("FRAMTEST=OK");
  } else {
    blePrintln("FRAMTEST=FAIL");
  }
}

// -------------------- BLE callbacks --------------------
void connect_callback(uint16_t conn_handle) {
  (void) conn_handle;
  printWelcome();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void) conn_handle;
  (void) reason;
}

// -------------------- Setup --------------------
void setup() {
  disableOnboardNeoPixel();

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Cat Shelter Monitor - v1.1 BLE Usability");

  // I2C / RTC
  Wire.begin();
  rtc_ok = rtc.begin();
  if (!rtc_ok) {
    Serial.println("RTC NOT FOUND (DS3231). Check SDA/SCL wiring.");
  } else {
    Serial.println("RTC OK");
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC lost power - set to compile time");
    }
  }

  // FRAM
  fram_ok = fram.begin();
  if (!fram_ok) {
    Serial.println("FRAM NOT FOUND. Check SPI wiring + CS=A1.");
  } else {
    Serial.println("FRAM OK");
  }

  // Determine capacity
  if (FRAM_BYTES <= DATA_BASE_ADDR) {
    max_records = 0;
  } else {
    max_records = (FRAM_BYTES - DATA_BASE_ADDR) / RECORD_SIZE;
  }
  Serial.print("Max records: ");
  Serial.println(max_records);

  // Load/init header (only if FRAM is OK)
  if (fram_ok) {
    if (!loadHeader()) {
      Serial.println("FRAM header missing/invalid -> initializing");
      initFreshHeader();
    } else {
      Serial.println("FRAM header loaded");
    }
  } else {
    // Keep hdr sane in RAM, but we will NOT log or download
    memset(&hdr, 0, sizeof(hdr));
    hdr.last_sent_index = 0xFFFFFFFFUL;
  }

  // DS18B20
  ds18b20.begin();
  ds18b20.setWaitForConversion(true);
  int count = ds18b20.getDeviceCount();
  Serial.print("DS18B20 devices found: ");
  Serial.println(count);
  if (count < 2) {
    Serial.println("WARNING: expected 2 probes. Check DATA=A2 and common GND/3V.");
  }

  // BLE
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("CatShelterLogger");

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  bleuart.begin();

  // Advertising
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

// -------------------- Logging --------------------
static void doLogOnce() {
  if (!fram_ok) {
    // Do not advance indexes if FRAM is missing
    return;
  }

  if (max_records == 0) return;

  if (hdr.write_index >= max_records) {
    Serial.println("FRAM FULL - logging stopped.");
    return;
  }

  ds18b20.requestTemperatures();

  float t0 = ds18b20.getTempCByIndex(0);
  float t1 = ds18b20.getTempCByIndex(1);

  bool ok0 = (t0 > -100.0f);
  bool ok1 = (t1 > -100.0f);

  uint32_t ts = 0;
  if (rtc_ok) {
    DateTime now = rtc.now();
    ts = (uint32_t) now.unixtime();
  } else {
    // If RTC missing, still log with ts=0 to make the fault obvious
    ts = 0;
  }

  Record r;
  r.ts = ts;
  r.t0_c_x100 = ok0 ? (int16_t) lroundf(t0 * 100.0f) : (int16_t) -32768;
  r.t1_c_x100 = ok1 ? (int16_t) lroundf(t1 * 100.0f) : (int16_t) -32768;

  if (!writeRecord(hdr.write_index, r)) {
    Serial.println("Record write failed.");
    return;
  }

  Serial.print("Logged #");
  Serial.print(hdr.write_index);
  Serial.print("  t0C=");
  Serial.print(ok0 ? t0 : NAN);
  Serial.print("  t1C=");
  Serial.println(ok1 ? t1 : NAN);

  hdr.write_index++;
  saveHeader();
}

// -------------------- DOWNLOAD implementation --------------------
static void doDownloadNew() {
  if (!fram_ok) {
    blePrintln("ERROR: FRAM NOT DETECTED");
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  if (hdr.write_index == 0) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  uint32_t start = (hdr.last_sent_index == 0xFFFFFFFFUL) ? 0 : (hdr.last_sent_index + 1);

  if (start >= hdr.write_index) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  uint32_t endIndex = hdr.write_index - 1;
  uint32_t sent = sendCsvRangeChunked(start, endIndex);

  if (sent > 0) {
    hdr.last_sent_index = endIndex;
    saveHeader();
  }

  uint32_t remaining = 0;
  if (hdr.last_sent_index == 0xFFFFFFFFUL) {
    remaining = hdr.write_index;
  } else if (hdr.last_sent_index + 1 < hdr.write_index) {
    remaining = hdr.write_index - (hdr.last_sent_index + 1);
  } else {
    remaining = 0;
  }

  bleuart.print("DOWNLOAD_COMPLETE,");
  bleuart.print(sent);
  bleuart.print(",");
  bleuart.println(remaining);
}

static void doDownloadAll() {
  if (!fram_ok) {
    blePrintln("ERROR: FRAM NOT DETECTED");
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  if (hdr.write_index == 0) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  uint32_t endIndex = hdr.write_index - 1;
  uint32_t sent = sendCsvRangeChunked(0, endIndex);

  uint32_t remaining = 0;
  if (sent < hdr.write_index) {
    remaining = hdr.write_index - sent;
  }

  bleuart.print("DOWNLOAD_COMPLETE,");
  bleuart.print(sent);
  bleuart.print(",");
  bleuart.println(remaining);
}

// -------------------- BLE command handling --------------------
static void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0) return;

  if (cmd == "DOWNLOAD")    { doDownloadNew(); return; }
  if (cmd == "DOWNLOADALL") { doDownloadAll(); return; }

  if (cmd == "HELP") { printHelpV11(); return; }

  if (cmd == "STATUS") {
    bleuart.print("write_index=");      bleuart.println(hdr.write_index);
    bleuart.print("last_sent_index=");  bleuart.println(hdr.last_sent_index);
    bleuart.print("max_records=");      bleuart.println(max_records);

    if (rtc_ok) {
      DateTime now = rtc.now();
      bleuart.print("rtc_unix="); bleuart.println((uint32_t)now.unixtime());
    } else {
      bleuart.print("rtc_unix="); bleuart.println((uint32_t)0);
    }

    bleuart.print("fram_ok="); bleuart.println(fram_ok ? 1 : 0);
    bleuart.print("rtc_ok=");  bleuart.println(rtc_ok ? 1 : 0);
    return;
  }

  if (cmd == "FRAMTEST") {
    doFramTest();
    return;
  }

  if (cmd == "CLEAR") {
    if (!fram_ok) {
      blePrintln("ERROR: FRAM NOT DETECTED");
      return;
    }
    hdr.write_index = 0;
    hdr.last_sent_index = 0xFFFFFFFFUL;
    saveHeader();
    blePrintln("OK: cleared indexes.");
    return;
  }

  // v1.0 compatibility
  if (cmd == "GETALL") {
    if (!fram_ok) {
      blePrintln("ERROR: FRAM NOT DETECTED");
      return;
    }
    if (hdr.write_index == 0) {
      blePrintln("No records.");
      return;
    }
    blePrintln("BEGIN_CSV");
    (void)sendCsvRangeChunked(0, hdr.write_index - 1);
    blePrintln("END_CSV");
    return;
  }

  if (cmd == "GETNEW") {
    if (!fram_ok) {
      blePrintln("ERROR: FRAM NOT DETECTED");
      return;
    }
    if (hdr.write_index == 0) {
      blePrintln("No records.");
      return;
    }
    uint32_t start = (hdr.last_sent_index == 0xFFFFFFFFUL) ? 0 : (hdr.last_sent_index + 1);
    if (start >= hdr.write_index) {
      blePrintln("No new records.");
      return;
    }
    blePrintln("BEGIN_CSV");
    (void)sendCsvRangeChunked(start, hdr.write_index - 1);
    blePrintln("END_CSV");
    hdr.last_sent_index = hdr.write_index - 1;
    saveHeader();
    return;
  }

  blePrintln("Unknown command. Type HELP");
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
