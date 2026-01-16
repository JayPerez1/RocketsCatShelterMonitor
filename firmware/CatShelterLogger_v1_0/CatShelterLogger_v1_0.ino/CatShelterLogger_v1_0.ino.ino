/*
  Cat Shelter Monitor - MVP Logger (RTC + FRAM + 2x DS18B20 + BLE CSV download)

  Hardware:
  - Adafruit Feather nRF52840 Express
  - DS3231 RTC (I2C)
  - 256KB SPI FRAM (CS on A1)
  - 2x DS18B20 (both on A2, shared 1-Wire bus, external pull-up already installed)

  BLE:
  - Uses Adafruit Bluefruit LE UART service.
  - Commands via BLE UART:
      HELP
      STATUS
      GETNEW        -> CSV since last GETNEW
      GETALL        -> CSV of all logged data
      CLEAR         -> clears log (resets indexes)
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
static const uint8_t PIN_FRAM_CS   = A1;
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
  uint32_t last_sent_index;  // last record index sent via GETNEW, init = 0xFFFFFFFF
  uint32_t reserved[10];
};

struct __attribute__((packed)) Record {
  uint32_t ts;     // Unix time (seconds)
  int16_t  t0_c_x100; // probe0 in C * 100
  int16_t  t1_c_x100; // probe1 in C * 100
};

static const uint16_t RECORD_SIZE = sizeof(Record);

// Capacity (stop logging when full, MVP)
static uint32_t max_records = 0;

// -------------------- Globals --------------------
RTC_DS3231 rtc;
Adafruit_FRAM_SPI fram = Adafruit_FRAM_SPI(PIN_FRAM_CS);

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

BLEUart bleuart;

FramHeader hdr;

uint32_t last_log_ms = 0;

// -------------------- Utility helpers --------------------
static void disableOnboardNeoPixel() {
#if defined(PIN_NEOPIXEL_POWER)
  pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_POWER, LOW); // cut power to NeoPixel if supported
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

static void blePrint(const char* s) {
  bleuart.print(s);
}

static void blePrintln(const char* s) {
  bleuart.println(s);
}

static void printHelp() {
  blePrintln("Commands:");
  blePrintln("  HELP");
  blePrintln("  STATUS");
  blePrintln("  GETNEW   (CSV since last GETNEW)");
  blePrintln("  GETALL   (CSV all)");
  blePrintln("  CLEAR    (erase indexes)");
}

static void sendCsvHeader() {
  blePrintln("ts_iso,ts_unix,probe0_C,probe0_F,probe1_C,probe1_F");
}

static void sendCsvRange(uint32_t startIndex, uint32_t endIndexInclusive) {
  if (endIndexInclusive < startIndex) return;

  sendCsvHeader();

  for (uint32_t i = startIndex; i <= endIndexInclusive; i++) {
    Record r;
    if (!readRecord(i, r)) break;

    DateTime dt((uint32_t)r.ts);

    // ISO-ish timestamp
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

    // Let BLE stack breathe
    delay(2);
  }
}

// -------------------- BLE callbacks --------------------
void connect_callback(uint16_t conn_handle) {
  (void) conn_handle;
  blePrintln("Connected.");
  printHelp();
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
  Serial.println("Cat Shelter Monitor - MVP Logger");

  // I2C / RTC
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC NOT FOUND (DS3231). Check SDA/SCL wiring.");
  } else {
    Serial.println("RTC OK");
    if (rtc.lostPower()) {
      // Set to compile time if RTC lost power
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC lost power - set to compile time");
    }
  }

  // FRAM
  if (!fram.begin()) {
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

  // Load/init header
  if (!loadHeader()) {
    Serial.println("FRAM header missing/invalid -> initializing");
    initFreshHeader();
  } else {
    Serial.println("FRAM header loaded");
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
  Bluefruit.setTxPower(4); // moderate
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
  Bluefruit.Advertising.setInterval(32, 244); // 20ms to ~152ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);

  Serial.println("BLE advertising as: CatShelterLogger");
  Serial.println("Ready.");
}

// -------------------- Logging --------------------
static void doLogOnce() {
  if (max_records == 0) return;

  if (hdr.write_index >= max_records) {
    Serial.println("FRAM FULL - logging stopped.");
    return;
  }

  ds18b20.requestTemperatures();

  float t0 = ds18b20.getTempCByIndex(0);
  float t1 = ds18b20.getTempCByIndex(1);

  // If a sensor is missing/disconnected, Dallas returns DEVICE_DISCONNECTED_C (-127)
  bool ok0 = (t0 > -100.0f);
  bool ok1 = (t1 > -100.0f);

  DateTime now = rtc.now();
  uint32_t ts = (uint32_t) now.unixtime();

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

// -------------------- BLE command handling --------------------
static void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0) return;

  if (cmd == "HELP") {
    printHelp();
    return;
  }

  if (cmd == "STATUS") {
    bleuart.print("write_index=");
    bleuart.println(hdr.write_index);

    bleuart.print("last_sent_index=");
    bleuart.println(hdr.last_sent_index);

    bleuart.print("max_records=");
    bleuart.println(max_records);

    DateTime now = rtc.now();
    bleuart.print("rtc_unix=");
    bleuart.println((uint32_t)now.unixtime());
    return;
  }

  if (cmd == "CLEAR") {
    hdr.write_index = 0;
    hdr.last_sent_index = 0xFFFFFFFFUL;
    saveHeader();
    blePrintln("OK: cleared indexes.");
    return;
  }

  if (cmd == "GETALL") {
    if (hdr.write_index == 0) {
      blePrintln("No records.");
      return;
    }
    blePrintln("BEGIN_CSV");
    sendCsvRange(0, hdr.write_index - 1);
    blePrintln("END_CSV");
    return;
  }

  if (cmd == "GETNEW") {
    if (hdr.write_index == 0) {
      blePrintln("No records.");
      return;
    }

    uint32_t start = 0;
    if (hdr.last_sent_index == 0xFFFFFFFFUL) {
      start = 0;
    } else {
      start = hdr.last_sent_index + 1;
    }

    if (start >= hdr.write_index) {
      blePrintln("No new records.");
      return;
    }

    blePrintln("BEGIN_CSV");
    sendCsvRange(start, hdr.write_index - 1);
    blePrintln("END_CSV");

    hdr.last_sent_index = hdr.write_index - 1;
    saveHeader();
    return;
  }

  blePrintln("Unknown command. Type HELP");
}

// -------------------- Main loop --------------------
void loop() {
  // Periodic logging
  uint32_t now_ms = millis();
  if ((uint32_t)(now_ms - last_log_ms) >= LOG_INTERVAL_MS) {
    last_log_ms = now_ms;
    doLogOnce();
  }

  // BLE UART input
  while (bleuart.available()) {
    String cmd = bleuart.readStringUntil('\n');
    handleCommand(cmd);
  }

  delay(5);
}
