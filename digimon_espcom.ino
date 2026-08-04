// ESPCom -- ESP32 A-Com for Digimon DM20.
// Protocol timings, packet structure and DM20 tables ported from the DMComm
// project by BladeSabre (MIT). wificom.dev integration derived from
// wificom-lib by mechawrench (MIT). See README for details.

#include <Arduino.h>

struct Segment {
  uint16_t bits;
  uint16_t copyMask;
  uint16_t invertMask;
  int      checksumTarget;
  int      checkDigitPos;
};

#define ENABLE_WIFI 1

#define ESPCOM_VERSION "0.1.0"

#define WIFICOM_COMPAT_VERSION "2.1.0"

#define HEARTBEAT_MS 20000

#define DIGIROM_LOOP_MS 5000

static String serialDigiRom = "";
static uint32_t lastSerialRun = 0;

#if ENABLE_WIFI
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <WebServer.h>
  #include <DNSServer.h>
  #include <Preferences.h>
  #include <PubSubClient.h>
  #include <ArduinoJson.h>
  #include "secrets.h"

  #define AP_SSID     "ESPCom-Setup"
  #define AP_PASSWORD "digimon2026"
#endif

#define LOG_LINES 24
static String logBuf[LOG_LINES];
static int logHead = 0;

static void out(const String &line) {
  Serial.print(line);
  Serial.print("\r\n");
  logBuf[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
}

static void outf(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  out(String(buf));
}

static const int  SIGNAL_PIN  = 33;
static const long SERIAL_BAUD = 115200;

static const uint32_t PRE_IDLE_SEND      = 3000;
static const uint32_t PRE_ACTIVE_MIN     = 40000;
static const uint32_t PRE_ACTIVE_SEND    = 59000;

static const uint32_t START_IDLE_MIN     = 1500;
static const uint32_t START_IDLE_SEND    = 2083;
static const uint32_t START_IDLE_MAX     = 2500;

static const uint32_t START_ACTIVE_MIN   = 600;
static const uint32_t START_ACTIVE_SEND  = 917;
static const uint32_t START_ACTIVE_MAX   = 1400;

static const uint32_t BIT_IDLE_MIN       = 700;
static const uint32_t BIT0_IDLE_SEND     = 1000;
static const uint32_t BIT_IDLE_THRESHOLD = 1800;
static const uint32_t BIT1_IDLE_SEND     = 2667;
static const uint32_t BIT_IDLE_MAX       = 3400;

static const uint32_t BIT_ACTIVE_MIN     = 1000;
static const uint32_t BIT1_ACTIVE_SEND   = 1667;
static const uint32_t BIT0_ACTIVE_SEND   = 3167;
static const uint32_t BIT_ACTIVE_MAX     = 4000;

static const uint32_t COOLDOWN_SEND      = 400;
static const uint32_t REPLY_TIMEOUT_MS   = 100;

static inline void driveIdle() {
  pinMode(SIGNAL_PIN, OUTPUT);
  digitalWrite(SIGNAL_PIN, HIGH);
}

static inline void driveActive() {
  pinMode(SIGNAL_PIN, OUTPUT);
  digitalWrite(SIGNAL_PIN, LOW);
}

static inline void releaseLine() {
  pinMode(SIGNAL_PIN, INPUT);
}

static inline bool lineHigh() {
  return digitalRead(SIGNAL_PIN) == HIGH;
}

void sendPacket(uint16_t bits) {
  driveIdle();
  delayMicroseconds(PRE_IDLE_SEND);

  driveActive();

  delay(PRE_ACTIVE_SEND / 1000);
  delayMicroseconds(PRE_ACTIVE_SEND % 1000);

  driveIdle();
  delayMicroseconds(START_IDLE_SEND);
  driveActive();
  delayMicroseconds(START_ACTIVE_SEND);

  for (int i = 0; i < 16; i++) {
    bool one = bits & 1;
    driveIdle();
    delayMicroseconds(one ? BIT1_IDLE_SEND : BIT0_IDLE_SEND);
    driveActive();
    delayMicroseconds(one ? BIT1_ACTIVE_SEND : BIT0_ACTIVE_SEND);
    bits >>= 1;
  }

  driveIdle();
  delayMicroseconds(COOLDOWN_SEND);
  releaseLine();
}

static uint32_t measurePulse(bool level, uint32_t timeoutUs) {
  uint32_t start = micros();
  while (lineHigh() == level) {
    if (micros() - start > timeoutUs) return 0;
  }
  return micros() - start;
}

static bool waitFor(bool level, uint32_t timeoutUs) {
  uint32_t start = micros();
  while (lineHigh() != level) {
    if (micros() - start > timeoutUs) return false;
  }
  return true;
}

bool receivePacket(uint16_t *out, uint32_t timeoutMs, const char **err) {
  releaseLine();

  if (!waitFor(false, timeoutMs * 1000UL)) { *err = "no activity"; return false; }

  uint32_t t = measurePulse(false, 200000);
  if (t < PRE_ACTIVE_MIN) { *err = "pre_active too short"; return false; }

  t = measurePulse(true, 10000);
  if (t < START_IDLE_MIN || t > START_IDLE_MAX) { *err = "bad start_idle"; return false; }

  t = measurePulse(false, 10000);
  if (t < START_ACTIVE_MIN || t > START_ACTIVE_MAX) { *err = "bad start_active"; return false; }

  uint16_t result = 0;
  for (int i = 0; i < 16; i++) {
    t = measurePulse(true, 10000);
    if (t < BIT_IDLE_MIN || t > BIT_IDLE_MAX) { *err = "bad bit_idle"; return false; }
    result >>= 1;
    if (t > BIT_IDLE_THRESHOLD) result |= 0x8000;

    t = measurePulse(false, 10000);
    if (i < 15 && (t < BIT_ACTIVE_MIN || t > BIT_ACTIVE_MAX)) {
      *err = "bad bit_active"; return false;
    }
  }

  *out = result;
  return true;
}

void runVoltageTest() {
  releaseLine();
  delay(5);

  bool digital = lineHigh();

  int raw = analogRead(SIGNAL_PIN);
  float volts = raw * 3.3f / 4095.0f;

  releaseLine();

  outf("Released line: %d raw (~%.2fV), digital reads %s",
       raw, volts, digital ? "HIGH" : "LOW");
  if (raw >= 4090) {
    out("ADC saturated. Increase R1 so the level drops below ~3.1V.");
  } else if (!digital) {
    out("Below digital threshold. Decrease R1 to raise the idle level.");
  } else {
    out("Idle level looks usable.");
  }
}

void captureAfterSend(uint16_t data, uint32_t captureMs) {
  Serial.printf("Sending 0x%04X, capturing %lums...\n", data, captureMs);
  sendPacket(data);

  uint32_t startUs = micros();
  uint32_t endMs = millis() + captureMs;
  bool last = lineHigh();
  int edges = 0;
  Serial.printf("t=0: %s\n", last ? "HIGH" : "LOW");

  while (millis() < endMs) {
    bool now = lineHigh();
    if (now != last) {
      Serial.printf("t=%luus -> %s\n", micros() - startUs, now ? "HIGH" : "LOW");
      last = now;
      if (++edges > 120) { Serial.println("(stopping, too many edges)"); break; }
    }
  }
  Serial.printf("Done. %d edges.\n", edges);
}

static const uint8_t ROSTER_ATTR[134] = {
  3,3,0,2,0,1,2,1,0,1,2,2,1,0,2,1,3,3,1,1,0,0,0,0,
  0,0,2,2,1,2,0,1,3,3,1,2,0,1,2,2,1,1,2,0,0,2,0,2,
  3,3,0,1,1,1,0,2,1,0,2,2,1,1,0,2,3,3,2,2,2,2,2,2,
  2,2,2,2,2,0,2,2,3,3,0,0,0,0,1,1,1,1,3,3,1,0,0,0,
  2,2,2,3,3,0,0,0,0,1,1,1,1,0,0,0,0,1,0,0,1,3,3,1,
  1,1,0,3,3,0,3,0,0,2,2,1,0,0
};
static const uint8_t ROSTER_SS[134] = {
  0,0,1,4,3,3,6,6,5,5,15,2,7,8,41,37,0,0,1,4,3,3,6,6,
  5,9,15,10,7,14,33,12,0,0,1,4,3,3,6,6,5,11,15,12,7,13,29,28,
  0,0,1,4,3,3,6,6,5,11,15,12,7,13,2,12,0,0,1,4,3,3,6,6,
  5,11,15,12,7,13,37,39,25,25,31,12,12,30,26,26,5,12,17,18,18,16,24,20,
  16,6,20,0,0,6,26,6,45,26,0,12,42,1,3,2,38,1,3,26,45,13,13,36,
  38,31,5,45,40,14,13,3,13,16,3,37,45,12
};
static const uint8_t ROSTER_SW[134] = {
  0,0,26,31,25,26,12,17,34,14,15,26,6,6,4,32,0,0,25,41,25,5,22,35,
  17,5,15,2,26,8,11,30,0,0,12,41,25,1,11,27,9,5,15,2,20,8,0,38,
  0,0,5,39,31,12,11,23,41,5,15,2,22,8,9,30,0,0,41,31,26,5,5,25,
  14,5,15,2,22,8,25,28,25,25,25,31,14,31,1,12,16,34,17,18,31,21,31,31,
  21,13,5,0,0,17,7,14,3,0,5,5,5,26,5,26,26,25,26,32,25,5,24,13,
  13,13,13,45,40,43,26,26,22,12,32,12,41,16
};
static const uint8_t ROSTER_POWER[134] = {
  0,0,18,10,75,70,65,60,55,50,40,126,118,107,188,176,0,0,18,10,75,70,65,60,
  55,50,40,126,118,107,169,188,0,0,18,10,75,70,65,60,55,50,40,126,118,107,176,169,
  0,0,18,10,75,70,65,60,55,50,40,126,118,107,188,176,0,0,18,10,75,70,65,60,
  55,50,40,126,118,107,188,176,0,0,34,90,155,210,34,90,155,210,0,0,34,80,143,199,
  80,143,199,0,0,27,80,135,199,27,80,135,199,25,83,135,199,25,77,135,199,0,0,27,
  90,143,210,0,0,27,80,143,210,238,238,238,238,238
};
static const uint8_t ROSTER_STAGE[134] = {
  1,2,3,3,4,4,4,4,4,4,4,5,5,5,6,6,1,2,3,3,4,4,4,4,
  4,4,4,5,5,5,6,6,1,2,3,3,4,4,4,4,4,4,4,5,5,5,6,6,
  1,2,3,3,4,4,4,4,4,4,4,5,5,5,6,6,1,2,3,3,4,4,4,4,
  4,4,4,5,5,5,6,6,1,2,3,4,5,6,3,4,5,6,1,2,3,4,5,6,
  4,5,6,1,2,3,4,5,6,3,4,5,6,3,4,5,6,3,4,5,6,1,2,3,
  4,5,6,1,2,3,4,5,6,7,7,7,7,7
};
static const char *const ROSTER_NAME[134] = {
  "Botamon", "Koromon", "Agumon", "Betamon", "Greymon", "Tyranomon",
  "Devimon", "Meramon", "Airdramon", "Seadramon", "Numemon", "Metal Greymon",
  "Mamemon", "Monzaemon", "Blitz Greymon", "Bancho Mamemon", "Punimon",
  "Tunomon", "Gabumon", "Elecmon", "Kabuterimon", "Garurumon", "Angemon",
  "Yukidarumon", "Birdramon", "Whamon", "Vegimon", "Skull Greymon",
  "Metal Mamemon", "Vademon", "Skull Mammon", "Cres Garurumon", "Poyomon",
  "Tokomon", "Patamon", "Kunemon", "Unimon", "Centalmon", "Ogremon",
  "Bakemon", "Shellmon", "Drimogemon", "Scumon", "Andromon", "Giromon",
  "Etemon", "Hi Andromon", "King Etemon", "Yuramon", "Tanemon", "Piyomon",
  "Palmon", "Monochromon", "Cockatrimon", "Leomon", "Kuwagamon", "Coelamon",
  "Mojyamon", "Nanimon", "Megadramon", "Piccolomon", "Digitamamon",
  "Aegisdramon", "Titamon", "Zurumon", "Pagumon", "Gazimon", "Gizamon",
  "Dark Tyranomon", "Cyclomon", "Devidramon", "Tuskmon", "Flymon",
  "Deltamon", "Raremon", "Metal Tyranomon", "Nanomon", "Ex-Tyranomon",
  "Mugendramon", "Pinochimon", "Sakumon", "Sakuttomon", "Zubamon",
  "Zubaeagermon", "Duramon", "Durandamon", "Hackmon", "Bao Hackmon",
  "Savior Hackmon", "Jesmon", "Petitmon", "Babydmon", "Dracomon",
  "Coredramon (Blue)", "Wingdramon", "Slayerdramon", "Coredramon (Green)",
  "Groundramon", "Breakdramon", "Pitchmon", "Pukamon", "Coronamon",
  "Firamon", "Flaremon", "Apollomon", "Lunamon", "Lekismon", "Crescemon",
  "Dianamon", "Taichi's Agumon", "Taichi's Greymon",
  "Taichi's Metal Greymon", "Taichi's War Greymon", "Yamato's Gabumon",
  "Yamato's Garurumon", "Yamato's Were Garurumon",
  "Yamato's Metal Garurumon", "Dodomon", "Dorimon", "DORUmon", "DORUgamon",
  "DORUguremon", "Alphamon", "Yukimibotamon", "Nyaromon", "Plotmon",
  "Meicoomon", "Meicrackmon", "Rasielmon", "Omegamon Alter S",
  "Rust Tyranomon", "Examon", "Grace Novamon", "Omegamon"
};

#define ROSTER_SIZE 134
static const char ROM_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ-!?";

static int romCharValue(char c) {
  for (int i = 0; ROM_CHARS[i]; i++) if (ROM_CHARS[i] == c) return i;
  return 0;
}

static String buildDm20Rom(int index, bool copyMode, bool win) {
  if (index < 0 || index >= ROSTER_SIZE) return "";

  const char *nm = "ESPC";
  int nv[4];
  for (int i = 0; i < 4; i++) nv[i] = romCharValue(nm[i]);

  bool battle   = !copyMode;
  int  mode     = copyMode ? 1 : 0;
  int  attr     = battle ? ROSTER_ATTR[index]  : 0;
  int  ss       = battle ? ROSTER_SS[index]    : 0;
  int  sw       = battle ? ROSTER_SW[index]    : 0;
  int  power    = battle ? ROSTER_POWER[index] : 0;
  int  hitMe    = battle ? (win ? 0x0 : 0xF) : 0;
  int  hitYou   = battle ? (hitMe ^ 0xF)     : 0;

  uint16_t seg[9];
  seg[0] = (nv[1] << 8) | nv[0];
  seg[1] = (nv[3] << 8) | nv[2];
  seg[2] = (1 << 15) | (0 << 10) | (mode << 8) | (0 << 4) | 0xE;
  seg[3] = (index << 6) | (attr << 4) | 0xE;
  seg[4] = (ss << 10) | (sw << 4) | 0xE;
  seg[5] = (power << 4) | 0xE;
  seg[6] = 0x000E;
  seg[7] = 0x000E;
  seg[8] = 0x000E;

  char buf[8];
  String rom = "V1";
  for (int i = 0; i < 9; i++) {
    snprintf(buf, sizeof(buf), "-%04X", seg[i]);
    rom += buf;
  }
  snprintf(buf, sizeof(buf), "-@0%X%XE", hitMe, hitYou);
  rom += buf;
  return rom;
}

struct Segment;

static bool parseSegment(const String &text, Segment *seg) {
  seg->bits = 0;
  seg->copyMask = 0;
  seg->invertMask = 0;
  seg->checksumTarget = -1;
  seg->checkDigitPos = 12;

  int cursor = 0;
  for (int i = 0; i < 4; i++) {
    int lsbPos = 12 - (i * 4);
    seg->bits <<= 4;

    if (cursor >= (int)text.length()) return false;
    char marker = text[cursor];
    char digitCh;
    if (marker == '@' || marker == '^') {
      cursor++;
      if (cursor >= (int)text.length()) return false;
      digitCh = text[cursor];
    } else {
      digitCh = marker;
    }

    int digit;
    if      (digitCh >= '0' && digitCh <= '9') digit = digitCh - '0';
    else if (digitCh >= 'A' && digitCh <= 'F') digit = digitCh - 'A' + 10;
    else if (digitCh >= 'a' && digitCh <= 'f') digit = digitCh - 'a' + 10;
    else return false;

    if (marker == '@') {
      seg->checksumTarget = digit;
      seg->checkDigitPos = lsbPos;
    } else if (marker == '^') {
      seg->copyMask   |= (uint16_t)((~digit & 0xF) << lsbPos);
      seg->invertMask |= (uint16_t)(digit << lsbPos);
    } else {
      seg->bits |= digit;
    }
    cursor++;
  }
  return cursor == (int)text.length();
}

static uint16_t buildSendBits(const Segment &c, uint16_t received, uint32_t *checksum) {
  uint16_t bits = c.bits;

  bits &= ~c.copyMask;
  bits |= c.copyMask & received;
  bits &= ~c.invertMask;
  bits |= c.invertMask & (uint16_t)~received;

  if (c.checksumTarget >= 0) {
    bits &= ~(uint16_t)(0xF << c.checkDigitPos);
  }

  for (int i = 0; i < 4; i++) {
    *checksum += (uint32_t)(bits >> (4 * i));
  }
  *checksum %= 16;

  if (c.checksumTarget >= 0) {
    int checkDigit = (c.checksumTarget - (int)(*checksum)) & 0xF;
    bits |= (uint16_t)(checkDigit << c.checkDigitPos);
    *checksum = (uint32_t)c.checksumTarget;
  }
  return bits;
}

String executeDigiRom(String line) {
  String result = "";
  line.trim();
  if (line.length() < 4 || (line[0] != 'V' && line[0] != 'v')) {
    return "error: expected a V-type DigiROM";
  }

  int turn = line[1] - '0';
  if (turn != 1 && turn != 2) return "error: turn must be 1 or 2";

  int pos = line.indexOf('-');
  if (pos < 0) return "error: malformed DigiROM";
  pos++;

  uint32_t checksum = 0;
  uint16_t lastReceived = 0;
  uint16_t got;
  const char *err = "unknown";
  char buf[16];

  if (turn == 2) {
    if (!receivePacket(&got, 5000, &err)) return result + "t";
    snprintf(buf, sizeof(buf), "r:%04X ", got);
    result += buf;
    lastReceived = got;
  }

  while (pos < (int)line.length()) {
    int next = line.indexOf('-', pos);
    String tok = (next < 0) ? line.substring(pos) : line.substring(pos, next);
    tok.trim();

    if (tok.length() > 0) {
      Segment seg;
      if (!parseSegment(tok, &seg)) return result + "error: bad segment " + tok;

      uint16_t val = buildSendBits(seg, lastReceived, &checksum);
      snprintf(buf, sizeof(buf), "s:%04X ", val);
      result += buf;
      sendPacket(val);

      if (!receivePacket(&got, REPLY_TIMEOUT_MS, &err)) return result + "t";
      snprintf(buf, sizeof(buf), "r:%04X ", got);
      result += buf;
      lastReceived = got;
    }

    if (next < 0) break;
    pos = next + 1;
  }
  result += "t";
  return result;
}

#if ENABLE_WIFI

static WiFiClientSecure tlsClient;
static PubSubClient mqttClient(tlsClient);

static String topicInput;
static String topicOutput;
static String currentDigiRom = "";
static String lastApplicationId = "";

static void buildTopics() {
  String prefix = String(MQTT_USERNAME) + "/f/";
  String ident  = String(USER_UUID) + "-" + String(DEVICE_UUID);
  topicInput  = prefix + ident + "/wificom-input";
  topicOutput = prefix + ident + "/wificom-output";
}

static void publishJson(JsonDocument &doc) {
  String payload;
  serializeJson(doc, payload);
  if (mqttClient.connected()) {
    mqttClient.publish(topicOutput.c_str(), payload.c_str());
  }
}

static void addIdentity(JsonDocument &doc) {
  doc["name"] = "wificom";
  doc["version"] = WIFICOM_COMPAT_VERSION;
  doc["circuitpython_version"] = "9.2.8";
  doc["circuitpython_board_id"] = "esp32";
  doc["has_display"] = false;
  doc["device_uuid"] = DEVICE_UUID;
}

static void publishStatus(bool verbose = true) {
  JsonDocument doc;
  addIdentity(doc);
  doc["output"] = "";
  publishJson(doc);
  if (verbose) out("MQTT: published status to output topic");
}

static void publishResult(const String &output) {
  JsonDocument doc;
  addIdentity(doc);
  doc["application_uuid"] = lastApplicationId;
  doc["output"] = output;
  publishJson(doc);
}

static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, payload, len);
  if (e) {
    Serial.printf("MQTT: bad JSON (%s)\n", e.c_str());
    return;
  }

  if (!doc["ack_id"].isNull()) {
    JsonDocument ack;
    ack["application_uuid"] = doc["application_id"];
    ack["device_uuid"] = DEVICE_UUID;
    ack["ack_id"] = doc["ack_id"];
    publishJson(ack);
  }

  if (!doc["topic_action"].isNull()) {
    Serial.println("MQTT: real-time battle request received (not supported in this build)");
    return;
  }

  if (!doc["application_id"].isNull()) {
    lastApplicationId = doc["application_id"].as<String>();
  }

  const char *rom = doc["digirom"];
  if (rom != nullptr && strlen(rom) > 0) {
    if (currentDigiRom != String(rom)) {
      outf("MQTT: got DigiROM %s", rom);
    }
    currentDigiRom = String(rom);
    serialDigiRom = "";
  }
}

static Preferences prefs;
static String staSsid, staPass;
static bool apMode = false;

static void loadCreds() {
  prefs.begin("espcom", false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  prefs.end();
}

static void clearCreds() {
  prefs.begin("espcom", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

static void saveCreds(const String &ssid, const String &pass) {
  prefs.begin("espcom", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  outf("AP: %s / %s", AP_SSID, AP_PASSWORD);
  out("AP: browse to http://192.168.4.1");
}

static void wifiConnect() {
  if (staSsid.length() == 0) {
    out("WiFi: no network configured");
    startAP();
    return;
  }

  outf("WiFi: joining %s", staSsid.c_str());
  apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    outf("WiFi: connected, http://%s", WiFi.localIP().toString().c_str());
  } else {
    out("WiFi: could not join, starting setup network");
    startAP();
  }
}

static void mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return;

  tlsClient.setInsecure();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);
  mqttClient.setKeepAlive(15);

  String clientId = "espcom-" + String(DEVICE_UUID).substring(0, 8);
  Serial.print("MQTT: connecting... ");
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("connected");
    mqttClient.subscribe(topicInput.c_str());
    Serial.printf("MQTT: subscribed to %s\n", topicInput.c_str());
    publishStatus(true);
  } else {
    Serial.printf("failed, state=%d (retrying)\n", mqttClient.state());
  }
}

static void networkLoop() {
  if (apMode) {
    if (staSsid.length() > 0 && WiFi.softAPgetStationNum() == 0) {
      static uint32_t lastRetry = 0;
      if (lastRetry == 0) lastRetry = millis();
      if (millis() - lastRetry > 120000) {
        out("WiFi: retrying configured network");
        delay(200);
        ESP.restart();
      }
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
    return;
  }
  if (!mqttClient.connected()) {
    static uint32_t lastAttempt = 0;
    if (millis() - lastAttempt > 5000) {
      lastAttempt = millis();
      mqttConnect();
    }
    return;
  }
  mqttClient.loop();

  static uint32_t lastRun = 0;
  if (currentDigiRom.length() > 0 && millis() - lastRun > DIGIROM_LOOP_MS) {
    lastRun = millis();
    String result = executeDigiRom(currentDigiRom);
    Serial.println(result);
    publishResult(result);
    return;
  }

  static uint32_t lastHeartbeat = 0;
  if (currentDigiRom.length() == 0 && millis() - lastHeartbeat > HEARTBEAT_MS) {
    lastHeartbeat = millis();
    publishStatus(false);
  }
}

#endif

static void printVersion() {
  Serial.print("name = \"dmcomm-python\"\r\r\n");
  Serial.print("version = \"v0.9.0\"\r\r\n");
  Serial.print("circuitpython_version = \"espcom-" ESPCOM_VERSION "\"\r\r\n");
  Serial.print("circuitpython_board_id = \"esp32_espcom\"\r\n");
}

static int countSegments(const String &rom) {
  int n = 0, pos = rom.indexOf('-');
  if (pos < 0) return 0;
  pos++;
  while (pos < (int)rom.length()) {
    int next = rom.indexOf('-', pos);
    String tok = (next < 0) ? rom.substring(pos) : rom.substring(pos, next);
    tok.trim();
    if (tok.length() > 0) n++;
    if (next < 0) break;
    pos = next + 1;
  }
  return n;
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "t") { runVoltageTest(); return; }
  if (line == "c") { captureAfterSend(0xFC03, 300); return; }

  String upper = line;
  upper.toUpperCase();

  if (upper == "I") { printVersion(); return; }

  if (upper == "P") {
    serialDigiRom = "";
    outf("got %d bytes: %s -> [pause]", line.length(), line.c_str());
    return;
  }

  if (upper == "T") {
    outf("got %d bytes: %s -> NotImplementedError('op=T')",
         line.length(), line.c_str());
    return;
  }

  if (upper[0] == 'V') {
    int turn = (line.length() > 1) ? (line[1] - '0') : -1;
    if (turn != 1 && turn != 2) {
      outf("got %d bytes: %s -> CommandError('turn=')",
           line.length(), line.c_str());
      return;
    }
    serialDigiRom = line;
    lastSerialRun = 0;
#if ENABLE_WIFI
    currentDigiRom = "";
#endif
    outf("got %d bytes: %s -> V%d-[%d packets]",
         line.length(), line.c_str(), turn, countSegments(line));
    return;
  }

  outf("got %d bytes: %s -> CommandError('op=')",
       line.length(), line.c_str());
}

static void serialDigiRomLoop() {
  if (serialDigiRom.length() == 0) return;
  if (millis() - lastSerialRun < DIGIROM_LOOP_MS) return;
  lastSerialRun = millis();
  out(executeDigiRom(serialDigiRom));
}

#if ENABLE_WIFI

static WebServer server(80);
static DNSServer dnsServer;

static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ESPCom</title>
<style>
:root{
  --lcd:#9caa8c; --lcd-dim:#8d9b7e; --ink:#161a10; --ink-soft:#4a5340;
  --shell:#33362c; --shell-hi:#43473a;
}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{
  margin:0; padding:14px 14px calc(14px + env(safe-area-inset-bottom));
  background:var(--shell); color:var(--ink);
  font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
}
h1{
  margin:2px 0 14px; font-size:12px; font-weight:600;
  letter-spacing:.28em; text-transform:uppercase; color:var(--lcd);
}
h1 span{opacity:.55}
.panel{
  background:var(--lcd); border:2px solid var(--ink);
  padding:12px; margin-bottom:12px;
  box-shadow:inset 0 2px 0 rgba(255,255,255,.22),inset 0 -2px 0 rgba(0,0,0,.12);
}
.lbl{
  display:block; font-size:10px; letter-spacing:.2em; text-transform:uppercase;
  background:var(--ink); color:var(--lcd);
  padding:3px 6px; margin:0 0 9px;
}
input,select{
  width:100%; font:inherit; color:var(--ink);
  background:var(--lcd-dim); border:1px solid var(--ink);
  padding:11px 9px; margin-bottom:9px; border-radius:0;
  -webkit-appearance:none; appearance:none;
}
input:focus,select:focus{outline:3px solid var(--ink); outline-offset:-1px}
input::placeholder{color:var(--ink-soft)}
button{
  font:inherit; font-size:12px; letter-spacing:.12em; text-transform:uppercase;
  background:var(--lcd); color:var(--ink);
  border:2px solid var(--ink); padding:11px 8px; border-radius:0;
  cursor:pointer; width:100%;
}
button:active,button.on{background:var(--ink); color:var(--lcd)}
.row{display:grid; grid-template-columns:repeat(3,1fr); gap:7px}
.row.two{grid-template-columns:repeat(2,1fr)}
#log{
  min-height:150px; max-height:44vh; overflow-y:auto;
  font-size:13px; white-space:pre-wrap; word-break:break-word;
}
.ln{margin:1px 0}
.ln.res{font-weight:600}
.stat{
  display:flex; justify-content:space-between; gap:10px;
  font-size:11px; letter-spacing:.08em; color:var(--lcd); margin:-4px 2px 12px;
}
.stat span{opacity:.75}
@media (prefers-reduced-motion:no-preference){
  button{transition:background .08s linear,color .08s linear}
}
</style></head><body>
<h1>ESPCom <span>/ DM20</span></h1>
<div class="stat"><span id="net">-</span><span id="mq">-</span></div>
<div class="panel">
  <label class="lbl" for="rom">DigiROM</label>
  <input id="rom" placeholder="V1-FC03-FD02" autocapitalize="characters"
         autocomplete="off" spellcheck="false">
  <div class="row two">
    <button onclick="send()">Run</button>
    <button onclick="cmd('P')">Stop</button>
  </div>
  <span class="lbl" style="margin-top:12px">Build from roster</span>
  <select id="mon"><option>loading...</option></select>
  <div class="row two">
    <select id="mode">
      <option value="battle">Battle</option>
      <option value="copy">Copymon</option>
    </select>
    <select id="out">
      <option value="lose">You lose</option>
      <option value="win">You win</option>
    </select>
  </div>
  <button onclick="build()">Build DigiROM</button>
</div>
<div class="panel">
  <span class="lbl">Terminal</span>
  <div id="log"></div>
  <input id="tin" placeholder="type a command" autocapitalize="characters"
         autocomplete="off" spellcheck="false" style="margin-top:9px">
  <div class="row">
    <button onclick="cmd('t')">Voltage</button>
    <button onclick="cmd('c')">Capture</button>
    <button onclick="cmd('I')">Version</button>
  </div>
</div>
<div class="panel">
  <label class="lbl" for="ssid">WiFi network</label>
  <input id="ssid" placeholder="Network name" autocapitalize="none"
         autocomplete="off" spellcheck="false">
  <input id="pass" type="password" placeholder="Password" autocomplete="off">
  <div class="row two">
    <button onclick="wifi()">Save</button>
    <button onclick="forget()">Forget</button>
  </div>
</div>
<script>
var STAGE=['','I','II','III','IV','V','VI','VI+'];
function loadRoster(){
  fetch('/api/roster').then(function(r){return r.json()}).then(function(list){
    var sel=document.getElementById('mon'), h='';
    for(var i=0;i<list.length;i++){
      var e=list[i];
      h+='<option value="'+e[0]+'">'+e[0]+'  '+e[1]+'  ['+STAGE[e[2]]+']</option>';
    }
    sel.innerHTML=h; sel.value='2';
  }).catch(function(){
    document.getElementById('mon').innerHTML='<option>roster unavailable</option>';
  });
}
function build(){
  var q='i='+document.getElementById('mon').value
       +'&m='+document.getElementById('mode').value
       +'&o='+document.getElementById('out').value;
  fetch('/api/build?'+q).then(function(r){return r.text()}).then(function(t){
    document.getElementById('rom').value=t;
  });
}
function post(u,b){
  return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
}
function send(){
  var v=document.getElementById('rom').value.trim();
  if(v) post('/api/cmd','c='+encodeURIComponent(v)).then(poll);
}
function cmd(c){ post('/api/cmd','c='+encodeURIComponent(c)).then(poll); }
function wifi(){
  var s=document.getElementById('ssid').value.trim();
  if(!s){ alert('Enter a network name first.'); return; }
  var p=document.getElementById('pass').value;
  post('/api/wifi','s='+encodeURIComponent(s)+'&p='+encodeURIComponent(p))
    .then(function(){ alert('Saved. Restarting — rejoin your network, then reopen this page.'); });
}
function forget(){
  if(!confirm('Erase the saved network and restart into setup mode?')) return;
  post('/api/forget','').then(function(){
    alert('Erased. Rejoin ESPCom-Setup to configure a new network.');
  });
}
function esc(t){ return t.replace(/[<>&]/g,function(c){return {'<':'&lt;','>':'&gt;','&':'&amp;'}[c]; }); }
function render(lines){
  var h='';
  for(var i=0;i<lines.length;i++){
    var L=lines[i];
    var res=/^(s:[0-9A-F]{4}|r:[0-9A-F]{4}|t)( |$)/.test(L);
    h+='<div class="ln'+(res?' res':'')+'">'+esc(L)+'</div>';
  }
  var d=document.getElementById('log');
  d.innerHTML=h; d.scrollTop=d.scrollHeight;
}
function poll(){
  fetch('/api/state').then(function(r){return r.json()}).then(function(j){
    document.getElementById('net').textContent=j.net;
    document.getElementById('mq').textContent=j.mqtt;
    render(j.log);
  }).catch(function(){});
}
document.getElementById('rom').addEventListener('keydown',function(e){
  if(e.key==='Enter') send();
});
document.getElementById('tin').addEventListener('keydown',function(e){
  if(e.key!=='Enter') return;
  var v=this.value.trim(); if(!v) return;
  this.value=''; cmd(v);
});
loadRoster(); poll(); setInterval(poll,2000);
</script>
</body></html>)HTML";

static void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

static void handleState() {
  String j = "{\"net\":\"";
  if (apMode) {
    j += "setup network 192.168.4.1";
  } else if (WiFi.status() == WL_CONNECTED) {
    j += staSsid + " " + WiFi.localIP().toString();
  } else {
    j += "offline";
  }
  j += "\",\"mqtt\":\"";
  j += mqttClient.connected() ? "online" : (apMode ? "-" : "offline");
  j += "\",\"log\":[";
  bool first = true;
  for (int i = 0; i < LOG_LINES; i++) {
    String &L = logBuf[(logHead + i) % LOG_LINES];
    if (L.length() == 0) continue;
    if (!first) j += ",";
    first = false;
    j += "\"";
    for (unsigned k = 0; k < L.length(); k++) {
      char c = L[k];
      if (c == '"' || c == '\\') { j += '\\'; j += c; }
      else if (c >= 32) j += c;
    }
    j += "\"";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

static void handleCmd() {
  String c = server.arg("c");
  server.send(200, "text/plain", "ok");
  if (c.length()) handleSerialLine(c);
}

static void handleWifi() {
  String ssid = server.arg("s");
  String pass = server.arg("p");
  if (ssid.length() == 0) { server.send(400, "text/plain", "no ssid"); return; }
  saveCreds(ssid, pass);
  server.send(200, "text/plain", "saved");
  outf("WiFi: saved %s, restarting", ssid.c_str());
  delay(400);
  ESP.restart();
}

static void handleRoster() {
  String j = "[";
  for (int i = 0; i < ROSTER_SIZE; i++) {
    if (i) j += ",";
    j += "[";
    j += i;
    j += ",\"";
    j += ROSTER_NAME[i];
    j += "\",";
    j += ROSTER_STAGE[i];
    j += "]";
  }
  j += "]";
  server.send(200, "application/json", j);
}

static void handleBuild() {
  int idx = server.arg("i").toInt();
  bool copyMode = server.arg("m") == "copy";
  bool win = server.arg("o") == "win";
  String rom = buildDm20Rom(idx, copyMode, win);
  if (rom.length() == 0) { server.send(400, "text/plain", "bad index"); return; }
  server.send(200, "text/plain", rom);
}

static void handleForget() {
  clearCreds();
  server.send(200, "text/plain", "cleared");
  out("WiFi: credentials erased, restarting");
  delay(400);
  ESP.restart();
}

static void startWebServer() {
  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/forget", HTTP_POST, handleForget);
  server.on("/api/roster", handleRoster);
  server.on("/api/build", handleBuild);

  server.onNotFound(handleRoot);
  server.begin();
  if (apMode) dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
}
#endif

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);
  releaseLine();
  out("espcom starting");

#if ENABLE_WIFI
  loadCreds();
  buildTopics();
  wifiConnect();
  startWebServer();
  if (!apMode) mqttConnect();
#endif
}

void loop() {
#if ENABLE_WIFI
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
  networkLoop();
#endif

  if (Serial.available()) {
    handleSerialLine(Serial.readStringUntil('\n'));
  }

  serialDigiRomLoop();
}
