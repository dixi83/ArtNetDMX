#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

/*
 * ArtNetDMX: ESP-07S / ESP8266 firmware
 *
 * Receives Art-Net ArtDMX packets over Wi-Fi and continuously transmits a
 * DMX512 universe through a MAX485-compatible RS-485 driver. A web interface
 * provides Run/Stop, network settings, a two-channel DMX test, and firmware
 * updates. No filesystem is used; persistent settings are stored in EEPROM
 * emulation backed by flash.
 *
 * Hardware assumptions:
 *   - GPIO1 / UART0 TX is connected to the RS-485 driver's DI input.
 *   - The RS-485 driver's DE pin is held high, so its transmitter is enabled.
 *   - The RGB LED is common-cathode (active-high) unless RGB_ACTIVE_LOW is set.
 */

// UART0 TX is also the ESP-07S programming/console TX pin. The firmware
// temporarily switches this pin to GPIO to generate each DMX BREAK, then
// returns it to the UART peripheral for the data bytes.
static constexpr uint8_t TX_PIN = 1;

// External RGB status LED connections from the device wiring.
static constexpr uint8_t LED_R_PIN = 12;
static constexpr uint8_t LED_G_PIN = 13;
static constexpr uint8_t LED_B_PIN = 14;
// Set to true for a common-anode LED; common-cathode LEDs use active-high.
static constexpr bool RGB_ACTIVE_LOW = false;

// Art-Net uses UDP port 6454. DMX_PORT is reserved for future use; DMX output
// itself is sent as a continuous UART stream, not over UDP.
static constexpr uint16_t ARTNET_PORT = 6454;
static constexpr uint16_t DMX_PORT = 3333;

// The current Settings structure occupies less than this EEPROM-emulation
// region. EEPROM emulation ultimately uses a flash sector on the ESP8266.
static constexpr uint16_t EEPROM_BYTES = 512;

// Stored alongside the settings to distinguish valid data from erased or
// incompatible EEPROM contents.
static constexpr uint32_t CONFIG_MAGIC = 0x41525444;

// Persistent configuration layout. Keep this structure stable when possible:
// changing its field order or types changes the bytes stored in EEPROM.
struct Settings {
  uint32_t magic;          // Must match CONFIG_MAGIC for this record to be trusted.
  char ssid[33];           // 32-byte Wi-Fi SSID plus null terminator.
  char password[65];       // 64-byte Wi-Fi passphrase plus null terminator.
  char apPassword[33];     // Setup AP password, including null terminator.
  char host[16];           // IPv4 Art-Net sender filter; "0.0.0.0" accepts any host.
  char ip[16];             // Static IPv4 address; "0.0.0.0" selects DHCP.
  char gateway[16];        // Gateway used with a static address.
  char subnet[16];         // Subnet mask used with a static address.
  uint16_t startAddress;   // First ArtDMX source slot copied to the DMX output.
  uint16_t endAddress;     // Last ArtDMX source slot copied, inclusive (1-512).
  uint16_t apTimeoutSeconds; // Wait before setup AP fallback if station Wi-Fi is unavailable.
};

// Global application objects and buffers. Keeping packet and slot buffers
// static avoids allocating large temporary buffers repeatedly on the ESP8266.
Settings cfg;
ESP8266WebServer server(80);
WiFiUDP artnet;
uint8_t dmxSlots[512] = {0};
uint8_t packet[600];

// Runtime state. The unit starts in Run mode after a successful Wi-Fi boot;
// AP fallback explicitly forces running=false.
bool running = true;
bool apMode = false;
bool testMode = false;

// Last values used by the test page. The same value is sent on both channels.
uint16_t testChannel1 = 1;
uint16_t testChannel2 = 2;
uint8_t testValue = 255;

// Millisecond timestamps for Art-Net activity, DMX frame scheduling, and the
// initial Wi-Fi connection timeout.
uint32_t lastPacketMs = 0;
uint32_t lastFrameMs = 0;
uint32_t wifiStartedMs = 0;
uint32_t lastLedMs = 0; // Reserved for time-based LED patterns; patterns currently use millis() directly.

// Status text is retained for diagnostics/future UI use; current page derives
// its status from running, testMode, apMode, and Wi-Fi state.
String statusText = "Starting";

// State for an in-progress HTTP firmware upload. The ESP8266 Update library
// stages the new image in flash; eboot copies it over the old image on reboot.
bool otaStarted = false;
bool otaSucceeded = false;
bool otaRejectedWhileRunning = false;
String otaError;

// Set the three RGB LED channels using the polarity selected above.
// Arguments are logical LED states: true means that colour should be lit.
void setRgb(bool red, bool green, bool blue) {
  const uint8_t on = RGB_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t off = RGB_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(LED_R_PIN, red ? on : off);
  digitalWrite(LED_G_PIN, green ? on : off);
  digitalWrite(LED_B_PIN, blue ? on : off);
}

// Populate a safe first-boot configuration. An empty SSID causes setup AP
// mode to start immediately; 0.0.0.0 for the device IP means DHCP.
void defaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CONFIG_MAGIC;
  strlcpy(cfg.host, "0.0.0.0", sizeof(cfg.host));
  strlcpy(cfg.ip, "0.0.0.0", sizeof(cfg.ip)); // DHCP
  strlcpy(cfg.gateway, "192.168.1.1", sizeof(cfg.gateway));
  strlcpy(cfg.subnet, "255.255.255.0", sizeof(cfg.subnet));
  strlcpy(cfg.apPassword, "artnetdmx", sizeof(cfg.apPassword));
  cfg.startAddress = 1;
  cfg.endAddress = 512;
  cfg.apTimeoutSeconds = 30;
}

// Load the raw Settings structure from EEPROM. If the marker or core ranges
// are invalid, discard the contents and use defaults instead.
void loadSettings() {
  EEPROM.begin(EEPROM_BYTES);
  EEPROM.get(0, cfg);
  if (cfg.magic != CONFIG_MAGIC || cfg.endAddress < 1 || cfg.endAddress > 512 ||
      cfg.startAddress < 1 || cfg.startAddress > cfg.endAddress || cfg.apTimeoutSeconds > 3600) {
    defaults();
  }
}

// Persist all current settings. EEPROM.commit() is required to write the
// emulated EEPROM contents back to flash.
void saveSettings() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// Escape user-configurable text before inserting it into HTML attributes or
// body text. This prevents Wi-Fi names/IPs from breaking the page markup.
String htmlEscape(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

// Build the complete control page in memory. Settings and the output-test
// form are shown only while stopped. In AP fallback the test control is hidden
// and the Run button is omitted because DMX must remain stopped in setup mode.
String page(const String &notice = "") {
  const bool connected = WiFi.status() == WL_CONNECTED;
  String html = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>ArtNet DMX</title><style>body{font:16px system-ui;margin:2rem auto;padding:0 1rem;max-width:620px;background:#10151c;color:#e8edf2}"
                  "h1{font-size:1.5rem}section{background:#1b2430;padding:1rem 1.2rem;border-radius:12px;margin:1rem 0}"
                  "label{display:block;margin:.8rem 0 .25rem}input{box-sizing:border-box;width:100%;padding:.7rem;border-radius:6px;border:1px solid #536274;background:#111923;color:white}"
                  "button{padding:.7rem 1rem;border:0;border-radius:7px;background:#24a879;color:white;font-weight:bold;margin:.7rem .4rem .2rem 0}"
                  ".stop{background:#d95b59}.muted{color:#aebbc9}.notice{color:#8ee0b6}</style></head><body><h1>ArtNet to DMX512</h1><section>");
  // Main state and network summary.
  html += F("<b>Status:</b> ");
  if (running && testMode) html += F("TEST OUTPUT");
  else html += running ? F("RUNNING") : F("STOPPED");
  html += F("<br><span class='muted'>Network: ");
  html += apMode ? F("Setup access point") : (connected ? String("Wi-Fi · ") + WiFi.localIP().toString() : F("Connecting"));
  html += F("<br>Art-Net: UDP 6454 · source "); html += htmlEscape(cfg.host);
  html += F("<br>DMX slots: "); html += String(cfg.startAddress); html += F("–"); html += String(cfg.endAddress);
  // Show the exact test channels and value while the test is active.
  if (running && testMode) {
    html += F("<br>Test: channel "); html += String(testChannel1); html += F(" and ");
    html += String(testChannel2); html += F(" at value "); html += String(testValue);
  }
  html += F("</span>");
  if (lastPacketMs && running) { html += F("<br><span class='muted'>Last ArtDMX: "); html += String((millis()-lastPacketMs)/1000); html += F("s ago</span>"); }
  if (notice.length()) { html += F("<p class='notice'>"); html += htmlEscape(notice); html += F("</p>"); }
  if (apMode) {
    html += F("<p>DMX is held stopped while the setup access point is active.</p>");
  } else {
    html += F("<form method='post' action='/run'><button class='");
    html += running ? F("stop") : F("");
    html += F("' name='run' value='");
    if (running) html += F("0'>Stop DMX</button>");
    else html += F("1'>Run DMX</button>");
    html += F("</form>");
  }
  html += F("</section>");

  // Configuration and diagnostic controls are intentionally available only
  // after stopping output, so edits cannot interrupt active Art-Net output.
  if (!running) {
    if (!apMode) {
      html += F("<section><h2>DMX output test</h2><p>Transmit two selected channels continuously; all other channels are zero.</p>"
                "<form method='post' action='/test'><label>Channel 1 (1-512)</label>"
                "<input type='number' min='1' max='512' name='channel1' value='"); html += String(testChannel1);
      html += F("'><label>Channel 2 (1-512)</label><input type='number' min='1' max='512' name='channel2' value='"); html += String(testChannel2);
      html += F("'><label>Value for both channels (0-255)</label><input type='number' min='0' max='255' name='value' value='"); html += String(testValue);
      html += F("'><button type='submit'>Start DMX test</button></form></section>");
    }
    html += F("<section><h2>Settings</h2><form method='post' action='/save'>"
              "<label>Wi-Fi network (SSID)</label><input name='ssid' maxlength='32' value='"); html += htmlEscape(cfg.ssid);
    html += F("'><label>Wi-Fi password</label><input name='password' type='password' maxlength='64' placeholder='Leave blank to keep current'>"
              "<label>Fallback AP password (8+ characters)</label><input name='apPassword' type='password' maxlength='32' placeholder='Leave blank to keep current'>"
              "<label>Device IP (0.0.0.0 for DHCP)</label><input name='ip' value='"); html += htmlEscape(cfg.ip);
    html += F("'><label>Gateway</label><input name='gateway' value='"); html += htmlEscape(cfg.gateway);
    html += F("'><label>Subnet mask</label><input name='subnet' value='"); html += htmlEscape(cfg.subnet);
    html += F("'><label>Accept Art-Net from host (0.0.0.0 = any)</label><input name='host' value='"); html += htmlEscape(cfg.host);
    html += F("'><label>DMX start address (1–512)</label><input type='number' min='1' max='512' name='start' value='"); html += String(cfg.startAddress);
    html += F("'><label>DMX end address (1–512)</label><input type='number' min='1' max='512' name='end' value='"); html += String(cfg.endAddress);
    html += F("'><label>Wi-Fi connection timeout before setup AP (seconds, 0 = immediate)</label><input type='number' min='0' max='3600' name='timeout' value='"); html += String(cfg.apTimeoutSeconds);
    html += F("'><button type='submit'>Save and restart</button></form></section>");

    html += F("<section><h2>Firmware update</h2>");
    html += F("<p>DMX is stopped. <a href='/update' style='color:#8ee0b6'>Open the OTA upload page</a>.</p>");
    html += F("</section>");
  }
  html += F("<p class='muted'>Setup AP: ArtNetDMX-Setup · default password: artnetdmx</p></body></html>");
  return html;
}

// GET / renders the current state and controls.
void handleRoot() { server.send(200, F("text/html; charset=utf-8"), page()); }

// POST /run switches between Art-Net Run mode and Stop. Starting normal Run
// clears test mode; stopping clears all DMX slots to zero.
void handleRun() {
  running = !apMode && server.arg("run") == "1";
  testMode = false;
  statusText = running ? "Running" : "Stopped by user";
  if (!running) memset(dmxSlots, 0, sizeof(dmxSlots));
  server.sendHeader(F("Location"), F("/")); server.send(303);
}

// Parse a short unsigned decimal form field with an explicit maximum. Limiting
// the length to three characters is sufficient for DMX channels and values
// and avoids accepting oversized/overflowing input.
bool parseBoundedNumber(const String &text, uint16_t maxValue, uint16_t &value) {
  if (!text.length() || text.length() > 3) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10 + (c - '0');
    if (parsed > maxValue) return false;
  }
  value = parsed;
  return true;
}

// POST /test starts a continuous local DMX test. The test requires a stopped
// device in station (Wi-Fi) mode, uses two distinct channels, clears every
// other slot, then lets the normal frame scheduler transmit the test values.
void handleTest() {
  if (apMode || running) {
    server.send(409, F("text/plain; charset=utf-8"), F("Stop DMX and connect in Wi-Fi mode before starting the test."));
    return;
  }
  uint16_t channel1, channel2, value;
  if (!parseBoundedNumber(server.arg("channel1"), 512, channel1) || channel1 < 1 ||
      !parseBoundedNumber(server.arg("channel2"), 512, channel2) || channel2 < 1 ||
      !parseBoundedNumber(server.arg("value"), 255, value) || channel1 == channel2) {
    server.send(400, F("text/plain; charset=utf-8"), F("Use channels 1-512 and a value from 0-255."));
    return;
  }
  testChannel1 = channel1;
  testChannel2 = channel2;
  testValue = value;
  memset(dmxSlots, 0, sizeof(dmxSlots));
  dmxSlots[testChannel1 - 1] = testValue;
  dmxSlots[testChannel2 - 1] = testValue;
  testMode = true;
  running = true;
  lastPacketMs = 0;
  statusText = "DMX test output";
  server.sendHeader(F("Location"), F("/"));
  server.send(303);
}

// Parse a dotted IPv4 address using the ESP8266 IPAddress helper.
bool validIP(const String &s, IPAddress &ip) {
  if (!ip.fromString(s)) return false;
  return true;
}

// POST /save validates and persists web settings, then restarts so that Wi-Fi
// mode, IP configuration, and fallback timeout are applied consistently.
// Blank Wi-Fi password fields mean "keep the saved password".
void handleSave() {
  String ssid = server.arg("ssid"), password = server.arg("password"), apPassword = server.arg("apPassword");
  IPAddress parsed;
  String ip = server.arg("ip"), gw = server.arg("gateway"), mask = server.arg("subnet"), host = server.arg("host");
  int start = server.arg("start").toInt(), end = server.arg("end").toInt(), timeout = server.arg("timeout").toInt();
  if (ssid.length() > 32 || password.length() > 64 || (apPassword.length() && apPassword.length() < 8) || apPassword.length() > 32 ||
      (!ip.equals("0.0.0.0") && !validIP(ip, parsed)) || !validIP(gw, parsed) || !validIP(mask, parsed) || !validIP(host, parsed) ||
      start < 1 || end > 512 || start > end || timeout < 0 || timeout > 3600) {
    server.send(400, F("text/html; charset=utf-8"), page(F("Invalid setting. Check IP addresses, passwords, and DMX range.")));
    return;
  }
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (password.length()) strlcpy(cfg.password, password.c_str(), sizeof(cfg.password));
  if (apPassword.length()) strlcpy(cfg.apPassword, apPassword.c_str(), sizeof(cfg.apPassword));
  strlcpy(cfg.ip, ip.c_str(), sizeof(cfg.ip)); strlcpy(cfg.gateway, gw.c_str(), sizeof(cfg.gateway));
  strlcpy(cfg.subnet, mask.c_str(), sizeof(cfg.subnet)); strlcpy(cfg.host, host.c_str(), sizeof(cfg.host));
  cfg.startAddress = start; cfg.endAddress = end; cfg.apTimeoutSeconds = timeout;
  saveSettings();
  server.send(200, F("text/html; charset=utf-8"), F("<meta http-equiv='refresh' content='3;url=/'><p>Saved. Restarting…</p>"));
  delay(500); ESP.restart();
}

// GET /update serves the firmware upload form. Updates are rejected while DMX
// is running; the user must stop output before changing firmware.
void handleUpdatePage() {
  if (running) {
    server.send(409, F("text/plain; charset=utf-8"), F("Stop DMX from the main page before updating firmware."));
    return;
  }
  String html = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Firmware update</title><style>body{font:16px system-ui;margin:2rem auto;padding:0 1rem;max-width:620px;background:#10151c;color:#e8edf2}"
                  "section{background:#1b2430;padding:1rem 1.2rem;border-radius:12px}input,button{margin:.6rem 0;padding:.7rem}a{color:#8ee0b6}</style></head>"
                  "<body><section><h1>Firmware update</h1><p>DMX is stopped. Select the PlatformIO firmware.bin file.</p>"
                  "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required>"
                  "<button type='submit'>Upload and install</button></form><p><a href='/'>Back</a></p></section></body></html>");
  server.send(200, F("text/html; charset=utf-8"), html);
}

// Multipart upload callback for POST /update. Update.begin() checks available
// flash space and boot state; each upload chunk is streamed directly to flash.
// The running firmware is left intact until Update.end() stages a valid image.
void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaStarted = false;
    otaSucceeded = false;
    otaRejectedWhileRunning = false;
    otaError = "";
    if (running) {
      otaRejectedWhileRunning = true;
      return;
    }
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    otaStarted = Update.begin(maxSketchSpace, U_FLASH);
    if (!otaStarted) otaError = Update.getErrorString();
  } else if (upload.status == UPLOAD_FILE_WRITE && otaStarted && !Update.hasError()) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) otaError = Update.getErrorString();
  } else if (upload.status == UPLOAD_FILE_END && otaStarted && !Update.hasError()) {
    otaSucceeded = Update.end(true);
    if (!otaSucceeded) otaError = Update.getErrorString();
  } else if (upload.status == UPLOAD_FILE_ABORTED && otaStarted) {
    Update.end();
    otaError = F("Upload aborted");
  }
}

// Final POST /update response. On success, return a page with a link and a
// 15-second redirect, then reboot so eboot can install the staged image.
void handleUpdateResult() {
  if (otaRejectedWhileRunning || running) {
    server.send(409, F("text/plain; charset=utf-8"), F("Stop DMX before updating firmware."));
    return;
  }
  if (!otaSucceeded) {
    String message = F("Firmware update failed: ");
    message += otaError.length() ? otaError : Update.getErrorString();
    server.send(500, F("text/plain; charset=utf-8"), message);
    return;
  }
  server.send(200, F("text/html; charset=utf-8"),
              F("<!doctype html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='15;url=/'>"
                "<title>Firmware installed</title></head><body>"
                "<h1>Firmware installed</h1><p>The device is restarting.</p>"
                "<p><a href='/'>Return to the main page</a></p>"
                "<p>This page will redirect automatically in 15 seconds.</p>"
                "</body></html>"));
  delay(1000);
  ESP.restart();
}

// Replace station mode with the protected setup AP and force DMX output off.
// The AP is available at 192.168.4.1 with the configured AP password.
void startAP() {
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  String apName = "ArtNetDMX-Setup";
  WiFi.softAP(apName.c_str(), cfg.apPassword[0] ? cfg.apPassword : "artnetdmx");
  apMode = true; running = false; statusText = "Setup AP active · DMX stopped";
  setRgb(true, false, false);
}

// Start station mode. A configured non-zero IP selects static addressing;
// otherwise the ESP8266 obtains its address via DHCP.
void startNetwork() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  IPAddress ip, gw, mask;
  if (strcmp(cfg.ip, "0.0.0.0") != 0 && ip.fromString(cfg.ip) && gw.fromString(cfg.gateway) && mask.fromString(cfg.subnet)) {
    WiFi.config(ip, gw, mask);
  }
  WiFi.begin(cfg.ssid, cfg.password);
  wifiStartedMs = millis();
  statusText = "Connecting to Wi-Fi";
}

// Register all HTTP routes. WiFiWebServer invokes these handlers from loop().
void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/run", HTTP_POST, handleRun);
  server.on("/test", HTTP_POST, handleTest);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
  server.onNotFound(handleRoot);
  server.begin();
}

// Configure UART0 for DMX512: 250000 baud, 8 data bits, no parity, 2 stop bits.
// GPIO1 is explicitly returned to the UART's alternate-function mux first.
void beginDmx() {
  pinMode(TX_PIN, SPECIAL); // restore UART0 TX mux after generating the break on GPIO1
  Serial.begin(250000, SERIAL_8N2);
}

// Emit one complete DMX512 frame:
//   1. Wait for the prior UART frame to finish.
//   2. Drive GPIO1 low for a 100 us BREAK.
//   3. Hold it high for a 12 us mark-after-break (MAB).
//   4. Restore UART0 TX and send start code 0 plus all 512 slots.
// A 25 ms scheduler interval targets about 40 full frames per second.
void sendDmxFrame() {
  // DMX break (>=88 us), mark-after-break (>=8 us), start code, then 512 slots.
  Serial.flush();
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, LOW);
  delayMicroseconds(100);
  digitalWrite(TX_PIN, HIGH);
  delayMicroseconds(12);
  pinMode(TX_PIN, SPECIAL);
  uint8_t frame[513];
  frame[0] = 0;
  memcpy(frame + 1, dmxSlots, sizeof(dmxSlots));
  Serial.write(frame, sizeof(frame));
  Serial.flush();
  lastFrameMs = millis();
}

// Process at most one queued UDP datagram per loop iteration. Accept only
// ArtDMX packets from the configured source host, or any host when host is
// 0.0.0.0. The Art-Net universe field is deliberately not filtered: all
// accepted ArtDMX universes write into the same 512-slot output buffer.
void receiveArtNet() {
  int size = artnet.parsePacket();
  // ArtDMX has an 18-byte header; discard undersized and oversized packets.
  if (size < 18 || size > (int)sizeof(packet)) { if (size > 0) while (artnet.available()) artnet.read(); return; }

  // The optional source-host filter compares the packet sender to cfg.host.
  IPAddress sender = artnet.remoteIP();
  IPAddress expected;
  if (strcmp(cfg.host, "0.0.0.0") && expected.fromString(cfg.host) && sender != expected) {
    while (artnet.available()) {
      artnet.read();
    }
    return;
  }

  // Read the packet and validate the Art-Net ID and little-endian ArtDMX opcode
  // (0x5000 is represented as bytes 0x00, 0x50 in the packet).
  int n = artnet.read(packet, min(size, (int)sizeof(packet)));
  static const uint8_t id[] = {'A','r','t','-','N','e','t',0};
  if (n < 18 || memcmp(packet, id, sizeof(id)) != 0 || packet[8] != 0x00 || packet[9] != 0x50) return; // ArtDMX opcode 0x5000, little endian
  uint16_t length = (uint16_t(packet[16]) << 8) | packet[17];
  if (length > 512 || length > n - 18) return;

  // Apply the configured inclusive source-slot range. Channel numbers remain
  // in their original positions; slots outside the range retain prior values.
  for (uint16_t a = cfg.startAddress; a <= cfg.endAddress; ++a) {
    uint16_t source = a - 1;
    dmxSlots[source] = source < length ? packet[18 + source] : 0;
  }
  lastPacketMs = millis();
}

// Status LED patterns:
//   red solid       = stopped or setup AP
//   green solid     = local DMX test mode
//   blue fast blink = Wi-Fi connection in progress
//   green double    = recent Art-Net input
//   blue heartbeat  = connected, but no recent Art-Net input
void updateLed() {
  // Red = stopped/setup AP; blue blink = connecting; green double pulse = recent
  // Art-Net; blue heartbeat = connected but no recent Art-Net.
  if (apMode || !running) { setRgb(true, false, false); return; }
  if (testMode) { setRgb(false, true, false); return; }
  uint32_t now = millis();
  uint32_t age = lastPacketMs ? now - lastPacketMs : now - wifiStartedMs;
  if (WiFi.status() != WL_CONNECTED) {
    bool on = ((now / 250) & 1) == 0;
    setRgb(false, false, on);
  } else if (lastPacketMs && age < 1500) {
    bool on = (now % 800) < 80 || ((now % 800) >= 180 && (now % 800) < 260);
    setRgb(false, on, false);
  } else {
    bool on = (now % 2000) < 70;
    setRgb(false, false, on);
  }
}

// One-time hardware and service initialization. The web server is started
// before networking so it is ready as soon as station/AP mode is available.
void setup() {
  pinMode(LED_R_PIN, OUTPUT); pinMode(LED_G_PIN, OUTPUT); pinMode(LED_B_PIN, OUTPUT);
  setRgb(false, false, false);
  loadSettings();
  beginDmx();
  setupWeb();
  if (cfg.ssid[0]) startNetwork();
  else { startAP(); }
  artnet.begin(ARTNET_PORT);
}

// Main cooperative loop. Keep web requests responsive, switch to setup AP
// when station Wi-Fi is unavailable past the configured deadline, receive
// Art-Net only in normal mode, and send frames at a steady interval. Test
// output is independent of Wi-Fi once started, but AP fallback still stops it.
void loop() {
  server.handleClient();
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    if (!running) { /* still serve control page */ }
  } else if (!apMode && millis() - wifiStartedMs >= uint32_t(cfg.apTimeoutSeconds) * 1000UL) {
    startAP();
  }
  if (running && !apMode) {
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (!testMode && wifiConnected) receiveArtNet();
    // Test output must not depend on Wi-Fi staying associated once started.
    if ((testMode || wifiConnected) && millis() - lastFrameMs >= 25) sendDmxFrame();
  }
  updateLed();
  yield();
}
