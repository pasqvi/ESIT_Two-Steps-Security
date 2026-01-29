/**
 * ESIT - ESP8266 + AWS IoT Device Shadow + IR Remote
 * - Stato serratura: "chiusa" / "aperta"
 * - ESP invia su Shadow una request quando premi OK dopo aver digitato un codice IR
 * - ESP legge SEMPRE lo stato attuale dal Device Shadow usando SOLO topic piccoli:
 *    - /get/accepted all'avvio
 *    - /update/accepted per gli aggiornamenti
 *  (NON usa /update/documents perché spesso è troppo grande e fa cadere MQTT)
 *
 * Librerie:
 * - MQTT (lwmqtt) -> stessa delle slide
 * - ArduinoJson
 * - IRremoteESP8266
 *
 * File richiesti:
 * - configuration.h (il tuo, come da slide)
 * - errors.h (il tuo, con lwMQTTErr e lwMQTTErrConnection)
 */

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <time.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

#define emptyString String()

#include "configuration.h"
#include "errors.h"

// ===================== PIN =====================
#define IR_PIN   D5   // ricevitore IR
#define LOCK_PIN D1   // LED/relè: HIGH=aperta, LOW=chiusa

// ===================== AWS / MQTT =====================
const int MQTT_PORT = 8883;

const char TOPIC_GET[]             = "$aws/things/" THINGNAME "/shadow/get";
const char TOPIC_GET_ACCEPTED[]    = "$aws/things/" THINGNAME "/shadow/get/accepted";
const char TOPIC_UPDATE[]          = "$aws/things/" THINGNAME "/shadow/update";
const char TOPIC_UPDATE_ACCEPTED[] = "$aws/things/" THINGNAME "/shadow/update/accepted";

// TLS (come slide)
WiFiClientSecure net;
BearSSL::X509List cert(cacert);
BearSSL::X509List client_crt(client_cert);
BearSSL::PrivateKey key(privkey);

// MQTT client: buffer più grande
MQTTClient client(4096);

// ===================== IR =====================
IRrecv irrecv(IR_PIN);
decode_results results;

String codeBuffer = "";
String lockState  = "chiusa";  // default richiesto

time_t now;
time_t nowish = 1510592825; // come slide

// ===================== IR mapping (dato da te) =====================
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
    case 0x43: return 'O'; // OK
    case 0x09: return 'C'; // CLEAR
    default:   return 0;
  }
}

// ===================== UTILS =====================
void applyLockState(const String& newState)
{
  if (newState != "aperta" && newState != "chiusa") return;

  if (newState != lockState) {
    lockState = newState;
    Serial.print("LOCK STATE UPDATED FROM AWS -> ");
    Serial.println(lockState);
  }

  digitalWrite(LOCK_PIN, (lockState == "aperta") ? HIGH : LOW);
}

#ifdef USE_SUMMER_TIME_DST
uint8_t DST = 1;
#else
uint8_t DST = 0;
#endif

void NTPConnect(void)
{
  Serial.print("Setting time using SNTP");
  configTime(TIME_ZONE * 3600, DST * 3600, "pool.ntp.org", "time.nist.gov");
  now = time(nullptr);
  while (now < nowish) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" done!");
}

void connectToWiFi(const String& init_str)
{
  if (init_str != emptyString) Serial.print(init_str);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  if (init_str != emptyString) Serial.println(" ok!");
}

void connectToMqtt(bool nonBlocking = false)
{
  Serial.print("MQTT connecting ");
  while (!client.connected()) {
    if (client.connect(THINGNAME)) {
      Serial.println("connected!");

      // SOLO topic piccoli (stabili)
      client.subscribe(TOPIC_GET_ACCEPTED);
      client.subscribe(TOPIC_UPDATE_ACCEPTED);

      // Richiedi stato iniziale
      client.publish(TOPIC_GET, "{}", false, 0);

    } else {
      Serial.print("failed, SSL err: ");
      Serial.println(net.getLastSSLError());
      Serial.print("reason -> ");
      lwMQTTErrConnection(client.returnCode());
      Serial.println();

      if (!nonBlocking) {
        delay(5000);
      }
    }
    if (nonBlocking) break;
  }
}

void verifyWiFiAndMQTT(void)
{
  connectToWiFi("Checking WiFi");
  connectToMqtt();
}

// Invia request su shadow (Node-RED la processa)
void sendIRCodeRequest(const String& code)
{
  String nonce = String(ESP.getChipId(), HEX) + "-" + String(millis());
  uint32_t ts = (uint32_t)time(nullptr);

  DynamicJsonDocument doc(512);
  JsonObject state = doc.createNestedObject("state");
  JsonObject rep = state.createNestedObject("reported");
  JsonObject req = rep.createNestedObject("request");
  req["type"]  = "toggle";
  req["code"]  = code;
  req["nonce"] = nonce;
  req["ts"]    = ts;

  String payload;
  serializeJson(doc, payload);

  Serial.print("Sending request to AWS shadow: ");
  Serial.println(payload);

  if (!client.publish(TOPIC_UPDATE, payload, false, 0)) {
    Serial.print("Publish error -> ");
    lwMQTTErr(client.lastError());
    Serial.println();
  }
}

// Ricezione messaggi shadow (get/accepted + update/accepted)
void messageReceived(String &topic, String &payload)
{
  if (topic != TOPIC_GET_ACCEPTED && topic != TOPIC_UPDATE_ACCEPTED) return;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  if (!doc.containsKey("state")) return;
  JsonObject state = doc["state"].as<JsonObject>();

  const char* repLock = state["reported"]["lock"] | nullptr;
  const char* desLock = state["desired"]["lock"]  | nullptr;

  String newLock = lockState;
  if (repLock) newLock = String(repLock);
  else if (desLock) newLock = String(desLock);

  applyLockState(newLock);
}

void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println();

  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LOCK_PIN, LOW); // chiusa default

  WiFi.hostname(THINGNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  connectToWiFi(String("Trying to connect with SSID: ") + String(ssid));

  NTPConnect();

  // TLS + MQTT (come slide)
  net.setTrustAnchors(&cert);
  net.setClientRSACert(&client_crt, &key);

  // opzionale ma utile con AWS TLS:
  net.setBufferSizes(1024, 1024);

  client.begin(MQTT_HOST, MQTT_PORT, net);
  client.onMessage(messageReceived);

  connectToMqtt();

  // IR
  irrecv.enableIRIn();

  Serial.println("Ready. Digita codice IR + OK.");
}

void loop()
{
  now = time(nullptr);

  if (!client.connected()) {
    verifyWiFiAndMQTT();
  } else {
    client.loop();
  }

  // Lettura IR
  if (irrecv.decode(&results)) {
    uint16_t cmd = results.command;
    char k = decodeKeyFromIR(cmd);

    if (k) {
      if (k >= '0' && k <= '9') {
        if (codeBuffer.length() < 8) codeBuffer += k;
        Serial.print("CODE: ");
        Serial.println(codeBuffer);
      } else if (k == 'C') {
        codeBuffer = "";
        Serial.println("CODE CLEARED");
      } else if (k == 'O') {
        if (codeBuffer.length() > 0) {
          Serial.print("OK pressed, sending code: ");
          Serial.println(codeBuffer);
          sendIRCodeRequest(codeBuffer);
          codeBuffer = "";
        }
      }
    }
    irrecv.resume();
  }
}
