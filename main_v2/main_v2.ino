#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPUI.h>
#include <ESPmDNS.h>

// ==================== CONFIGURATION ====================
const bool IS_MASTER = true;
const bool CREATE_AP = true;
const uint8_t DEVICE_ID = 1;
const uint8_t MODULE_TYPE = 0;

// Timing constants
const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;
const unsigned long UI_UPDATE_INTERVAL = 5000; // Reduced frequency to prevent flickering
const unsigned long DEVICE_TIMEOUT = 5 * 60 * 1000; // 5 minutes

// ==================== WIFI & AP SETTINGS ====================
const char* wifi_ssid = "YOUR_WIFI_SSID";
const char* wifi_password = "YOUR_WIFI_PASSWORD";
const char* ap_ssid = "ESP32-SensorHub";
const char* ap_password = "sensornetwork";
const char* hostname = "esp32-sensor-hub";

const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

// ==================== DISPLAY SETTINGS ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306* displayPtr = nullptr;

// ==================== ESP-NOW SETTINGS ====================
#define MAX_PEERS 20

typedef struct {
  uint8_t id;
  uint8_t address[6];
  char name[32];
  uint8_t module_type;
} device_info;

device_info devices[] = {
  {1, {0xd4, 0x8a, 0xfc, 0x9f, 0x2f, 0x98}, "Master", 0},
  {2, {0x94, 0xb5, 0x55, 0xf9, 0xff, 0xf0}, "Outdoor", 1},
  {3, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, "Indoor", 2}
};
const int NUM_DEVICES = sizeof(devices) / sizeof(device_info);

// ==================== DATA STRUCTURES ====================
typedef struct {
  uint8_t sender_id;
  uint8_t module_type;
  float temperature;
  float humidity;
  float light_value;
  unsigned long timestamp;
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

// UI update control
bool uiUpdateNeeded = false;
unsigned long lastUIDataChange = 0;
unsigned long refreshButtonResetTime = 0;

// ==================== ESPUI ELEMENTS ====================
uint16_t activeDevicesLabel, uptimeLabel, networkModeLabel, ipAddressLabel;
uint16_t deviceStatusLabels[NUM_DEVICES], deviceLastSeenLabels[NUM_DEVICES];
uint16_t temperatureLabels[NUM_DEVICES], humidityLabels[NUM_DEVICES], lightLabels[NUM_DEVICES];
uint16_t refreshButton, updateIntervalSlider;
uint16_t currentUpdateInterval = UPDATE_INTERVAL / 1000;

// ==================== FUNCTION DECLARATIONS ====================
void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &light);
void sendReadings();
void updateDisplay();
void handleReceivedData(const sensor_message &reading);
void handleReceivedCommand(const command_message &command);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

void setupNetwork();
void initESPUI();
void updateUI();
String getDeviceName(uint8_t id);
String getModuleTypeName(uint8_t type);
String getTimeAgo(unsigned long timestamp);
String formatUptime(unsigned long ms);

// ESPUI Callbacks
void refreshButtonCallback(Control* sender, int type);
void updateIntervalCallback(Control* sender, int type);

// Global variables for button reset
extern unsigned long refreshButtonResetTime;

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.printf("Initializing %s - Device ID: %d, Module Type: %s\n", 
                IS_MASTER ? "MASTER" : "SLAVE", DEVICE_ID, getModuleTypeName(MODULE_TYPE).c_str());

  // Initialize device arrays
  for (int i = 0; i < MAX_PEERS; i++) {
    deviceReadings[i] = {0, 0, -999, -999, -999, 0};
  }

  // Set this device's own data
  deviceReadings[DEVICE_ID - 1] = {DEVICE_ID, MODULE_TYPE, -999, -999, -999, millis()};
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();

  Wire.begin();
  
  if (IS_MASTER) {
    // Initialize display
    displayPtr = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    if (displayPtr->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
      displayPtr->clearDisplay();
      displayPtr->setTextSize(1);
      displayPtr->setTextColor(WHITE);
      displayPtr->setCursor(0, 0);
      displayPtr->println("ESP32 Master");
      displayPtr->println("Initializing...");
      displayPtr->display();
    } else {
      Serial.println("Display initialization failed");
      delete displayPtr;
      displayPtr = nullptr;
    }
    
    setupNetwork();
    initESPUI();
  }

  initESPNow();
}

// ==================== MAIN LOOP ====================
void loop() {
  static unsigned long lastSendTime = 0;
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastUIUpdate = 0;
  static unsigned long refreshButtonResetTime = 0;
  
  unsigned long currentTime = millis();
  
  // Send sensor data if this device has sensors
  if (currentTime - lastSendTime >= (currentUpdateInterval * 1000) && MODULE_TYPE > 0) {
    float temp, hum, light;
    getSensorReadings(temp, hum, light);
    
    // Update local readings
    int idx = DEVICE_ID - 1;
    deviceReadings[idx].temperature = temp;
    deviceReadings[idx].humidity = hum;
    deviceReadings[idx].light_value = light;
    deviceReadings[idx].timestamp = currentTime;
    deviceLastSeen[idx] = currentTime;
    
    sendReadings();
    lastSendTime = currentTime;
  }
  
  // Master-specific updates
  if (IS_MASTER) {
    if (currentTime - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
      updateDisplay();
      lastDisplayUpdate = currentTime;
    }
    
    // Only update UI when needed and not too frequently
    if ((currentTime - lastUIUpdate >= UI_UPDATE_INTERVAL) && 
        (uiUpdateNeeded || (currentTime - lastUIDataChange >= UI_UPDATE_INTERVAL))) {
      updateUI();
      lastUIUpdate = currentTime;
      uiUpdateNeeded = false;
    }
    
    // Reset refresh button text after delay
    if (refreshButtonResetTime > 0 && currentTime >= refreshButtonResetTime) {
      ESPUI.updateButton(refreshButton, "Refresh");
      refreshButtonResetTime = 0;
    }
  }
  
  delay(10);
}

// ==================== NETWORK FUNCTIONS ====================
void setupNetwork() {
  WiFi.mode(CREATE_AP ? WIFI_AP_STA : WIFI_STA);
  
  if (CREATE_AP) {
    WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
    bool success = (strlen(ap_password) >= 8) ? 
                   WiFi.softAP(ap_ssid, ap_password) : 
                   WiFi.softAP(ap_ssid);
    
    if (success) {
      Serial.printf("AP started - SSID: %s, IP: %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
      if (displayPtr) {
        displayPtr->clearDisplay();
        displayPtr->setCursor(0, 0);
        displayPtr->printf("AP: %s\nIP: %s", ap_ssid, WiFi.softAPIP().toString().c_str());
        displayPtr->display();
        delay(2000);
      }
    } else {
      Serial.println("AP start failed");
    }
  } else {
    WiFi.begin(wifi_ssid, wifi_password);
    Serial.print("Connecting to WiFi");
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s, IP: %s\n", wifi_ssid, WiFi.localIP().toString().c_str());
      if (MDNS.begin(hostname)) {
        Serial.printf("mDNS started: http://%s.local\n", hostname);
      }
    } else {
      Serial.println("\nWiFi connection failed");
    }
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
  for (int i = 0; i < NUM_DEVICES; i++) {
    // Master adds all slaves, slaves add only master
    if ((IS_MASTER && devices[i].id == DEVICE_ID) || 
        (!IS_MASTER && devices[i].id != 1)) {
      continue;
    }
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, devices[i].address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.printf("Peer added: Device %d (%s)\n", devices[i].id, devices[i].name);
    } else {
      Serial.printf("Failed to add peer: Device %d\n", devices[i].id);
    }
  }
}

// ==================== SENSOR FUNCTIONS ====================
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
  
  if (IS_MASTER) {
    Serial.println("Master: Not sending sensor data (no sensors)");
    return;
  }
  
  // Slave sends to master only
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == 1) {
      esp_err_t result = esp_now_send(devices[i].address, (uint8_t *)&message, sizeof(message));
      Serial.printf("Data sent to master: %s\n", result == ESP_OK ? "Success" : "Failed");
      break;
    }
  }
}

// ==================== CALLBACK FUNCTIONS ====================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("Send status: %s\n", status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
}

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len == sizeof(sensor_message)) {
    sensor_message *msg = (sensor_message*)data;
    
    if (IS_MASTER) {
      handleReceivedData(*msg);
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

void handleReceivedData(const sensor_message &reading) {
  if (reading.sender_id > 0 && reading.sender_id <= MAX_PEERS) {
    int idx = reading.sender_id - 1;
    
    // Check if this is actually new data
    bool isNewData = (deviceReadings[idx].timestamp != reading.timestamp) ||
                     (deviceReadings[idx].temperature != reading.temperature) ||
                     (deviceReadings[idx].humidity != reading.humidity) ||
                     (deviceReadings[idx].light_value != reading.light_value);
    
    deviceReadings[idx] = reading;
    deviceActive[idx] = true;
    deviceLastSeen[idx] = millis();
    
    // Mark UI update needed only if data actually changed
    if (isNewData) {
      uiUpdateNeeded = true;
      lastUIDataChange = millis();
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

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplay() {
  if (!IS_MASTER || !displayPtr) return;
  
  int activeCount = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && (millis() - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
      activeCount++;
    }
  }
  
  displayPtr->clearDisplay();
  displayPtr->setCursor(0, 0);
  displayPtr->printf("ESP32 Network - %d\n", DEVICE_ID);
  displayPtr->printf("Active: %d/%d\n", activeCount, NUM_DEVICES);
  
  if (CREATE_AP) {
    displayPtr->printf("AP: %s\n%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    displayPtr->printf("WiFi: %s\n%s\n", wifi_ssid, WiFi.localIP().toString().c_str());
  } else {
    displayPtr->println("Offline Mode");
  }
  
  displayPtr->println("---");
  
  // Show sensor data from active devices
  int shown = 0;
  for (int i = 0; i < MAX_PEERS && shown < 2; i++) {
    if (deviceActive[i] && deviceReadings[i].module_type > 0 && 
        (millis() - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
      displayPtr->printf("%s: ", getDeviceName(i+1).c_str());
      
      switch (deviceReadings[i].module_type) {
        case 1:
          displayPtr->printf("%.1fC %.1f%%\n", deviceReadings[i].temperature, deviceReadings[i].humidity);
          break;
        case 2:
          displayPtr->printf("Light %.0f%%\n", deviceReadings[i].light_value);
          break;
      }
      shown++;
    }
  }
  
  if (shown == 0) {
    displayPtr->println("No sensor data");
  }
  
  displayPtr->display();
}

// ==================== ESPUI FUNCTIONS ====================
void initESPUI() {
  ESPUI.setVerbosity(Verbosity::Quiet);
  
  // Main tab
  uint16_t mainTab = ESPUI.addControl(ControlType::Tab, "Dashboard", "Dashboard");
  
  ESPUI.addControl(ControlType::Separator, "Network Status", "", ControlColor::None, mainTab);
  networkModeLabel = ESPUI.addControl(ControlType::Label, "Network Mode", 
                                     CREATE_AP ? "Access Point" : "WiFi Client", 
                                     ControlColor::Emerald, mainTab);
  
  String ipAddr = CREATE_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  ipAddressLabel = ESPUI.addControl(ControlType::Label, "IP Address", ipAddr, ControlColor::Emerald, mainTab);
  activeDevicesLabel = ESPUI.addControl(ControlType::Label, "Active Devices", "0/" + String(NUM_DEVICES), 
                                       ControlColor::Alizarin, mainTab);
  uptimeLabel = ESPUI.addControl(ControlType::Label, "Uptime", "0s", ControlColor::Carrot, mainTab);
  refreshButton = ESPUI.addControl(ControlType::Button, "Refresh Data", "Refresh", 
                                  ControlColor::Peterriver, mainTab, &refreshButtonCallback);
  
  // Device tabs
  for (int i = 0; i < NUM_DEVICES; i++) {
    String tabName = getDeviceName(devices[i].id) + " (" + String(devices[i].id) + ")";
    uint16_t deviceTab = ESPUI.addControl(ControlType::Tab, tabName.c_str(), tabName.c_str());
    
    ESPUI.addControl(ControlType::Separator, "Device Info", "", ControlColor::None, deviceTab);
    ESPUI.addControl(ControlType::Label, "Module Type", getModuleTypeName(devices[i].module_type), 
                    ControlColor::Wetasphalt, deviceTab);
    
    deviceStatusLabels[i] = ESPUI.addControl(ControlType::Label, "Status", "🔴 Offline", 
                                            ControlColor::Alizarin, deviceTab);
    deviceLastSeenLabels[i] = ESPUI.addControl(ControlType::Label, "Last Seen", "Never", 
                                              ControlColor::Carrot, deviceTab);
    
    if (devices[i].module_type > 0) {
      ESPUI.addControl(ControlType::Separator, "Sensor Data", "", ControlColor::None, deviceTab);
      
      if (devices[i].module_type == 1) {
        temperatureLabels[i] = ESPUI.addControl(ControlType::Label, "Temperature", "--°C", 
                                               ControlColor::Turquoise, deviceTab);
        humidityLabels[i] = ESPUI.addControl(ControlType::Label, "Humidity", "--%", 
                                           ControlColor::Emerald, deviceTab);
      } else if (devices[i].module_type == 2) {
        lightLabels[i] = ESPUI.addControl(ControlType::Label, "Light Level", "--%", 
                                         ControlColor::Sunflower, deviceTab);
      }
    }
  }
  
  // Settings tab
  uint16_t configTab = ESPUI.addControl(ControlType::Tab, "Settings", "Settings");
  ESPUI.addControl(ControlType::Separator, "Configuration", "", ControlColor::None, configTab);
  
  updateIntervalSlider = ESPUI.addControl(ControlType::Slider, "Update Interval (seconds)", 
                                         String(currentUpdateInterval), ControlColor::Alizarin, configTab, 
                                         &updateIntervalCallback);
  ESPUI.addControl(ControlType::Min, "", "5", ControlColor::None, updateIntervalSlider);
  ESPUI.addControl(ControlType::Max, "", "60", ControlColor::None, updateIntervalSlider);
  
  ESPUI.addControl(ControlType::Separator, "System Info", "", ControlColor::None, configTab);
  ESPUI.addControl(ControlType::Label, "Device ID", String(DEVICE_ID), ControlColor::Wetasphalt, configTab);
  ESPUI.addControl(ControlType::Label, "MAC Address", WiFi.macAddress(), ControlColor::Wetasphalt, configTab);
  
  ESPUI.begin("ESP32 Sensor Network");
  Serial.println("ESPUI started");
}

void updateUI() {
  if (!IS_MASTER) return;
  
  static int lastActiveCount = -1;
  static bool lastDeviceStates[NUM_DEVICES] = {false};
  static float lastTemps[NUM_DEVICES], lastHumidity[NUM_DEVICES], lastLight[NUM_DEVICES];
  static bool firstRun = true;
  
  unsigned long currentTime = millis();
  int activeCount = 0;
  bool stateChanged = false;
  
  // Check for device state changes and count active devices
  for (int i = 0; i < NUM_DEVICES; i++) {
    int idx = devices[i].id - 1;
    if (idx < 0 || idx >= MAX_PEERS) continue;
    
    bool wasActive = lastDeviceStates[i];
    bool isActive = deviceActive[idx] && (currentTime - deviceLastSeen[idx] <= DEVICE_TIMEOUT);
    
    if (isActive) activeCount++;
    
    // Check if device state changed
    if (wasActive != isActive) {
      lastDeviceStates[i] = isActive;
      stateChanged = true;
      
      // Update device status
      ESPUI.updateLabel(deviceStatusLabels[i], isActive ? "🟢 Online" : "🔴 Offline");
      ESPUI.updateLabel(deviceLastSeenLabels[i], isActive ? getTimeAgo(deviceLastSeen[idx]) : "Disconnected");
    }
    
    // Update sensor readings only if device is active and data changed
    if (isActive && devices[i].module_type > 0) {
      bool dataChanged = false;
      
      switch (deviceReadings[idx].module_type) {
        case 1:
          if (deviceReadings[idx].temperature > -900 && 
              (firstRun || abs(lastTemps[i] - deviceReadings[idx].temperature) > 0.1)) {
            ESPUI.updateLabel(temperatureLabels[i], String(deviceReadings[idx].temperature, 1) + "°C");
            lastTemps[i] = deviceReadings[idx].temperature;
            dataChanged = true;
          }
          if (deviceReadings[idx].humidity > -900 && 
              (firstRun || abs(lastHumidity[i] - deviceReadings[idx].humidity) > 0.1)) {
            ESPUI.updateLabel(humidityLabels[i], String(deviceReadings[idx].humidity, 1) + "%");
            lastHumidity[i] = deviceReadings[idx].humidity;
            dataChanged = true;
          }
          break;
        case 2:
          if (deviceReadings[idx].light_value > -900 && 
              (firstRun || abs(lastLight[i] - deviceReadings[idx].light_value) > 1.0)) {
            ESPUI.updateLabel(lightLabels[i], String(deviceReadings[idx].light_value, 1) + "%");
            lastLight[i] = deviceReadings[idx].light_value;
            dataChanged = true;
          }
          break;
      }
      
      if (dataChanged && !firstRun) {
        // Update last seen time only when data changes
        ESPUI.updateLabel(deviceLastSeenLabels[i], getTimeAgo(deviceLastSeen[idx]));
      }
    } else if (!isActive && devices[i].module_type > 0 && wasActive) {
      // Device went offline, reset readings
      switch (devices[i].module_type) {
        case 1:
          ESPUI.updateLabel(temperatureLabels[i], "--°C");
          ESPUI.updateLabel(humidityLabels[i], "--%");
          break;
        case 2:
          ESPUI.updateLabel(lightLabels[i], "--%");
          break;
      }
    }
    
    // Mark inactive devices
    if (!isActive && deviceActive[idx]) {
      deviceActive[idx] = false;
      stateChanged = true;
    }
  }
  
  // Update main dashboard only if count changed
  if (activeCount != lastActiveCount || firstRun) {
    ESPUI.updateLabel(activeDevicesLabel, String(activeCount) + "/" + String(NUM_DEVICES));
    lastActiveCount = activeCount;
  }
  
  // Update uptime less frequently
  static unsigned long lastUptimeUpdate = 0;
  if (currentTime - lastUptimeUpdate >= 10000 || firstRun) { // Every 10 seconds
    ESPUI.updateLabel(uptimeLabel, formatUptime(currentTime));
    lastUptimeUpdate = currentTime;
  }
  
  firstRun = false;
}

// ==================== CALLBACK FUNCTIONS ====================
void refreshButtonCallback(Control* sender, int type) {
  if (type == B_DOWN) {
    Serial.println("Refresh requested");
    uiUpdateNeeded = true; // Trigger immediate UI update
    lastUIDataChange = millis();
    
    // Provide user feedback without blocking
    ESPUI.updateButton(sender->id, "Refreshed!");
    refreshButtonResetTime = millis() + 1000; // Reset after 1 second
  }
}

void updateIntervalCallback(Control* sender, int type) {
  if (type == SL_VALUE) {
    currentUpdateInterval = sender->value.toInt();
    Serial.printf("Update interval: %d seconds\n", currentUpdateInterval);
  }
}

// ==================== UTILITY FUNCTIONS ====================
String getDeviceName(uint8_t id) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == id) return String(devices[i].name);
  }
  return "Unknown";
}

String getModuleTypeName(uint8_t type) {
  switch (type) {
    case 0: return "Master/No Sensors";
    case 1: return "Temperature & Humidity";
    case 2: return "Light Sensor";
    default: return "Unknown";
  }
}

String getTimeAgo(unsigned long timestamp) {
  unsigned long elapsed = millis() - timestamp;
  
  if (elapsed < 1000) return "just now";
  if (elapsed < 60000) return String(elapsed / 1000) + "s ago";
  if (elapsed < 3600000) return String(elapsed / 60000) + "m ago";
  if (elapsed < 86400000) return String(elapsed / 3600000) + "h ago";
  return String(elapsed / 86400000) + "d ago";
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