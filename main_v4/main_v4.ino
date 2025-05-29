#include <esp_now.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ESPmDNS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// ==================== CONFIGURATION ====================
// These will be loaded from EEPROM or set during first-time setup
bool IS_MASTER = true;
bool CREATE_AP = true;
uint8_t DEVICE_ID = 1;
uint8_t MODULE_TYPE = 0;

// Hardware setup button pin
#define SETUP_BUTTON_PIN 0
bool lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
const unsigned long BUTTON_HOLD_TIME = 3000;

// Configuration structure for EEPROM storage
struct DeviceConfig {
  bool configured = false;
  bool is_master = true;
  bool create_ap = true;
  uint8_t device_id = 1;
  uint8_t module_type = 0;
  char device_name[32] = "Device";
  uint8_t master_mac[6] = {0};

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
            <h1>🛠️ Device Setup</h1>
            <p>Configure your ESP32 sensor device</p>
        </div>
        
        <div class="info-box">
            <h3>📱 Current Device Info</h3>
            <p><strong>MAC Address:</strong> )" + WiFi.macAddress() + R"(</p>
            <p><strong>Setup IP:</strong> )" + WiFi.softAPIP().toString() + R"(</p>
        </div>
        
        <form id="setupForm">
            <div class="form-group">
                <label for="deviceName">Device Name</label>
                <input type="text" id="deviceName" name="deviceName" placeholder="e.g., Kitchen Sensor" required>
            </div>
            
            <div class="form-group">
                <div class="checkbox-group">
                    <input type="checkbox" id="isMaster" name="isMaster">
                    <label for="isMaster">This device is a Master (receives data from other devices)</label>
                </div>
            </div>
            
            <div class="form-group">
                <label for="moduleType">Module Type</label>
                <select id="moduleType" name="moduleType">
                    <option value="0">No Sensors (Master only)</option>
                    <option value="1">Temperature & Humidity Sensor</option>
                    <option value="2">Light Sensor</option>
                </select>
            </div>
            
            <div class="form-group conditional" id="masterSection">
                <label for="slaveMacs">Slave Device MAC Addresses (one per line)</label>
                <textarea id="slaveMacs" name="slaveMacs" placeholder="AA:BB:CC:DD:EE:F1&#10;AA:BB:CC:DD:EE:F2&#10;AA:BB:CC:DD:EE:F3"></textarea>
                <small>Enter the MAC addresses of all slave devices</small>
            </div>
            
            <div class="form-group conditional" id="slaveSection">
                <label for="masterMac">Master Device MAC Address</label>
                <input type="text" id="masterMac" name="masterMac" placeholder="AA:BB:CC:DD:EE:FF">
                <small>Enter the MAC address of the master device</small>
            </div>
            
            <div class="form-group">
                <label for="wifiSsid">WiFi Network Name (optional)</label>
                <input type="text" id="wifiSsid" name="wifiSsid" placeholder="Your WiFi network name">
            </div>
            
            <div class="form-group">
                <label for="wifiPassword">WiFi Password (optional)</label>
                <input type="password" id="wifiPassword" name="wifiPassword" placeholder="Your WiFi password">
            </div>
            
            <button type="submit" class="btn">💾 Save Configuration & Restart</button>
        </form>
    </div>

    <script>
        const isMasterCheckbox = document.getElementById('isMaster');
        const masterSection = document.getElementById('masterSection');
        const slaveSection = document.getElementById('slaveSection');
        
        function toggleSections() {
            if (isMasterCheckbox.checked) {
                masterSection.style.display = 'block';
                slaveSection.style.display = 'none';
            } else {
                masterSection.style.display = 'none';
                slaveSection.style.display = 'block';
            }
        }
        
        isMasterCheckbox.addEventListener('change', toggleSections);
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
};
  uint8_t slave_macs[10][6] = {0};
  uint8_t slave_count = 0;
  char wifi_ssid[64] = "";
  char wifi_password[64] = "";
  uint32_t checksum = 0;
};

DeviceConfig config;
bool setupMode = false;

// Timing constants
const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long WS_UPDATE_INTERVAL = 2000; // Fast WebSocket updates
const unsigned long DEVICE_TIMEOUT = 5 * 60 * 1000;

// ==================== WIFI & AP SETTINGS ====================
const char* ap_ssid = "ESP32-SensorHub";
const char* ap_password = "sensornetwork";
const char* hostname = "esp32-sensor-hub";

const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

// ==================== ESP-NOW SETTINGS ====================
#define MAX_PEERS 20

// ==================== DATA STRUCTURES ====================
// Enhanced data structure that includes device name
typedef struct {
  uint8_t sender_id;
  uint8_t module_type;
  float temperature;
  float humidity;
  float light_value;
  unsigned long timestamp;
  char device_name[32];  // Add device name to the message
} sensor_message;

typedef struct {
  uint8_t sender_id;
  uint8_t target_id;
  uint8_t command_type;
  uint32_t parameter;
  unsigned long timestamp;
} command_message;

// Global data arrays
sensor_message deviceReadings[MAX_PEERS];
bool deviceActive[MAX_PEERS] = {false};
unsigned long deviceLastSeen[MAX_PEERS] = {0};

// Web server and WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Update control
bool dataChanged = false;
unsigned long lastWSUpdate = 0;
uint16_t currentUpdateInterval = UPDATE_INTERVAL / 1000;

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

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP32 Lightweight Sensor Network Starting...");

  EEPROM.begin(1024);
  loadConfiguration();
  
  if (!config.configured) {
    Serial.println("First-time setup required - entering setup mode");
    enterSetupMode();
    return;
  }
  
  // Apply loaded configuration
  IS_MASTER = config.is_master;
  CREATE_AP = config.create_ap;
  DEVICE_ID = config.device_id;
  MODULE_TYPE = config.module_type;
  
  Serial.printf("Configuration loaded - %s, ID: %d, Module: %s\n", 
                IS_MASTER ? "MASTER" : "SLAVE", DEVICE_ID, getModuleTypeName(MODULE_TYPE).c_str());

  // Initialize device arrays
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
}

// ==================== MAIN LOOP ====================
void loop() {
  checkSetupButton();
  
  if (setupMode) {
    delay(10);
    return;
  }
  
  static unsigned long lastSendTime = 0;
  unsigned long currentTime = millis();
  
  // Send sensor data if this device has sensors
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
  
  // WebSocket updates (fast and lightweight)
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
    Serial.println("Configuration checksum mismatch - using defaults");
    config = DeviceConfig();
  } else {
    Serial.println("Configuration loaded successfully");
  }
}

void saveConfiguration() {
  config.checksum = calculateChecksum(config);
  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println("Configuration saved to EEPROM");
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
  WiFi.softAP("ESP32-Setup", "12345678");
  
  Serial.println("Setup AP started - SSID: ESP32-Setup, Password: 12345678");
  Serial.print("Setup URL: http://");
  Serial.println(WiFi.softAPIP());
  
  setupWebServer(); // Setup web server for configuration
}

// ==================== NETWORK FUNCTIONS ====================
void setupNetwork() {
  WiFi.mode(config.create_ap ? WIFI_AP_STA : WIFI_STA);
  
  if (config.create_ap) {
    WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
    bool success = (strlen(ap_password) >= 8) ? 
                   WiFi.softAP(ap_ssid, ap_password) : 
                   WiFi.softAP(ap_ssid);
    
    if (success) {
      Serial.printf("AP started - SSID: %s, IP: %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
    }
  } else if (strlen(config.wifi_ssid) > 0) {
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    Serial.print("Connecting to WiFi");
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s, IP: %s\n", config.wifi_ssid, WiFi.localIP().toString().c_str());
      if (MDNS.begin(hostname)) {
        Serial.printf("mDNS started: http://%s.local\n", hostname);
      }
    }
  }
}

// ==================== WEB SERVER SETUP ====================
void setupWebServer() {
  // Serve main page
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
    <div class="connection-status" id="connectionStatus">Connecting...</div>
    <div class="container">
        <div class="header">
            <h1>🌐 ESP32 Sensor Network</h1>
            <p>Real-time monitoring dashboard</p>
        </div>
        
        <div class="status-bar">
            <div class="status-item">
                <h3>Network Status</h3>
                <span id="networkMode">Loading...</span>
            </div>
            <div class="status-item">
                <h3>Active Devices</h3>
                <span id="activeDevices">0/0</span>
            </div>
            <div class="status-item">
                <h3>Uptime</h3>
                <span id="uptime">0s</span>
            </div>
            <div class="status-item">
                <h3>Last Update</h3>
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
            connectionStatus.textContent = '🟢 Connected';
            connectionStatus.className = 'connection-status connected';
            console.log('WebSocket connected');
        };
        
        ws.onclose = function() {
            connectionStatus.textContent = '🔴 Disconnected';
            connectionStatus.className = 'connection-status disconnected';
            console.log('WebSocket disconnected');
            // Auto-reconnect after 3 seconds
            setTimeout(() => location.reload(), 3000);
        };
        
        ws.onerror = function(error) {
            connectionStatus.textContent = '⚠️ Error';
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
            // Update status bar
            document.getElementById('networkMode').textContent = data.networkMode || 'Unknown';
            document.getElementById('activeDevices').textContent = `${data.activeCount || 0}/${data.totalDevices || 0}`;
            document.getElementById('uptime').textContent = data.uptime || '0s';
            document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            
            // Update device grid
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
                        <div class="sensor-label">Temperature</div>
                    </div>
                    <div class="sensor-item">
                        <div class="sensor-value">${device.humidity || '--'}%</div>
                        <div class="sensor-label">Humidity</div>
                    </div>
                `;
            } else if (device.moduleType === 2) {
                sensorData = `
                    <div class="sensor-item">
                        <div class="sensor-value">${device.light || '--'}%</div>
                        <div class="sensor-label">Light Level</div>
                    </div>
                `;
            } else {
                sensorData = '<div style="text-align: center; color: #718096;">No sensors</div>';
            }
            
            card.innerHTML = `
                <div class="device-header">
                    <div class="device-name">${device.name || 'Unknown Device'}</div>
                    <div class="device-status ${statusClass}">${statusText}</div>
                </div>
                <div class="sensor-data">
                    ${sensorData}
                </div>
                ${device.lastSeen ? `<div style="text-align: center; color: #718096; margin-top: 10px; font-size: 0.9em;">Last seen: ${device.lastSeen}</div>` : ''}
            `;
            
            return card;
        }
        
        // Send ping every 30 seconds to keep connection alive
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
      // In setup mode, serve configuration page instead
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
    doc["networkMode"] = config.create_ap ? "Access Point" : "WiFi Client";
    doc["uptime"] = millis();
    
    serializeJson(doc, *response);
    request->send(response);
  });

// Configuration API endpoint
server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, 
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, (char*)data);
    
    // Parse and save configuration
    strncpy(config.device_name, doc["deviceName"] | "Device", sizeof(config.device_name) - 1);
    config.is_master = doc["isMaster"] | false;
    config.module_type = doc["moduleType"] | 0;
    
    // IMPROVED DEVICE ID ASSIGNMENT
    if (config.is_master) {
      config.device_id = 1;  // Master is always ID 1
    } else {
      // For slaves, either keep existing ID or assign a new one
      if (config.device_id == 0 || config.device_id == 1) {
        // Generate a unique device ID based on MAC address
        uint8_t mac[6];
        WiFi.macAddress(mac);
        config.device_id = 2 + (mac[5] % 18);  // IDs 2-19 for slaves
      }
      // If device already has a valid slave ID (2-20), keep it
    }
    
    // Parse WiFi settings
    strncpy(config.wifi_ssid, doc["wifiSsid"] | "", sizeof(config.wifi_ssid) - 1);
    strncpy(config.wifi_password, doc["wifiPassword"] | "", sizeof(config.wifi_password) - 1);
    config.create_ap = (strlen(config.wifi_ssid) == 0);
    
    // Parse MAC addresses
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
    
    // Send success response with assigned device ID
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument responseDoc(256);
    responseDoc["success"] = true;
    responseDoc["deviceId"] = config.device_id;
    responseDoc["deviceName"] = config.device_name;
    serializeJson(responseDoc, *response);
    request->send(response);
    
    Serial.printf("Configuration saved - Device ID: %d, Name: %s\n", config.device_id, config.device_name);
    
    // Restart after a delay
    delay(1000);
    ESP.restart();
  });


  server.begin();
  Serial.println("Web server started");
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
  
  // Device data
  JsonArray devices = doc.createNestedArray("devices");
  
  // Add this device (master) if it has sensors OR if it's the master
  if (MODULE_TYPE > 0 || IS_MASTER) {
    JsonObject masterDevice = devices.createNestedObject();
    masterDevice["id"] = DEVICE_ID;
    // Use the configured device name
    masterDevice["name"] = String(config.device_name) + (IS_MASTER ? " (Master)" : "");
    masterDevice["status"] = "online";
    masterDevice["moduleType"] = MODULE_TYPE;
    
    if (MODULE_TYPE == 1 && deviceReadings[DEVICE_ID - 1].temperature > -999) {
      masterDevice["temperature"] = deviceReadings[DEVICE_ID - 1].temperature;
      masterDevice["humidity"] = deviceReadings[DEVICE_ID - 1].humidity;
    } else if (MODULE_TYPE == 2 && deviceReadings[DEVICE_ID - 1].light_value > -999) {
      masterDevice["light"] = deviceReadings[DEVICE_ID - 1].light_value;
    }
  }
  
  // Add slave devices
  for (int i = 0; i < MAX_PEERS; i++) {
    if (i == (DEVICE_ID - 1)) continue; // Skip this device
    
    bool isActive = deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT);
    if (!isActive && deviceReadings[i].sender_id == 0) continue; // Skip never-seen devices
    
    JsonObject device = devices.createNestedObject();
    device["id"] = i + 1;
    
    // Try to get a meaningful device name
    if (deviceReadings[i].sender_id > 0) {
      device["name"] = "Slave Device " + String(i + 1);
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
    // Respond to ping with pong
    client->text("{\"type\":\"pong\"}");
    Serial.println("WebSocket ping received, pong sent");
  } 
  else if (type == "setInterval") {
    int newInterval = doc["value"].as<int>();
    if (newInterval >= 1 && newInterval <= 300) { // 1 second to 5 minutes
      currentUpdateInterval = newInterval;
      Serial.printf("Update interval changed to: %d seconds\n", currentUpdateInterval);
      
      // Send confirmation back to client
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
    // Force immediate status update for requesting client
    dataChanged = true;
    Serial.println("Status update requested via WebSocket");
  }
  else if (type == "restart") {
    // Remote restart command (be careful with this!)
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
    Serial.println("Setup button pressed - hold for 3 seconds to enter setup mode");
  }
  
  if (currentButtonState == LOW && lastButtonState == LOW) {
    unsigned long holdTime = millis() - buttonPressTime;
    
    if (holdTime >= BUTTON_HOLD_TIME) {
      Serial.println("Setup button held for 3 seconds - entering setup mode");
      
      config = DeviceConfig();
      saveConfiguration();
      
      Serial.println("Configuration cleared - restarting in setup mode");
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
            <h1>🛠️ Device Setup</h1>
            <p>Configure your ESP32 sensor device</p>
        </div>
        
        <div class="info-box">
            <h3>📱 Current Device Info</h3>
            <p><strong>MAC Address:</strong> )" + WiFi.macAddress() + R"(</p>
            <p><strong>Setup IP:</strong> )" + WiFi.softAPIP().toString() + R"(</p>
        </div>
        
        <form id="setupForm">
            <div class="form-group">
                <label for="deviceName">Device Name</label>
                <input type="text" id="deviceName" name="deviceName" placeholder="e.g., Kitchen Sensor" required>
            </div>
            
            <div class="form-group">
                <div class="checkbox-group">
                    <input type="checkbox" id="isMaster" name="isMaster">
                    <label for="isMaster">This device is a Master (receives data from other devices)</label>
                </div>
            </div>
            
            <div class="form-group">
                <label for="moduleType">Module Type</label>
                <select id="moduleType" name="moduleType">
                    <option value="0">No Sensors (Master only)</option>
                    <option value="1">Temperature & Humidity Sensor</option>
                    <option value="2">Light Sensor</option>
                </select>
            </div>
            
            <div class="form-group conditional" id="masterSection">
                <label for="slaveMacs">Slave Device MAC Addresses (one per line)</label>
                <textarea id="slaveMacs" name="slaveMacs" placeholder="AA:BB:CC:DD:EE:F1&#10;AA:BB:CC:DD:EE:F2&#10;AA:BB:CC:DD:EE:F3"></textarea>
                <small>Enter the MAC addresses of all slave devices</small>
            </div>
            
            <div class="form-group conditional" id="slaveSection">
                <label for="masterMac">Master Device MAC Address</label>
                <input type="text" id="masterMac" name="masterMac" placeholder="AA:BB:CC:DD:EE:FF">
                <small>Enter the MAC address of the master device</small>
            </div>
            
            <div class="form-group">
                <label for="wifiSsid">WiFi Network Name (optional)</label>
                <input type="text" id="wifiSsid" name="wifiSsid" placeholder="Your WiFi network name">
            </div>
            
            <div class="form-group">
                <label for="wifiPassword">WiFi Password (optional)</label>
                <input type="password" id="wifiPassword" name="wifiPassword" placeholder="Your WiFi password">
            </div>
            
            <button type="submit" class="btn">💾 Save Configuration & Restart</button>
        </form>
    </div>

    <script>
        const isMasterCheckbox = document.getElementById('isMaster');
        const masterSection = document.getElementById('masterSection');
        const slaveSection = document.getElementById('slaveSection');
        
        function toggleSections() {
            if (isMasterCheckbox.checked) {
                masterSection.style.display = 'block';
                slaveSection.style.display = 'none';
            } else {
                masterSection.style.display = 'none';
                slaveSection.style.display = 'block';
            }
        }
        
        isMasterCheckbox.addEventListener('change', toggleSections);
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