#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <time.h>

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

#include <Servo.h>

#include <configuration.h>
#include <errors.h>

#define emptyString String()

// PIN 
#define IR_PIN   D5
#define LOCK_PIN D6

// LCD I2C
#define I2C_SDA_PIN D2
#define I2C_SCL_PIN D1

// LCD Settings
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// -------------------- SERVO MOTOR --------------------
Servo lockServo;

static const uint8_t SERVO_CLOSED_ANGLE = 0;
static const uint8_t SERVO_OPEN_ANGLE   = 180;

// -------------------- AWS IoT --------------------
const int MQTT_PORT = 8883;

const char TOPIC_GET[]             = "$aws/things/" THINGNAME "/shadow/get";
const char TOPIC_GET_ACCEPTED[]    = "$aws/things/" THINGNAME "/shadow/get/accepted";
const char TOPIC_UPDATE[]          = "$aws/things/" THINGNAME "/shadow/update";
const char TOPIC_UPDATE_ACCEPTED[] = "$aws/things/" THINGNAME "/shadow/update/accepted";

const char TOPIC_LOCK_STATE[]      = "$aws/things/" THINGNAME "/custom/lock_state";
const char TOPIC_AUTH_EVENT[]      = "$aws/things/" THINGNAME "/custom/auth_event";

WiFiClientSecure net;
BearSSL::X509List cert(cacert);
BearSSL::X509List client_crt(client_cert);
BearSSL::PrivateKey key(privkey);

MQTTClient client(4096);

// -------------------- IR --------------------
IRrecv irrecv(IR_PIN);
decode_results results;

String codeBuffer = "";

// -------------------- STATO SERRATURA --------------------
bool serratura_aperta = false;

// -------------------- TIME --------------------
time_t now;
time_t nowish = 1510592825;

// -------------------- LCD --------------------
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

String lcdLast0 = "";
String lcdLast1 = "";

// -------------------- UI / BANNER --------------------
bool bannerWrong = false;
uint32_t bannerWrongUntilMs = 0;

bool bannerDisabled = false;
uint32_t bannerDisabledUntilMs = 0;

bool bannerTimeout = false;
uint32_t bannerTimeoutUntilMs = 0;

bool countdownActive = false;
uint32_t countdownEndMs = 0;
int lastCountdownShown = -1;

bool backendCheckActive = false;
uint32_t backendCheckStartMs = 0;

// Shadow GET periodico
uint32_t lastShadowGetMs = 0;
const uint32_t SHADOW_GET_PERIOD_MS = 20000;

// -------------------- TEST TIMING --------------------
bool authCandidatePending = false;
uint32_t authCandidateStartMs = 0;

bool closeCandidatePending = false;
uint32_t closeCandidateStartMs = 0;

bool testAuthRunning = false;
uint32_t testAuthStartMs = 0;

bool testCloseRunning = false;
uint32_t testCloseStartMs = 0;

const uint32_t TEST_AUTH_TIMEOUT_MS  = 30000;
const uint32_t TEST_CLOSE_TIMEOUT_MS = 30000;

void printTestTime(const char* name, uint32_t elapsedMs, const char* suffix = nullptr) {
  Serial.print(name);
  Serial.print(": ");
  Serial.print(elapsedMs);
  Serial.print(" ms");
  if (suffix && suffix[0]) {
    Serial.print(" ");
    Serial.print(suffix);
  }
  Serial.println();
}

// -------------------- LCD Functions --------------------
static String pad16(const String& s) {
  if (s.length() == 16) return s;
  if (s.length() > 16) return s.substring(0, 16);
  String out = s;
  while (out.length() < 16) out += ' ';
  return out;
}

void lcdWrite(const String& l0, const String& l1) {
  String a = pad16(l0);
  String b = pad16(l1);

  if (a == lcdLast0 && b == lcdLast1) return;

  lcd.setCursor(0, 0);
  lcd.print(a);
  lcd.setCursor(0, 1);
  lcd.print(b);

  lcdLast0 = a;
  lcdLast1 = b;
}

void startWrongBanner() {
  bannerWrong = true;
  bannerWrongUntilMs = millis() + 3000;
}

void startDisabledBanner() {
  bannerDisabled = true;
  bannerDisabledUntilMs = millis() + 3000;
}

void startTimeoutBanner() {
  bannerTimeout = true;
  bannerTimeoutUntilMs = millis() + 3000;
}

void startBackendCheck() {
  backendCheckActive = true;
  backendCheckStartMs = millis();
}

void stopBackendCheck() {
  backendCheckActive = false;
}

void cancelPendingOperationState() {
  stopBackendCheck();
  countdownActive = false;
  lastCountdownShown = -1;
  authCandidatePending = false;
  closeCandidatePending = false;
  testAuthRunning = false;
  testCloseRunning = false;
}

void startCountdownMs(uint32_t remainingMs) {
  stopBackendCheck();
  countdownActive = true;
  countdownEndMs = millis() + remainingMs;
  lastCountdownShown = -1;
}

void startCountdownSeconds(uint32_t seconds) {
  uint64_t remainingMs64 = (uint64_t)seconds * 1000ULL;
  if (remainingMs64 > 0xFFFFFFFFULL) remainingMs64 = 0xFFFFFFFFULL;
  startCountdownMs((uint32_t)remainingMs64);
}

void startCountdownUntilEpoch(uint32_t expiresTs) {
  time_t nowTs = time(nullptr);
  int64_t remainingSec = (int64_t)expiresTs - (int64_t)nowTs;
  if (remainingSec < 0) remainingSec = 0;
  startCountdownSeconds((uint32_t)remainingSec);
}

// -------------------- IR decode --------------------
char decodeKeyFromIR(uint16_t command) {
  switch (command) {
    case 0x16: return '0';
    case 0x0C: return '1';
    case 0x18: return '2';
    case 0x5E: return '3';
    case 0x08: return '4';
    case 0x1C: return '5';
    case 0x5A: return '6';
    case 0x42: return '7';
    case 0x52: return '8';
    case 0x4A: return '9';
    case 0x43: return 'O';
    case 0x09: return 'C';
    default:   return 0;
  }
}

// -------------------- APPLY LOCK --------------------
void applyLockBool(bool open) {
  serratura_aperta = open;

  stopBackendCheck();

  if (serratura_aperta) {
    countdownActive = false;
    bannerTimeout = false;
  }

  lockServo.write(serratura_aperta ? SERVO_OPEN_ANGLE : SERVO_CLOSED_ANGLE);

  Serial.print("LOCK_STATE (custom/shadow) -> ");
  Serial.println(serratura_aperta ? "aperta" : "chiusa");
}

void parseCustomLockPayload(const String& p) {
  String s = p;
  s.trim();
  s.toLowerCase();

  if (s.indexOf("aperta") >= 0) { applyLockBool(true); return; }
  if (s.indexOf("chiusa") >= 0) { applyLockBool(false); return; }

  Serial.print("Custom lock payload NOT recognized: ");
  Serial.println(p);
}

void parseAuthEventPayload(const String& payload) {
  String raw = payload;
  raw.trim();

  DynamicJsonDocument doc(384);
  DeserializationError err = deserializeJson(doc, raw);

  String event = "";
  uint32_t expiresTs = 0;
  uint32_t timeoutSec = 0;

  if (!err) {
    event = (const char*)(doc["event"] | "");
    expiresTs = doc["expires_ts"] | 0;
    timeoutSec = doc["timeout_sec"] | 0;
  } else {
    event = raw;
    event.trim();
    event.toLowerCase();
  }

  event.trim();
  event.toLowerCase();

  Serial.print("AUTH_EVENT -> ");
  Serial.println(event);

  if (event == "countdown_start") {
    if (authCandidatePending) {
      testAuthRunning = true;
      testAuthStartMs = authCandidateStartMs;
      Serial.println("TestAutenticazioneCompleta: START");
    }

    if (expiresTs > 0) {
      startCountdownUntilEpoch(expiresTs);
    } else if (timeoutSec > 0) {
      startCountdownSeconds(timeoutSec);
    } else {
      startCountdownSeconds(10);
    }
    return;
  }

  if (event == "close_authorized") {
    stopBackendCheck();
    if (closeCandidatePending) {
      testCloseRunning = true;
      testCloseStartMs = closeCandidateStartMs;
      Serial.println("TestChiusuraSerratura: START");

      if (!serratura_aperta) {
        uint32_t elapsed = (uint32_t)(millis() - testCloseStartMs);
        printTestTime("TestChiusuraSerratura", elapsed);
        testCloseRunning = false;
        closeCandidatePending = false;
      }
    }
    return;
  }

  if (event == "wrong_code") {
    cancelPendingOperationState();
    startWrongBanner();
    return;
  }

  if (event == "disabled_user") {
    cancelPendingOperationState();
    startDisabledBanner();
    return;
  }

  if (event == "cancel_wait") {
    cancelPendingOperationState();
    return;
  }
}

#ifdef USE_SUMMER_TIME_DST
uint8_t DST = 1;
#else
uint8_t DST = 0;
#endif

void rebootWithReason(const String& reason) {
  Serial.println();
  Serial.println(String("FATAL: ") + reason);
  Serial.println("Reboot in 2s...");
  delay(2000);
  ESP.restart();
}

bool NTPConnect(uint32_t windowMs = 15000, uint8_t maxRetries = 5) {
  for (uint8_t attempt = 1; attempt <= maxRetries; attempt++) {
    Serial.printf("Setting time using SNTP (attempt %u/%u)", attempt, maxRetries);

    configTime(TIME_ZONE * 3600, DST * 3600, "pool.ntp.org", "time.nist.gov");

    uint32_t start = millis();
    now = time(nullptr);
    while (now < nowish && (millis() - start) < windowMs) {
      delay(500);
      Serial.print(".");
      now = time(nullptr);
      yield();
    }

    if (now >= nowish) {
      Serial.println(" done!");
      return true;
    }

    Serial.println(" failed.");
    delay(500);
  }

  rebootWithReason("NTP sync failed too many times");
  return false;
}

bool connectToWiFi(const String& init_str, uint32_t windowMs = 20000, uint8_t maxRetries = 6) {
  if (WiFi.status() == WL_CONNECTED) {
    if (init_str != emptyString) Serial.println(" ok!");
    return true;
  }

  if (init_str != emptyString) Serial.print(init_str);

  uint8_t retries = 0;
  uint32_t windowStart = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if ((millis() - windowStart) >= windowMs) {
      retries++;
      Serial.println();
      Serial.printf("WiFi still not connected. Forcing reconnect (%u/%u)...\n", retries, maxRetries);

      WiFi.disconnect();
      delay(250);
      WiFi.begin(ssid, pass);

      windowStart = millis();
    }

    if (retries >= maxRetries) {
      rebootWithReason("WiFi connect failed too many times");
    }

    yield();
  }

  if (init_str != emptyString) Serial.println(" ok!");
  return true;
}

void messageReceived(String &topic, String &payload) {
  if (topic == TOPIC_LOCK_STATE) {
    Serial.print("RX custom lock_state: ");
    Serial.println(payload);
    parseCustomLockPayload(payload);
    return;
  }

  if (topic == TOPIC_AUTH_EVENT) {
    Serial.print("RX custom auth_event: ");
    Serial.println(payload);
    parseAuthEventPayload(payload);
    return;
  }

  if (topic == TOPIC_GET_ACCEPTED || topic == TOPIC_UPDATE_ACCEPTED) {
    String s = payload;
    s.toLowerCase();
    if (s.indexOf("\"lock\"") >= 0) {
      if (s.indexOf("aperta") >= 0) applyLockBool(true);
      else if (s.indexOf("chiusa") >= 0) applyLockBool(false);
    }
  }
}

bool connectToMqtt(bool nonBlocking = false, uint8_t maxRetries = 10) {
  Serial.print("MQTT connecting ");
  uint8_t tries = 0;

  while (!client.connected()) {
    if (client.connect(THINGNAME)) {
      Serial.println("connected!");

      client.subscribe(TOPIC_GET_ACCEPTED);
      client.subscribe(TOPIC_UPDATE_ACCEPTED);
      client.subscribe(TOPIC_LOCK_STATE);
      client.subscribe(TOPIC_AUTH_EVENT);

      client.publish(TOPIC_GET, "{}", false, 0);
      lastShadowGetMs = millis();
      return true;
    }

    tries++;

    Serial.print("failed, SSL err: ");
    Serial.println(net.getLastSSLError());
    Serial.print("reason -> ");
    lwMQTTErrConnection(client.returnCode());
    Serial.println();

    if (nonBlocking) break;

    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi("MQTT -> WiFi down, reconnecting");
    } else if (tries % 3 == 0) {
      Serial.println("MQTT still failing -> forcing WiFi reconnect...");
      WiFi.disconnect();
      delay(250);
      WiFi.begin(ssid, pass);
      connectToWiFi("Reconnecting WiFi");
    }

    if (tries >= maxRetries) {
      rebootWithReason("MQTT connect failed too many times");
    }

    delay(5000);
  }

  return client.connected();
}

void verifyWiFiAndMQTT(void) {
  connectToWiFi("Checking WiFi");
  connectToMqtt();
}

void sendIRCodeRequest(const String& code) {
  String nonce = String(ESP.getChipId(), HEX) + "-" + String(millis());
  uint32_t ts = (uint32_t)time(nullptr);

  DynamicJsonDocument doc(640);
  JsonObject state = doc.createNestedObject("state");
  JsonObject rep = state.createNestedObject("reported");
  JsonObject req = rep.createNestedObject("request");

  req["type"]  = "toggle";
  req["code"]  = code;
  req["nonce"] = nonce;
  req["ts"]    = ts;

  req["src"]   = "ir";
  req["thing"] = THINGNAME;
  req["fw"]    = "esit-esp8266-db-2.0";

  String out;
  serializeJson(doc, out);

  Serial.print("Sending request to AWS shadow: ");
  Serial.println(out);

  if (!client.publish(TOPIC_UPDATE, out, false, 0)) {
    Serial.print("Publish error -> ");
    lwMQTTErr(client.lastError());
    Serial.println();
  }
}

void updateLCDUi() {
  uint32_t ms = millis();

  if (bannerWrong && ms < bannerWrongUntilMs) {
    lcdWrite("CODICE ERRATO", "");
    return;
  } else {
    bannerWrong = false;
  }

  if (bannerDisabled && ms < bannerDisabledUntilMs) {
    lcdWrite("UTENTE DISABIL.", "ACCESSO NEGATO");
    return;
  } else {
    bannerDisabled = false;
  }

  if (bannerTimeout && ms < bannerTimeoutUntilMs) {
    lcdWrite("TEMPO SCADUTO", "SERRATURA CHIUSA");
    return;
  } else {
    bannerTimeout = false;
  }

  if (countdownActive) {
    if (ms >= countdownEndMs) {
      countdownActive = false;

      if (!serratura_aperta) {
        startTimeoutBanner();
        if (testAuthRunning) {
          uint32_t elapsed = (uint32_t)(millis() - testAuthStartMs);
          printTestTime("TestAutenticazioneCompleta", elapsed, "(TIMEOUT)");
          testAuthRunning = false;
        }
        authCandidatePending = false;
      }

      lcdWrite("TEMPO SCADUTO", "SERRATURA CHIUSA");
      return;
    }

    uint32_t remMs = countdownEndMs - ms;
    int remS = (int)((remMs + 999UL) / 1000UL);
    if (remS < 0) remS = 0;

    if (remS != lastCountdownShown) {
      lastCountdownShown = remS;
      lcdWrite("CONFERMA ENTRO", "T-" + String(remS) + "s");
    }
    return;
  }

  if (backendCheckActive) {
    lcdWrite("VERIFICA CODICE", "ATTENDI...");
    return;
  }

  if (codeBuffer.length() > 0) {
    lcdWrite("CODICE: " + codeBuffer, "");
    return;
  }

  if (serratura_aperta) {
    lcdWrite("SERRATURA", "APERTA");
  } else {
    lcdWrite("SERRATURA", "CHIUSA");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();

  lockServo.attach(LOCK_PIN);
  lockServo.write(SERVO_CLOSED_ANGLE);
  delay(200);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.begin();
  lcd.backlight();
  lcd.clear();
  lcdWrite("AVVIO...", "");

  WiFi.hostname(THINGNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  connectToWiFi(String("Trying SSID: ") + String(ssid));

  NTPConnect();

  net.setTrustAnchors(&cert);
  net.setClientRSACert(&client_crt, &key);
  net.setBufferSizes(1024, 1024);

  client.begin(MQTT_HOST, MQTT_PORT, net);
  client.onMessage(messageReceived);

  connectToMqtt();

  irrecv.enableIRIn();

  Serial.println("Ready. Digita codice IR + OK.");
  lcdWrite("PRONTO!", "IR + OK");
  delay(1000);
}

void loop() {
  now = time(nullptr);

  if (!client.connected()) {
    verifyWiFiAndMQTT();
  } else {
    client.loop();
  }

  if (client.connected()) {
    uint32_t ms = millis();
    if (ms - lastShadowGetMs >= SHADOW_GET_PERIOD_MS) {
      client.publish(TOPIC_GET, "{}", false, 0);
      lastShadowGetMs = ms;
    }
  }

  if (testAuthRunning && serratura_aperta) {
    uint32_t elapsed = (uint32_t)(millis() - testAuthStartMs);
    printTestTime("TestAutenticazioneCompleta", elapsed);
    testAuthRunning = false;
    authCandidatePending = false;
  }

  if (testCloseRunning && !serratura_aperta) {
    uint32_t elapsed = (uint32_t)(millis() - testCloseStartMs);
    printTestTime("TestChiusuraSerratura", elapsed);
    testCloseRunning = false;
    closeCandidatePending = false;
  }

  if (testAuthRunning && (uint32_t)(millis() - testAuthStartMs) > TEST_AUTH_TIMEOUT_MS) {
    uint32_t elapsed = (uint32_t)(millis() - testAuthStartMs);
    printTestTime("TestAutenticazioneCompleta", elapsed, "(TIMEOUT)");
    testAuthRunning = false;
    authCandidatePending = false;
  }

  if (testCloseRunning && (uint32_t)(millis() - testCloseStartMs) > TEST_CLOSE_TIMEOUT_MS) {
    uint32_t elapsed = (uint32_t)(millis() - testCloseStartMs);
    printTestTime("TestChiusuraSerratura", elapsed, "(TIMEOUT)");
    testCloseRunning = false;
    closeCandidatePending = false;
  }

  if (irrecv.decode(&results)) {
    uint16_t cmd = results.command;
    char k = decodeKeyFromIR(cmd);

    if (k) {
      if (backendCheckActive || countdownActive) {
        Serial.println("Input ignored: operation already in progress.");
      } else if (k >= '0' && k <= '9') {
        if (codeBuffer.length() < 8) codeBuffer += k;
        Serial.print("CODE: ");
        Serial.println(codeBuffer);
      } else if (k == 'C') {
        codeBuffer = "";
        Serial.println("CODE CLEARED");
      } else if (k == 'O') {
        if (codeBuffer.length() > 0) {
          String entered = codeBuffer;
          codeBuffer = "";

          Serial.print("OK pressed, sending code: ");
          Serial.println(entered);

          if (!serratura_aperta) {
            authCandidatePending = true;
            authCandidateStartMs = millis();
            testAuthRunning = false;
          } else {
            closeCandidatePending = true;
            closeCandidateStartMs = millis();
            testCloseRunning = false;
          }

          startBackendCheck();
          sendIRCodeRequest(entered);
        }
      }
    }

    irrecv.resume();
  }

  updateLCDUi();
}