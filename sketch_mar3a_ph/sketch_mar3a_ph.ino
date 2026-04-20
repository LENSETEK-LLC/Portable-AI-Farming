#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>

// WiFi credentials will be loaded from EEPROM
char ssid[33];
char password[65];

const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

const char* TOPIC = "smartfarming/portable/ph";

unsigned long lastMsg = 0;
const unsigned long intervalMs = 5000;

// --- Variabel Sensor pH ---
const int pHPin = A0;
int nilai_sampel[10];
int temp;
float nilai_kalibrasi = 22.90; //aslinya 21.34 dikalibrasi 22.90 

void saveCredentials(const char* s, const char* p) {
  for (int i = 0; i < 32; ++i) EEPROM.write(i, s[i]);
  for (int i = 0; i < 64; ++i) EEPROM.write(32 + i, p[i]);
  EEPROM.commit();
  Serial.println("\n[SYSTEM] WiFi Berhasil Disimpan ke EEPROM!");
}

bool loadCredentials() {
  for (int i = 0; i < 32; ++i) ssid[i] = EEPROM.read(i);
  for (int i = 0; i < 64; ++i) password[i] = EEPROM.read(32 + i);
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
  if (Serial.available() < 6) return;
  
  if (Serial.peek() != 'I') {
    Serial.read();
    return;
  }
  
  char header[7];
  Serial.readBytes(header, 6);
  header[6] = '\0';
  
  if (strcmp(header, "IMPROV") == 0) {
    while (Serial.available() < 3);
    uint8_t version = Serial.read();
    uint8_t type = Serial.read();
    uint8_t len = Serial.read();
    
    uint8_t buffer[128];
    while (Serial.available() < len);
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
        uint8_t response[] = {'I', 'M', 'P', 'R', 'O', 'V', 0x01, 0x01, 0x01, 0x04, 0x1A}; 
        Serial.write(response, 11);
        
        delay(1000);
        ESP.restart();
      }
    }
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
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Mencoba terhubung ke MQTT... ");
    String cid = "ESP8266_pH_" + String(ESP.getChipId(), HEX);

    if (client.connect(cid.c_str())) {
      Serial.println("Berhasil Terhubung ke Broker!");
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
    if (!client.connected()) reconnect();
    client.loop();
  } else {
    // Heartbeat saat WiFi belum terhubung agar terlihat di Serial Monitor
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 3000) {
      lastHeartbeat = millis();
      Serial.println("[SYSTEM] Menunggu detail WiFi dari Web App... (Tutup Serial Monitor saat mengirim)");
    }
  }

  unsigned long now = millis();
  if (now - lastMsg >= intervalMs) {
    lastMsg = now;

    // 1. Ambil 10 sampel data
    for(int i = 0; i < 10; i++) { 
      nilai_sampel[i] = analogRead(pHPin);
      delay(30);
    }
    
    // 2. Urutkan data
    for(int i = 0; i < 9; i++) {
      for(int j = i + 1; j < 10; j++) {
        if(nilai_sampel[i] > nilai_sampel[j]) {
          temp = nilai_sampel[i];
          nilai_sampel[i] = nilai_sampel[j];
          nilai_sampel[j] = temp;
        }
      }
    }
    
    // 3. Ambil 6 nilai tengah
    long total = 0;
    for(int i = 2; i < 8; i++) { 
      total += nilai_sampel[i];
    }
    
    // 4. Hitung rata-rata
    float rata_rata_analog = (float)total / 6.0;
    
    // 5. Konversi ke Volt
    float voltage = rata_rata_analog * 3.3 / 1024.0;
    
    // 6. Konversi ke pH dengan Proteksi Disconnect
    float nilai_pH;
    char ph_str[16];
    
    // Jika analogRead sangat rendah (kabel lepas), jangan hitung rumus
    if (rata_rata_analog < 10) { 
      nilai_pH = 0.0;
      strcpy(ph_str, "OFF"); 
    } else {
      nilai_pH = (-5.70 * voltage) + nilai_kalibrasi;
      dtostrf(nilai_pH, 1, 2, ph_str);
    }

    // 7. Kirim ke MQTT Dashboard
    client.publish(TOPIC, ph_str);
    
    // 8. Pantau di Serial Monitor
    Serial.print(TOPIC);
    Serial.print("RAW=");
    Serial.print(rata_rata_analog, 1);
    Serial.print(" V=");
    Serial.print(voltage, 3);
    Serial.print(" pH=");
    Serial.println(ph_str);
    
    if (rata_rata_analog < 10) {
      Serial.println("[WARNING] Sensor kemungkinan terputus/BNC longgar!");
    }
  }
}