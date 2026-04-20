#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <math.h>

// WiFi credentials will be loaded from EEPROM
char ssid[33];
char password[65];

const char *mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;
const char *base_topic = "smartfarming/portable/tds";

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
const unsigned long intervalMs = 5000;

void saveCredentials(const char *s, const char *p) {
  for (int i = 0; i < 32; ++i)
    EEPROM.write(i, s[i]);
  for (int i = 0; i < 64; ++i)
    EEPROM.write(32 + i, p[i]);
  EEPROM.commit();
  Serial.println("\n[SYSTEM] WiFi Berhasil Disimpan ke EEPROM!");
}

bool loadCredentials() {
  for (int i = 0; i < 32; ++i)
    ssid[i] = EEPROM.read(i);
  for (int i = 0; i < 64; ++i)
    password[i] = EEPROM.read(32 + i);
  ssid[32] = '\0';
  password[64] = '\0';

  if (strlen(ssid) > 0 && (uint8_t)ssid[0] != 255) {
    Serial.print("\n[SYSTEM] WiFi Terdeteksi: ");
    Serial.println(ssid);
    return true;
  }
  return false;
}

void handleImprovSerial() {
  if (Serial.available() < 6)
    return;

  if (Serial.peek() != 'I') {
    Serial.read();
    return;
  }

  char header[7];
  Serial.readBytes(header, 6);
  header[6] = '\0';

  if (strcmp(header, "IMPROV") == 0) {
    while (Serial.available() < 3)
      ;
    uint8_t version = Serial.read();
    uint8_t type = Serial.read();
    uint8_t len = Serial.read();

    uint8_t buffer[128];
    while (Serial.available() < len)
      ;
    Serial.readBytes(buffer, len);
    uint8_t checksum = Serial.read();

    if (type == 0x03) { // RPC Command
      uint8_t command = buffer[0];
      if (command == 0x01) { // WiFi Settings
        int ssidLen = buffer[1];
        char newSsid[33];
        memcpy(newSsid, &buffer[2], ssidLen);
        newSsid[ssidLen] = '\0';

        int passLen = buffer[2 + ssidLen];
        char newPass[65];
        memcpy(newPass, &buffer[3 + ssidLen], passLen);
        newPass[passLen] = '\0';

        saveCredentials(newSsid, newPass);

        // Simpan konfirmasi balikan ke Web
        uint8_t response[] = {'I',  'M',  'P',  'R',  'O', 'V',
                              0x01, 0x01, 0x01, 0x04, 0x1A};
        Serial.write(response, 11);

        delay(1000);
        ESP.restart();
      }
    }
  }
}

// --- MQTT Callback (Menerima Perintah) ---
void callback(char *topic, byte *payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++)
    msg += (char)payload[i];

  Serial.print("\n[MQTT CMD] Topic: ");
  Serial.print(topic);
  Serial.print(" | Msg: ");
  Serial.println(msg);

  String logAck = "[ACK] Received command: " + msg;
  String logTopic = String(base_topic) + "/logs";
  client.publish(logTopic.c_str(), logAck.c_str());

  if (msg == "REBOOT") {
    client.publish(logTopic.c_str(), "[SYSTEM] Rebooting device...");
    delay(1000);
    ESP.restart();
  } else if (msg == "SCAN") {
    client.publish(logTopic.c_str(), "[SYSTEM] Scanning sensors...");
  }
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  delay(1000);

  Serial.println("\n\n============================");
  Serial.println("  SMART FARMING - STARTING  ");
  Serial.println("============================");

  if (loadCredentials()) {
    WiFi.begin(ssid, password);
    Serial.print("Menghubungkan ke ");
    Serial.println(ssid);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(500);
      Serial.print(".");
      handleImprovSerial();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[SYSTEM] WiFi Berhasil Terhubung!");
    } else {
      Serial.println("\n[SYSTEM] WiFi Timeout.");
    }
  } else {
    Serial.println("[SYSTEM] Belum ada WiFi. Gunakan Web App untuk Setup.");
  }

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// --- MQTT Reconnect ---
void reconnect() {
  while (!client.connected()) {
    Serial.print("Mencoba terhubung ke MQTT... ");
    // Unique Client ID untuk TDS
    String cid = "ESP8266_TDS_" + String(ESP.getChipId(), HEX);

    if (client.connect(cid.c_str())) {
      Serial.println("Berhasil Terhubung ke Broker!");
      String cmdTopic = String(base_topic) + "/cmd";
      client.subscribe(cmdTopic.c_str());
      Serial.print("[SYSTEM] Subscribed to: ");
      Serial.println(cmdTopic);
    } else {
      Serial.print("Gagal, status=");
      Serial.println(client.state());
      delay(2000);
    }
  }
}

void loop() {
  handleImprovSerial();

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected())
      reconnect();
    client.loop();
  } else {
    // Heartbeat saat WiFi belum terhubung agar terlihat di Serial Monitor
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 3000) {
      lastHeartbeat = millis();
      Serial.println("[SYSTEM] Menunggu detail WiFi dari Web App... (Tutup "
                     "Serial Monitor saat mengirim)");
    }
  }

  unsigned long now = millis();
  if (now - lastMsg >= intervalMs) {
    lastMsg = now;

    int rawValue = analogRead(A0);
    float voltage = rawValue * 3.3 / 1024.0;

    // Rumus konversi TDS
    float tdsValue = (133.42 * pow(voltage, 3) - 255.86 * pow(voltage, 2) +
                      857.39 * voltage) *
                     0.5;

    // Konversi nilai TDS ke string
    char tds_str[16];
    dtostrf(tdsValue, 1, 2, tds_str);

    // --- PERUBAHAN TOPIK DI SINI ---
    // Publish ke topik baru khusus TDS
    client.publish(base_topic, tds_str);

    // Debugging
    Serial.print("Kirim ke Dashboard -> Topic: ");
    Serial.print(base_topic);
    Serial.print(" | Value: ");
    Serial.println(tds_str);

    // Kirim Log (Otomatis ditambahkan /logs)
    String logTopic = String(base_topic) + "/logs";
    String logData = "RAW=" + String(rawValue) + " V=" + String(voltage, 3) +
                     " TDS=" + String(tds_str);
    client.publish(logTopic.c_str(), logData.c_str());
    
    Serial.print(base_topic);
    Serial.print(" Detail: ");
    Serial.println(logData);
  }
}