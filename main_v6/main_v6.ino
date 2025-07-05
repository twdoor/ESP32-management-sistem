#include <esp_now.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ESPmDNS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


// ==================== Configurare OLED DISPLAY ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define I2C_SDA 21
#define I2C_SCL 22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;
int displayPage = 0;
const int MAX_DISPLAY_PAGES = 3;
unsigned long pageChangeTime = 0;
const unsigned long PAGE_DURATION = 3000;

// ==================== Configurare ====================
bool IS_MASTER = true;
bool CREATE_AP = true;
uint8_t DEVICE_ID = 1;
uint8_t MODULE_TYPE = 0;

#define SETUP_BUTTON_PIN 0
bool lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
const unsigned long BUTTON_HOLD_TIME = 3000;

struct DeviceConfig {
  bool configured = false;
  bool is_master = true;
  bool create_ap = true;
  uint8_t device_id = 1;
  uint8_t module_type = 0;
  char device_name[32] = "Device";
  uint8_t master_mac[6] = {0};
  uint8_t slave_macs[10][6] = {0};
  uint8_t slave_count = 0;
  char wifi_ssid[64] = "";
  char wifi_password[64] = "";
  char ap_name[32] = "SistemEsp32";
  char ap_password[32] = "parola_test";
  uint32_t checksum = 0;
};

DeviceConfig config;
bool setupMode = false;

const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long WS_UPDATE_INTERVAL = 2000;
const unsigned long DEVICE_TIMEOUT = 5 * 60 * 1000;

// ==================== WIFI & AP SETTINGS ====================
const char* ap_ssid = "SistemEsp32";
const char* ap_password = "parola_test";
const char* hostname = "sistem-host";

const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

// ==================== ESP-NOW SETTINGS ====================
#define MAX_PEERS 20

// ==================== DATA STRUCTURES ====================
typedef struct {
  uint8_t sender_id;
  uint8_t module_type;
  float temperature;
  float humidity;
  float light_value;
  unsigned long timestamp;
  char device_name[32];
} sensor_message;

typedef struct {
  uint8_t sender_id;
  uint8_t target_id;
  uint8_t command_type;
  uint32_t parameter;
  unsigned long timestamp;
} command_message;

sensor_message deviceReadings[MAX_PEERS];
bool deviceActive[MAX_PEERS] = {false};
unsigned long deviceLastSeen[MAX_PEERS] = {0};

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

bool dataChanged = false;
unsigned long lastWSUpdate = 0;
uint16_t currentUpdateInterval = UPDATE_INTERVAL / 1000;

// ==================== DISPLAY FUNCTIONS ====================
void initDisplay() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 alocare nereusita"));
    return;
  }
  
  Serial.println("Initializarea ecranului reusita");

  display.clearDisplay();

  displayStartupMessage();
  
  delay(2000);
}

void displayStartupMessage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("Sistemul Esp32"));
  display.println(F("=================="));
  display.println();
  display.print(F("Modul: "));
  display.println(config.device_name);
  display.print(F("tip: "));
  display.println(IS_MASTER ? "Lider" : "Modul");
  display.print(F("ID: "));
  display.println(DEVICE_ID);
  
  display.display();
}

void displaySetupMode() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("Mod Initializare"));
  display.println(F("=========="));
  display.println();
  display.println(F("Conectativa la WiFi:"));
  display.println(F("Init_Mod"));
  display.println(F("Parola: 12345678"));
  display.println();
  display.print(F("IP: "));
  display.println(WiFi.softAPIP());
  
  display.display();
}

void updateDisplay() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    if (setupMode) {
      displaySetupMode();
    } else {
      if (currentTime - pageChangeTime >= PAGE_DURATION) {
        displayPage = (displayPage + 1) % MAX_DISPLAY_PAGES;
        pageChangeTime = currentTime;
      }
      
      switch (displayPage) {
        case 0:
          displaySystemInfo();
          break;
        case 1:
          displaySensorData();
          break;
        case 2:
          displayNetworkInfo();
          break;
      }
    }
    
    lastDisplayUpdate = currentTime;
  }
}

void displaySystemInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("Status sistem"));
  display.println(F("============="));
  
  display.print(F("Modul: "));
  display.println(config.device_name);
  
  display.print(F("tip: "));
  display.print(IS_MASTER ? "Lider" : "Modul");
  display.print(F(" (ID:"));
  display.print(DEVICE_ID);
  display.println(F(")"));
  
  display.print(F("Uptime: "));
  unsigned long uptimeSeconds = millis() / 1000;
  if (uptimeSeconds < 60) {
    display.print(uptimeSeconds);
    display.println(F("s"));
  } else if (uptimeSeconds < 3600) {
    display.print(uptimeSeconds / 60);
    display.println(F("m"));
  } else {
    display.print(uptimeSeconds / 3600);
    display.println(F("h"));
  }
  
  if (IS_MASTER) {
    int activeCount = 0;
    unsigned long currentTime = millis();
    for (int i = 0; i < MAX_PEERS; i++) {
      if (deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
        activeCount++;
      }
    }
    display.print(F("Activ: "));
    display.print(activeCount);
    display.print(F("/"));
    display.println(config.slave_count + 1);
  }
  
  display.setCursor(115, 57);
  display.print(F("1/3"));
  
  display.display();
}

void displaySensorData() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("Date Masurate"));
  display.println(F("==========="));
  
  if (MODULE_TYPE > 0) {
    int idx = DEVICE_ID - 1;
    display.print(F("Local (ID "));
    display.print(DEVICE_ID);
    display.println(F("):"));
    
    if (MODULE_TYPE == 1) { // Temp & Umid
      if (deviceReadings[idx].temperature > -999) {
        display.print(F("Temp: "));
        display.print(deviceReadings[idx].temperature, 1);
        display.println(F("C"));
        
        display.print(F("Umid: "));
        display.print(deviceReadings[idx].humidity, 1);
        display.println(F("%"));
      } else {
        display.println(F("No temp/umid data"));
      }
    } else if (MODULE_TYPE == 2) { // Lumina
      if (deviceReadings[idx].light_value > -999) {
        display.print(F("Niv Lum: "));
        display.print(deviceReadings[idx].light_value, 1);
        display.println(F("%"));
      } else {
        display.println(F("No lum data"));
      }
    }
  } else if (IS_MASTER) {
    bool foundData = false;
    unsigned long currentTime = millis();
    
    for (int i = 0; i < MAX_PEERS && !foundData; i++) {
      if (i == (DEVICE_ID - 1)) continue;
      
      if (deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
        display.print(F("Modul "));
        display.print(i + 1);
        display.println(F(":"));
        
        if (deviceReadings[i].module_type == 1) {
          display.print(F("T:"));
          display.print(deviceReadings[i].temperature, 1);
          display.print(F("C H:"));
          display.print(deviceReadings[i].humidity, 1);
          display.println(F("%"));
        } else if (deviceReadings[i].module_type == 2) {
          display.print(F("L: "));
          display.print(deviceReadings[i].light_value, 1);
          display.println(F("%"));
        }
        foundData = true;
      }
    }
    
    if (!foundData) {
      display.println(F("Fara date gasite"));
    }
  } else {
    display.println(F("Niciun senzor"));
    display.println(F("in modul"));
  }
  
  display.setCursor(115, 57);
  display.print(F("2/3"));
  
  display.display();
}

void displayNetworkInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("Info Network"));
  display.println(F("============"));
  
  if (config.create_ap) {
    display.println(F("Mod: Punct de acces"));
    display.print(F("Wifi: "));
    display.println(config.ap_name);
    display.print(F("IP: "));
    display.println(WiFi.softAPIP());
  } else if (WiFi.status() == WL_CONNECTED) {
    display.println(F("Mod: WiFi client"));
    display.print(F("Wifi: "));
    display.println(config.wifi_ssid);
    display.print(F("IP: "));
    display.println(WiFi.localIP());
  } else {
    display.println(F("WiFi: Deconectat"));
  }
  
  String mac = WiFi.macAddress();
  display.print(F("MAC: ..."));
  display.println(mac.substring(12));
  
  display.print(F("ESP-NOW: "));
  display.println(F("Active"));
  
  display.setCursor(115, 57);
  display.print(F("3/3"));
  
  display.display();
}

void displayError(const char* message) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(F("ERROR"));
  display.println(F("====="));
  display.println();
  display.println(message);
  
  display.display();
}

void displayMessage(const char* title, const char* message, int displayTime = 2000) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  
  display.println(title);
  display.println(F("============="));
  display.println();
  display.println(message);
  
  display.display();
  delay(displayTime);
}

// ==================== FUNCTION DECLARATIONS ====================
void loadConfiguration();
void saveConfiguration();
uint32_t calculateChecksum(const DeviceConfig& cfg);
void enterSetupMode();

void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &light);
void sendReadings();
void handleReceivedData(const sensor_message &reading);
void handleReceivedCommand(const command_message &command);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

void setupNetwork();
void setupWebServer();
void setupWebSocket();
void sendWebSocketUpdate();
void handleWebSocketMessage(AsyncWebSocketClient *client, char *data);
String getSetupPageHTML();

void checkSetupButton();
String getModuleTypeName(uint8_t type);
String macToString(const uint8_t* mac);
bool stringToMac(const String& str, uint8_t* mac);
String formatUptime(unsigned long ms);
String getTimeAgo(unsigned long timestamp);

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Initializare Network Esp32...");

  initDisplay();

  EEPROM.begin(1024);
  loadConfiguration();
  
  if (!config.configured) {
    Serial.println("Necesita initializare de baza");
    displayMessage("SETUP REQUIRED", "First-time setup\nneeded", 3000);
    enterSetupMode();
    return;
  }
  
  IS_MASTER = config.is_master;
  CREATE_AP = config.create_ap;
  DEVICE_ID = config.device_id;
  MODULE_TYPE = config.module_type;
  
  Serial.printf("Configurare incarcata - %s, ID: %d, Modul: %s\n", 
                IS_MASTER ? "LIDER" : "MODUL", DEVICE_ID, getModuleTypeName(MODULE_TYPE).c_str());

  displayStartupMessage();

  for (int i = 0; i < MAX_PEERS; i++) {
    deviceReadings[i] = {0, 0, -999, -999, -999, 0};
  }

  deviceReadings[DEVICE_ID - 1] = {DEVICE_ID, MODULE_TYPE, -999, -999, -999, millis()};
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();

  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(SETUP_BUTTON_PIN);
  
  if (IS_MASTER) {
    setupNetwork();
    if (!setupMode) {
      setupWebServer();
      setupWebSocket();
    }
  }

  if (!setupMode) {
    initESPNow();
  }

  // Initialize display timing
  lastDisplayUpdate = millis();
  pageChangeTime = millis();
}


// ==================== MAIN LOOP ====================
void loop() {
  checkSetupButton();
  
  // Update display
  updateDisplay();
  
  if (setupMode) {
    delay(10);
    return;
  }
  
  static unsigned long lastSendTime = 0;
  unsigned long currentTime = millis();
  
  if (currentTime - lastSendTime >= (currentUpdateInterval * 1000) && MODULE_TYPE > 0) {
    float temp, hum, light;
    getSensorReadings(temp, hum, light);
    
    int idx = DEVICE_ID - 1;
    deviceReadings[idx].temperature = temp;
    deviceReadings[idx].humidity = hum;
    deviceReadings[idx].light_value = light;
    deviceReadings[idx].timestamp = currentTime;
    deviceLastSeen[idx] = currentTime;
    
    sendReadings();
    dataChanged = true;
    lastSendTime = currentTime;
  }
  
  if (IS_MASTER && (currentTime - lastWSUpdate >= WS_UPDATE_INTERVAL || dataChanged)) {
    sendWebSocketUpdate();
    lastWSUpdate = currentTime;
    dataChanged = false;
  }
  
  delay(10);
}


// ==================== CONFIGURATION FUNCTIONS ====================
void loadConfiguration() {
  EEPROM.get(0, config);
  uint32_t calculated = calculateChecksum(config);
  
  if (config.checksum != calculated) {
    Serial.println("Control nepotrivit - configurare cu valori implicite");
    config = DeviceConfig();
  } else {
    Serial.println("Configurare incarcata cu succes");
  }
}

void saveConfiguration() {
  config.checksum = calculateChecksum(config);
  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println("Configurare salvata in EEPROM");
}

uint32_t calculateChecksum(const DeviceConfig& cfg) {
  uint32_t checksum = 0;
  const uint8_t* data = (const uint8_t*)&cfg;
  size_t size = sizeof(DeviceConfig) - sizeof(cfg.checksum);
  
  for (size_t i = 0; i < size; i++) {
    checksum += data[i];
  }
  return checksum;
}

void enterSetupMode() {
  setupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
  WiFi.softAP("Init_Mod", "12345678");
  
  Serial.println("Punct de Initializare - Wifi: Init_Mod, Password: 12345678");
  Serial.print("Setup URL: http://");
  Serial.println(WiFi.softAPIP());

  displaySetupMode();
  
  setupWebServer();
}

// ==================== NETWORK FUNCTIONS ====================
void setupNetwork() {
  WiFi.mode(config.create_ap ? WIFI_AP_STA : WIFI_STA);
  
  if (config.create_ap) {
    WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
    bool success;

    if (strlen(config.ap_password) >= 8) {
      success = WiFi.softAP(config.ap_name, config.ap_password);
    } else {
      success = WiFi.softAP(config.ap_name);
    }
    
    if (success) {
      Serial.printf("AP Deschis - Wif: %s, IP: %s\n", config.ap_name, WiFi.softAPIP().toString().c_str());
      displayMessage("AP DESCHIS", ("Wifi: " + String(config.ap_name) + "\nIP: " + WiFi.softAPIP().toString()).c_str());
    }
  } else if (strlen(config.wifi_ssid) > 0) {
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    Serial.print("Conectare la WiFi");
    displayMessage("CONECTARE LA", ("WiFi: " + String(config.wifi_ssid)).c_str());
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnectat la %s, IP: %s\n", config.wifi_ssid, WiFi.localIP().toString().c_str());
      displayMessage("WIFI CONECTAT", ("IP: " + WiFi.localIP().toString()).c_str());
      if (MDNS.begin(hostname)) {
        Serial.printf("mDNS started: http://%s.local\n", hostname);
      }
    } else {
      displayMessage("WIFI FAILED", "Connection failed");
    }
  }
}

// ==================== WEB SERVER SETUP ====================
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"(
<!DOCTYPE html> 
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Sensor Network</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh; padding: 20px; color: #333;
        }
        .container { 
            max-width: 1200px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            border-radius: 15px; padding: 30px; 
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
        }
        .header { 
            text-align: center; margin-bottom: 30px; 
            border-bottom: 2px solid #eee; padding-bottom: 20px;
        }
        .header h1 { 
            color: #4a5568; font-size: 2.5em; margin-bottom: 10px;
            background: linear-gradient(45deg, #667eea, #764ba2);
            -webkit-background-clip: text; -webkit-text-fill-color: transparent;
        }
        .status-bar { 
            display: flex; justify-content: space-between; 
            margin-bottom: 30px; flex-wrap: wrap; gap: 15px;
        }
        .status-item { 
            background: #f8f9fa; padding: 15px 20px; 
            border-radius: 10px; flex: 1; min-width: 200px;
            border-left: 4px solid #667eea;
        }
        .status-item h3 { color: #4a5568; margin-bottom: 5px; }
        .status-item span { font-size: 1.2em; font-weight: bold; color: #2d3748; }
        .grid { 
            display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); 
            gap: 20px; margin-bottom: 20px;
        }
        .device-card { 
            background: white; border-radius: 10px; padding: 20px; 
            border: 1px solid #e2e8f0; box-shadow: 0 4px 15px rgba(0,0,0,0.05);
            transition: transform 0.2s ease, box-shadow 0.2s ease;
        }
        .device-card:hover { 
            transform: translateY(-2px); 
            box-shadow: 0 8px 25px rgba(0,0,0,0.1);
        }
        .device-header { 
            display: flex; justify-content: space-between; 
            align-items: center; margin-bottom: 15px;
        }
        .device-name { font-size: 1.3em; font-weight: bold; color: #2d3748; }
        .device-status { 
            padding: 5px 12px; border-radius: 20px; 
            font-size: 0.9em; font-weight: bold;
        }
        .status-online { background: #c6f6d5; color: #22543d; }
        .status-offline { background: #fed7d7; color: #742a2a; }
        .sensor-data { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 15px; }
        .sensor-item { text-align: center; }
        .sensor-value { 
            font-size: 2em; font-weight: bold; 
            background: linear-gradient(45deg, #667eea, #764ba2);
            -webkit-background-clip: text; -webkit-text-fill-color: transparent;
        }
        .sensor-label { color: #718096; margin-top: 5px; }
        .connection-status { 
            position: fixed; top: 20px; right: 20px; 
            padding: 10px 15px; border-radius: 5px; 
            font-weight: bold; z-index: 1000;
        }
        .connected { background: #c6f6d5; color: #22543d; }
        .disconnected { background: #fed7d7; color: #742a2a; }
        @media (max-width: 768px) {
            .container { padding: 15px; }
            .status-bar { flex-direction: column; }
            .grid { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="connection-status" id="connectionStatus">Conectare...</div>
    <div class="container">
        <div class="header">
            <h1>🌐 Sistem Esp32</h1>
            <p>Bord Monitorizare in timp real</p>
        </div>
        
        <div class="status-bar">
            <div class="status-item">
                <h3>Status Network</h3>
                <span id="networkMode">Incarca...</span>
            </div>
            <div class="status-item">
                <h3>Module Active</h3>
                <span id="activeDevices">0/0</span>
            </div>
            <div class="status-item">
                <h3>Timp functionare</h3>
                <span id="uptime">0s</span>
            </div>
            <div class="status-item">
                <h3>Ultima actualizare</h3>
                <span id="lastUpdate">Never</span>
            </div>
        </div>
        
        <div class="grid" id="deviceGrid">
            <!-- Device cards will be populated by JavaScript -->
        </div>
    </div>

    <script>
        const ws = new WebSocket(`ws://${window.location.host}/ws`);
        const connectionStatus = document.getElementById('connectionStatus');
        
        ws.onopen = function() {
            connectionStatus.textContent = '🟢 Connectat';
            connectionStatus.className = 'connection-status connected';
            console.log('WebSocket connected');
        };
        
        ws.onclose = function() {
            connectionStatus.textContent = '🔴 Deconectat';
            connectionStatus.className = 'connection-status disconnected';
            console.log('WebSocket disconnected');
            // Auto-reconnect after 3 seconds
            setTimeout(() => location.reload(), 3000);
        };
        
        ws.onerror = function(error) {
            connectionStatus.textContent = '⚠️ Eroare';
            connectionStatus.className = 'connection-status disconnected';
            console.error('WebSocket error:', error);
        };
        
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                updateDashboard(data);
            } catch (e) {
                console.error('Failed to parse WebSocket message:', e);
            }
        };
        
        function updateDashboard(data) {
            document.getElementById('networkMode').textContent = data.networkMode || 'Unknown';
            document.getElementById('activeDevices').textContent = `${data.activeCount || 0}/${data.totalDevices || 0}`;
            document.getElementById('uptime').textContent = data.uptime || '0s';
            document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            
            const deviceGrid = document.getElementById('deviceGrid');
            deviceGrid.innerHTML = '';
            
            if (data.devices && data.devices.length > 0) {
                data.devices.forEach(device => {
                    const deviceCard = createDeviceCard(device);
                    deviceGrid.appendChild(deviceCard);
                });
            } else {
                deviceGrid.innerHTML = '<div style="text-align: center; color: #718096; grid-column: 1/-1;">No devices found</div>';
            }
        }
        
        function createDeviceCard(device) {
            const card = document.createElement('div');
            card.className = 'device-card';
            
            const isOnline = device.status === 'online';
            const statusClass = isOnline ? 'status-online' : 'status-offline';
            const statusText = isOnline ? '🟢 Online' : '🔴 Offline';
            
            let sensorData = '';
            if (device.moduleType === 1) {
                sensorData = `
                    <div class="sensor-item">
                        <div class="sensor-value">${device.temperature || '--'}°</div>
                        <div class="sensor-label">Temperatura</div>
                    </div>
                    <div class="sensor-item">
                        <div class="sensor-value">${device.humidity || '--'}%</div>
                        <div class="sensor-label">Umiditate</div>
                    </div>
                `;
            } else if (device.moduleType === 2) {
                sensorData = `
                    <div class="sensor-item">
                        <div class="sensor-value">${device.light || '--'}%</div>
                        <div class="sensor-label">Nivel Lumina</div>
                    </div>
                `;
            } else {
                sensorData = '<div style="text-align: center; color: #718096;">Fara senzori</div>';
            }
            
            card.innerHTML = `
                <div class="device-header">
                    <div class="device-name">${device.name || 'Unknown Device'}</div>
                    <div class="device-status ${statusClass}">${statusText}</div>
                </div>
                <div class="sensor-data">
                    ${sensorData}
                </div>
                ${device.lastSeen ? `<div style="text-align: center; color: #718096; margin-top: 10px; font-size: 0.9em;">Ultima actualizare: ${device.lastSeen}</div>` : ''}
            `;
            
            return card;
        }
        
        setInterval(() => {
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({type: 'ping'}));
            }
        }, 30000);
    </script>
</body>
</html>
)";
    

    if (setupMode) {
      html = getSetupPageHTML();
    }
    
    request->send(200, "text/html", html);
  });

  // API endpoints for AJAX calls (optional)
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(1024);
    
    doc["deviceId"] = DEVICE_ID;
    doc["isConfigured"] = config.configured;
    doc["networkMode"] = config.create_ap ? "Punct de Acces" : "Client WiFi";
    doc["uptime"] = millis();
    
    serializeJson(doc, *response);
    request->send(response);
  });

server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, 
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, (char*)data);
    
    strncpy(config.device_name, doc["deviceName"] | "Modul", sizeof(config.device_name) - 1);
    config.is_master = doc["isMaster"] | false;
    config.module_type = doc["moduleType"] | 0;
    
    if (config.is_master) {
      config.device_id = 1;
    } else {
      if (config.device_id == 0 || config.device_id == 1) {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        config.device_id = 2 + (mac[5] % 18);
      }
    }
    
    if (config.is_master) {
      String networkMode = doc["networkMode"] | "ap";
      config.create_ap = (networkMode == "ap");
      
      if (config.create_ap) {
        strncpy(config.ap_name, doc["apName"] | "SistemEsp32", sizeof(config.ap_name) - 1);
        strncpy(config.ap_password, doc["apPassword"] | "", sizeof(config.ap_password) - 1);
        strcpy(config.wifi_ssid, "");
        strcpy(config.wifi_password, "");
      } else {
        strncpy(config.wifi_ssid, doc["wifiSsid"] | "", sizeof(config.wifi_ssid) - 1);
        strncpy(config.wifi_password, doc["wifiPassword"] | "", sizeof(config.wifi_password) - 1);
      }
    } else {
      config.create_ap = false;
      strcpy(config.wifi_ssid, "");
      strcpy(config.wifi_password, "");
      strcpy(config.ap_name, "");
      strcpy(config.ap_password, "");
    }
    
    if (config.is_master) {
      String slaveMacs = doc["slaveMacs"] | "";
      config.slave_count = 0;
      
      int startPos = 0;
      while (startPos < slaveMacs.length() && config.slave_count < 10) {
        int endPos = slaveMacs.indexOf('\n', startPos);
        if (endPos == -1) endPos = slaveMacs.length();
        
        String macStr = slaveMacs.substring(startPos, endPos);
        macStr.trim();
        
        if (macStr.length() > 0 && stringToMac(macStr, config.slave_macs[config.slave_count])) {
          config.slave_count++;
        }
        
        startPos = endPos + 1;
      }
    } else {
      String masterMac = doc["masterMac"] | "";
      stringToMac(masterMac, config.master_mac);
    }
    
    config.configured = true;
    saveConfiguration();
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument responseDoc(256);
    responseDoc["success"] = true;
    responseDoc["deviceId"] = config.device_id;
    responseDoc["deviceName"] = config.device_name;
    serializeJson(responseDoc, *response);
    request->send(response);
    
    Serial.printf("COnfigurare salvata - Modul ID: %d, Name: %s\n", config.device_id, config.device_name);
    
    delay(1000);
    ESP.restart();
  });


  server.begin();
  Serial.println("Web server deschis");
}

// ==================== WEBSOCKET SETUP ====================
void setupWebSocket() {
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        dataChanged = true; // Force immediate update for new client
        break;
      case WS_EVT_DISCONNECT:
        Serial.printf("WebSocket client #%u disconnected\n", client->id());
        break;
      case WS_EVT_DATA:
        handleWebSocketMessage(client, (char*)data);
        break;
      case WS_EVT_ERROR:
        Serial.printf("WebSocket client #%u error\n", client->id());
        break;
    }
  });
  
  server.addHandler(&ws);
  Serial.println("WebSocket server started");
}

void sendWebSocketUpdate() {
  if (ws.count() == 0) return; // No clients connected
  
  DynamicJsonDocument doc(2048);
  
  // System status
  doc["networkMode"] = config.create_ap ? "Access Point" : (WiFi.status() == WL_CONNECTED ? "WiFi Client" : "Offline");
  doc["uptime"] = formatUptime(millis());
  
  // Count active devices
  int activeCount = 0;
  unsigned long currentTime = millis();
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
      activeCount++;
    }
  }
  
  doc["activeCount"] = activeCount;
  doc["totalDevices"] = config.slave_count + 1;
  
  JsonArray devices = doc.createNestedArray("devices");
  
  if (MODULE_TYPE > 0 || IS_MASTER) {
    JsonObject masterDevice = devices.createNestedObject();
    masterDevice["id"] = DEVICE_ID;
    // Use the configured device name
    masterDevice["name"] = String(config.device_name) + (IS_MASTER ? " (Lider)" : "");
    masterDevice["status"] = "online";
    masterDevice["moduleType"] = MODULE_TYPE;
    
    if (MODULE_TYPE == 1 && deviceReadings[DEVICE_ID - 1].temperature > -999) {
      masterDevice["temperature"] = deviceReadings[DEVICE_ID - 1].temperature;
      masterDevice["humidity"] = deviceReadings[DEVICE_ID - 1].humidity;
    } else if (MODULE_TYPE == 2 && deviceReadings[DEVICE_ID - 1].light_value > -999) {
      masterDevice["light"] = deviceReadings[DEVICE_ID - 1].light_value;
    }
  }
  
  for (int i = 0; i < MAX_PEERS; i++) {
    if (i == (DEVICE_ID - 1)) continue;
    
    bool isActive = deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT);
    if (!isActive && deviceReadings[i].sender_id == 0) continue;
    
    JsonObject device = devices.createNestedObject();
    device["id"] = i + 1;
    
    if (deviceReadings[i].sender_id > 0 && strlen(deviceReadings[i].device_name) > 0) {
      device["name"] = String(deviceReadings[i].device_name);
    } else {
      device["name"] = "Device " + String(i + 1);
    }
    
    device["status"] = isActive ? "online" : "offline";
    device["moduleType"] = deviceReadings[i].module_type;
    
    if (isActive && deviceReadings[i].module_type > 0) {
      if (deviceReadings[i].module_type == 1) {
        device["temperature"] = deviceReadings[i].temperature;
        device["humidity"] = deviceReadings[i].humidity;
      } else if (deviceReadings[i].module_type == 2) {
        device["light"] = deviceReadings[i].light_value;
      }
      device["lastSeen"] = getTimeAgo(deviceLastSeen[i]);
    }
  }
  
  String message;
  serializeJson(doc, message);
  ws.textAll(message);
}

void handleWebSocketMessage(AsyncWebSocketClient *client, char *data) {
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, data);
  
  if (error) {
    Serial.printf("WebSocket JSON parse error: %s\n", error.c_str());
    return;
  }
  
  String type = doc["type"].as<String>();
  
  if (type == "ping") {
    client->text("{\"type\":\"pong\"}");
    Serial.println("WebSocket ping received, pong sent");
  } 
  else if (type == "setInterval") {
    int newInterval = doc["value"].as<int>();
    if (newInterval >= 1 && newInterval <= 300) {
      currentUpdateInterval = newInterval;
      Serial.printf("Update interval changed to: %d seconds\n", currentUpdateInterval);
      
      DynamicJsonDocument response(256);
      response["type"] = "intervalUpdated";
      response["value"] = currentUpdateInterval;
      String responseStr;
      serializeJson(response, responseStr);
      client->text(responseStr);
    } else {
      Serial.printf("Invalid interval requested: %d\n", newInterval);
    }
  }
  else if (type == "getStatus") {
    dataChanged = true;
    Serial.println("Status update requested via WebSocket");
  }
  else if (type == "restart") {
    Serial.println("Remote restart requested via WebSocket");
    client->text("{\"type\":\"restarting\"}");
    delay(1000);
    ESP.restart();
  }
  else {
    Serial.printf("Unknown WebSocket message type: %s\n", type.c_str());
  }
}

// ==================== ESP-NOW FUNCTIONS ====================
void initESPNow() {
  if (!IS_MASTER) {
    WiFi.mode(WIFI_STA);
  }
  
  Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  registerPeers();
}

void registerPeers() {
  if (IS_MASTER) {
    for (int i = 0; i < config.slave_count; i++) {
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, config.slave_macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      
      if (esp_now_add_peer(&peerInfo) == ESP_OK) {
        Serial.printf("Master: Slave peer added - %s\n", macToString(config.slave_macs[i]).c_str());
      }
    }
  } else {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, config.master_mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.printf("Slave: Master peer added - %s\n", macToString(config.master_mac).c_str());
    }
  }
}

void getSensorReadings(float &temp, float &hum, float &light) {
  temp = hum = light = -999;
  
  switch (MODULE_TYPE) {
    case 1: // Temperature & Humidity
      temp = 25.0 + (random(-50, 51) / 10.0);
      hum = 50.0 + (random(-100, 101) / 10.0);
      Serial.printf("Temp: %.1f°C, Humidity: %.1f%%\n", temp, hum);
      break;
      
    case 2: // Light sensor
      light = (analogRead(A0) / 4095.0) * 100.0;
      Serial.printf("Light: %.1f%%\n", light);
      break;
  }
}

void sendReadings() {
  if (MODULE_TYPE == 0) return;
  
  sensor_message message = {
    DEVICE_ID, MODULE_TYPE,
    deviceReadings[DEVICE_ID - 1].temperature,
    deviceReadings[DEVICE_ID - 1].humidity,
    deviceReadings[DEVICE_ID - 1].light_value,
    millis()
  };
  
  // Copy device name into the message
  strncpy(message.device_name, config.device_name, sizeof(message.device_name) - 1);
  message.device_name[sizeof(message.device_name) - 1] = '\0';
  
  if (IS_MASTER) {
    Serial.println("Master: Not sending sensor data (no sensors)");
    return;
  }
  
  esp_err_t result = esp_now_send(config.master_mac, (uint8_t *)&message, sizeof(message));
  Serial.printf("Data sent to master: %s\n", result == ESP_OK ? "Success" : "Failed");
}

// Update handleReceivedData to store device names
void handleReceivedData(const sensor_message &reading) {
  if (reading.sender_id > 0 && reading.sender_id <= MAX_PEERS) {
    int idx = reading.sender_id - 1;
    deviceReadings[idx] = reading;
    deviceActive[idx] = true;
    deviceLastSeen[idx] = millis();
    
    Serial.printf("Received data from: %s (ID: %d)\n", reading.device_name, reading.sender_id);
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("Send status: %s\n", status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
}

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len == sizeof(sensor_message)) {
    sensor_message *msg = (sensor_message*)data;
    
    if (IS_MASTER) {
      handleReceivedData(*msg);
      dataChanged = true; // Trigger WebSocket update
      Serial.printf("Received from Device %d (%s): ", msg->sender_id, getModuleTypeName(msg->module_type).c_str());
      
      switch (msg->module_type) {
        case 1:
          Serial.printf("%.1f°C, %.1f%%\n", msg->temperature, msg->humidity);
          break;
        case 2:
          Serial.printf("Light: %.1f%%\n", msg->light_value);
          break;
      }
    }
  } else if (len == sizeof(command_message) && !IS_MASTER) {
    command_message *cmd = (command_message*)data;
    if (cmd->target_id == DEVICE_ID || cmd->target_id == 0) {
      handleReceivedCommand(*cmd);
    }
  }
}

void handleReceivedCommand(const command_message &command) {
  Serial.printf("Command %d received with parameter: %lu\n", command.command_type, command.parameter);
  
  switch (command.command_type) {
    case 1: // Update interval
      currentUpdateInterval = command.parameter;
      Serial.printf("Update interval: %d seconds\n", currentUpdateInterval);
      break;
    case 2: // Reset
      Serial.println("Reset command - restarting in 3s");
      delay(3000);
      ESP.restart();
      break;
  }
}

// ==================== BUTTON FUNCTIONS ====================
void checkSetupButton() {
  bool currentButtonState = digitalRead(SETUP_BUTTON_PIN);
  
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressTime = millis();
    Serial.println("Buton Reset Apasat - Apasa pentru 3s pentru reseta");
    displayMessage("RESET APASAT", "Tine 3s apasat", 1000);
  }
  
  if (currentButtonState == LOW && lastButtonState == LOW) {
    unsigned long holdTime = millis() - buttonPressTime;
    
    if (holdTime >= BUTTON_HOLD_TIME) {
      Serial.println("Reset Apasat 3s - reconfigurare...");
      displayMessage("ENTERING SETUP", "Reconfigurare...", 2000);
      
      config = DeviceConfig();
      saveConfiguration();
      
      Serial.println("Reconfigurare reusita - resetare inmediata");
      delay(1000);
      ESP.restart();
    }
  }
  
  if (currentButtonState == HIGH && lastButtonState == LOW) {
    unsigned long holdTime = millis() - buttonPressTime;
    if (holdTime < BUTTON_HOLD_TIME) {
      Serial.printf("Setup button released after %.1f seconds (need 3.0s)\n", holdTime / 1000.0);
    }
  }
  
  lastButtonState = currentButtonState;
}


// ==================== UTILITY FUNCTIONS ====================
String getModuleTypeName(uint8_t type) {
  switch (type) {
    case 0: return "Master/No Sensors";
    case 1: return "Temperature & Humidity";
    case 2: return "Light Sensor";
    default: return "Unknown";
  }
}

String macToString(const uint8_t* mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

bool stringToMac(const String& str, uint8_t* mac) {
  if (str.length() != 17) return false;
  
  for (int i = 0; i < 6; i++) {
    String byteStr = str.substring(i * 3, i * 3 + 2);
    char* endPtr;
    long val = strtol(byteStr.c_str(), &endPtr, 16);
    
    if (*endPtr != '\0' || val < 0 || val > 255) {
      return false;
    }
    
    mac[i] = (uint8_t)val;
    
    if (i < 5 && str.charAt(i * 3 + 2) != ':') {
      return false;
    }
  }
  
  return true;
}

String formatUptime(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;
  
  seconds %= 60;
  minutes %= 60;
  hours %= 24;
  
  String uptime = "";
  if (days > 0) uptime += String(days) + "d ";
  if (hours > 0) uptime += String(hours) + "h ";
  if (minutes > 0) uptime += String(minutes) + "m ";
  uptime += String(seconds) + "s";
  
  return uptime;
}

String getTimeAgo(unsigned long timestamp) {
  unsigned long elapsed = millis() - timestamp;
  
  if (elapsed < 1000) return "just now";
  if (elapsed < 60000) return String(elapsed / 1000) + "s ago";
  if (elapsed < 3600000) return String(elapsed / 60000) + "m ago";
  if (elapsed < 86400000) return String(elapsed / 3600000) + "h ago";
  return String(elapsed / 86400000) + "d ago";
}

// ==================== SETUP PAGE HTML ====================
String getSetupPageHTML() {
  return R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Device Setup</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh; padding: 20px; color: #333;
        }
        .container { 
            max-width: 600px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            border-radius: 15px; padding: 30px; 
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
        }
        .header { text-align: center; margin-bottom: 30px; }
        .header h1 { 
            color: #4a5568; font-size: 2.5em; margin-bottom: 10px;
            background: linear-gradient(45deg, #667eea, #764ba2);
            -webkit-background-clip: text; -webkit-text-fill-color: transparent;
        }
        .form-group { margin-bottom: 20px; }
        .form-group label { 
            display: block; margin-bottom: 8px; 
            font-weight: bold; color: #4a5568; 
        }
        .form-group input, .form-group select, .form-group textarea { 
            width: 100%; padding: 12px; border: 2px solid #e2e8f0; 
            border-radius: 8px; font-size: 16px;
            transition: border-color 0.2s ease;
        }
        .form-group input:focus, .form-group select:focus, .form-group textarea:focus { 
            outline: none; border-color: #667eea; 
        }
        .form-group textarea { 
            resize: vertical; min-height: 100px; 
            font-family: monospace;
        }
        .checkbox-group { 
            display: flex; align-items: center; gap: 10px; 
        }
        .checkbox-group input[type="checkbox"] { 
            width: auto; transform: scale(1.2); 
        }
        .btn { 
            background: linear-gradient(45deg, #667eea, #764ba2); 
            color: white; border: none; padding: 15px 30px; 
            border-radius: 8px; font-size: 16px; font-weight: bold; 
            cursor: pointer; width: 100%; margin-top: 10px;
            transition: transform 0.2s ease;
        }
        .btn:hover { transform: translateY(-2px); }
        .btn:active { transform: translateY(0); }
        .info-box { 
            background: #e6fffa; border: 1px solid #81e6d9; 
            border-radius: 8px; padding: 15px; margin-bottom: 20px;
            color: #234e52;
        }
        .info-box h3 { margin-bottom: 10px; color: #1a202c; }
        .conditional { display: none; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🛠️ Configurare Modul</h1>
            <p>Configura modulul Esp32</p>
        </div>
        
        <div class="info-box">
            <h3>📱 Info Modul Curent</h3>
            <p><strong>MAC Address:</strong> )" + WiFi.macAddress() + R"(</p>
            <p><strong>Setup IP:</strong> )" + WiFi.softAPIP().toString() + R"(</p>
        </div>
        
        <form id="setupForm">
            <div class="form-group">
                <label for="deviceName">Nume Modul</label>
                <input type="text" id="deviceName" name="deviceName" placeholder="e.g., Kitchen Sensor" required>
            </div>
            
            <div class="form-group">
                <div class="checkbox-group">
                    <input type="checkbox" id="isMaster" name="isMaster">
                    <label for="isMaster">Modul Lider (primeste date)</label>
                </div>
            </div>
            
            <div class="form-group">
                <label for="moduleType">Tip Modul</label>
                <select id="moduleType" name="moduleType">
                    <option value="0">Fara Senzori (Numai pt Lider)</option>
                    <option value="1">Temperatura si Umiditate</option>
                    <option value="2">Nivel Luminos</option>
                </select>
            </div>
            
            <div class="form-group conditional" id="masterSection">
                <label for="slaveMacs">Adrese MAC Module (una per linie)</label>
                <textarea id="slaveMacs" name="slaveMacs" placeholder="AA:BB:CC:DD:EE:F1&#10;AA:BB:CC:DD:EE:F2&#10;AA:BB:CC:DD:EE:F3"></textarea>
                <small>Adauga toate adresele MAC ale modulelor dorite.</small>
            </div>
            
            <div class="form-group conditional" id="slaveSection">
                <label for="masterMac">Adresa MAC a Liderului</label>
                <input type="text" id="masterMac" name="masterMac" placeholder="AA:BB:CC:DD:EE:FF">
                <small>Adauga adresa MAC a modului Lider/small>
            </div>
            
            <div class="form-group conditional" id="networkSection">
                <label for="networkMode">Network Mode</label>
                <select id="networkMode" name="networkMode">
                    <option value="ap">Creare Punct Acces</option>
                    <option value="wifi">Conecteaza la un WiFI existent</option>
                </select>
            </div>
            
            <div class="form-group conditional" id="apSection">
                <label for="apName">Nume Punct Acces</label>
                <input type="text" id="apName" name="apName" placeholder="SistemEsp32" value="SistemEsp32">
                <label for="apPassword" style="margin-top: 10px;">Parola Punct Acces (min 8 caratere)</label>
                <input type="password" id="apPassword" name="apPassword" placeholder="parola_test" value="parola_test" minlength="8">
                <small>Lasa liber pentru sistem deschis (nerecomandat)</small>
            </div>
            
            <div class="form-group conditional" id="wifiSection">
                <label for="wifiSsid">Nume WiFi Network</label>
                <input type="text" id="wifiSsid" name="wifiSsid" placeholder="Your WiFi network name">
                <label for="wifiPassword" style="margin-top: 10px;">Parola WiFi</label>
                <input type="password" id="wifiPassword" name="wifiPassword" placeholder="Your WiFi password">
            </div>
            
            <button type="submit" class="btn">💾 Salveaza si redeschide</button>
        </form>
    </div>

    <script>
        const isMasterCheckbox = document.getElementById('isMaster');
        const masterSection = document.getElementById('masterSection');
        const slaveSection = document.getElementById('slaveSection');
        const networkSection = document.getElementById('networkSection');
        const networkMode = document.getElementById('networkMode');
        const apSection = document.getElementById('apSection');
        const wifiSection = document.getElementById('wifiSection');
        
        function toggleSections() {
            if (isMasterCheckbox.checked) {
                masterSection.style.display = 'block';
                slaveSection.style.display = 'none';
                networkSection.style.display = 'block';
                toggleNetworkMode();
            } else {
                masterSection.style.display = 'none';
                slaveSection.style.display = 'block';
                networkSection.style.display = 'none';
                apSection.style.display = 'none';
                wifiSection.style.display = 'none';
            }
        }
        
        function toggleNetworkMode() {
            if (networkMode.value === 'ap') {
                apSection.style.display = 'block';
                wifiSection.style.display = 'none';
            } else {
                apSection.style.display = 'none';
                wifiSection.style.display = 'block';
            }
        }
        
        isMasterCheckbox.addEventListener('change', toggleSections);
        networkMode.addEventListener('change', toggleNetworkMode);
        toggleSections(); // Initial setup
        
        document.getElementById('setupForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const formData = new FormData(this);
            const data = {
                deviceName: formData.get('deviceName'),
                isMaster: isMasterCheckbox.checked,
                moduleType: parseInt(formData.get('moduleType')),
                slaveMacs: formData.get('slaveMacs'),
                masterMac: formData.get('masterMac'),
                networkMode: formData.get('networkMode'),
                apName: formData.get('apName'),
                apPassword: formData.get('apPassword'),
                wifiSsid: formData.get('wifiSsid'),
                wifiPassword: formData.get('wifiPassword')
            };
            
            fetch('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(data)
            })
            .then(response => response.json())
            .then(result => {
                if (result.success) {
                    alert('Configuration saved! Device will restart in 3 seconds.');
                    setTimeout(() => location.reload(), 3000);
                } else {
                    alert('Error: ' + (result.error || 'Failed to save configuration'));
                }
            })
            .catch(error => {
                console.error('Error:', error);
                alert('Failed to save configuration');
            });
        });
    </script>
</body>
</html>
)";
}