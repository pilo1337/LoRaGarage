#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "mbedtls/aes.h"
#include "time.h"
#include <Wire.h>
#include "SSD1306Wire.h"
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ==========================
// LoRa Pins Heltec V1
// ==========================
#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_SS      18
#define LORA_RST     14
#define LORA_DIO0    26

#define BAND 868E6   // EU868

// ==========================
// OLED Pins Heltec V1
// ==========================
#define OLED_SDA 4 
#define OLED_SCL 15
#define OLED_RST 16
#define OLED_I2C 0x3C

// ==========================
// LoRa Config
// ==========================
struct LoRaConfig {
  long frequency;
  int spreadingFactor;
  long bandwidth;
  int codingRate;
  int preambleLength;
  byte syncWord;
  bool crc;
};

LoRaConfig loraConfig;
Preferences prefs;

const uint8_t DEVICE_ID = 0;
const uint8_t MagicByte = 0xAF;

const uint8_t TYPE_STATUS = 0x01;
const uint8_t TYPE_ACK = 0x02;
const uint8_t TYPE_COMMAND = 0x03;

const uint8_t CMD_ON = 0x01;
const uint8_t CMD_OFF = 0x02;
const uint8_t CMD_MOMENT = 0x03;
const uint8_t CMD_GETSTAT = 0x04;

const uint8_t MAX_RETRIES = 3;
const uint32_t ACK_TIMEOUT = 2000;
struct LoRaPacket {
  uint8_t magic;
  uint8_t src;
  uint8_t dst;
  uint8_t counter;
  uint8_t type;
  uint8_t data;
};

struct PendingAck {
  uint8_t dst;
  uint8_t counter;
  String payload;
  uint8_t retryCount;
  unsigned long nextRetryTime; // <--- millis() Zeitpunkt
};

std::vector<PendingAck> pendingAcks;

String serialInput = "";

typedef void (*LoRaCallback)(LoRaPacket pkt, int rssi, float snr);
LoRaCallback onLoRaMessage = nullptr;

char buf[32];

// ==========================
// WLAN / MQTT Settings
// ==========================
const char* ssid = "IoT";
const char* password = "mYxmcvC5uiPF";

const char* mqtt_server = "192.168.2.6"; // IP oder Domain
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

const char* mqtt_topic_send = "garage/command";
const char* mqtt_topic_response = "garage/status";
const char* HA_PREFIX = "homeassistant";
const char* HA_DEVICE_DOMAIN = "garage_lora";

// ==========================
// Encryption
// ==========================
const char* aesKeyHex = "CHANGEME HERE";
uint8_t aesKey[32];

void hexStringToBytes(const char* hex, uint8_t* output, int len) {
  for (int i = 0; i < len; i++) {
    sscanf(hex + 2*i, "%2hhx", &output[i]);
  }
}

void decryptAES(uint8_t* input, uint8_t* output) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, aesKey, 256);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input, output);
  mbedtls_aes_free(&aes);
}

void encryptAES(uint8_t* input, uint8_t* output) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, aesKey, 256);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);
  mbedtls_aes_free(&aes);
}

String verifyChecksum(String msg) {
  String receivedChecksum = msg.substring(0,4);
  String payload = msg.substring(4);

  uint8_t checksum = 0;
  for (int i = 0; i < payload.length(); i++) {
    checksum ^= payload[i];
  }

  char buf[5];
  sprintf(buf, "%04x", checksum);

  if (receivedChecksum.equalsIgnoreCase(String(buf))) {
    return payload;
  }

  return "";
}

// ==========================
// LoRa Settings
// ==========================
unsigned long lastStatusSend = 0;
const unsigned long intervalStatusSend = 60UL * 1000UL;

void setDefaultLoRaConfig() {
  loraConfig.frequency = 868E6;
  loraConfig.spreadingFactor = 10;
  loraConfig.bandwidth = 125E3;
  loraConfig.codingRate = 5;
  loraConfig.preambleLength = 10;
  loraConfig.syncWord = 0x12;
  loraConfig.crc = true;
}

void loadLoRaConfig() {
  prefs.begin("lora", true);

  if (!prefs.isKey("freq")) {
    setDefaultLoRaConfig();
    prefs.end();
    return;
  }

  loraConfig.frequency = prefs.getLong("freq");
  loraConfig.spreadingFactor = prefs.getInt("sf");
  loraConfig.bandwidth = prefs.getLong("bw");
  loraConfig.codingRate = prefs.getInt("cr");
  loraConfig.preambleLength = prefs.getInt("pre");
  loraConfig.syncWord = prefs.getUChar("sync");
  loraConfig.crc = prefs.getBool("crc");

  prefs.end();
}

void saveLoRaConfig() {
  prefs.begin("lora", false);

  prefs.putLong("freq", loraConfig.frequency);
  prefs.putInt("sf", loraConfig.spreadingFactor);
  prefs.putLong("bw", loraConfig.bandwidth);
  prefs.putInt("cr", loraConfig.codingRate);
  prefs.putInt("pre", loraConfig.preambleLength);
  prefs.putUChar("sync", loraConfig.syncWord);
  prefs.putBool("crc", loraConfig.crc);

  prefs.end();
}

void applyLoRaConfig() {

  LoRa.end();

  if (!LoRa.begin(loraConfig.frequency)) {
    Serial.println("LoRa re-init failed!");
    return;
  }

  LoRa.setSpreadingFactor(loraConfig.spreadingFactor);
  LoRa.setSignalBandwidth(loraConfig.bandwidth);
  LoRa.setCodingRate4(loraConfig.codingRate);
  LoRa.setPreambleLength(loraConfig.preambleLength);
  LoRa.setSyncWord(loraConfig.syncWord);

  if (loraConfig.crc)
    LoRa.enableCrc();
  else
    LoRa.disableCrc();

  Serial.println("LoRa reconfigured!");
}

// ==========================
// LoRa Protokoll
// ==========================
bool decodeMessage(uint8_t* encrypted, LoRaPacket &pkt) {
  uint8_t decrypted[16];
  decryptAES(encrypted, decrypted);

  // in String wandeln und trimmen
  String msg = "";
  for(int i=0; i<16; i++) msg += (char)decrypted[i];
  msg.trim();

  // Checksum prüfen
  String payload = verifyChecksum(msg);
  if (payload == "" || payload.length() < 6) {
    Serial.println("Checksum fail or payload too short");
    return false;
  }

  // in Struktur umwandeln
  pkt.magic   = (uint8_t)payload[0];
  pkt.src     = (uint8_t)payload[1];
  pkt.dst     = (uint8_t)payload[2];
  pkt.counter = (uint8_t)payload[3];
  pkt.type    = (uint8_t)payload[4];
  pkt.data    = (uint8_t)payload[5];

  if (pkt.magic != MagicByte) {
    Serial.println("invalid magic byte");
    return false;
  }
  return true;
}

String encodeMessage(uint8_t dst, uint8_t type, uint8_t data, uint8_t counter) {
    String payload = "";
    payload += (char)MagicByte;   // Magic
    payload += (char)DEVICE_ID;   // Src
    payload += (char)dst;         // Dst
    payload += (char)counter;     // Counter
    payload += (char)type;        // Type
    payload += (char)data;        // Data

    return payload;  // Das ist der "encoded payload" zum Verschlüsseln
}

void sendAck(uint8_t dst, uint8_t counter) {
    String payload = "";
    payload += (char)MagicByte;   // Magic
    payload += (char)DEVICE_ID;   // Src
    payload += (char)dst;         // Dst
    payload += (char)counter;     // Counter
    payload += (char)0x02;        // Type = ACK
    payload += (char)0x00;        // Data optional 0

    sendEncrypted(payload);
    Serial.printf("Sent ACK to %d for counter %d\n", dst, counter);
}
void checkRetries() {
    unsigned long now = millis();
    for (int i = pendingAcks.size() - 1; i >= 0; i--) {
        PendingAck &ack = pendingAcks[i];
        if ((long)(now - ack.nextRetryTime) >= 0) {
            if (ack.retryCount < MAX_RETRIES) {
                sendEncrypted(ack.payload);
                ack.retryCount++;
                ack.nextRetryTime = now + ACK_TIMEOUT;
                Serial.printf("Retry #%d for dst %d number %d\n", ack.retryCount, ack.dst, ack.counter);
            } else {
                Serial.printf("Giving up for dst %d, counter %d\n", ack.dst, ack.counter);
                pendingAcks.erase(pendingAcks.begin() + i);
            }
        }
    }
}
void sendWithRetry(String payload, uint8_t dst, uint8_t counter) {
    PendingAck ack;
    ack.dst = dst;
    ack.counter = counter;
    ack.payload = payload;
    ack.retryCount = 0;
  ack.nextRetryTime = millis() + ACK_TIMEOUT;
  
    pendingAcks.push_back(ack);

    sendEncrypted(payload); // erstes Senden
}


void handleAck(uint8_t src, uint8_t counter) {
    auto it = std::find_if(pendingAcks.begin(), pendingAcks.end(),
        [src, counter](PendingAck &p){ return p.dst == src && p.counter == counter; });

    if (it != pendingAcks.end()) {
        pendingAcks.erase(it);
        Serial.printf("ACK received from %d for counter %d -> stopping retry\n", src, counter);
    }
}


void sendEncrypted(String payload) {

  uint8_t checksum = 0;
  for (int i = 0; i < payload.length(); i++) {
    checksum ^= payload[i];
  }

  char checksumHex[5];
  sprintf(checksumHex, "%04x", checksum);

  String fullMsg = String(checksumHex) + payload;
  while (fullMsg.length() < 16) fullMsg += " ";

  uint8_t input[16];
  uint8_t encrypted[16];
  memcpy(input, fullMsg.c_str(), 16);
  encryptAES(input, encrypted);

  LoRa.beginPacket();
  LoRa.write(encrypted, 16);
  LoRa.endPacket();

  Serial.println("Encrypted packet sent: " + payload);
}

// ==========================
// WebServer für Änderung der Parameter
// ==========================
WebServer server(80);

void handleGetConfig() {
  StaticJsonDocument<256> doc;

  doc["frequency"] = loraConfig.frequency;
  doc["sf"] = loraConfig.spreadingFactor;
  doc["bw"] = loraConfig.bandwidth;
  doc["cr"] = loraConfig.codingRate;
  doc["preamble"] = loraConfig.preambleLength;
  doc["sync"] = loraConfig.syncWord;
  doc["crc"] = loraConfig.crc;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSetConfig() {

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing body");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "text/plain", "JSON parse failed");
    return;
  }

  loraConfig.frequency = doc["frequency"] | loraConfig.frequency;
  loraConfig.spreadingFactor = doc["sf"] | loraConfig.spreadingFactor;
  loraConfig.bandwidth = doc["bw"] | loraConfig.bandwidth;
  loraConfig.codingRate = doc["cr"] | loraConfig.codingRate;
  loraConfig.preambleLength = doc["preamble"] | loraConfig.preambleLength;
  loraConfig.syncWord = doc["sync"] | loraConfig.syncWord;
  loraConfig.crc = doc["crc"] | loraConfig.crc;

  saveLoRaConfig();
  applyLoRaConfig();

  server.send(200, "text/plain", "LoRa config updated");
}

void setLoRaCallback(LoRaCallback cb) {
  onLoRaMessage = cb;
}

// ==========================
// OLED
// ==========================
SSD1306Wire display(OLED_I2C, OLED_SDA, OLED_SCL); // SDA, SCL
String lastOLEDMessage ="";
String lastOLEDMessageTime = "";
String lastOLEDMessageDirection = "";
unsigned long lastOledUpdate = 0;
const unsigned long oledInterval = 1000; // 1 Sekunde
int scrollOffset = 0;          // globale Variable
const int scrollStep = 6;      // wie viele Zeichen pro Update verschoben werden
const int displayChars = 50;   // wie viele Zeichen maximal angezeigt werden

// ==========================
// RTC / NTP
// ==========================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;  // MEZ
const int   daylightOffset_sec = 3600;

// ==========================
// MQTT reconnect
// ==========================
void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("LoRaBridgeESP32")) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_send);
      publishHABridgeDiscovery();
      sendStatusMQTT();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5s");
      delay(5000);
    }
  }
}

// ==========================
// OLED Update
// ==========================
void updateOLED() {
  display.clear();

    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)) {
      display.drawString(0,0,"RTC error");
    } else {
      sprintf(buf,"Time: %02d:%02d:%02d",timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
      display.drawString(0,0,buf);
    }
  display.drawLine(0, 15, 127, 15);
  display.drawString(0,20,lastOLEDMessageDirection);
  display.drawString(0,35,lastOLEDMessageTime);
  
  // Scrollende Nachricht
  String msgToShow = lastOLEDMessage;
  if(msgToShow.length() > displayChars) {
      if(scrollOffset + displayChars > msgToShow.length())
          scrollOffset = 0; // zurück zum Anfang
      msgToShow = msgToShow.substring(scrollOffset, scrollOffset + displayChars);
      scrollOffset += scrollStep;
  } else {
      scrollOffset = 0; // keine Scroll nötig
  }

  display.drawString(0,50,msgToShow);
  display.display();
}
// ==========================
// HA Discovery
// ==========================
void publishHABridgeDiscovery() {
    if (!client.connected()) return;

    uint8_t id = DEVICE_ID;
    
    // JSON erstellen
    String jsonStr;
    StaticJsonDocument<4096> doc;

    // Components (cmps)
    JsonObject cmps = doc.createNestedObject("cmps");

    // Buttons
    JsonObject button_clearAll = cmps.createNestedObject("button_clearAll");
    button_clearAll["name"] = "clear all devices";
    button_clearAll["p"] = "button";
    button_clearAll["payload_press"] = "{\"dst\": " + String(id) + ", \"command\": \"clearDevices\"}";
    button_clearAll["unique_id"] = "lorabridge_button_clearall";

    JsonObject button_getConfig = cmps.createNestedObject("button_getConfig");
    button_getConfig["name"] = "Get lora config";
    button_getConfig["p"] = "button";
    button_getConfig["payload_press"] = "{\"dst\": " + String(id) + ", \"command\": \"getLoRaConfig\"}";
    button_getConfig["unique_id"] = "lorabridge_button_getconfig";

    JsonObject button_restart = cmps.createNestedObject("button_restart");
    button_restart["name"] = "Restart";
    button_restart["p"] = "button";
    button_restart["payload_press"] = "{\"dst\": " + String(id) + ", \"command\": \"rebootBridge\"}";
    button_restart["unique_id"] = "lorabridge_button_reboot";

    // Textfelder (LoRa Config)
    JsonObject text_freq = cmps.createNestedObject("text_freq");
    text_freq["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"freq\": {{ value }} }}";
    text_freq["name"] = "LoRa Frequency";
    text_freq["p"] = "text";
    text_freq["unique_id"] = "lorabridge_n_freq";
    text_freq["value_template"] = "{{ value_json.data.freq }}";

    JsonObject text_sf = cmps.createNestedObject("text_sf");
    text_sf["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"sf\": {{ value }} }}";
    text_sf["name"] = "LoRa spreading Factor";
    text_sf["p"] = "text";
    text_sf["unique_id"] = "lorabridge_n_sf";
    text_sf["value_template"] = "{{ value_json.data.sf }}";

    JsonObject text_bw = cmps.createNestedObject("text_bw");
    text_bw["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"bw\": {{ value }} }}";
    text_bw["name"] = "LoRa bandwidth";
    text_bw["p"] = "text";
    text_bw["unique_id"] = "lorabridge_n_bw";
    text_bw["value_template"] = "{{ value_json.data.bw }}";

    JsonObject text_cr = cmps.createNestedObject("text_cr");
    text_cr["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"cr\": {{ value }} }}";
    text_cr["name"] = "LoRa Coding Rate";
    text_cr["p"] = "text";
    text_cr["unique_id"] = "lorabridge_n_cr";
    text_cr["value_template"] = "{{ value_json.data.cr }}";

    JsonObject text_pr = cmps.createNestedObject("text_pr");
    text_pr["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"preamble\": {{ value }} }}";
    text_pr["name"] = "LoRa Preamble";
    text_pr["p"] = "text";
    text_pr["unique_id"] = "lorabridge_n_pr";
    text_pr["value_template"] = "{{ value_json.data.preamble }}";

    JsonObject text_sync = cmps.createNestedObject("text_sync");
    text_sync["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"sync\": {{ value }} }}";
    text_sync["name"] = "LoRa Syncword";
    text_sync["p"] = "text";
    text_sync["unique_id"] = "lorabridge_n_sync";
    text_sync["value_template"] = "{{ value_json.data.sync }}";

    // Switch CRC
    JsonObject switch_crc = cmps.createNestedObject("switch_crc");
    switch_crc["payload_on"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"crc\": true }}";
    switch_crc["payload_off"] = "{\"dst\": " + String(id) + ", \"command\": \"setLoRaConfig\", \"data\":{\"crc\": false }}";
    switch_crc["state_on"] = true;
    switch_crc["state_off"] = false;
    switch_crc["name"] = "LoRa CRC";
    switch_crc["p"] = "switch";
    switch_crc["unique_id"] = "lorabridge_sw_crc";
    switch_crc["value_template"] = "{{ value_json.data.crc }}";

    // Delete Device
    JsonObject text_clearDev = cmps.createNestedObject("text_clearDev");
    text_clearDev["command_template"] = "{\"dst\": " + String(id) + ", \"command\": \"clearDevice\", \"data\":{\"id\": {{ value }} }}";
    text_clearDev["name"] = "Delete Device";
    text_clearDev["p"] = "text";
    text_clearDev["unique_id"] = "lorabridge_n_clearDev";
    text_clearDev["value_template"] = "";

    // Sensoren
    JsonObject s_lastPacket = cmps.createNestedObject("s_lastPacket");
    s_lastPacket["p"] = "sensor";
    s_lastPacket["name"] = "Last Packet received";
    s_lastPacket["value_template"] = "{{ value_json.timestamp }}";
    s_lastPacket["unique_id"] = "lorabridge_s_lastPacket";

    JsonObject s_numdev = cmps.createNestedObject("s_numdev");
    s_numdev["p"] = "sensor";
    s_numdev["name"] = "Number of Devices";
    s_numdev["value_template"] = "{{ value_json.data.numberDev }}";
    s_numdev["unique_id"] = "lorabridge_s_numdev";

    // Command / State Topics
    doc["command_topic"] = "garage/command";
    doc["state_topic"] = "garage/status/" + String(id) + "";
    doc["qos"] = 2;

    // Device info
    JsonObject dev = doc.createNestedObject("dev");
    dev["hw"] = "v1";
    dev["ids"] = "LoRaBridge";
    dev["mdl"] = "ESP32";
    dev["mf"] = "pilo";
    dev["name"] = "LoRaBridge";
    dev["sn"] = String(id);
    dev["sw"] = "4.1";

    // Other info
    JsonObject o = doc.createNestedObject("o");
    o["name"] = "LoRa2MQTT";
    o["sw"] = "4.1";
        

    
    // Topic für HA Discovery
    char topic[128];
    sprintf(topic, "%s/device/LoRaBridge/config", HA_PREFIX);
    
    serializeJson(doc, jsonStr);
    client.publish(topic, jsonStr.c_str(), true);
    Serial.println("bridge discovery sent");
    Serial.printf("Payload length: %d\n", jsonStr.length());
}

void publishHADiscovery(uint8_t id) {
    if (!client.connected()) return;

    char topic[128];
    String jsonStr;
    StaticJsonDocument<4096> doc;

    // Device info
    JsonObject dev = doc.createNestedObject("dev");
    dev["ids"] = String("LoRaShelly") + "_" + id;
    dev["name"] = String("LoRaShelly") + "_" + id;
    dev["mf"] = "pilo";
    dev["mdl"] = "LoRaShellyNode";
    dev["sw"] = "1.0";
    dev["hw"] = "v4.1";
    dev["sn"] = String(id);  // Seriennummer optional

    //Owner / Integration info
    JsonObject o = doc.createNestedObject("o");
    o["name"] = "LoRa2MQTT";
    o["sw"] = "4.1";

    // Components
    JsonObject cmps = doc.createNestedObject("cmps");

    // Input sensor
    JsonObject input = cmps.createNestedObject("input");
    input["p"] = "binary_sensor";
    input["name"] = "Input";
    input["device_class"] = "opening";
    input["payload_on"] = "OFF";
    input["payload_off"] = "ON";
    input["value_template"] = "{{ value_json.input }}";
    input["unique_id"] = String("LoRaShelly_") + id + "_input";

    // Output binary sensor
    JsonObject output = cmps.createNestedObject("output");
    output["p"] = "binary_sensor";
    output["name"] = "Output";
    output["value_template"] = "{{ value_json.relay }}";
    output["unique_id"] = String("LoRaShelly_") + id + "_output";

    // RSSI sensor
    JsonObject rssi = cmps.createNestedObject("rssi");
    rssi["p"] = "sensor";
    rssi["name"] = "RSSI";
    rssi["value_template"] = "{{ value_json.rssi }}";
    rssi["unit_of_measurement"] = "dBm";
    rssi["unique_id"] = String("LoRaShelly_") + id + "_rssi";

    // SNR sensor
    JsonObject snr = cmps.createNestedObject("snr");
    snr["p"] = "sensor";
    snr["name"] = "SNR";
    snr["value_template"] = "{{ value_json.snr }}";
    snr["unit_of_measurement"] = "dB";
    snr["unique_id"] = String("LoRaShelly_") + id + "_snr";

    // Last packet sensor
    JsonObject lastPacket = cmps.createNestedObject("lastPacket");
    lastPacket["p"] = "sensor";
    lastPacket["name"] = "Last Packet received";
    lastPacket["value_template"] = "{{ value_json.timestamp }}";
    lastPacket["unique_id"] = String("LoRaShelly_") + id + "_lastPacket";

    // Relay switch
    JsonObject relay = cmps.createNestedObject("relay");
    relay["p"] = "switch";
    relay["name"] = "Output";
    relay["value_template"] = "{{ value_json.relay }}";
    relay["state_on"] = "ON";
    relay["state_off"] = "OFF";
    relay["payload_on"] = "{\"dst\": " + String(id) + ", \"command\": \"on\"}";
    relay["payload_off"] = "{\"dst\": " + String(id) + ", \"command\": \"off\"}";
    relay["unique_id"] = String("LoRaShelly_") + id + "_relay";

    // Momentary button
    JsonObject button_mom = cmps.createNestedObject("button_momentary");
    button_mom["p"] = "button";
    button_mom["name"] = "Momentary";
    button_mom["payload_press"] = "{\"dst\": " + String(id) + ", \"command\": \"mom\"}";
    button_mom["unique_id"] = String("LoRaShelly_") + id + "_button_momentary";

    // GetStatus button
    JsonObject button_getstatus = cmps.createNestedObject("button_getstatus");
    button_getstatus["p"] = "button";
    button_getstatus["name"] = "Get Status";
    button_getstatus["payload_press"] = "{\"dst\": " + String(id) + ", \"command\": \"getstatus\"}";
    button_getstatus["unique_id"] = String("LoRaShelly_") + id + "_button_getstatus";

    // --- Topics ---
    doc["state_topic"] = "garage/status/"+String(id);
    doc["command_topic"] = "garage/command";
    doc["qos"] = 2;

    // Topic für HA Discovery
    sprintf(topic, "%s/device/LoRaShelly_%u/config", HA_PREFIX, id);
    serializeJson(doc, jsonStr);
    client.publish(topic, jsonStr.c_str(), true);
    Serial.println(jsonStr.length());
    Serial.print(topic);
    Serial.printf(" Published HA discovery (single payload) for device %d\n", id);
    Serial.println(jsonStr);
}
void removeHADiscovery(uint8_t id) {
    char topic[128];
    sprintf(topic, "%s/device/LoRaShelly_%u/config", HA_PREFIX, id);
    client.publish(topic, "", true); // leeren Payload = löschen in HA
    Serial.printf("removed %u from ha",id);
}
// ==========================
// Device Database
// ==========================
#define MAX_KNOWN_DEVICES 32

struct KnownDevice {
  uint8_t id;
  time_t firstSeen;
  time_t lastSeen;
};

std::vector<KnownDevice> knownDevices;

bool isKnownDevice(uint8_t id) {
  for (auto &d : knownDevices) {
    if (d.id == id) return true;
  }
  return false;
}

void rememberDevice(uint8_t id) {

  for (auto &d : knownDevices) {
    if (d.id == id) {
      time_t now;
      time(&now);
      if (now - d.lastSeen > 60) {
        d.lastSeen = now;
        saveDevices();
      }
      return;
    }
  }

  if (knownDevices.size() >= MAX_KNOWN_DEVICES) {
    Serial.println("Device list full");
    return;
  }

  KnownDevice dev;
  dev.id = id;
  time_t now;
  time(&now);
  dev.firstSeen = now;
  dev.lastSeen = now;

  knownDevices.push_back(dev);

  saveDevices();
  Serial.printf("New device discovered: %d\n", id);

  onNewDevice(id);
}

void onNewDevice(uint8_t id) {

  Serial.printf("First time device seen: %d\n", id);
  publishHADiscovery(id);


}
void saveDevices() {

  Preferences devPrefs;
  devPrefs.begin("devices", false);

  devPrefs.putUInt("count", knownDevices.size());

  for (int i = 0; i < knownDevices.size(); i++) {
    char key[8];
    sprintf(key, "d%d", i);
    devPrefs.putBytes(key, &knownDevices[i], sizeof(KnownDevice));
  }

  devPrefs.end();

  Serial.println("Device list saved");
}
void loadDevices() {

  knownDevices.clear();
  
  Preferences devPrefs;
  devPrefs.begin("devices", true);

  int count = devPrefs.getUInt("count", 0);

  for (int i = 0; i < count; i++) {

    char key[8];
    sprintf(key, "d%d", i);

    KnownDevice dev;

    size_t len = devPrefs.getBytes(key, &dev, sizeof(KnownDevice));

    if (len == sizeof(KnownDevice)) {
      knownDevices.push_back(dev);
    }
  }

  devPrefs.end();

  Serial.printf("Loaded %d known devices\n", knownDevices.size());
}
void handleListDevices() {

  StaticJsonDocument<512> doc;
  JsonArray arr = doc.createNestedArray("devices");

  for (auto &d : knownDevices) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = d.id;
    obj["firstSeen"] = d.firstSeen;
    obj["lastSeen"] = d.lastSeen;
  }

  String response;
  serializeJson(doc, response);

  server.send(200, "application/json", response);
}
void handleDeleteDevice() {

  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "missing id");
    return;
  }

  uint8_t id = server.arg("id").toInt();

  for (auto it = knownDevices.begin(); it != knownDevices.end(); ++it) {
    if (it->id == id) {
      knownDevices.erase(it);
      server.send(200, "text/plain", "device removed");
      saveDevices();
      removeHADiscovery(id);
      return;
    }
  }

  server.send(404, "text/plain", "device not found");
}
// ==========================
// MQTT 
// ==========================
void sendStatusMQTT() {
  
    struct tm timeinfo;
    StaticJsonDocument<512> response;
    JsonObject data = response.createNestedObject("data");
    
    if (getLocalTime(&timeinfo)) {
    sprintf(buf,"%02d:%02d:%02d",
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec);
    response["timestamp"] = buf;
  }

    response["src"] = 0;
    if (getLocalTime(&timeinfo)) {
          response["timestamp"] = buf;
    }
    
    response["type"] = "Status";

    data["freq"] = loraConfig.frequency;
    data["sf"] = loraConfig.spreadingFactor;
    data["bw"] = loraConfig.bandwidth;
    data["cr"] = loraConfig.codingRate;
    data["preamble"] = loraConfig.preambleLength;
    data["sync"] = loraConfig.syncWord;
    data["crc"] = loraConfig.crc;

    data["numberDev"] = knownDevices.size();
    String payload;
    serializeJson(response, payload);

    client.publish("garage/status/0", payload.c_str());
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, mqtt_topic_send) != 0) return;
  
    String msg = "";
    for (unsigned int i = 0; i < length; i++) { msg += (char)payload[i]; }

    Serial.println(msg);
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) {
      Serial.println("MQTT JSON parse failed");
      return;
    }
    
   struct tm timeinfo;
   
    int dst = doc["dst"] | 0;
    String cmdStr = doc["command"] | "";
    Serial.println("MQTT command received: " + msg);

    if (dst == DEVICE_ID) {
    Serial.println("command for bridge");
    StaticJsonDocument<512> response;
    JsonObject data = response.createNestedObject("data");
    
    if (getLocalTime(&timeinfo)) {
    sprintf(buf,"%02d:%02d:%02d",
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec);
    response["timestamp"] = buf;
  }

    response["src"] = 0;
    if (getLocalTime(&timeinfo)) {
          response["timestamp"] = buf;
    }

  if (cmdStr == "listDevices") {

    response["type"] = "listDevices";

    JsonArray arr = data.createNestedArray("devices");

    for (auto &d : knownDevices) {

      JsonObject obj = arr.createNestedObject();
      obj["id"] = d.id;
      obj["firstSeen"] = d.firstSeen;
      obj["lastSeen"] = d.lastSeen;

    }
  }
  else if (cmdStr == "clearDevices")
  {
    for (auto &d : knownDevices) {
      Serial.printf("Removing device %d from HA\n", d.id);
        removeHADiscovery(d.id);
    }
    
    knownDevices.clear();
    saveDevices();

    response["type"] = "clearDevices";
    data["result"] = "ok"; 
  }
    else if (cmdStr == "clearDevice") {

    uint8_t id = doc["data"]["id"] | 0;

    bool removed = false;

    for (auto it = knownDevices.begin(); it != knownDevices.end(); ++it) {
      if (it->id == id) {
        knownDevices.erase(it);
        removed = true;
        removeHADiscovery(id);
        break;
      }
    }

    if (removed) saveDevices();

    response["type"] = "clearDevice";
    data["id"] = id;
    data["removed"] = removed;
  }
  
   else if (cmdStr == "getLoRaConfig") {

    response["type"] = "getLoRaconfig";

    data["freq"] = loraConfig.frequency;
    data["sf"] = loraConfig.spreadingFactor;
    data["bw"] = loraConfig.bandwidth;
    data["cr"] = loraConfig.codingRate;
    data["preamble"] = loraConfig.preambleLength;
    data["sync"] = loraConfig.syncWord;
    data["crc"] = loraConfig.crc;
  }
  else if (cmdStr == "setLoRaConfig") {

    JsonObject cfg = doc["data"];

    if(cfg.containsKey("freq")) loraConfig.frequency = cfg["freq"];
    if(cfg.containsKey("sf")) loraConfig.spreadingFactor = cfg["sf"];
    if(cfg.containsKey("bw")) loraConfig.bandwidth = cfg["bw"];
    if(cfg.containsKey("cr")) loraConfig.codingRate = cfg["cr"];
    if(cfg.containsKey("preamble")) loraConfig.preambleLength = cfg["preamble"];
    if(cfg.containsKey("sync")) loraConfig.syncWord = cfg["sync"];
    if(cfg.containsKey("crc")) loraConfig.crc = cfg["crc"];

    saveLoRaConfig();
    applyLoRaConfig();

    response["type"] = "setLoRaConfig";

    data["freq"] = loraConfig.frequency;
    data["sf"] = loraConfig.spreadingFactor;
    data["bw"] = loraConfig.bandwidth;
    data["cr"] = loraConfig.codingRate;
    data["preamble"] = loraConfig.preambleLength;
    data["sync"] = loraConfig.syncWord;
    data["crc"] = loraConfig.crc;
  }
  else if (cmdStr == "rebootBridge")
  {
    response["type"] = "rebootBridge";
    data["result"] = "ok";
    ESP.restart();
  }
    else {
    Serial.println("Unknown bridge command");
    response["type"] = cmdStr;
    data["result"] = "unknown command";
  }

  String payload;
  serializeJson(response, payload);

  client.publish("garage/status/0", payload.c_str());

  Serial.println("Bridge response:");
  Serial.println(payload);
    // End bridge command
    sendStatusMQTT();
    return;
    
    }
    
    uint8_t cmdData = 0;
    if (cmdStr == "on") cmdData = CMD_ON;
    else if (cmdStr == "off") cmdData = CMD_OFF;
    else if (cmdStr == "mom") cmdData = CMD_MOMENT;
    else if (cmdStr == "getstatus") cmdData = CMD_GETSTAT;
    else {
      Serial.println("Unknown command: " + cmdStr);
      return;
    }
    uint8_t counter = millis() & 0xFF;
    String payloadStr = encodeMessage(dst, TYPE_COMMAND, cmdData, counter);
    
    if (dst == 0xFF) {
      sendEncrypted(payloadStr);
      Serial.println("Broadcast LoRa sent: " + payloadStr);
    } else {
      sendWithRetry(payloadStr, dst, counter);
      Serial.printf("LoRa sent to %02X: cmd=%s counter=%02X\n", dst, cmdStr.c_str(), counter);
    }

      if(getLocalTime(&timeinfo)) {
        sprintf(buf,"Time: %02d:%02d:%02d ",timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
        Serial.print(buf);
        lastOLEDMessageTime = buf;
      }
    lastOLEDMessageDirection = "MQTT -> LoRa";
    lastOLEDMessage = String(dst) + ": "+cmdStr;
    Serial.println("MQTT -> LoRa: " + msg);
    
    updateOLED();
}
// ==========================
// LoRa Handler Function
// ==========================
void callbackLoRa(LoRaPacket pkt, int rssi, float snr) {
  Serial.printf("Received LoRa: counter=%d src=%d dst=%d type=%d data=%d RSSI=%d SNR=%.1f\n",
                pkt.counter, pkt.src, pkt.dst, pkt.type, pkt.data, rssi, snr);

  if (pkt.dst != DEVICE_ID && pkt.dst != 0xFF) {
    Serial.println("not for me or broadcast, ignoring");
    return;
  }
  if (pkt.type == TYPE_ACK && pkt.dst == DEVICE_ID) {
    handleAck(pkt.src, pkt.counter);
    return;
  }
  if (pkt.type != TYPE_ACK && pkt.dst == DEVICE_ID) {
    sendAck(pkt.src, pkt.counter);
    Serial.println("Incoming paket for me, acking");
    
  }

  if (pkt.type == TYPE_STATUS) {
  rememberDevice(pkt.src);
  }

  String typeStr;
       switch(pkt.type) {
        case 0x01: typeStr = "Status"; break;
        case 0x02: typeStr = "Ack"; break;
        case 0x03: typeStr = "Command"; break;
        default: typeStr = "Unknown"; break;
  }
  
  Serial.printf("Type: %s, Data: Input=%s Relay=%s, Raw: 0x%02X bits: ",
              typeStr.c_str(),
              (pkt.data & 0x01) ? "ON" : "OFF",
              (pkt.data & 0x02) ? "ON" : "OFF",
              pkt.data);

  for (int i = 7; i >= 0; i--) Serial.print((pkt.data >> i) & 0x01);
  Serial.println();


  // OLED Update vorbereiten
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)) {
      sprintf(buf,"Time: %02d:%02d:%02d",timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
      lastOLEDMessageTime = buf;
  }
  lastOLEDMessageDirection = "LoRa -> MQTT";
  lastOLEDMessage = String(pkt.src) + ":" + typeStr + ", " +
                  " In:" + ((pkt.data & 0x01) ? "ON" : "OFF") + 
                  " Relay:" + ((pkt.data & 0x02) ? "ON" : "OFF");

  // MQTT
  StaticJsonDocument<256> mqttDoc;

  mqttDoc["src"] = String(pkt.src);
  mqttDoc["dst"] = String(pkt.dst);
  mqttDoc["counter"] = String(pkt.counter);
  mqttDoc["type"] = typeStr;
  mqttDoc["input"] = (pkt.data & 0x01) ? "ON" : "OFF";
  mqttDoc["relay"] = (pkt.data & 0x02) ? "ON" : "OFF";
  mqttDoc["rssi"] = rssi;
  mqttDoc["snr"] = snr;

  // Timestamp: aktuelle lokale Zeit als HH:MM:SS
  if(getLocalTime(&timeinfo)) {
    sprintf(buf, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    mqttDoc["timestamp"] = buf;
  } else {
    mqttDoc["timestamp"] = "00:00:00";
  }

  String mqttPayload;
  serializeJson(mqttDoc, mqttPayload);

  if(client.connected()) {
    String topic = String(mqtt_topic_response) + "/" + String(pkt.src);
    client.publish(topic.c_str(), mqttPayload.c_str());
    Serial.println("LoRa packet pushed to MQTT: " + mqttPayload);
  } else {
    Serial.println("MQTT not connected, packet not sent");
  }
  // OLED
  updateOLED();
}

// ==========================
// Setup
// ==========================
void setup() {
  pinMode(OLED_RST,OUTPUT);
  digitalWrite(OLED_RST,LOW);
  delay(50);
  digitalWrite(OLED_RST,HIGH);
  
  Serial.begin(115200);
  
  Wire.begin(OLED_SDA,OLED_SCL);
  display.init();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0,0,"LoRa AES MQTT Bridge");
  display.drawLine(0, 15, 127, 15);
  display.drawString(0,20, "booting..");
  display.display();

  Serial.println("Heltec LoRa AES MQTT Bridge");

  // WLAN
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  loadLoRaConfig();
  applyLoRaConfig();

  setLoRaCallback(callbackLoRa);
  loadDevices();
  
  server.on("/lora/config", HTTP_GET, handleGetConfig);
  server.on("/lora/config", HTTP_POST, handleSetConfig);
  server.on("/devices", HTTP_GET, handleListDevices);
  server.on("/devices/delete", HTTP_POST, handleDeleteDevice);
  server.begin();

  Serial.println("REST API started");

  hexStringToBytes(aesKeyHex, aesKey, 32);
  Serial.println("LoRa initialized.");

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callbackMQTT);
  client.setBufferSize(4096);

  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  ArduinoOTA.onStart([]() {
    display.clear();
    display.drawString(0,0,"OTA Start");
    display.display();
  });
  ArduinoOTA.onEnd([]() {
    display.clear();
    display.drawString(0,0,"OTA End");
    display.drawString(0,20,"Rebooting...");
    display.display();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    sprintf(buf, "Progress: %u%%\r\n", (progress / (total / 100)));
    display.drawString(0,35,buf);
    display.display();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.setHostname("MQTTLoRaBridge");
  ArduinoOTA.begin();
 
}

// ==========================
// Loop
// ==========================
void loop() {

  // MQTT Reconnect & Loop
  if (!client.connected()) reconnect();
  client.loop();

  // OTA
  ArduinoOTA.handle();

  // Webserver
  server.handleClient();

  // retry send nötig?
  checkRetries();
  
  // LoRa Empfang
  int packetSize = LoRa.parsePacket();
  if (packetSize == 16) {  // wir erwarten genau 16 Byte
    uint8_t buffer[16];
    int len = 0;

    while (LoRa.available() && len < 16) {
      buffer[len++] = LoRa.read();
    }

    if (len == 16) {
      LoRaPacket pkt;
      if (decodeMessage(buffer, pkt)) {
        // Callback aufrufen
        if (onLoRaMessage) {
          onLoRaMessage(pkt, LoRa.packetRssi(), LoRa.packetSnr());
        }
      } else {
        Serial.println("LoRa checksum invalid!");
        lastOLEDMessage = "LoRa checksum invalid!";
        updateOLED();
      }
    }
  }

while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { // Ende einer Eingabezeile
        serialInput.trim();
        if (serialInput.length() > 0) {
            // Parsing: z.B. "dst=01 type=0x01 data=0x03"
            uint8_t dst = 0, type = 0, data = 0;
            int idx;

            // dst
            idx = serialInput.indexOf("dst=");
            if (idx >= 0) dst = (uint8_t) strtol(serialInput.substring(idx+4, serialInput.indexOf(" ", idx)).c_str(), nullptr, 16);

            // type
            idx = serialInput.indexOf("type=");
            if (idx >= 0) type = (uint8_t) strtol(serialInput.substring(idx+5, serialInput.indexOf(" ", idx) != -1 ? serialInput.indexOf(" ", idx) : serialInput.length()).c_str(), nullptr, 16);

            // data
            idx = serialInput.indexOf("data=");
            if (idx >= 0) data = (uint8_t) strtol(serialInput.substring(idx+5).c_str(), nullptr, 16);

            // Counter generieren (einfaches Beispiel: millis() & 0xFF)
            uint8_t counter = millis() & 0xFF;

            // Payload bauen
            String payload = encodeMessage(dst, type, data, counter);

            // Senden
            if (dst == 0xFF) {
                sendEncrypted(payload); // Broadcast ohne ACK
                Serial.println("Broadcast packet sent: " + payload);
            } else {
                sendWithRetry(payload, dst, counter); // Normales Paket mit ACK
                Serial.printf("Packet sent to %02X: type=%02X data=%02X counter=%02X\n", dst, type, data, counter);
            }
        }

        serialInput = ""; // Reset Input
    } else {
        serialInput += c; // Zeile zusammenbauen
    }
}
  

  unsigned long now = millis();
  if(now - lastOledUpdate >= oledInterval) {
    lastOledUpdate = now;
    updateOLED();
  }
  if (now - lastStatusSend >= intervalStatusSend) {
    lastStatusSend = now;
    sendStatusMQTT();
  }
}
