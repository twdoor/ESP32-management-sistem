#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPUI.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// ==================== CONFIGURATION ====================
// Set to true for master device, false for slave
const bool IS_MASTER = true;

// Access Point mode configuration (Master only)
const bool CREATE_AP = true;  // Set to true to create an AP, false to connect to existing WiFi

// Device IDs - must be unique for each device in your network
const uint8_t DEVICE_ID = 1; // 1 for master, 2+ for slaves

// Module type configuration - determines which sensors this device has
// 0 = No sensors (master only, no sensor data)
// 1 = Temperature & Humidity sensor (e.g., DHT22, BME280)
// 2 = Light sensor (e.g., LDR, TSL2561)
const uint8_t MODULE_TYPE = 0;  // Set to 0 for master, 1 for temp/hum, 2 for light

// Update interval in milliseconds
const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;
const unsigned long UI_UPDATE_INTERVAL = 2000;

// ==================== WIFI & AP SETTINGS ====================
// For connecting to existing WiFi (when CREATE_AP = false)
const char* wifi_ssid = "YOUR_WIFI_SSID";
const char* wifi_password = "YOUR_WIFI_PASSWORD";

// For creating an Access Point (when CREATE_AP = true)
const char* ap_ssid = "ESP32-SensorHub";
const char* ap_password = "sensornetwork";
const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

// Set a hostname for the master device
const char* hostname = "esp32-sensor-hub";

// ==================== DISPLAY SETTINGS ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

// Only declare and use the display object if this is a master
Adafruit_SSD1306* displayPtr = NULL;

// ==================== ESP-NOW SETTINGS ====================
#define MAX_PEERS 20

// Known device MAC addresses and their types
typedef struct {
  uint8_t id;
  uint8_t address[6];
  char name[32];
  uint8_t module_type; // 0=none, 1=temp/hum, 2=light
} device_info;

// Add all device MACs here - master should be first
device_info devices[] = {
  {1, {0xd4, 0x8a, 0xfc, 0x9f, 0x2f, 0x98}, "Master", 1},     // Master - no sensors
  {2, {0x94, 0xb5, 0x55, 0xf9, 0xff, 0xf0}, "Outdoor", 2},   // Temp/Humidity sensor
  {3, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, "Indoor", 2}     // Light sensor
  // Add more devices as needed
};
const int NUM_DEVICES = sizeof(devices) / sizeof(device_info);

// ==================== DATA STRUCTURES ====================
// Data structure for sending readings
typedef struct sensor_message {
  uint8_t sender_id;
  uint8_t module_type;     // Type of module sending data
  float temperature;       // Only valid for module_type 1
  float humidity;          // Only valid for module_type 1
  float light_value;       // Only valid for module_type 2 (0-100%)
  unsigned long timestamp;
} sensor_message;

// Command message structure for master-to-slave communication
typedef struct command_message {
  uint8_t sender_id;       // Always 1 (master)
  uint8_t target_id;       // Target slave device ID (0 = broadcast to all)
  uint8_t command_type;    // 1=update_interval, 2=reset, 3=sleep, etc.
  uint32_t parameter;      // Command parameter (e.g., new interval in seconds)
  unsigned long timestamp;
} command_message;

// Array to store the latest readings from each device
sensor_message deviceReadings[MAX_PEERS];
bool deviceActive[MAX_PEERS] = {false};
unsigned long deviceLastSeen[MAX_PEERS] = {0};

// Variable to store transmission status
String lastTransmitStatus = "None";

// ==================== ESPUI ELEMENTS ====================
// UI Element IDs
uint16_t networkStatusLabel;
uint16_t activeDevicesLabel;
uint16_t networkModeLabel;
uint16_t ipAddressLabel;
uint16_t uptimeLabel;

// Device status elements
uint16_t deviceTabs[NUM_DEVICES];
uint16_t deviceStatusLabels[NUM_DEVICES];
uint16_t deviceTypeLabels[NUM_DEVICES];
uint16_t deviceLastSeenLabels[NUM_DEVICES];
uint16_t temperatureLabels[NUM_DEVICES];
uint16_t humidityLabels[NUM_DEVICES];
uint16_t lightLabels[NUM_DEVICES];

// Control elements
uint16_t refreshButton;
uint16_t configTab;
uint16_t updateIntervalSlider;
uint16_t displayTimeoutSlider;

// Settings
uint16_t currentUpdateInterval = UPDATE_INTERVAL / 1000; // in seconds
uint16_t displayTimeout = 30; // seconds

// ==================== FUNCTION DECLARATIONS ====================
void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &light);
void sendReadings();
void sendCommand(uint8_t targetId, uint8_t commandType, uint32_t parameter);
void updateDisplay();
void handleReceivedData(const sensor_message &reading);
void handleReceivedCommand(const command_message &command);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

// WiFi and web server functions (Master only)
void setupWiFi();
void setupAccessPoint();
void initESPUI();
void updateUI();
String getDeviceName(uint8_t id);
String getModuleTypeName(uint8_t type);
String getTimeAgo(unsigned long timestamp);
String formatUptime(unsigned long ms);

// ESPUI Callbacks
void refreshButtonCallback(Control* sender, int type);
void updateIntervalCallback(Control* sender, int type);
void displayTimeoutCallback(Control* sender, int type);

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.print("Initializing ");
  Serial.println(IS_MASTER ? "MASTER" : "SLAVE");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);
  Serial.print("Module Type: ");
  Serial.print(MODULE_TYPE);
  Serial.print(" (");
  Serial.print(getModuleTypeName(MODULE_TYPE));
  Serial.println(")");

  // Initialize device array with invalid values
  for (int i = 0; i < MAX_PEERS; i++) {
    deviceReadings[i].sender_id = 0;
    deviceReadings[i].module_type = 0;
    deviceReadings[i].temperature = -999;
    deviceReadings[i].humidity = -999;
    deviceReadings[i].light_value = -999;
    deviceReadings[i].timestamp = 0;
  }

  // Set this device's own reading
  deviceReadings[DEVICE_ID - 1].sender_id = DEVICE_ID;
  deviceReadings[DEVICE_ID - 1].module_type = MODULE_TYPE;
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();

  // Initialize I2C
  Wire.begin();
  
  // If master, initialize display and WiFi
  if (IS_MASTER) {
    displayPtr = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    
    if (!displayPtr->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
      Serial.println(F("SSD1306 allocation failed"));
      delete displayPtr;
      displayPtr = NULL;
    } else {
      displayPtr->clearDisplay();
      displayPtr->setTextSize(1);
      displayPtr->setTextColor(WHITE);
      displayPtr->setCursor(0, 0);
      displayPtr->println("ESP32 Master");
      displayPtr->println("Initializing...");
      displayPtr->display();
    }
    
    // Setup WiFi or Access Point based on configuration
    if (CREATE_AP) {
      setupAccessPoint();
    } else {
      setupWiFi();
    }
    
    // Initialize ESPUI
    initESPUI();
  }

  // Initialize ESP-NOW
  initESPNow();
}

// ==================== MAIN LOOP ====================
unsigned long lastSendTime = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastUIUpdate = 0;

void loop() {
  unsigned long currentTime = millis();
  
  // Check if it's time to send data (only if module has sensors)
  if (currentTime - lastSendTime >= (currentUpdateInterval * 1000) && MODULE_TYPE > 0) {
    // Get sensor readings based on module type
    float temp, hum, light;
    getSensorReadings(temp, hum, light);
    
    // Update local device readings
    deviceReadings[DEVICE_ID - 1].temperature = temp;
    deviceReadings[DEVICE_ID - 1].humidity = hum;
    deviceReadings[DEVICE_ID - 1].light_value = light;
    deviceReadings[DEVICE_ID - 1].timestamp = currentTime;
    deviceLastSeen[DEVICE_ID - 1] = currentTime;
    
    // Send readings to all peers
    sendReadings();
    
    // Update last send time
    lastSendTime = currentTime;
  }
  
  // If master, update display and handle web server
  if (IS_MASTER) {
    // Update display periodically
    if (currentTime - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
      if (displayPtr != NULL) {
        updateDisplay();
      }
      lastDisplayUpdate = currentTime;
    }
    
    // Update UI periodically
    if (currentTime - lastUIUpdate >= UI_UPDATE_INTERVAL) {
      updateUI();
      lastUIUpdate = currentTime;
    }
  }
  
  // Small delay to prevent CPU hogging
  delay(10);
}

// ==================== WIFI/AP FUNCTIONS ====================
void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  Serial.print("Connecting to WiFi");
  
  int wifiRetries = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetries < 20) {
    delay(500);
    Serial.print(".");
    wifiRetries++;
    
    if (displayPtr != NULL) {
      displayPtr->clearDisplay();
      displayPtr->setCursor(0, 0);
      displayPtr->println("Connecting to WiFi");
      displayPtr->print("Attempt: ");
      displayPtr->println(wifiRetries);
      displayPtr->display();
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(wifi_ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    if (displayPtr != NULL) {
      displayPtr->clearDisplay();
      displayPtr->setCursor(0, 0);
      displayPtr->println("WiFi Connected");
      displayPtr->print("IP: ");
      displayPtr->println(WiFi.localIP());
      displayPtr->display();
      delay(2000);
    }
    
    if (MDNS.begin(hostname)) {
      Serial.println("mDNS responder started");
      Serial.print("You can access the server at http://");
      Serial.print(hostname);
      Serial.println(".local");
    }
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed");
    
    if (displayPtr != NULL) {
      displayPtr->clearDisplay();
      displayPtr->setCursor(0, 0);
      displayPtr->println("WiFi Failed");
      displayPtr->println("Running in offline mode");
      displayPtr->display();
      delay(2000);
    }
  }
}

void setupAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
  
  bool apStarted = false;
  if (strlen(ap_password) >= 8) {
    apStarted = WiFi.softAP(ap_ssid, ap_password);
  } else {
    apStarted = WiFi.softAP(ap_ssid);
  }
  
  if (apStarted) {
    Serial.println("Access Point started");
    Serial.print("SSID: ");
    Serial.println(ap_ssid);
    if (strlen(ap_password) >= 8) {
      Serial.print("Password: ");
      Serial.println(ap_password);
    } else {
      Serial.println("Open network (no password)");
    }
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    
    if (displayPtr != NULL) {
      displayPtr->clearDisplay();
      displayPtr->setCursor(0, 0);
      displayPtr->println("AP Mode Started");
      displayPtr->print("SSID: ");
      displayPtr->println(ap_ssid);
      displayPtr->print("IP: ");
      displayPtr->println(WiFi.softAPIP());
      displayPtr->display();
      delay(2000);
    }
  } else {
    Serial.println("Failed to start Access Point");
    
    if (displayPtr != NULL) {
      displayPtr->clearDisplay();
      displayPtr->setCursor(0, 0);
      displayPtr->println("AP Start Failed");
      displayPtr->display();
      delay(2000);
    }
  }
}

// ==================== ESP-NOW FUNCTIONS ====================
void initESPNow() {
  if (!IS_MASTER) {
    WiFi.mode(WIFI_STA);
  }
  
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  
  registerPeers();
}

void registerPeers() {
  if (IS_MASTER) {
    // Master adds all slave devices as peers
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (devices[i].id == DEVICE_ID) continue; // Skip self
      
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, devices[i].address, 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      
      esp_err_t result = esp_now_add_peer(&peerInfo);
      if (result == ESP_OK) {
        Serial.print("Master: Peer added successfully - Device ");
        Serial.print(devices[i].id);
        Serial.print(" (");
        Serial.print(devices[i].name);
        Serial.print(" - ");
        Serial.print(getModuleTypeName(devices[i].module_type));
        Serial.println(")");
      } else {
        Serial.print("Master: Failed to add peer - Device ");
        Serial.print(devices[i].id);
        Serial.print(" (");
        Serial.print(devices[i].name);
        Serial.println(")");
      }
    }
  } else {
    // Slave devices only add the master as a peer
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (devices[i].id == 1) { // Find master device (ID = 1)
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, devices[i].address, 6);
        peerInfo.channel = 0;
        peerInfo.encrypt = false;
        
        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result == ESP_OK) {
          Serial.print("Slave: Master peer added successfully - Device ");
          Serial.print(devices[i].id);
          Serial.print(" (");
          Serial.print(devices[i].name);
          Serial.println(")");
        } else {
          Serial.print("Slave: Failed to add master peer - Device ");
          Serial.print(devices[i].id);
          Serial.print(" (");
          Serial.print(devices[i].name);
          Serial.println(")");
        }
        break; // Only add master, then exit
      }
    }
  }
}

// ==================== SENSOR FUNCTIONS ====================
void getSensorReadings(float &temp, float &hum, float &light) {
  // Initialize all values to invalid
  temp = -999;
  hum = -999;
  light = -999;
  
  switch (MODULE_TYPE) {
    case 0:
      // No sensors - do nothing
      break;
      
    case 1: {
      // Temperature & Humidity sensor
      // Replace with actual sensor code (e.g., DHT22, BME280)
      temp = 25.0 + (random(100) - 50) / 10.0; // 20.0 - 30.0°C
      hum = 50.0 + (random(200) - 100) / 10.0; // 40.0 - 60.0%
      Serial.print("Temperature: ");
      Serial.print(temp);
      Serial.print("°C, Humidity: ");
      Serial.print(hum);
      Serial.println("%");
      break;
    }
      
    case 2: {
      // Light sensor
      // Replace with actual sensor code (e.g., LDR on analog pin, TSL2561)
      int rawValue = analogRead(A0); // Read from analog pin A0
      light = (rawValue / 4095.0) * 100.0; // Convert to percentage (0-100%)
      Serial.print("Light value: ");
      Serial.print(light);
      Serial.println("%");
      break;
    }
      
    default:
      Serial.println("Unknown module type");
      break;
  }
}

// ==================== COMMUNICATION FUNCTIONS ====================
void sendReadings() {
  // Only send if we have sensors
  if (MODULE_TYPE == 0) return;
  
  // Prepare message
  sensor_message message;
  message.sender_id = DEVICE_ID;
  message.module_type = MODULE_TYPE;
  message.temperature = deviceReadings[DEVICE_ID - 1].temperature;
  message.humidity = deviceReadings[DEVICE_ID - 1].humidity;
  message.light_value = deviceReadings[DEVICE_ID - 1].light_value;
  message.timestamp = millis();
  
  if (IS_MASTER) {
    // Master sends to all slave devices (if needed for commands/config)
    // In most cases, master doesn't need to send sensor data
    Serial.println("Master: Not sending sensor data (master has no sensors by default)");
  } else {
    // Slave sends only to master
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (devices[i].id == 1) { // Find master device (ID = 1)
        esp_err_t result = esp_now_send(devices[i].address, (uint8_t *)&message, sizeof(message));
        
        if (result == ESP_OK) {
          Serial.print("Slave: Data sent to master (Device ");
          Serial.print(devices[i].id);
          Serial.println(")");
        } else {
          Serial.print("Slave: Failed to send to master (Device ");
          Serial.print(devices[i].id);
          Serial.println(")");
        }
        break; // Only send to master, then exit
      }
    }
  }
}

// ==================== CALLBACK FUNCTIONS ====================
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  
  Serial.print("Last Packet Sent to: ");
  Serial.print(macStr);
  Serial.print(" Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
  
  lastTransmitStatus = status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL";
}

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len == sizeof(sensor_message)) {
    // Received sensor data
    sensor_message *incomingData = (sensor_message*)data;
    
    if (IS_MASTER) {
      // Master receives sensor data from slaves
      handleReceivedData(*incomingData);
      
      Serial.print("Master: Received sensor data from Device ");
      Serial.print(incomingData->sender_id);
      Serial.print(" (");
      Serial.print(getModuleTypeName(incomingData->module_type));
      Serial.println(")");
      
      switch (incomingData->module_type) {
        case 1:
          Serial.print("Temperature: ");
          Serial.print(incomingData->temperature);
          Serial.print("°C, Humidity: ");
          Serial.print(incomingData->humidity);
          Serial.println("%");
          break;
        case 2:
          Serial.print("Light: ");
          Serial.print(incomingData->light_value);
          Serial.println("%");
          break;
      }
    } else {
      // Slaves should not receive sensor data from other slaves
      Serial.print("Slave: Unexpected sensor data from Device ");
      Serial.print(incomingData->sender_id);
      Serial.println(" (ignoring)");
    }
    Serial.println();
  } else if (len == sizeof(command_message)) {
    // Received command data
    command_message *incomingCommand = (command_message*)data;
    
    if (!IS_MASTER && (incomingCommand->target_id == DEVICE_ID || incomingCommand->target_id == 0)) {
      // Slave receives command from master
      handleReceivedCommand(*incomingCommand);
    } else if (IS_MASTER) {
      Serial.println("Master: Received unexpected command (ignoring)");
    }
  }
}

void handleReceivedData(const sensor_message &reading) {
  if (reading.sender_id > 0 && reading.sender_id <= MAX_PEERS) {
    deviceReadings[reading.sender_id - 1] = reading;
    deviceActive[reading.sender_id - 1] = true;
    deviceLastSeen[reading.sender_id - 1] = millis();
  }
}

// ==================== COMMAND FUNCTIONS ====================
void sendCommand(uint8_t targetId, uint8_t commandType, uint32_t parameter) {
  if (!IS_MASTER) {
    Serial.println("Only master can send commands");
    return;
  }
  
  command_message command;
  command.sender_id = DEVICE_ID;
  command.target_id = targetId;
  command.command_type = commandType;
  command.parameter = parameter;
  command.timestamp = millis();
  
  if (targetId == 0) {
    // Broadcast to all slaves
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (devices[i].id == DEVICE_ID) continue; // Skip master
      
      esp_err_t result = esp_now_send(devices[i].address, (uint8_t *)&command, sizeof(command));
      Serial.print("Master: Command broadcast to Device ");
      Serial.print(devices[i].id);
      Serial.println(result == ESP_OK ? " - Success" : " - Failed");
    }
  } else {
    // Send to specific slave
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (devices[i].id == targetId) {
        esp_err_t result = esp_now_send(devices[i].address, (uint8_t *)&command, sizeof(command));
        Serial.print("Master: Command sent to Device ");
        Serial.print(targetId);
        Serial.println(result == ESP_OK ? " - Success" : " - Failed");
        break;
      }
    }
  }
}

void handleReceivedCommand(const command_message &command) {
  Serial.print("Slave: Received command ");
  Serial.print(command.command_type);
  Serial.print(" from master with parameter: ");
  Serial.println(command.parameter);
  
  switch (command.command_type) {
    case 1: // Update interval
      currentUpdateInterval = command.parameter;
      Serial.print("Slave: Update interval changed to ");
      Serial.print(currentUpdateInterval);
      Serial.println(" seconds");
      break;
    case 2: // Reset command
      Serial.println("Slave: Reset command received - restarting in 3 seconds");
      delay(3000);
      ESP.restart();
      break;
    case 3: // Sleep command
      Serial.print("Slave: Sleep command received - sleeping for ");
      Serial.print(command.parameter);
      Serial.println(" seconds");
      esp_sleep_enable_timer_wakeup(command.parameter * 1000000ULL); // Convert to microseconds
      esp_deep_sleep_start();
      break;
    default:
      Serial.print("Slave: Unknown command type: ");
      Serial.println(command.command_type);
      break;
  }
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplay() {
  if (!IS_MASTER || displayPtr == NULL) return;
  
  int activeCount = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i]) activeCount++;
  }
  
  displayPtr->clearDisplay();
  displayPtr->setCursor(0, 0);
  displayPtr->print("ESP32 Network - ");
  displayPtr->println(DEVICE_ID);
  
  displayPtr->print("Active: ");
  displayPtr->print(activeCount);
  displayPtr->print("/");
  displayPtr->println(NUM_DEVICES);
  
  // Display network mode and IP
  if (CREATE_AP) {
    displayPtr->print("AP: ");
    displayPtr->println(ap_ssid);
    displayPtr->print("IP: ");
    displayPtr->println(WiFi.softAPIP());
  } else if (WiFi.status() == WL_CONNECTED) {
    displayPtr->print("WiFi: ");
    displayPtr->println(wifi_ssid);
    displayPtr->print("IP: ");
    displayPtr->println(WiFi.localIP());
  } else {
    displayPtr->println("Offline Mode");
  }
  
  // Show active sensor devices
  displayPtr->println("-------------------");
  int shownDevices = 0;
  for (int i = 0; i < MAX_PEERS && shownDevices < 2; i++) {
    if (deviceActive[i] && deviceReadings[i].module_type > 0) {
      displayPtr->print(getDeviceName(i+1));
      displayPtr->print(": ");
      
      switch (deviceReadings[i].module_type) {
        case 1:
          displayPtr->print(deviceReadings[i].temperature, 1);
          displayPtr->print("C ");
          displayPtr->print(deviceReadings[i].humidity, 1);
          displayPtr->println("%");
          break;
        case 2:
          displayPtr->print("Light ");
          displayPtr->print(deviceReadings[i].light_value, 0);
          displayPtr->println("%");
          break;
        default:
          displayPtr->println("No data");
          break;
      }
      shownDevices++;
    }
  }
  
  if (shownDevices == 0) {
    displayPtr->println("No sensor data");
  }
  
  displayPtr->display();
}

// ==================== ESPUI FUNCTIONS ====================
void initESPUI() {
  // Set theme and basic properties
  ESPUI.setVerbosity(Verbosity::Quiet);
  
  // Main Dashboard Tab
  uint16_t mainTab = ESPUI.addControl(ControlType::Tab, "Dashboard", "Dashboard");
  
  // Network Status Section
  ESPUI.addControl(ControlType::Separator, "Network Status", "", ControlColor::None, mainTab);
  
  networkModeLabel = ESPUI.addControl(ControlType::Label, "Network Mode", 
                                     CREATE_AP ? "Access Point" : "WiFi Client", 
                                     ControlColor::Emerald, mainTab);
  
  String ipAddr = CREATE_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  ipAddressLabel = ESPUI.addControl(ControlType::Label, "IP Address", ipAddr, ControlColor::Emerald, mainTab);
  
  activeDevicesLabel = ESPUI.addControl(ControlType::Label, "Active Devices", "0/" + String(NUM_DEVICES), 
                                       ControlColor::Alizarin, mainTab);
  
  uptimeLabel = ESPUI.addControl(ControlType::Label, "Uptime", "0 seconds", ControlColor::Carrot, mainTab);
  
  // Refresh Button
  refreshButton = ESPUI.addControl(ControlType::Button, "Refresh Data", "Refresh", 
                                  ControlColor::Peterriver, mainTab, &refreshButtonCallback);
  
  // Create tabs for each device
  for (int i = 0; i < NUM_DEVICES; i++) {
    uint8_t deviceId = devices[i].id;
    String tabName = getDeviceName(deviceId) + " (" + String(deviceId) + ")";
    
    deviceTabs[i] = ESPUI.addControl(ControlType::Tab, tabName.c_str(), tabName.c_str());
    
    // Device Information
    ESPUI.addControl(ControlType::Separator, "Device Information", "", ControlColor::None, deviceTabs[i]);
    
    deviceTypeLabels[i] = ESPUI.addControl(ControlType::Label, "Module Type", 
                                          getModuleTypeName(devices[i].module_type), 
                                          ControlColor::Wetasphalt, deviceTabs[i]);
    
    deviceStatusLabels[i] = ESPUI.addControl(ControlType::Label, "Status", "🔴 Offline", 
                                            ControlColor::Alizarin, deviceTabs[i]);
    
    deviceLastSeenLabels[i] = ESPUI.addControl(ControlType::Label, "Last Seen", "Never", 
                                              ControlColor::Carrot, deviceTabs[i]);
    
    // Sensor Data Section (only for devices with sensors)
    if (devices[i].module_type > 0) {
      ESPUI.addControl(ControlType::Separator, "Sensor Readings", "", ControlColor::None, deviceTabs[i]);
      
      if (devices[i].module_type == 1) { // Temperature & Humidity
        temperatureLabels[i] = ESPUI.addControl(ControlType::Label, "Temperature", "--°C", 
                                               ControlColor::Turquoise, deviceTabs[i]);
        humidityLabels[i] = ESPUI.addControl(ControlType::Label, "Humidity", "--%", 
                                           ControlColor::Emerald, deviceTabs[i]);
      } else if (devices[i].module_type == 2) { // Light Sensor
        lightLabels[i] = ESPUI.addControl(ControlType::Label, "Light Level", "--%", 
                                         ControlColor::Sunflower, deviceTabs[i]);
      }
    }
  }
  
  // Configuration Tab
  configTab = ESPUI.addControl(ControlType::Tab, "Settings", "Settings");
  
  ESPUI.addControl(ControlType::Separator, "System Configuration", "", ControlColor::None, configTab);
  
  updateIntervalSlider = ESPUI.addControl(ControlType::Slider, "Update Interval (seconds)", 
                                         String(currentUpdateInterval), ControlColor::Alizarin, configTab, 
                                         &updateIntervalCallback);
  ESPUI.addControl(ControlType::Min, "", "5", ControlColor::None, updateIntervalSlider);
  ESPUI.addControl(ControlType::Max, "", "60", ControlColor::None, updateIntervalSlider);
  
  displayTimeoutSlider = ESPUI.addControl(ControlType::Slider, "Display Timeout (seconds)", 
                                         String(displayTimeout), ControlColor::Carrot, configTab, 
                                         &displayTimeoutCallback);
  ESPUI.addControl(ControlType::Min, "", "10", ControlColor::None, displayTimeoutSlider);
  ESPUI.addControl(ControlType::Max, "", "300", ControlColor::None, displayTimeoutSlider);
  
  // System Information
  ESPUI.addControl(ControlType::Separator, "System Information", "", ControlColor::None, configTab);
  ESPUI.addControl(ControlType::Label, "Device ID", String(DEVICE_ID), ControlColor::Wetasphalt, configTab);
  ESPUI.addControl(ControlType::Label, "MAC Address", WiFi.macAddress(), ControlColor::Wetasphalt, configTab);
  ESPUI.addControl(ControlType::Label, "Firmware Version", "v1.0.0", ControlColor::Wetasphalt, configTab);
  
  // Start ESPUI
  ESPUI.begin("ESP32 Sensor Network");
  Serial.println("ESPUI started");
}

void updateUI() {
  if (!IS_MASTER) return;
  
  unsigned long currentTime = millis();
  const unsigned long TIMEOUT = 5 * 60 * 1000; // 5 minutes timeout
  
  // Count active devices
  int activeCount = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && (currentTime - deviceLastSeen[i] <= TIMEOUT)) {
      activeCount++;
    } else if (deviceActive[i] && (currentTime - deviceLastSeen[i] > TIMEOUT)) {
      deviceActive[i] = false;
    }
  }
  
  // Update main dashboard
  ESPUI.updateLabel(activeDevicesLabel, String(activeCount) + "/" + String(NUM_DEVICES));
  ESPUI.updateLabel(uptimeLabel, formatUptime(currentTime));
  
  // Update device-specific information
  for (int i = 0; i < NUM_DEVICES; i++) {
    uint8_t deviceId = devices[i].id;
    int idx = deviceId - 1;
    
    if (idx >= 0 && idx < MAX_PEERS) {
      bool isActive = deviceActive[idx] && (currentTime - deviceLastSeen[idx] <= TIMEOUT);
      
      // Update device status with visual indicators
      if (isActive) {
        ESPUI.updateLabel(deviceStatusLabels[i], "🟢 Online");
        ESPUI.updateLabel(deviceLastSeenLabels[i], getTimeAgo(deviceLastSeen[idx]));
      } else {
        ESPUI.updateLabel(deviceStatusLabels[i], "🔴 Offline");
        ESPUI.updateLabel(deviceLastSeenLabels[i], "Disconnected");
      }
      
      // Update sensor readings if device is active and has sensors
      if (isActive && devices[i].module_type > 0) {
        switch (deviceReadings[idx].module_type) {
          case 1: // Temperature & Humidity
            if (deviceReadings[idx].temperature > -900) {
              ESPUI.updateLabel(temperatureLabels[i], String(deviceReadings[idx].temperature, 1) + "°C");
            }
            if (deviceReadings[idx].humidity > -900) {
              ESPUI.updateLabel(humidityLabels[i], String(deviceReadings[idx].humidity, 1) + "%");
            }
            break;
          case 2: // Light Sensor
            if (deviceReadings[idx].light_value > -900) {
              ESPUI.updateLabel(lightLabels[i], String(deviceReadings[idx].light_value, 1) + "%");
            }
            break;
        }
      } else if (devices[i].module_type > 0) {
        // Device is offline, show no data
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
    }
  }
}

// ==================== ESPUI CALLBACK FUNCTIONS ====================
void refreshButtonCallback(Control* sender, int type) {
  if (type == B_DOWN) {
    Serial.println("Refresh button pressed");
    // Force immediate UI update
    updateUI();
    
    // Update button feedback
    ESPUI.updateButton(sender->id, "Refreshed!");
    delay(1000);
    ESPUI.updateButton(sender->id, "Refresh");
  }
}

void updateIntervalCallback(Control* sender, int type) {
  if (type == SL_VALUE) {
    currentUpdateInterval = sender->value.toInt();
    Serial.print("Update interval changed to: ");
    Serial.print(currentUpdateInterval);
    Serial.println(" seconds");
  }
}

void displayTimeoutCallback(Control* sender, int type) {
  if (type == SL_VALUE) {
    displayTimeout = sender->value.toInt();
    Serial.print("Display timeout changed to: ");
    Serial.print(displayTimeout);
    Serial.println(" seconds");
  }
}

// ==================== UTILITY FUNCTIONS ====================
String getDeviceName(uint8_t id) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == id) {
      return String(devices[i].name);
    }
  }
  return "Unknown Device";
}

String getModuleTypeName(uint8_t type) {
  switch (type) {
    case 0:
      return "Master/No Sensors";
    case 1:
      return "Temperature & Humidity";
    case 2:
      return "Light Sensor";
    default:
      return "Unknown";
  }
}

String getTimeAgo(unsigned long timestamp) {
  unsigned long elapsed = millis() - timestamp;
  
  if (elapsed < 1000) {
    return "just now";
  } else if (elapsed < 60000) {
    return String(elapsed / 1000) + " seconds ago";
  } else if (elapsed < 3600000) {
    return String(elapsed / 60000) + " minutes ago";
  } else if (elapsed < 86400000) {
    return String(elapsed / 3600000) + " hours ago";
  } else {
    return String(elapsed / 86400000) + " days ago";
  }
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
  if (days > 0) {
    uptime += String(days) + "d ";
  }
  if (hours > 0) {
    uptime += String(hours) + "h ";
  }
  if (minutes > 0) {
    uptime += String(minutes) + "m ";
  }
  uptime += String(seconds) + "s";
  
  return uptime;
}