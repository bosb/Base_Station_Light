/*
 * Tools -> Board -> Boards Manager: esp32 by Espressif Systems 3.3.11 at time of writing
 * https://github.com/espressif/arduino-esp32
 * 
 * Tools -> Board -> ESP32 Arduino -> ESP32C3 Dev Module
 * USB CDC on boot: enabled
 * Flash Mode: DIO
 * JTAG Adapert: Integrated USB JTAG
 * 
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>    // our own config namespace "cfg"
#include <esp_ota_ops.h>
#include <esp_system.h>     // esp_random() for the lock password
#include <nvs.h>            // raw NVS API (used by the /test diag)
#include <mbedtls/pkcs5.h>  // mbedtls_pkcs5_pbkdf2_hmac_ext (pkcs5.h pulls in md.h)
#include <esp_wifi.h>       // esp_wifi_set_max_tx_power() / get_max_tx_power()
#include <esp_mac.h>        // esp_read_mac() for the MAC-derived default AP ssid

// ---------------------------------------------------------------------------
// Legacy NVS (the Tractive BaseStation firmware's esp_wifi storage)
//
// The original firmware configured WiFi with storage=FLASH, so its credentials
// live in the NVS namespace "nvs.net80211". This firmware only *reads* that
// namespace (RAM storage for our own WiFi calls) so the old record stays
// intact and can be shown / migrated. Blob layouts (verified against real
// device NVS dumps):
//
//   ap.ssid     {u32 len, ssid[len], pad}           36 B
//   ap.passwd   passphrase bytes, 0x00-padded       65 B
//   ap.pmk      raw 32-byte WPA2 PMK                32 B
//   sta.ssid    {u32 len, ssid[len], pad}           36 B   ("" when unused)
//   sta.pswd    passphrase bytes, 0x00-padded       65 B   ("" when unused)
//
// readLegacyBlob() reads these via Preferences and drops every leading
// non-printable byte instead of assuming a fixed prefix width (4 B for ssid,
// 0 B for passwd), then stops at the first NUL.
//
// The PMK is the standard IEEE 802.11 pre-shared key:
//   PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, iterations=4096, dklen=32)
// so we can *verify* ap.pmk against ap.ssid + ap.passwd instead of just
// trusting that a 32-byte blob is meaningful.
//
// (Declared up here, before the first function, so arduino-cli's auto-generated
// forward prototypes below can reference them.)
// ---------------------------------------------------------------------------

enum PmkStatus { PMK_OK, PMK_MISSING, PMK_EMPTY, PMK_BAD, PMK_UNVERIFIABLE };

struct LegacyNvs {
  bool present = false;  // namespace "nvs.net80211" opened (Tractive data exists)
  String apSsid;         // old firmware's AP ssid ("" if missing/invalid)
  String apPasswd;       // old firmware's AP passphrase
  String staSsid;        // old firmware's STA ssid ("" = not configured)
  String staPasswd;      // old firmware's STA passphrase
  PmkStatus pmk = PMK_MISSING;
};

const char* host = "pfoten";

// This firmware ("Pfoten") is based on https://github.com/bosb/Base_Station_Light
#define FW_VERSION "0.1.0"

// Defaults, used ONLY on a fresh device with no user-saved config. The real
// values live in the Preferences namespace "cfg" (keys: pf.ap.ssid,
// pf.ap.passwd, pf.sta.ssid, pf.sta.passwd, pf.usr, pf.ver, pf.sig), filled by
// the /config page. The old Tractive NVS is NOT imported: a fresh device starts
// with the MAC-derived default AP ssid (defaultSsid()) + this password, and STA
// disabled until the home network is configured in the web UI.
const char* DEFAULT_PASS = "MakeYouWannaMoveYourDancingFeetNow";

// cfg namespace keys. Dotted pf.* names mirror the Tractive firmware's legacy
// keys (ap.ssid, ap.passwd, sta.ssid, sta.pswd) so the two configs are easy to
// tell apart.
#define K_AP_SSID   "pf.ap.ssid"
#define K_AP_PASSWD "pf.ap.passwd"
#define K_STA_SSID  "pf.sta.ssid"
#define K_STA_PASSWD "pf.sta.passwd"
#define K_USR       "pf.usr"
#define K_VER       "pf.ver"
#define K_SIG       "pf.sig"
#define K_APOFF     "pf.apoff"

// Runtime config, loaded once in loadCfg() (setup, before any WiFi call).
String cfgSsid, cfgPass;      // our own AP credentials
String cfgCssid, cfgCpass;    // client (STA) network; empty = STA disabled
bool   cfgUsr = false;        // 1 once the user saved via /config
bool   cfgApoff = false;      // 1 = AP hidden (stealth): no SSID broadcast

// AP state machine. The AP always broadcasts cfgSsid (unless hidden via
// /apoff), but accepts connections only while the device has no client (STA)
// link to the home network, or for 15 minutes after a 3 s long-press of the
// button. The long-press open window has NO password (open network) so a phone
// can always join it; the "open because no STA link" state uses cfgPass. While
// locked it uses a random 16-char password nobody knows (generated once at
// boot), so the AP cannot be used to reach the device over the air once it is
// reachable over the LAN. Hidden = the AP interface is torn down entirely
// (WiFi.enableAP(false)) so no beacon/SSID is emitted; the long-press open
// window temporarily brings it back, then it hides again.
enum ApState { AP_HIDDEN, AP_OPEN, AP_LOCKED };
static ApState   apState = AP_OPEN;    // state currently applied
static bool      apPwless = false;     // true while AP_OPEN uses no password (long-press window)
static uint32_t  apOpenUntil = 0;      // millis() deadline for the long-press open window
static char      lockPw[17];           // random lock password (16 chars + NUL)

// Pin-3 LED = "AP open" indicator: on long-press recognition it blinks fast
// 3 times, then slow-blinks while the AP is open with no client, goes steady
// on once a client joins, and is off while the AP is locked.
static uint8_t  ledFlashCount = 0;    // remaining flash toggles (6 = 3 blinks)
static bool     ledFlashOn = false;
static uint32_t ledFlashTick = 0;
static bool     ledBlinkOn = false;
static uint32_t ledBlinkTick = 0;

// AP signal-strength display, cycled by short button press and persisted in the
// cfg namespace (pf.sig): 0 = low, 1 = middle, 2 = high.
//   low    -> only LED 10 (left) on
//   middle -> only LED 5  (middle) on
//   high   -> LED 10 + LED 5 on
static uint8_t sigLevel = 0;

// LED dimming: LEDs run at full brightness for DIM_DELAY_MS after boot or any
// button press, then drop to LED_DIM (~1% of 255). All three LEDs are driven by
// LEDC PWM (5 kHz, 8-bit) so the duty is software-controlled.
static const uint8_t  LED_FULL = 255;
static const uint8_t  LED_DIM  = 3;
static const uint32_t DIM_DELAY_MS = 5000UL;
static uint32_t ledDimAt = 0;   // millis() deadline; LEDs dim after this

// Button debounce / long-press bookkeeping.
static uint32_t pressStart = 0;
static bool     wasPressed = false;
static bool     longPressFired = false;

// gpio: 0, 1, 2, 3-d3, 4-sw1, 5-d2, 6, 7, 8, 9-h4, 10-d1, 11.
const int buttonPin = 4;     // the number of the pushbutton pin
const int ledPin =  10;      // the number of the LED pin

WebServer server(80);

void handleFavicon() {
  const char favicon_16x16_png[] = { // xxd -i
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
  0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
  0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0xf3, 0xff, 0x61, 0x00, 0x00, 0x01,
  0xd8, 0x49, 0x44, 0x41, 0x54, 0x38, 0x4f, 0xa5, 0x91, 0x31, 0x68, 0x13,
  0x51, 0x18, 0xc7, 0xff, 0xdf, 0x25, 0xf7, 0x79, 0xf1, 0x70, 0x12, 0x9b,
  0xa5, 0x14, 0x5d, 0x02, 0xea, 0xe4, 0xe0, 0xd2, 0xad, 0x0e, 0x77, 0x9e,
  0x21, 0x59, 0xca, 0x81, 0xe2, 0x20, 0xae, 0xa2, 0x93, 0x43, 0x17, 0xb7,
  0x8a, 0x4e, 0x3a, 0x88, 0x75, 0x50, 0xa8, 0x41, 0x04, 0x87, 0x2a, 0x42,
  0x0e, 0xbc, 0xbc, 0x20, 0xad, 0x83, 0x08, 0x82, 0x8b, 0x43, 0x95, 0xac,
  0xa5, 0x93, 0x16, 0x24, 0x91, 0x3c, 0x2e, 0x2f, 0xbc, 0x7b, 0x72, 0xd1,
  0x0b, 0x18, 0xdb, 0xb4, 0xe0, 0x6f, 0x7a, 0x7c, 0xff, 0xef, 0xfb, 0xc1,
  0xe3, 0x4f, 0xf8, 0x4f, 0x68, 0xfc, 0x02, 0xe0, 0xfb, 0xfe, 0x69, 0x21,
  0xc4, 0x66, 0x10, 0x04, 0xb3, 0xe5, 0x72, 0x79, 0xa7, 0xd1, 0x68, 0x24,
  0xe3, 0x70, 0x0f, 0xc6, 0x82, 0x6a, 0xb5, 0x7a, 0x92, 0x99, 0xbf, 0x48,
  0x29, 0xaf, 0x03, 0xb8, 0x03, 0xe0, 0xad, 0x10, 0x62, 0x31, 0xcf, 0xf7,
  0x62, 0x24, 0xa8, 0xd7, 0xeb, 0x47, 0x94, 0x52, 0x27, 0x6c, 0xdb, 0xfe,
  0xac, 0x94, 0xba, 0x0c, 0xe0, 0x3c, 0x80, 0xc3, 0x00, 0x66, 0x32, 0x99,
  0x10, 0xa2, 0x95, 0x1f, 0x4c, 0x32, 0x12, 0x78, 0x9e, 0xf7, 0x86, 0x88,
  0xe6, 0x88, 0x68, 0x31, 0x4d, 0xd3, 0x6b, 0x44, 0x64, 0x1b, 0x63, 0x1e,
  0x10, 0xd1, 0x33, 0x22, 0x7a, 0xdd, 0x6a, 0xb5, 0xee, 0xe6, 0x07, 0x93,
  0xe4, 0x82, 0x15, 0x00, 0x57, 0x88, 0x68, 0xc9, 0x71, 0x9c, 0x87, 0x69,
  0x9a, 0x42, 0x29, 0xa5, 0x8c, 0x31, 0x37, 0xda, 0xed, 0xf6, 0xe3, 0x7c,
  0x79, 0x37, 0x46, 0x82, 0x30, 0x0c, 0x0b, 0xdd, 0x6e, 0xf7, 0x28, 0x80,
  0x17, 0xcc, 0x7c, 0x4e, 0x6b, 0x0d, 0xc7, 0x71, 0x46, 0x0b, 0x49, 0x92,
  0xcc, 0xc4, 0x71, 0xfc, 0xfd, 0xf7, 0xfa, 0xbf, 0x4c, 0xb6, 0xa0, 0x5d,
  0xd7, 0xb5, 0xfa, 0xfd, 0xfe, 0x0f, 0x00, 0x5f, 0x01, 0xbc, 0x23, 0xa2,
  0x27, 0xc6, 0x98, 0xf9, 0x4a, 0xa5, 0xf2, 0xaa, 0xd3, 0xe9, 0xdc, 0x27,
  0xa2, 0x4f, 0x42, 0x88, 0xa7, 0xf9, 0xcd, 0xa4, 0xe0, 0x2a, 0x11, 0x9d,
  0x22, 0xa2, 0xe5, 0x38, 0x8e, 0x7b, 0x7f, 0x66, 0xeb, 0xc5, 0x62, 0x71,
  0x41, 0x6b, 0x3d, 0x5f, 0x2a, 0x95, 0x3e, 0x48, 0x29, 0x57, 0x99, 0x79,
  0x29, 0x8a, 0xa2, 0x9d, 0x2c, 0xff, 0x4b, 0xb0, 0x1b, 0xbe, 0xef, 0x2f,
  0x00, 0x38, 0x0b, 0xe0, 0x38, 0x00, 0x65, 0x59, 0xd6, 0x9a, 0xe3, 0x38,
  0xef, 0x93, 0x24, 0xa9, 0xc7, 0x71, 0x1c, 0x4d, 0x15, 0x84, 0x61, 0x58,
  0xea, 0xf5, 0x7a, 0x8f, 0x00, 0x5c, 0x62, 0xe6, 0x43, 0xc3, 0xe1, 0x10,
  0xc6, 0x98, 0x35, 0x00, 0xb3, 0xcc, 0x7c, 0x31, 0x8a, 0xa2, 0xad, 0xa9,
  0x82, 0x20, 0x08, 0x8e, 0x15, 0x0a, 0x85, 0x6f, 0xb6, 0x6d, 0x43, 0x4a,
  0x09, 0x66, 0xce, 0xda, 0xc9, 0x1a, 0xfb, 0x08, 0x60, 0x5b, 0x08, 0xb1,
  0x31, 0x55, 0x90, 0x11, 0x04, 0xc1, 0x19, 0xad, 0xf5, 0x6d, 0x66, 0xbe,
  0x40, 0x44, 0x18, 0x0c, 0x06, 0xcf, 0x89, 0x28, 0x04, 0xb0, 0x22, 0x84,
  0xb8, 0xb9, 0xaf, 0x20, 0xc7, 0xf3, 0xbc, 0x65, 0xd7, 0x75, 0x6f, 0x49,
  0x29, 0xef, 0x59, 0x96, 0xf5, 0xd2, 0xb6, 0xed, 0xcd, 0x66, 0xb3, 0xf9,
  0xf3, 0xc0, 0x82, 0x8c, 0x5a, 0xad, 0x36, 0x97, 0xfd, 0x7b, 0x3c, 0x38,
  0x48, 0x0b, 0xfb, 0xf1, 0x0b, 0x00, 0x98, 0xae, 0x11, 0x83, 0x80, 0xc1,
  0x44, 0x00, 0x00, 0x00, 0x10, 0x64, 0x65, 0x42, 0x47, 0x42, 0x44, 0x31,
  0x45, 0x46, 0x32, 0x34, 0x39, 0x34, 0x41, 0x46, 0x33, 0x45, 0x41, 0x41,
  0x46, 0xbd, 0x73, 0xb6, 0x9c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
  0x44, 0xae, 0x42, 0x60, 0x82
};
    server.send_P(200, "image/png" , favicon_16x16_png, sizeof(favicon_16x16_png));
}

void handleTest() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send ( 200, "text/html", "start");
  server.sendContent("");
}

void handleFlash() {
  const uint32_t flashSize = ESP.getFlashChipSize();
  static const size_t chunkSize = 4096;
  static uint8_t buf[chunkSize];

  server.client().setNoDelay(true);
  server.sendHeader("Content-Disposition", "attachment; filename=\"flash.bin\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(flashSize);
  server.send(200, "application/octet-stream", "");

  uint32_t offset = 0;
  uint32_t failed = 0;
  uint32_t lastLog = 0;
  while (offset < flashSize) {
    if (!server.client().connected()) {
      log_w("flash dump: client disconnected at 0x%08x", offset);
      break;
    }
    size_t chunk = (flashSize - offset >= chunkSize) ? chunkSize : (flashSize - offset);
    if (!ESP.flashRead(offset, (uint32_t *)buf, chunk)) {
      log_e("flashRead failed at 0x%08x", offset);
      break;
    }
    size_t sent = server.client().write(buf, chunk);
    if (sent < chunk) {
      if (++failed >= 5) {
        log_w("flash dump: write stalled, abort at 0x%08x", offset);
        break;
      }
    } else {
      failed = 0;
    }
    offset += sent;
    if (offset - lastLog >= (256UL * 1024)) {
      Serial.printf("flash dump: %u / %u\n", offset, flashSize);
      lastLog = offset;
    }
    yield();
  }
}

void handleOldApp() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *old = esp_ota_get_next_update_partition(running);
  if (!old) {
    server.send(404, "text/plain", "no inactive app partition");
    return;
  }
  static const size_t chunkSize = 4096;
  static uint8_t buf[chunkSize];

  uint32_t end = 0;
  for (uint32_t off = 0; off < old->size; off += chunkSize) {
    size_t chunk = (old->size - off >= chunkSize) ? chunkSize : (old->size - off);
    if (!ESP.partitionRead(old, off, (uint32_t *)buf, chunk)) {
      log_e("partitionRead failed at 0x%x", off);
      server.send(500, "text/plain", "read failed");
      return;
    }
    for (size_t i = 0; i < chunk; i++) {
      if (buf[i] != 0xFF) {
        end = off + i + 1;
      }
    }
  }
  if (end == 0) {
    end = old->size;
  }

  server.client().setNoDelay(true);
  server.sendHeader("Content-Disposition", "attachment; filename=\"oldapp.bin\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(end);
  server.send(200, "application/octet-stream", "");

  uint32_t offset = 0;
  uint32_t failed = 0;
  while (offset < end) {
    if (!server.client().connected()) {
      log_w("oldapp dump: client disconnected at 0x%x", offset);
      break;
    }
    size_t chunk = (end - offset >= chunkSize) ? chunkSize : (end - offset);
    if (!ESP.partitionRead(old, offset, (uint32_t *)buf, chunk)) {
      log_e("partitionRead failed at 0x%x", offset);
      break;
    }
    size_t sent = server.client().write(buf, chunk);
    if (sent < chunk) {
      if (++failed >= 5) {
        log_w("oldapp dump: write stalled, abort at 0x%x", offset);
        break;
      }
    } else {
      failed = 0;
    }
    offset += sent;
    yield();
  }
}

static uint32_t usedBytes(uint32_t addr, uint32_t size) {
  static uint8_t buf[4096];
  uint32_t used = 0;
  for (uint32_t off = 0; off < size; off += sizeof(buf)) {
    uint32_t n = size - off;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (!ESP.flashRead(addr + off, (uint32_t*)buf, n)) return size;
    for (uint32_t i = 0; i < n; i++)
      if (buf[i] != 0xFF) used = off + i + 1;
  }
  return used;
}

String memMapHtml() {
  struct Ent { const char* name; uint32_t addr; uint32_t size; uint8_t kind; uint32_t used; };
  static const size_t maxN = 17;
  Ent e[maxN];
  size_t n = 0;
  e[n++] = {"bootloader", 0, 0x9000, 0, 0};

  const esp_partition_t* running = esp_ota_get_running_partition();
  for (int type = ESP_PARTITION_TYPE_APP; type <= ESP_PARTITION_TYPE_DATA; type++) {
    esp_partition_iterator_t it = esp_partition_find((esp_partition_type_t)type,
                                                     ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it && n < maxN; it = esp_partition_next(it)) {
      const esp_partition_t* p = esp_partition_get(it);
      uint8_t kind = p->type == ESP_PARTITION_TYPE_APP ? (p == running ? 1 : 2) : 0;
      e[n++] = {p->label, p->address, p->size, kind, 0};
    }
    esp_partition_iterator_release(it);
  }
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j && e[j].addr < e[j-1].addr; j--) {
      Ent t = e[j]; e[j] = e[j-1]; e[j-1] = t;
    }

  const uint32_t total = ESP.getFlashChipSize();
  uint32_t covered = 0;
  for (size_t i = 0; i < n; i++) {
    covered += e[i].size;
    e[i].used = usedBytes(e[i].addr, e[i].size);
  }
  if (covered < total)
    e[n++] = {"unused", covered, total - covered, 0, 0};

  String h = "<div class=card><b>flash layout</b> <span class=muted>"
           + String(total / 1024 / 1024) + " MB</span><div class=labels>";
  for (size_t i = 0; i < n; i++) {
    float w = 100.0f * e[i].size / total;
    h += "<span class='" + String(w < 3.0f ? "tall" : "") + "' style='width:" + String(w, 1)
      + "%'>" + e[i].name + "</span>";
  }
  h += "</div><div class=bar>";
  for (size_t i = 0; i < n; i++) {
    float w = 100.0f * e[i].size / total;
    float wu = e[i].used * 100.0f / e[i].size;
    if (wu > 99.9f) wu = 100.0f;
    float wf = 100.0f - wu;
    const char* cls = e[i].kind == 1 ? "appactive" : e[i].kind == 2 ? "appinactive" : "other";
    h += "<div class=sec style='width:" + String(w, 1) + "%' title='" + e[i].name + " @0x"
      + String(e[i].addr, HEX) + ": " + String(e[i].used) + "/" + String(e[i].size) + " B'>";
    if (wf < 0.2f) {
      h += "<div class='fill " + String(cls) + "'>" + String(e[i].kind == 1 ? "ACTIVE" : "") + "</div>";
    } else {
      h += "<div class='fill " + String(cls) + "' style='width:" + String(wu, 1) + "%'>"
        + String(e[i].kind == 1 ? "ACTIVE" : "") + "</div>";
      h += "<div class='fill free' style='width:" + String(wf, 1) + "%'></div>";
    }
    h += "</div>";
  }
  h += "</div><div class=legend><span class=appactive></span>active app "
       "<span class=appinactive></span>inactive app "
       "<span class=other></span>other <span class=free></span>free</div>";

  h += "<table class=ptab><tr><th>name</th><th>addr</th><th>size</th><th>used</th><th>free</th></tr>";
  for (size_t i = 0; i < n; i++) {
    h += "<tr><td>" + String(e[i].name) + (e[i].kind == 1 ? " *" : "") + "</td><td>0x"
      + String(e[i].addr, HEX) + "</td><td>" + String(e[i].size) + "</td><td>"
      + String(e[i].used) + "</td><td>" + String(e[i].size - e[i].used) + "</td></tr>";
  }
  h += "</table><p class=muted>* running app</p></div>";
  return h;
}

// ---------------------------------------------------------------------------
// Legacy NVS helpers (types declared at the top of the file)
// ---------------------------------------------------------------------------

// PBKDF2-HMAC-SHA1 (32-byte WPA2 PMK) via mbedtls pkcs5 — core 3.3.11 ships
// mbedtls_pkcs5_pbkdf2_hmac_ext (core 2.0.13 lacked it, hence the old manual
// block-chaining loop).
static bool pmkMatches(const String& pass, const String& ssid, const uint8_t stored[32]) {
  uint8_t expected[32];
  if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1,
                                    (const unsigned char*)pass.c_str(), pass.length(),
                                    (const unsigned char*)ssid.c_str(), ssid.length(),
                                    4096, 32, expected) != 0)
    return false;
  bool ok = memcmp(stored, expected, sizeof(expected)) == 0;
  Serial.printf("[dbg] pmk computed=");
  for (int i = 0; i < 32; i++) Serial.printf("%02x", expected[i]);
  Serial.printf(" stored=");
  for (int i = 0; i < 32; i++) Serial.printf("%02x", stored[i]);
  Serial.println(ok ? " OK" : " MISMATCH");
  return ok;
}

// Read a legacy string BLOB (ap.ssid / ap.passwd / sta.ssid / sta.pswd).
// esp_wifi stores these as raw bytes with a length prefix (u32 LE for ssid,
// nothing for passwd) and 0x00 padding. The prefix width is not assumed:
// every leading byte that is not printable ASCII is dropped, then the string
// runs until the first NUL.
static String readLegacyBlob(Preferences& p, const char* key) {
  int n = p.getBytesLength(key);
  if (n <= 0 || n > 128) return String();
  uint8_t buf[128];
  int got = p.getBytes(key, buf, sizeof(buf));
  if (got <= 0) return String();
  size_t i = 0;
  while (i < (size_t)got && (buf[i] < 0x20 || buf[i] > 0x7E)) i++;
  String s;
  for (; i < (size_t)got && buf[i] != 0; i++) s += (char)buf[i];
  return s;
}

static void readLegacyNvs(LegacyNvs& l) {
  // Read-only open of the Tractive firmware's namespace via the normal NVS
  // driver (Preferences). Confirmed working on the live unit via /test; the
  // earlier "esp_flash_read returns garbage" diagnosis is no longer reproduced,
  // so the TEMP spi_flash_mmap decoder is gone.
  Preferences p;
  if (!p.begin("nvs.net80211", true)) return;  // fresh flash: no legacy record
  l.present = true;

  l.apSsid    = readLegacyBlob(p, "ap.ssid");
  l.apPasswd  = readLegacyBlob(p, "ap.passwd");
  l.staSsid   = readLegacyBlob(p, "sta.ssid");
  l.staPasswd = readLegacyBlob(p, "sta.pswd");

  // --- ap.pmk: verify, don't just trust the 32-byte blob ---
  uint8_t foundPmk[32];
  int foundPmkLen = p.getBytes("ap.pmk", foundPmk, sizeof(foundPmk));
  p.end();
  Serial.println("[dbg] prefs decode apSsid='" + l.apSsid + "' apPasswd='" + l.apPasswd + "' pmkLen=" + String(foundPmkLen));
  if (foundPmkLen == 0) {
    l.pmk = PMK_MISSING;
  } else if (foundPmkLen != 32) {
    l.pmk = PMK_EMPTY;
  } else if (l.apSsid.length() == 0 || l.apPasswd.length() == 0) {
    l.pmk = PMK_UNVERIFIABLE;  // no ssid+passphrase to derive the key from
  } else {
    l.pmk = pmkMatches(l.apPasswd, l.apSsid, foundPmk) ? PMK_OK : PMK_BAD;
  }
}

// Escape HTML special characters before embedding untrusted strings.
static String esc(const String& s) {
  String r;
  for (unsigned i = 0; i < s.length(); i++) {
    switch (s[i]) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      default:  r += s[i];
    }
  }
  return r;
}

// HTML card summarising what the previous (Tractive) firmware stored. Used on
// /debug and /config so the user can see the migration source at a glance.
static String legacyNvsHtml() {
  LegacyNvs l;
  readLegacyNvs(l);

  String h = "<div class=card><b>previous firmware NVS</b> "
             "<span class=muted>(read-only &mdash; what the old Tractive app stored)</span>";
  if (!l.present) {
    h += "<p class=muted>no legacy NVS found (fresh flash) &mdash; this firmware "
         "falls back to its default credentials.</p></div>";
    return h;
  }

  h += "<table class=ptab>";
  h += "<tr><th>old AP ssid</th><td>" + (l.apSsid.length() ? esc(l.apSsid) : "<i>none</i>") + "</td></tr>";
  h += "<tr><th>old AP password</th><td><code>" + (l.apPasswd.length() ? esc(l.apPasswd) : "<i>none</i>") + "</code></td></tr>";
  if (l.staSsid.length()) {
    h += "<tr><th>old STA ssid</th><td>" + esc(l.staSsid) + "</td></tr>";
    if (l.staPasswd.length())
      h += "<tr><th>old STA password</th><td><code>" + esc(l.staPasswd) + "</code></td></tr>";
  }
  h += "</table>";

  // PMK status: the key reason the old firmware's AP could reject clients.
  const char* color; const char* text;
  switch (l.pmk) {
    case PMK_OK:
      color = "#2e9e44"; text = "OK &mdash; verified (PBKDF2 matches ssid+password)";
      break;
    case PMK_MISSING:
      color = "#c00"; text = "missing &mdash; no ap.pmk stored, the old firmware's AP "
                             "could not accept any client";
      break;
    case PMK_EMPTY:
      color = "#c00"; text = "not a valid PMK (wrong size) &mdash; the old firmware's AP "
                             "could not accept any client";
      break;
    case PMK_BAD:
      color = "#c00"; text = "invalid &mdash; stored ap.pmk does not match ssid+password, "
                             "the old firmware's AP could not accept any client";
      break;
    default:  // PMK_UNVERIFIABLE
      color = "#e0a000"; text = "unverifiable &mdash; ssid or password missing, cannot "
                                "recompute the PMK to check it";
  }
  h += "<p><b>old AP WPA2 key (ap.pmk):</b> <span style=\"color:" + String(color) + "\">"
       + text + "</span></p>";

  h += "</div>";
  return h;
}

void handleDebug() {
  String html = "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten debug</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:64rem;margin:1.5rem auto;padding:0 1rem;line-height:1.5;width:100%;box-sizing:border-box}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    ".muted{color:#888;font-weight:400}"
    ".bar{display:flex;height:26px;border:1px solid #333;border-radius:4px;overflow:hidden;margin:.6rem 0;font-size:11px;color:#fff}"
    ".sec{display:flex;height:100%;box-sizing:border-box;border-right:2px solid #444}"
    ".sec:last-child{border-right:none}"
    ".fill{height:100%;box-sizing:border-box;overflow:hidden;text-align:center;line-height:26px;white-space:nowrap}"
    ".appactive{background:#2e9e44}.appinactive{background:#4a90d9}"
    ".other{background:#b0b0b0}.free{background:#f5f5f5}"
    ".legend{font-size:12px;color:#444}.legend span{display:inline-block;width:11px;height:11px;border:1px solid #999;margin:0 4px 0 12px;vertical-align:-1px}"
    ".legend span:first-child{margin-left:0}"
    ".labels{display:flex;font-size:10px;color:#666;margin-top:.4rem;align-items:center}"
    ".labels span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:center;box-sizing:border-box;border-right:2px solid #444}"
    ".labels span:last-child{border-right:none}"
    ".labels span.tall{writing-mode:vertical-rl;text-overflow:clip}"
    ".ptab{width:100%;border-collapse:collapse;font-size:12px;margin-top:.6rem}"
    ".ptab th,.ptab td{border:1px solid #ddd;padding:.2rem .4rem;text-align:left}"
    ".ptab th{background:#f2f2f2}"
    "</style></head><body><h1>Pfoten debug</h1>";
  html += legacyNvsHtml();
  html += memMapHtml();
  html += "<div class=card><a href=/>back</a></div></body></html>";
  server.send(200, "text/html", html);
}

static String rootHtml() {
  String staState = WiFi.isConnected() ? "<b class=ok>connected</b>"
                                       : "<b class=bad>not connected</b>";
  // STA link display: the network name with the received signal level (RSSI) in
  // dBm next to it when connected; this is how well *we* hear the home network,
  // unrelated to this device's own AP TX power.
  String staNet = WiFi.isConnected()
      ? esc(cfgCssid) + " <i>(" + String(WiFi.RSSI()) + " dBm)</i> "
      : String("");
  String apLabel;
  switch (apState) {
    case AP_HIDDEN: apLabel = "<b class=bad>&#128263; HIDDEN (AP off)</b>"; break;
    case AP_OPEN:   apLabel = (millis() < apOpenUntil)
        ? "<b class=ok>&#128275; OPEN (no password)</b>"
        : "<b class=ok>&#128275; OPEN (cfgPass)</b>"; break;
    default:        apLabel = "<b class=bad>&#128274; LOCKED (lockPw)</b>"; break;
  }
  String window;
  if (apOpenUntil == 0) window = "never opened";
  else if (millis() >= apOpenUntil) window = "expired";
  else window = String((apOpenUntil - millis()) / 1000) + "s left";
  String sig = sigLevel == 0 ? "low" : (sigLevel == 1 ? "middle" : "high");

  String h = "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    ".warn{background:#fff7e0;border-color:#e0a000}"
    ".ok{color:#2e9e44;font-weight:600}"
    ".bad{color:#c00;font-weight:600}"
    "a{color:#0467c8;font-weight:600}"
    "code{background:#eee;padding:.1rem .3rem;border-radius:4px}"
    "</style></head><body><h1>&#128062; Pfoten</h1>";
  h += "<div class=card><b>status</b><br>"
       "&#127777; cpu <b>" + String((int)temperatureRead()) + "&deg;C</b> &middot; "
       "&#128424; STA IP <b>" + (WiFi.isConnected() ? WiFi.localIP().toString() : String("&mdash;")) + "</b><br>"
       "&#128200; STA " + staNet + staState + " &middot; AP " + apLabel + "<br>"
       "&#9202; AP open window: " + window + " &middot; "
       "&#128241; stations: " + String(WiFi.softAPgetStationNum()) + "<br>"
       "&#128225; signal: " + sig + "</div>";
  h += "<div class=card>&#128240; Pfoten v" FW_VERSION " &middot; built " __DATE__ " " __TIME__ "<br>"
       "MAC <code>" + WiFi.softAPmacAddress() + "</code><br>"
       "&#127474;&#127475; default pw: <code>" + String(DEFAULT_PASS) + "</code><br>"
       "based on <a href=https://github.com/bosb/Base_Station_Light>Base_Station_Light</a></div>";
  h += "<div class=\"card warn\"><b>before you flash &mdash; back up first:</b>"
       "<ol><li><a href=/flash>download full chip</a> (<code>flash.bin</code>)</li>"
       "<li><a href=/oldapp>download old app</a> (<code>oldapp.bin</code>)</li></ol>"
       "<p><i>an OTA write can go wrong (bad image, power loss). keep these two files safe &mdash; "
       "you can restore any bricked unit with <code>esptool.py write_flash 0x0 flash.bin</code>.</i></p></div>";
  h += "<div class=card><b>OTA update:</b><br>"
       "<form method=POST action=/update enctype=multipart/form-data>"
       "<input type=file name=update><input type=submit value=Flash></form></div>";
  h += "<div class=card><a href=/config>configure</a> &mdash; AP / client credentials</div>";
  h += "<div class=card><a href=/apoff>&#128263; AP stealth</a> &mdash; hide the AP (no SSID, not trackable)</div>";
  h += "<div class=card><a href=/debug>debug</a> &mdash; flash layout &amp; free space</div>";
  h += "</body></html>";
  return h;
}

void handleStatus() {
  String staState = WiFi.isConnected() ? "<b class=ok>&#9989; connected</b>"
                                       : "<b class=bad>&#10060; not connected</b>";
  // STA link: the network name with the received signal level (RSSI) in dBm —
  // how well *we* hear the home network, unrelated to this device's own AP TX power.
  String staNet = WiFi.isConnected()
      ? esc(cfgCssid) + " <i>(" + String(WiFi.RSSI()) + " dBm)</i>"
      : String("&mdash;");
  String apLabel;
  switch (apState) {
    case AP_HIDDEN: apLabel = "<b class=bad>&#128263; HIDDEN (AP off)</b>"; break;
    case AP_OPEN:   apLabel = (millis() < apOpenUntil)
        ? "<b class=ok>&#128275; OPEN (no password)</b>"
        : "<b class=ok>&#128275; OPEN (cfgPass)</b>"; break;
    default:        apLabel = "<b class=bad>&#128274; LOCKED (lockPw)</b>"; break;
  }
  String window;
  if (apOpenUntil == 0) window = "never opened";
  else if (millis() >= apOpenUntil) window = "expired";
  else window = String((apOpenUntil - millis()) / 1000) + "s left";
  String sig = sigLevel == 0 ? "low" : (sigLevel == 1 ? "middle" : "high");
  String sigDot  = sigLevel == 0 ? "&#128308;" : (sigLevel == 1 ? "&#128993;" : "&#128994;");
  String sigCol  = sigLevel == 0 ? "#c00" : (sigLevel == 1 ? "#e0a000" : "#2e9e44");

  String h = "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten status</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    ".ok{color:#2e9e44;font-weight:600}"
    ".bad{color:#c00;font-weight:600}"
    "a{color:#0467c8;font-weight:600}"
    "code{background:#eee;padding:.1rem .3rem;border-radius:4px}"
    "</style></head><body><h1>&#128062; Pfoten</h1>";
  h += "<div class=card><b>&#128200; status</b><br>"
       "&#127777; cpu <b>" + String((int)temperatureRead()) + "&deg;C</b> &middot; "
       "&#128424; STA IP <b>" + (WiFi.isConnected() ? WiFi.localIP().toString() : String("&mdash;")) + "</b><br>"
       "&#128225; STA " + staNet + " &middot; " + staState + "<br>"
       "&#128274; AP " + apLabel + "<br>"
       "&#9202; AP open window: " + window + "<br>"
       "&#128241; AP stations: " + String(WiFi.softAPgetStationNum()) + "<br>"
       "&#128225; signal level: <b style=\"color:" + sigCol + "\">" + sigDot + " " + sig + "</b></div>";
  h += "<div class=card><a href=/>&#127968; back</a> &middot; <a href=/debug>debug</a> &middot; <a href=/config>configure</a></div>";
  h += "</body></html>";
  server.send(200, "text/html", h);
}

// ---------------------------------------------------------------------------
// Config storage (Preferences namespace "cfg")
//
// The Tractive legacy data lives in nvs.net80211 and is READ-ONLY for us (see
// readLegacyNvs). Our own config lives in a separate Preferences namespace so
// the legacy record stays untouched:
//
//   key         type  meaning
//   pf.ap.ssid  STR   our AP ssid (1..32 chars)
//   pf.ap.passwd STR  our AP password (8..63 chars)
//   pf.sta.ssid STR   client (STA) network ssid, "" = STA disabled
//   pf.sta.passwd STR client network password
//   pf.usr      U8    1 = user saved via /config
//   pf.ver      U8    config schema version (future migrations)
//   pf.apoff    U8    1 = AP hidden (stealth, no SSID); set via /apoff
//
// The Tractive legacy data is NOT imported: a fresh device boots with the
// MAC-derived default AP ssid + DEFAULT_PASS and STA disabled until the user
// configures the home network via /config. The legacy record stays read-only
// info on the /debug and /config pages (see legacyNvsHtml).
// ---------------------------------------------------------------------------

// Fresh-install default AP ssid: "Pfote_0" + the last 4 hex chars of the MAC,
// incremented by 1 (same rule the old Tractive SSIDs used), e.g. MAC
// ...f6:e4 -> f6e5 -> "Pfote_0F6E5". Read from eFuse, so it works before WiFi
// init and stays stable across boots.
static String defaultSsid() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint32_t v = ((uint32_t)mac[4] << 8) | mac[5];  // last 4 hex digits (last two bytes)
  v = (v + 1) & 0xFFFF;
  char buf[16];
  snprintf(buf, sizeof(buf), "Pfote_0%04X", v);
  return String(buf);
}

// Load the cfg namespace into the runtime globals, with safe fallbacks.
static void loadCfg() {
  Preferences p;
  if (!p.begin("cfg", false)) {
    log_e("cfg: Preferences open failed, using defaults");
    cfgSsid = defaultSsid();
    cfgPass = DEFAULT_PASS;
    return;
  }
  cfgSsid  = p.getString(K_AP_SSID,  "");
  cfgPass  = p.getString(K_AP_PASSWD, DEFAULT_PASS);
  cfgCssid = p.getString(K_STA_SSID, "");
  cfgCpass = p.getString(K_STA_PASSWD, "");
  cfgUsr   = p.getUChar(K_USR, 0) != 0;
  cfgApoff = p.getUChar(K_APOFF, 0) != 0;
  sigLevel = p.getUChar(K_SIG, 0);        // persisted AP signal-strength display
  if (sigLevel > 2) sigLevel = 0;
  p.putUChar(K_VER, 1);                   // stamp current schema version
  p.end();

  // Never let an empty / absurd value through (would make the AP open or the
  // softAP call fail). An empty stored ssid means "fresh device": use the
  // MAC-derived default instead.
  if (cfgSsid.isEmpty() || cfgSsid.length() > 32) cfgSsid = defaultSsid();
  if (cfgPass.length() < 8 || cfgPass.length() > 63) cfgPass = DEFAULT_PASS;

  log_i("cfg: ssid='%s' pass=%u c_ssid='%s' usr=%d",
        cfgSsid.c_str(), cfgPass.length(), cfgCssid.c_str(), cfgUsr);
}

// ---------------------------------------------------------------------------
// /config page (GET: info + form, POST: validate + save)
// ---------------------------------------------------------------------------

static String configHtml() {
  String h = "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten config</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    "label{display:block;margin-top:.6rem;font-weight:600}"
    "input[type=text]{width:100%;box-sizing:border-box;padding:.35rem;margin-top:.2rem}"
    "input[type=submit]{margin-top:1rem;padding:.5rem 1.2rem}"
    "a{color:#0467c8;font-weight:600} code{background:#eee;padding:.1rem .3rem;border-radius:4px}"
    ".muted{color:#888;font-weight:400}"
    "</style></head><body><h1>Pfoten config</h1>";
  h += legacyNvsHtml();   // shared card: what the old Tractive firmware stored + pmk check
  h += "<div class=card><b>this firmware</b><p class=muted>"
       "The AP always broadcasts its ssid. It accepts connections only while "
       "it has no client-network link, or for 15 minutes after a 3&nbsp;s "
       "long-press of the button &mdash; during that window the AP is open "
       "with <b>no password</b> so any phone can join. Once it joins the "
       "client network the AP locks (a random password nobody knows), so keep "
       "the client ssid and password correct to reach the device over the LAN. "
       "The AP can be hidden entirely (no SSID broadcast) from the "
       "<a href=/apoff>AP stealth</a> page.</p>"
     "<form method=POST action=/config>"
    "<label>AP ssid (empty = keep current)</label>"
    "<input type=text name=ssid value=\"" + esc(cfgSsid) + "\" maxlength=32>"
    "<label>AP password (8..63 chars; empty = keep current)</label>"
    "<input type=text name=password value=\"" + esc(cfgPass) + "\" maxlength=63>"
    "<label>Client network ssid (empty = disabled, AP stays open)</label>"
    "<input type=text name=ssid_client value=\"" + esc(cfgCssid) + "\" maxlength=32>"
    "<label>Client network password (required if client ssid is set)</label>"
    "<input type=text name=password_client value=\"" + esc(cfgCpass) + "\" maxlength=63>"
    "<input type=submit value=Save>"
    "</form></div>";
  h += "<div class=card><a href=/>back</a></div></body></html>";
  return h;
}

void handleConfigGet() {
  server.send(200, "text/html", configHtml());
}

void handleConfigPost() {
  String newSsid  = server.arg("ssid");
  String newPass  = server.arg("password");
  String newCssid = server.arg("ssid_client");
  String newCpass = server.arg("password_client");
  newSsid.trim(); newPass.trim(); newCssid.trim(); newCpass.trim();

  String msg, cls;
  bool bad = false;
  if (!newSsid.isEmpty() && newSsid.length() > 32) bad = true;
  if (!newPass.isEmpty() && (newPass.length() < 8 || newPass.length() > 63)) bad = true;
  if (!newCssid.isEmpty() && newCpass.isEmpty()) bad = true;   // client needs a password
  if (newCssid.length() > 32) bad = true;

  if (bad) {
    msg = "<b style=\"color:#c00\">Invalid values.</b> Rules: AP ssid 1..32 chars, "
          "AP password 8..63 chars (empty keeps the current one), a client ssid "
          "requires a client password.";
    cls = "warn";
  } else {
    Preferences p;
    if (p.begin("cfg", false)) {
      if (!newSsid.isEmpty()) p.putString(K_AP_SSID, newSsid);
      if (!newPass.isEmpty()) p.putString(K_AP_PASSWD, newPass);
      p.putString(K_STA_SSID, newCssid);
      p.putString(K_STA_PASSWD, newCpass);
      p.putUChar(K_USR, 1);   // user saved: this is now the authoritative config
      p.putUChar(K_VER, 1);
      p.end();
    }
    msg = "<b>Saved.</b> Reboot to apply (STA + AP credentials load in setup).";
    cls = "ok";
  }

  String html = "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten config</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    ".ok{background:#e9f7ec;border-color:#2e9e44}.warn{background:#fff7e0;border-color:#e0a000}"
    "a{color:#0467c8;font-weight:600}"
    "</style></head><body><div class='card " + cls + "'>" + msg + "</div>"
    "<div class=card><a href=/config>edit again</a> &middot; "
    "<a href=/reboot>reboot now</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handleReboot() {
  server.send(200, "text/html",
    "<!DOCTYPE html><html><body><h1>rebooting&#8230;</h1><p>back in ~15 s.</p></body></html>");
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// /apoff — AP stealth: hide the AP entirely (no SSID broadcast) so the device
// is not trackable by Wi-Fi scanners. STA (LAN) keeps running, so the device
// stays reachable at pfoten.local. Recovery without a LAN link: 3 s button
// long-press opens the AP (no password) for the 15-min window.
// ---------------------------------------------------------------------------

static String apoffStateHtml() {
  return cfgApoff ? "<b style=\"color:#c00\">HIDDEN (AP off)</b>"
                  : "<b style=\"color:#2e9e44\">broadcasting</b>";
}

void handleApoffGet() {
  String html = "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten AP stealth</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    "a{color:#0467c8;font-weight:600}"
    "input[type=submit]{padding:.5rem 1.2rem}"
    "code{background:#eee;padding:.1rem .3rem;border-radius:4px}"
    "</style></head><body><h1>&#128263; AP stealth</h1>"
    "<div class=card>AP is currently: " + apoffStateHtml() + "</div>"
    "<div class=card><b>why?</b> a visible Wi-Fi name identifies the device to scanners (a tracking "
    "indicator). With the AP hidden the device sends no SSID and is not trackable over Wi-Fi; the "
    "LAN link (<code>pfoten.local</code>) keeps working.</div>"
    "<div class=card><b>recovery</b>: a 3&nbsp;s button long-press opens the AP (no password) for "
    "15&nbsp;min even while hidden, so you can always connect and switch it back on here.</div>"
    "<form method=POST action=/apoff>"
    "<input type=hidden name=apoff value=\"" + String(cfgApoff ? "0" : "1") + "\">"
    "<input type=submit value=\"" + String(cfgApoff ? "Show AP (broadcast)" : "Hide AP (stealth)") + "\">"
    "</form>"
    "<div class=card><a href=/>back</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handleApoffPost() {
  cfgApoff = (server.arg("apoff") == "1");
  Preferences p;
  if (p.begin("cfg", false)) { p.putUChar(K_APOFF, cfgApoff ? 1 : 0); p.end(); }
  applyApMode(true);
  String html = "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Pfoten AP stealth</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:36rem;margin:2rem auto;line-height:1.5}"
    ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:1rem 0}"
    "a{color:#0467c8;font-weight:600}"
    "</style></head><body><div class=card>Saved. AP is now: " + apoffStateHtml() + ".</div>"
    "<div class=card><a href=/apoff>stealth page</a> &middot; <a href=/>home</a></div></body></html>";
  server.send(200, "text/html", html);
}

static const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external pin";
    case ESP_RST_SW:         return "software (ESP.restart)";
    case ESP_RST_PANIC:      return "PANIC (crash)";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "other watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep-sleep wake";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_CPU_LOCKUP: return "cpu lockup";
    case ESP_RST_EFUSE:      return "efuse";
    default:                 return "other";
  }
}

// Direct esp_event registration: the Arduino onEvent system in 3.3.11 does NOT
// translate WIFI_EVENT_AP_* / WIFI_EVENT_STA_* into arduino events (only SCAN,
// WPS, FTM, SC, PROV), so WiFi.onEvent never fires for AP join/leave. Register
// our own handler straight on the esp_event loop to capture the deauth reason.
static void apEventCb(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
      wifi_event_ap_staconnected_t* d = (wifi_event_ap_staconnected_t*)data;
      Serial.printf("[ape] client JOINED mac=%02x:%02x:%02x:%02x:%02x:%02x aid=%u\n",
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
      wifi_event_ap_stadisconnected_t* d = (wifi_event_ap_stadisconnected_t*)data;
      Serial.printf("[ape] client LEFT reason=%u aid=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    (unsigned)d->reason, (unsigned)d->aid,
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
      wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
      Serial.printf("[ape] STA iface disconnected reason=%u\n", (unsigned)d->reason);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
      Serial.println("[ape] STA connected");
    } else if (id == WIFI_EVENT_AP_START) {
      Serial.println("[ape] AP started");
    } else if (id == WIFI_EVENT_AP_STOP) {
      Serial.println("[ape] AP STOPPED");
    } else if (id == WIFI_EVENT_STA_START) {
      Serial.println("[ape] STA started");
    }
  } else if (base == IP_EVENT) {
    if (id == IP_EVENT_AP_STAIPASSIGNED) {
      ip_event_ap_staipassigned_t* d = (ip_event_ap_staipassigned_t*)data;
      Serial.printf("[ape] DHCP: assigned ip=%u.%u.%u.%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    IP2STR(&d->ip), d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    }
  }
}

// Generate a fresh random 16-char lock password, used while the AP is locked.
static void generateLockPw() {
  static const char charset[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int i = 0; i < 16; i++) lockPw[i] = charset[esp_random() % (sizeof(charset) - 1)];
  lockPw[16] = '\0';
}

// Print the exact AP credentials handed to WiFi.softAP() (ASCII + raw bytes),
// so any encoding/mangling is visible in the boot serial log.
static void dbgApCreds(const char* ssid, const char* pass) {
  Serial.printf("[ap] softAP ssid='%s' (len=%u) hex:", ssid, (unsigned)strlen(ssid));
  for (size_t i = 0; i < strlen(ssid); i++) Serial.printf(" %02x", (uint8_t)ssid[i]);
  Serial.printf("\n[ap] softAP pass='%s' (len=%u) hex:", pass, (unsigned)strlen(pass));
  for (size_t i = 0; i < strlen(pass); i++) Serial.printf(" %02x", (uint8_t)pass[i]);
  Serial.printf("\n[ap] softAP lockPw='%s' (len=%u) hex:", lockPw, (unsigned)strlen(lockPw));
  for (size_t i = 0; i < strlen(lockPw); i++) Serial.printf(" %02x", (uint8_t)lockPw[i]);
  Serial.println();
}

// Real AP TX power per signal-strength level (0=low, 1=middle, 2=high). Uses
// esp_wifi_set_max_tx_power() (unit 0.25 dBm) so the levels are exact, unlike
// the Arduino wifi_power_t enum whose 19.5 dBm setting actually applies 18 dBm:
//   8  -> 2 dBm   lowest
//   44 -> 11 dBm  middle of the usable 2..20 dBm range
//   80 -> 20 dBm  highest
// Applied at boot, on every short-press cycle, and after each softAP() reconfig
// (a reconfig can otherwise reset the radio's TX power).
static void applyTxPower() {
  if (sigLevel > 2) return;
  static const int8_t pwr[3] = { 8, 44, 80 };
  esp_err_t e = esp_wifi_set_max_tx_power(pwr[sigLevel]);
  int8_t now = 0;
  esp_wifi_get_max_tx_power(&now);
  Serial.printf("[rf] sigLevel=%d setTxPower(%.1f dBm) -> %s (now %.1f dBm)\n",
                sigLevel, pwr[sigLevel] / 4.0,
                e == ESP_OK ? "OK" : esp_err_to_name(e), now / 4.0);
}

// Re-apply the AP state machine:
//   hidden  -> AP interface torn down (WiFi.enableAP(false)): no SSID broadcast
//              whenever cfgApoff is set and we are NOT inside the long-press
//              open window. The long-press window is the only way back in when
//              there is no LAN link.
//   open    -> no password, while inside the long-press open window
//              (millis() < apOpenUntil) — anyone can join a long-pressed unit;
//   open    -> cfgPass,     while there is no STA link (no window active);
//   locked  -> lockPw,    otherwise
// Called at boot (force), after a /config or /apoff save (force), on long-press
// (force, so an already-open cfgPass AP flips to passwordless right away), and
// from loop() so any STA-link change or open-window expiry takes effect. Only
// reconfigures when the state or the password mode actually flips, so a stable
// AP is never churned.
static void applyApMode(bool force) {
  bool inWindow = millis() < apOpenUntil;
  bool pwless   = inWindow;                        // long-press window = open, no password
  ApState want;
  if (cfgApoff && !inWindow)      want = AP_HIDDEN;
  else if (!WiFi.isConnected() || inWindow) want = AP_OPEN;
  else                            want = AP_LOCKED;
  if (!force && want == apState && pwless == apPwless) return;

  const char* name = want == AP_HIDDEN ? "HIDDEN (AP off)" :
                     want == AP_OPEN   ? (pwless ? "OPEN (no password)" : "OPEN (cfgPass)") : "LOCKED (lockPw)";
  Serial.printf("[lock] AP -> %s (up=%lus)\n", name, millis() / 1000);
  if (want == AP_HIDDEN) {
    WiFi.enableAP(false);              // tear down the AP interface: no beacons
  } else {
    const char* pass = want == AP_OPEN ? (pwless ? "" : cfgPass.c_str()) : lockPw;
    dbgApCreds(cfgSsid.c_str(), pass);
    if (!WiFi.softAP(cfgSsid.c_str(), pass)) {
      log_e("softAP reconfigure failed");
    }
    applyTxPower();                    // a reconfig may reset TX power: re-apply
  }
  apState = want;
  apPwless = pwless;
  applySigLed();                       // signal LEDs follow hidden/open state
}

// Non-blocking pin-3 LED driver: finish the 3-blink burst from a long-press,
// then show the state — a connected client = steady on (any mode), hidden or
// locked with no client = off, open with no client = slow blink. Pin 3's LED
// is active-HIGH (verified on-device 2026-08-09).
static void updateOpenLed() {
  if (ledFlashCount > 0) {
    if (millis() - ledFlashTick >= 100) {
      ledFlashTick = millis();
      ledFlashOn = !ledFlashOn;
      ledcWrite(3, ledFlashOn ? ledDuty() : 0);
      ledFlashCount--;
    }
    return;
  }
  if (WiFi.softAPgetStationNum() > 0) {
    ledcWrite(3, ledDuty());            // client joined (open or locked): steady on
  } else if (apState != AP_OPEN) {
    ledcWrite(3, 0);                    // hidden or locked, no client: off
  } else if (millis() - ledBlinkTick >= 500) {
    ledBlinkTick = millis();            // open, no client: slow blink
    ledBlinkOn = !ledBlinkOn;
    ledcWrite(3, ledBlinkOn ? ledDuty() : 0);
  }
}

// Dimming duty: full brightness until the ledDimAt deadline, then 10%.
static uint8_t ledDuty() {
  return (ledDimAt != 0 && millis() < ledDimAt) ? LED_FULL : LED_DIM;
}

// Re-arm the 5 s full-brightness window (called at boot and on every press).
static void armDim() {
  ledDimAt = millis() + DIM_DELAY_MS;
}

// Drive LEDs 5 + 10 from sigLevel (active-HIGH). Called at boot, on every
// short-press cycle, and on every applyApMode() transition; while the AP is
// hidden the signal display stays off.
static void applySigLed() {
  if (apState == AP_HIDDEN) {           // stealth: no signal LEDs either
    ledcWrite(ledPin, 0);
    ledcWrite(5, 0);
    return;
  }
  uint8_t duty = ledDuty();
  bool leftOn = (sigLevel == 0) || (sigLevel == 2);   // LED 10: low + high
  bool midOn  = (sigLevel == 1) || (sigLevel == 2);   // LED 5:  middle + high
  ledcWrite(ledPin, leftOn ? duty : 0);
  ledcWrite(5, midOn ? duty : 0);
}

void setup() {
  Serial.begin(115200);
  // USB-CDC (HWCDC) writes block for up to ~2 s (20 x 100 ms) each when the
  // host is attached but not reading (e.g. serial monitor closed). That would
  // stall the boot logs and slow the whole loop. With a 0 ms timeout the debug
  // prints drop bytes instead of blocking the firmware.
  Serial.setTxTimeoutMs(0);
  Serial.printf("\n=== BOOT, reset reason: %s (%d) ===\n",
                resetReasonName(), (int)esp_reset_reason());

  // Config must be loaded before any WiFi call: fills the runtime globals from
  // the cfg namespace (no legacy import — a fresh device uses the MAC-derived
  // default below).
  loadCfg();
  Serial.printf("[boot] fresh default ssid='%s'\n", defaultSsid().c_str());
  Serial.printf("[boot] cfg: ssid='%s' pass=%u c_ssid='%s' usr=%d heap=%u\n",
                cfgSsid.c_str(), cfgPass.length(), cfgCssid.c_str(), cfgUsr,
                ESP.getFreeHeap());
  Serial.printf("[boot] cfg PASS CONTENT='%s'\n", cfgPass.c_str());
  Serial.printf("[boot] cfg PASS HEX:");
  for (unsigned i = 0; i < cfgPass.length(); i++) Serial.printf(" %02x", (uint8_t)cfgPass[i]);
  Serial.printf(" (len=%u)\n", cfgPass.length());

  // Boot-time diag: what the legacy Tractive NVS holds + our pmk verdict.
  LegacyNvs dbg;
  readLegacyNvs(dbg);
  Serial.println("[dbg] boot legacy present=" + String(dbg.present)
                 + " apSsid='" + dbg.apSsid + "' pmk=" + String(dbg.pmk));

  pinMode(9, OUTPUT); // on the 8-pin header
  pinMode(3, OUTPUT); // right
  pinMode(5, OUTPUT); // middle
  pinMode(ledPin, OUTPUT); // left
  pinMode(buttonPin, INPUT_PULLUP); // INPUT_PULLUP INPUT_PULLDOWN INPUT
  digitalWrite(ledPin, LOW); // off...
  ledcAttach(3, 5000, 8);     // all three LEDs via LEDC so the duty is dimmable
  ledcAttach(5, 5000, 8);
  ledcAttach(ledPin, 5000, 8);
  armDim();                   // full brightness for the first 5 s, then dim

  // RAM-only WiFi storage: our WiFi calls must never overwrite the Tractive
  // record in nvs.net80211 (that namespace is read-only to us).
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);   // must come BEFORE esp_event registration: the default
                            // event loop is only created during WiFi init.

  // Real AP TX power per the persisted signal-strength level (see applyTxPower).
  applyTxPower();

  // Direct esp_event registration must happen AFTER WiFi.mode(): registering
  // earlier fails with ESP_ERR_INVALID_STATE (no default event loop yet).
  esp_err_t er = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                     apEventCb, NULL, NULL);
  esp_err_t er2 = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                      apEventCb, NULL, NULL);
  Serial.printf("[boot] esp_event register -> wifi=%s ip=%s\n",
                esp_err_to_name(er), esp_err_to_name(er2));

  generateLockPw();
  Serial.printf("[boot] starting softAP ssid='%s' (AP %s at boot)\n",
                cfgSsid.c_str(), apState == AP_OPEN ? "open" : "locked");
  applyApMode(true);   // initial softAP; the STA link (if any) is not up yet, so this is open
  Serial.printf("[boot] softAP up: ip=%s heap=%u state=%s\n",
                WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap(),
                apState == AP_OPEN ? "open" : "locked");

  server.on("/favicon.ico", handleFavicon);
  server.on("/test", handleTest);
  server.on("/flash", handleFlash);
  server.on("/oldapp", handleOldApp);
  server.on("/debug", handleDebug);
  server.on("/status", handleStatus);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/apoff", HTTP_GET, handleApoffGet);
  server.on("/apoff", HTTP_POST, handleApoffPost);
  server.on("/reboot", handleReboot);
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", rootHtml());
  });
  /*handling uploading firmware file */
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
 }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();

  // Client (STA) connection: only when a client ssid is configured. Empty
  // client creds = STA disabled, so the AP stays open (fallback to reach it).
  if (!cfgCssid.isEmpty()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfgCssid.c_str(), cfgCpass.c_str());
    log_i("STA: connecting to '%s'", cfgCssid.c_str());
    Serial.printf("[boot] STA: connecting to '%s'\n", cfgCssid.c_str());
  } else {
    log_i("STA: disabled (no client ssid) -> AP stays open");
    Serial.println("[boot] STA: disabled (no client ssid) -> AP stays open");
  }

  ledcWrite(3, ledDuty());   // pin-3 LED on at boot (full until the dim deadline)

  applySigLed();   // initial AP signal-strength display (persisted level)
   /*use mdns for host name resolution*/
  if (!MDNS.begin(host)) {
    log_e("Error setting up MDNS responder!");
    Serial.println("[boot] FATAL: mdns failed");
    while (1) {
      delay(1000);
    }
  }
  Serial.printf("[boot] ready. mDNS=pfoten.local heap=%u\n", ESP.getFreeHeap());

}

void loop() {
  // --- button -----------------------------------------------------------
  // Button wiring (verified 2026-08-09): pin 4 connects to GND when pressed,
  // so with INPUT_PULLUP the idle state is HIGH and a press reads LOW.
  bool pressed = (digitalRead(buttonPin) == LOW);
  if (pressed && !wasPressed) {
    pressStart = millis(); longPressFired = false;
    Serial.printf("[btn] pressed at %lus\n", millis() / 1000);
  }
  if (!pressed && wasPressed) {
    Serial.printf("[btn] released after %lums\n", millis() - pressStart);
    if (!longPressFired) {              // short press: cycle AP signal-strength display
      sigLevel = (sigLevel + 1) % 3;
      applySigLed();
      applyTxPower();   // live AP TX power for the new level
      armDim();         // LEDs full-bright for another 5 s
      Preferences p;
      if (p.begin("cfg", false)) { p.putUChar(K_SIG, sigLevel); p.end(); }
      Serial.printf("[btn] signal level -> %s\n",
                    sigLevel == 0 ? "low" : (sigLevel == 1 ? "middle" : "high"));
    }
  }
  if (pressed && wasPressed && !longPressFired && millis() - pressStart >= 3000UL) {
    longPressFired = true;
    apOpenUntil = millis() + 15UL * 60 * 1000UL;   // AP open (no password) for 15 minutes
    applyApMode(true);   // force: flip an already-open cfgPass AP to passwordless now
    ledFlashCount = 6; ledFlashOn = false; ledFlashTick = millis();   // 3 blinks
    armDim();   // button feedback LEDs full-bright for another 5 s
    Serial.printf("[btn] LONG-PRESS -> AP open (no password) until up=%lus\n", apOpenUntil / 1000);
  }
  wasPressed = pressed;
  updateOpenLed();   // pin 3 LED: client-joined = on (any mode), open blink, locked off

  // AP lock state machine: picks up STA-link changes and open-window expiry.
  applyApMode(false);

  // --- periodic status: dim check (1 s), [st] log (~60 s) + client edges -----
  static uint32_t lastTick = 0;
  static uint8_t lastStations = 0xFF;
  uint8_t stations = WiFi.softAPgetStationNum();
  if (stations != lastStations) {
    Serial.printf("[ap] stations now %u\n", stations);
    lastStations = stations;
  }
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    static bool lastDimmed = false;
    bool dimmed = !(ledDimAt != 0 && millis() < ledDimAt);
    if (dimmed != lastDimmed) {
      lastDimmed = dimmed;
      Serial.printf("[led] %s\n", dimmed ? "dim to 1%" : "full");
      applySigLed();   // refresh LEDs 5/10 when the dim deadline passes
    }
  }
  static uint32_t lastSt = 0;
  if (millis() - lastSt >= 60000UL) {
    lastSt = millis();
    Serial.printf("[st] up=%lus stations=%u heap=%u\n",
                  millis() / 1000, stations, ESP.getFreeHeap());
  }

  server.handleClient();
}
