/*

  Cat Shelter Monitor - v1.1 BLE Usability (RTC + FRAM + 2x DS18B20 + BLE CSV download)

  v1.1 additions:
  - Self-guided BLE output (banner/help on connect)
  - DOWNLOAD / DOWNLOADALL
  - Chunked CSV output with small delays to improve terminal reliability
  - STATUS command
  - SETUNIX <unix> command (phone sets time)

  NOTE (Demo Mode):
  - FRAM is currently offline (hardware wiring issue). This firmware supports RAM-only logging + download.

*/

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <Adafruit_FRAM_SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <bluefruit.h>

// -------------------- Pins / Settings --------------------
static const uint8_t PIN_FRAM_CS   = 10;
static const uint8_t PIN_DS18B20   = A2;

// Logging interval (30 seconds)
static const uint32_t LOG_INTERVAL_MS = 30UL * 1000UL;

// FRAM sizing (won't be used if FRAM missing)
static const uint32_t FRAM_BYTES = 256UL * 1024UL;

static const uint32_t HDR_ADDR         = 0;
static const uint32_t HDR_SIZE         = 64;
static const uint32_t DATA_BASE_ADDR   = HDR_ADDR + HDR_SIZE;

static const uint32_t MAGIC = 0x43534C47; // 'CSLG'

// Packet/terminal pacing (helps some Android BLE terminals)
static const uint16_t CSV_LINE_DELAY_MS = 3;
static const uint16_t CSV_YIELD_EVERY_N_LINES = 25;
static const uint16_t CSV_YIELD_DELAY_MS = 20;

// -------------------- Data Structures --------------------
struct __attribute__((packed)) FramHeader {
  uint32_t magic;
  uint32_t write_index;
  uint32_t last_sent_index;
  uint32_t reserved[13];
};

struct __attribute__((packed)) Record {
  uint32_t ts;         // Unix time (seconds)
  int16_t  t0_c_x100;  // probe0 in C * 100
  int16_t  t1_c_x100;  // probe1 in C * 100
};

static const uint16_t RECORD_SIZE = sizeof(Record);

// -------------------- Globals --------------------
static uint32_t max_records = 0;

RTC_DS3231 rtc;
Adafruit_FRAM_SPI fram = Adafruit_FRAM_SPI(PIN_FRAM_CS, &SPI, 500000);

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

BLEUart bleuart;

FramHeader hdr;
uint32_t last_log_ms = 0;

static bool rtc_ok  = false;
static bool fram_ok = false;
static bool ram_only = false;

// RAM-only log buffer (demo mode)
// max_records is used as capacity; write_index counts entries in RAM too.
static Record* ram_records = nullptr;

// -------------------- Helpers --------------------
static float cFromX100(int16_t v) { return ((float)v) / 100.0f; }

static void blePrintln(const char* s) { bleuart.println(s); }

static void printWelcome() {
  blePrintln("CatShelterLogger v1.1");
  blePrintln("Type HELP for commands.");
}

static void printHelpV11() {
  blePrintln("Commands:");
  blePrintln("  HELP");
  blePrintln("  STATUS");
  blePrintln("  SETUNIX <unix_seconds>");
  blePrintln("  DOWNLOAD        (CSV since last_sent_index)");
  blePrintln("  DOWNLOADALL     (CSV all)");
  blePrintln("  CLEAR           (erase indexes / RAM buffer)");
}

// -------------------- FRAM helpers (no-op if FRAM missing) --------------------
static void framWriteBytes(uint32_t addr, const uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) fram.write8(addr + i, data[i]);
}

static void framReadBytes(uint32_t addr, uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) data[i] = fram.read8(addr + i);
}

static void saveHeader() {
  if (!fram_ok) return;
  framWriteBytes(HDR_ADDR, (uint8_t*)&hdr, sizeof(hdr));
}

static bool loadHeader() {
  if (!fram_ok) return false;
  FramHeader tmp;
  framReadBytes(HDR_ADDR, (uint8_t*)&tmp, sizeof(tmp));
  if (tmp.magic != MAGIC) return false;
  hdr = tmp;
  return true;
}

static void initFreshHeader() {
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MAGIC;
  hdr.write_index = 0;
  hdr.last_sent_index = 0xFFFFFFFFUL;
  saveHeader();
}

static uint32_t recordAddr(uint32_t index) {
  return DATA_BASE_ADDR + index * RECORD_SIZE;
}

static bool writeRecord(uint32_t index, const Record& r) {
  if (!fram_ok) return false;
  if (index >= max_records) return false;
  framWriteBytes(recordAddr(index), (const uint8_t*)&r, sizeof(r));
  return true;
}

static bool readRecord(uint32_t index, Record& r) {
  if (!fram_ok) return false;
  if (index >= hdr.write_index) return false;
  framReadBytes(recordAddr(index), (uint8_t*)&r, sizeof(r));
  return true;
}

// -------------------- CSV Output --------------------
static void sendCsvHeader() {
  // Packet-safe CSV (keeps each row short enough for basic BLE terminal apps)
  // Format: MM/DD HH:MM,t0C,t1C   (Eastern Time; Celsius rounded to whole degrees)
  blePrintln("datetime,t0C,t1C");
  delay(200);
}

static void sendCsvRecordLine(const Record& r) {
  // Packet-safe, Excel-friendly timestamp + Celsius (rounded)
  // datetime = MM/DD HH:MM in Eastern Time (UTC-5) for the demo
  uint32_t ts = r.ts;
  if (ts >= 18000UL) ts -= 18000UL; // Eastern Standard Time (UTC-5). (No DST handling.)

  DateTime dt((uint32_t)ts);

  float t0c = cFromX100(r.t0_c_x100);
  float t1c = cFromX100(r.t1_c_x100);

  int t0i = (int)(t0c >= 0 ? (t0c + 0.5f) : (t0c - 0.5f));
  int t1i = (int)(t1c >= 0 ? (t1c + 0.5f) : (t1c - 0.5f));

  char line[32];
  // "02/21 16:19,-10,22" fits in a single BLE notification in most terminal apps
  snprintf(line, sizeof(line),
           "%02d/%02d %02d:%02d,%d,%d",
           dt.month(), dt.day(),
           dt.hour(), dt.minute(),
           t0i, t1i);

  bleuart.println(line);
}

// Send a CSV range in chunks with delays so some terminals don’t drop data.
// Returns number of records sent.
static uint32_t sendCsvRangeChunked(uint32_t startIndex, uint32_t endIndexInclusive) {
  if (endIndexInclusive < startIndex) return 0;

  uint32_t sent = 0;
  for (uint32_t i = startIndex; i <= endIndexInclusive; i++) {
    Record r;

    if (ram_only) {
      if (i >= hdr.write_index) break;
      r = ram_records[i];
    } else {
      if (!readRecord(i, r)) break;
    }

    sendCsvRecordLine(r);
    sent++;

    delay(CSV_LINE_DELAY_MS);
    if ((sent % CSV_YIELD_EVERY_N_LINES) == 0) delay(CSV_YIELD_DELAY_MS);
  }
  return sent;
}

// -------------------- Commands --------------------
static void doStatus() {
  bleuart.print("fw_build=");
  bleuart.print(__DATE__);
  bleuart.print(" ");
  bleuart.println(__TIME__);

  bleuart.print("write_index=");
  bleuart.println(hdr.write_index);

  bleuart.print("last_sent_index=");
  bleuart.println(hdr.last_sent_index);

  bleuart.print("max_records=");
  bleuart.println(max_records);

  bleuart.print("fram_ok=");
  bleuart.println(fram_ok ? 1 : 0);

  bleuart.print("rtc_ok=");
  bleuart.println(rtc_ok ? 1 : 0);

  bleuart.print("ram_only=");
  bleuart.println(ram_only ? 1 : 0);
}

static void doClear() {
  hdr.write_index = 0;
  hdr.last_sent_index = 0xFFFFFFFFUL;

  if (ram_only && ram_records) {
    // Optional: not necessary to erase contents
  }

  saveHeader();
  blePrintln("OK: CLEARED");
}

static void doSetUnix(uint32_t unix_ts) {
  // For demo: even if RTC is not wired, we still accept and use unix_ts for logging
  rtc_ok = false; // physical RTC may be broken

  // If RTClib RTC is available, we *could* attempt rtc.adjust(...)
  // but your RTC is known bad for the demo, so we rely on stored unix_ts per record.

  // We don't store a global; we just use millis-based + last known unix in the logger loop.
  // We'll set "time base" variables used in loop().
  // (These are declared below.)
  extern uint32_t g_phone_unix_base;
  extern uint32_t g_phone_millis_base;

  g_phone_unix_base = unix_ts;
  g_phone_millis_base = millis();

  blePrintln("OK: PHONE TIME SET");
}

static void doDownloadNew() {
  if (hdr.write_index == 0) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  sendCsvHeader();

  uint32_t start = (hdr.last_sent_index == 0xFFFFFFFFUL) ? 0 : (hdr.last_sent_index + 1);
  if (start >= hdr.write_index) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  uint32_t end = hdr.write_index - 1;
  uint32_t sent = sendCsvRangeChunked(start, end);

  hdr.last_sent_index = end;
  saveHeader();

  bleuart.print("DOWNLOAD_COMPLETE,");
  bleuart.print(sent);
  bleuart.println(",0");
}

static void doDownloadAll() {
  if (hdr.write_index == 0) {
    blePrintln("DOWNLOAD_COMPLETE,0,0");
    return;
  }

  sendCsvHeader();

  uint32_t sent = sendCsvRangeChunked(0, hdr.write_index - 1);

  hdr.last_sent_index = hdr.write_index - 1;
  saveHeader();

  bleuart.print("DOWNLOAD_COMPLETE,");
  bleuart.print(sent);
  bleuart.println(",0");
}

static void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "HELP" || cmd == "H") {
    printHelpV11();
    return;
  }
  if (cmd == "STATUS") {
    doStatus();
    return;
  }
  if (cmd == "CLEAR") {
    doClear();
    return;
  }
  if (cmd == "DOWNLOAD") {
    doDownloadNew();
    return;
  }
  if (cmd == "DOWNLOADALL") {
    doDownloadAll();
    return;
  }

  if (cmd.startsWith("SETUNIX")) {
    // SETUNIX <unix>
    int sp = cmd.indexOf(' ');
    if (sp < 0) {
      blePrintln("ERR: usage SETUNIX <unix>");
      return;
    }
    String val = cmd.substring(sp + 1);
    val.trim();
    uint32_t t = (uint32_t)val.toInt();
    doSetUnix(t);
    return;
  }

  blePrintln("Unknown command. Type HELP");
}

// -------------------- BLE Callbacks --------------------
void connect_callback(uint16_t conn_handle) {
  (void)conn_handle;
  printWelcome();
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  (void)reason;
}

// -------------------- Time base (phone-set unix) --------------------
uint32_t g_phone_unix_base = 0;
uint32_t g_phone_millis_base = 0;

// Compute current unix from phone base + millis delta.
// If phone time not set yet, returns 0.
static uint32_t currentUnixFromPhone() {
  if (g_phone_unix_base == 0) return 0;
  uint32_t dt_ms = millis() - g_phone_millis_base;
  return g_phone_unix_base + (dt_ms / 1000UL);
}

// -------------------- Logging --------------------
static void ensureRamBuffer() {
  if (ram_records) return;

  // Capacity target: 30 sec logging, up to ~2 days is 5760 records/day => 11520 for ~2 days
  // You currently show max_records=11520 in status; keep that.
  if (max_records == 0) max_records = 11520;

  ram_records = (Record*)malloc(max_records * sizeof(Record));
  if (!ram_records) {
    // If malloc fails, reduce capacity
    max_records = 5760; // ~1 day @ 30s
    ram_records = (Record*)malloc(max_records * sizeof(Record));
  }
}

static void doLogOnce() {
  if (max_records == 0) return;

  if (hdr.write_index >= max_records) {
    // Buffer full
    return;
  }

  ds18b20.requestTemperatures();
  float t0 = ds18b20.getTempCByIndex(0);
  float t1 = ds18b20.getTempCByIndex(1);

  // Store as x100
  int16_t t0_x100 = (int16_t)(t0 * 100.0f);
  int16_t t1_x100 = (int16_t)(t1 * 100.0f);

  Record r;
  r.ts = currentUnixFromPhone(); // may be 0 until SETUNIX is done
  r.t0_c_x100 = t0_x100;
  r.t1_c_x100 = t1_x100;

  if (ram_only) {
    ensureRamBuffer();
    if (ram_records) {
      ram_records[hdr.write_index] = r;
      hdr.write_index++;
    }
    return;
  }

  // FRAM mode
  if (!fram_ok) return;

  if (hdr.write_index >= max_records) return;
  if (!writeRecord(hdr.write_index, r)) return;

  hdr.write_index++;
  saveHeader();
}

// -------------------- Setup / Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // Try FRAM
  SPI.begin();
  fram_ok = fram.begin();

  // Try RTC (known bad for demo)
  Wire.begin();
  rtc_ok = rtc.begin();

  // Compute FRAM max_records if FRAM present
  if (fram_ok) {
    uint32_t usable = FRAM_BYTES - DATA_BASE_ADDR;
    max_records = usable / RECORD_SIZE;

    if (!loadHeader()) {
      initFreshHeader();
    }
  } else {
    // Demo RAM-only mode
    ram_only = true;
    max_records = 11520; // ~2 days @ 30 sec
    initFreshHeader();   // keep hdr sane
    ensureRamBuffer();
  }

  // DS18B20
  ds18b20.begin();
  ds18b20.setWaitForConversion(true);

  // BLE
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("CatSh");

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
}

void loop() {
  // Logging timer
  uint32_t now = millis();
  if ((now - last_log_ms) >= LOG_INTERVAL_MS) {
    last_log_ms = now;
    doLogOnce();
  }

  // BLE command handling
  if (bleuart.available()) {
    String cmd = bleuart.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) handleCommand(cmd);
  }
}