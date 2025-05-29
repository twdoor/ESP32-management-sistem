#include <esp_now.h>
#include <WiFi.h>
// #include <Wire.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_SSD1306.h>
#include <ESPUI.h>
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
#define SETUP_BUTTON_PIN 0  // GPIO 0 (boot button on most ESP32 boards)
bool lastButtonState = HIGH;
unsigned long buttonPressTime = 0;
const unsigned long BUTTON_HOLD_TIME = 3000; // 3 seconds

// Configuration structure for EEPROM storage
struct DeviceConfig {
  bool configured = false;
  bool is_master = true;
  bool create_ap = true;
  uint8_t device_id = 1;
  uint8_t module_type = 0;
  char device_name[32] = "Device";
  uint8_t master_mac[6] = {0};
  uint8_t slave_macs[10][6] = {0}; // Support up to 10 slaves
  uint8_t slave_count = 0;
  char wifi_ssid[64] = "";
  char wifi_password[64] = "";
  uint32_t checksum = 0;
};

DeviceConfig config;
bool setupMode = false;

// Timing constants
const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;
const unsigned long UI_UPDATE_INTERVAL = 10000; // Increased from 5000 to 10000 to reduce flickering
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
// #define SCREEN_WIDTH 128
// #define SCREEN_HEIGHT 64
// #define OLED_ADDRESS 0x3C

// Adafruit_SSD1306* displayPtr = nullptr;

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

// WebSocket connection stability
unsigned long lastWebSocketEvent = 0;
bool webSocketStable = false;

// ==================== ESPUI ELEMENTS ====================
uint16_t activeDevicesLabel, uptimeLabel, networkModeLabel, ipAddressLabel;
uint16_t refreshButton, updateIntervalSlider;
uint16_t currentUpdateInterval = UPDATE_INTERVAL / 1000;

// Dynamic sensor data labels (will be created based on active devices)
uint16_t sensorDataTab = 0;
uint16_t deviceLabels[MAX_PEERS] = {0};
uint16_t temperatureLabels[MAX_PEERS] = {0};
uint16_t humidityLabels[MAX_PEERS] = {0};
uint16_t lightLabels[MAX_PEERS] = {0};
uint16_t statusLabels[MAX_PEERS] = {0};
uint16_t lastSeenLabels[MAX_PEERS] = {0};

// ==================== FUNCTION DECLARATIONS ====================
void loadConfiguration();
void saveConfiguration();
uint32_t calculateChecksum(const DeviceConfig& cfg);
void enterSetupMode();
void setupConfigInterface();

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
void updateSensorDataUI(bool forceUpdate);
String getDeviceName(uint8_t id);
String getModuleTypeName(uint8_t type);
String getTimeAgo(unsigned long timestamp);
String formatUptime(unsigned long ms);
String macToString(const uint8_t* mac);
bool stringToMac(const String& str, uint8_t* mac);

// Check for setup button press (hold for 3 seconds)
void checkSetupButton();

// WebSocket event tracking
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len);

// ESPUI Callbacks
void refreshButtonCallback(Control* sender, int type);
void updateIntervalCallback(Control* sender, int type);

// Global callback handler - catches all ESPUI events for debugging
void globalCallback(Control* sender, int type) {
  Serial.printf("Global callback - ID: %d, Type: %d, Value: '%s'\n", 
                sender->id, type, sender->value.c_str());
}
void masterToggleCallback(Control* sender, int type);
void moduleTypeCallback(Control* sender, int type);
void deviceNameCallback(Control* sender, int type);
void masterMacCallback(Control* sender, int type);
void slaveMacCallback(Control* sender, int type);
void wifiSsidCallback(Control* sender, int type);
void wifiPasswordCallback(Control* sender, int type);
void saveConfigCallback(Control* sender, int type);
void resetConfigCallback(Control* sender, int type);

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP32 Sensor Network Starting...");

  // Initialize EEPROM with larger size to be safe
  EEPROM.begin(1024);
  
  // Load configuration from EEPROM
  loadConfiguration();
  
  // FORCE FIRST-TIME SETUP - Uncomment the line below to always enter setup mode
  // config.configured = false;
  
  // Check if device needs first-time setup
  if (!config.configured) {
    Serial.println("First-time setup required - entering setup mode");
    enterSetupMode();
    return; // Exit setup, device will run in setup mode
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

  // Set this device's own data
  deviceReadings[DEVICE_ID - 1] = {DEVICE_ID, MODULE_TYPE, -999, -999, -999, millis()};
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();

  // Initialize setup button
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  lastButtonState = digitalRead(SETUP_BUTTON_PIN);

  // Wire.begin();
  
  if (IS_MASTER) {
    // Initialize display
    // displayPtr = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    // if (displayPtr->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    //   displayPtr->clearDisplay();
    //   displayPtr->setTextSize(1);
    //   displayPtr->setTextColor(WHITE);
    //   displayPtr->setCursor(0, 0);
    //   displayPtr->println("ESP32 Master");
    //   displayPtr->println("Initializing...");
    //   displayPtr->display();
    // } else {
    //   Serial.println("Display initialization failed");
    //   delete displayPtr;
    //   displayPtr = nullptr;
    // }
    
    setupNetwork();
    if (!setupMode) {
      initESPUI();
    }
  }

  if (!setupMode) {
    initESPNow();
  }
}

// ==================== MAIN LOOP ====================
void loop() {
  // Check for setup button press (works in any mode)
  checkSetupButton();
  
  // If in setup mode, just handle the web server
  if (setupMode) {
    delay(10);
    return;
  }
  
  static unsigned long lastSendTime = 0;
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastUIUpdate = 0;
  
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
      // updateDisplay();
      lastDisplayUpdate = currentTime;
    }
    
    // Only update UI when needed and not too frequently
    if ((currentTime - lastUIUpdate >= UI_UPDATE_INTERVAL) && 
        (uiUpdateNeeded || (currentTime - lastUIDataChange >= UI_UPDATE_INTERVAL * 2))) {
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

// ==================== CONFIGURATION FUNCTIONS ====================
void loadConfiguration() {
  Serial.println("=== LOADING CONFIGURATION ===");
  
  EEPROM.get(0, config);
  
  // Debug: Print raw loaded data
  Serial.printf("Raw loaded data:\n");
  Serial.printf("  configured: %s\n", config.configured ? "true" : "false");
  Serial.printf("  is_master: %s\n", config.is_master ? "true" : "false");
  Serial.printf("  device_id: %d\n", config.device_id);
  Serial.printf("  device_name: '%s'\n", config.device_name);
  Serial.printf("  module_type: %d\n", config.module_type);
  Serial.printf("  stored_checksum: %lu\n", config.checksum);
  
  // Verify checksum
  uint32_t calculated = calculateChecksum(config);
  Serial.printf("  calculated_checksum: %lu\n", calculated);
  
  if (config.checksum != calculated) {
    Serial.println("✗ Configuration checksum mismatch - using defaults");
    Serial.printf("Expected: %lu, Got: %lu\n", calculated, config.checksum);
    config = DeviceConfig(); // Reset to defaults
  } else if (!config.configured) {
    Serial.println("✗ Device not configured - using defaults");
    config = DeviceConfig(); // Reset to defaults
  } else {
    Serial.println("✓ Configuration loaded successfully");
    Serial.printf("Device configured as: %s\n", config.is_master ? "Master" : "Slave");
    Serial.printf("Device name: %s\n", config.device_name);
    Serial.printf("Module type: %s\n", getModuleTypeName(config.module_type).c_str());
  }
  
  Serial.println("=== END LOAD CONFIGURATION ===");
}

void saveConfiguration() {
  Serial.println("=== SAVING CONFIGURATION ===");
  
  // Debug: Print what we're about to save
  Serial.printf("Configured: %s\n", config.configured ? "true" : "false");
  Serial.printf("Is Master: %s\n", config.is_master ? "true" : "false");
  Serial.printf("Device ID: %d\n", config.device_id);
  Serial.printf("Device Name: '%s'\n", config.device_name);
  Serial.printf("Module Type: %d (%s)\n", config.module_type, getModuleTypeName(config.module_type).c_str());
  Serial.printf("Create AP: %s\n", config.create_ap ? "true" : "false");
  Serial.printf("WiFi SSID: '%s'\n", config.wifi_ssid);
  Serial.printf("Slave Count: %d\n", config.slave_count);
  
  if (config.is_master) {
    Serial.println("Slave MAC addresses:");
    for (int i = 0; i < config.slave_count; i++) {
      Serial.printf("  Slave %d: %s\n", i + 1, macToString(config.slave_macs[i]).c_str());
    }
  } else {
    Serial.printf("Master MAC: %s\n", macToString(config.master_mac).c_str());
  }
  
  // Calculate and set checksum
  config.checksum = calculateChecksum(config);
  Serial.printf("Calculated checksum: %lu\n", config.checksum);
  
  // Clear EEPROM first
  for (int i = 0; i < sizeof(DeviceConfig); i++) {
    EEPROM.write(i, 0);
  }
  
  // Write configuration
  EEPROM.put(0, config);
  bool success = EEPROM.commit();
  
  Serial.printf("EEPROM write result: %s\n", success ? "SUCCESS" : "FAILED");
  
  if (success) {
    Serial.println("Configuration saved successfully to EEPROM");
    
    // Verify by reading back
    DeviceConfig verifyConfig;
    EEPROM.get(0, verifyConfig);
    uint32_t verifyChecksum = calculateChecksum(verifyConfig);
    
    Serial.printf("Verification - Stored checksum: %lu, Calculated: %lu\n", 
                  verifyConfig.checksum, verifyChecksum);
    
    if (verifyConfig.checksum == verifyChecksum && verifyConfig.configured) {
      Serial.println("✓ Configuration verified successfully");
    } else {
      Serial.println("✗ Configuration verification FAILED");
    }
  } else {
    Serial.println("✗ EEPROM commit failed!");
  }
  
  Serial.println("=== END SAVE CONFIGURATION ===");
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
  
  // Force AP mode for setup
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
  bool success = WiFi.softAP("ESP32-Setup", "12345678");
  
  if (success) {
    Serial.println("Setup AP started");
    Serial.println("SSID: ESP32-Setup");
    Serial.println("Password: 12345678");
    Serial.print("Setup URL: http://");
    Serial.println(WiFi.softAPIP());
    
    // if (displayPtr == nullptr) {
    //   Wire.begin();
    //   displayPtr = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    //   displayPtr->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
    // }
    
    // if (displayPtr) {
    //   displayPtr->clearDisplay();
    //   displayPtr->setTextSize(1);
    //   displayPtr->setTextColor(WHITE);
    //   displayPtr->setCursor(0, 0);
    //   displayPtr->println("SETUP MODE");
    //   displayPtr->println("WiFi: ESP32-Setup");
    //   displayPtr->println("Pass: 12345678");
    //   displayPtr->println("---");
    //   displayPtr->print("IP: ");
    //   displayPtr->println(WiFi.softAPIP());
    //   displayPtr->println("Open browser to");
    //   displayPtr->println("configure device");
    //   displayPtr->display();
    // }
    
    setupConfigInterface();
  } else {
    Serial.println("Failed to start setup AP");
  }
}

void setupConfigInterface() {
  ESPUI.setVerbosity(Verbosity::Quiet);
  
  // Main setup tab
  uint16_t setupTab = ESPUI.addControl(ControlType::Tab, "Device Setup", "Device Setup");
  
  // Current device info
  ESPUI.addControl(ControlType::Separator, "Device Information", "", ControlColor::None, setupTab);
  ESPUI.addControl(ControlType::Label, "MAC Address", WiFi.macAddress(), ControlColor::Wetasphalt, setupTab);
  ESPUI.addControl(ControlType::Label, "Setup IP", WiFi.softAPIP().toString(), ControlColor::Wetasphalt, setupTab);
  
  // Basic configuration
  ESPUI.addControl(ControlType::Separator, "Basic Configuration", "", ControlColor::None, setupTab);
  
  ESPUI.addControl(ControlType::Text, "Device Name", config.device_name, ControlColor::Alizarin, setupTab, &deviceNameCallback);
  
  ESPUI.addControl(ControlType::Switcher, "Master Device", config.is_master ? "1" : "0", 
                                          ControlColor::Emerald, setupTab, &masterToggleCallback);
  
  uint16_t moduleSelect = ESPUI.addControl(ControlType::Select, "Module Type", String(config.module_type), 
                                          ControlColor::Turquoise, setupTab, &moduleTypeCallback);
  ESPUI.addControl(ControlType::Option, "No Sensors", "0", ControlColor::Alizarin, moduleSelect);
  ESPUI.addControl(ControlType::Option, "Temperature & Humidity", "1", ControlColor::Alizarin, moduleSelect);
  ESPUI.addControl(ControlType::Option, "Light Sensor", "2", ControlColor::Alizarin, moduleSelect);
  
  // Add debug info display
  ESPUI.addControl(ControlType::Separator, "Debug Info", "", ControlColor::None, setupTab);
  String debugInfo = "Current MAC: " + WiFi.macAddress() + "\n";
  debugInfo += "Master: " + String(config.is_master ? "Yes" : "No") + "\n";
  debugInfo += "Module: " + getModuleTypeName(config.module_type);
  ESPUI.addControl(ControlType::Label, "Current Settings", debugInfo, ControlColor::Wetasphalt, setupTab);
  
  // Network configuration  
  ESPUI.addControl(ControlType::Separator, "Network Configuration", "", ControlColor::None, setupTab);
  
  ESPUI.addControl(ControlType::Text, "Master MAC Address", 
                  config.is_master ? "Not needed for master" : macToString(config.master_mac), 
                  ControlColor::Carrot, setupTab, &masterMacCallback);
  
  ESPUI.addControl(ControlType::Text, "Slave MAC Addresses (one per line)", 
                  config.is_master ? "Enter slave MACs here" : "Not needed for slave", 
                  ControlColor::Sunflower, setupTab, &slaveMacCallback);
  
  // WiFi settings (optional)
  ESPUI.addControl(ControlType::Separator, "WiFi Settings (Optional)", "", ControlColor::None, setupTab);
  ESPUI.addControl(ControlType::Text, "WiFi SSID", config.wifi_ssid, ControlColor::Peterriver, setupTab, &wifiSsidCallback);
  ESPUI.addControl(ControlType::Text, "WiFi Password", config.wifi_password, ControlColor::Peterriver, setupTab, &wifiPasswordCallback);
  
  // Action buttons
  ESPUI.addControl(ControlType::Separator, "Actions", "", ControlColor::None, setupTab);
  ESPUI.addControl(ControlType::Button, "Save Configuration", "Save & Restart", 
                  ControlColor::Emerald, setupTab, &saveConfigCallback);
  ESPUI.addControl(ControlType::Button, "Reset to Defaults", "Reset", 
                  ControlColor::Alizarin, setupTab, &resetConfigCallback);
  
  // Quick setup options
  ESPUI.addControl(ControlType::Separator, "Quick Setup", "", ControlColor::None, setupTab);
  ESPUI.addControl(ControlType::Button, "Clear All Settings", "Factory Reset", 
                  ControlColor::Carrot, setupTab, &resetConfigCallback);
  
  // Instructions
  uint16_t helpTab = ESPUI.addControl(ControlType::Tab, "Help", "Help");
  ESPUI.addControl(ControlType::Label, "Setup Instructions", 
                  "1. Choose if this device is a Master or Slave\n"
                  "2. Select the module type based on connected sensors\n" 
                  "3. If Master: Enter MAC addresses of all slave devices\n"
                  "4. If Slave: Enter the MAC address of the master device\n"
                  "5. Optionally configure WiFi for internet access\n"
                  "6. Click 'Save & Restart' to apply configuration", 
                  ControlColor::Wetasphalt, helpTab);
  
  ESPUI.addControl(ControlType::Label, "Finding MAC Addresses", 
                  "Each ESP32 displays its MAC address on startup in the Serial Monitor. "
                  "You can also find it in the Device Information section above. "
                  "Format: AA:BB:CC:DD:EE:FF", 
                  ControlColor::Carrot, helpTab);
  
  ESPUI.begin("ESP32 Device Setup");
  Serial.println("Setup interface started");
}

// ==================== SETUP MODE CALLBACKS ====================
void masterToggleCallback(Control* sender, int type) {
  Serial.printf("Master toggle callback triggered - Type: %d, Value: '%s'\n", type, sender->value.c_str());
  
  // Handle all possible callback types (ESPUI uses various numbers)
  // Common types: 0, 1, 10, 11, S_ACTIVE, etc.
  bool newMasterState = (sender->value.toInt() == 1) || (sender->value == "1") || (sender->value == "true");
  config.is_master = newMasterState;
  Serial.printf("Master mode set to: %s (from value: '%s')\n", 
                config.is_master ? "enabled" : "disabled", sender->value.c_str());
  config.device_id = config.is_master ? 1 : 2; // Auto-assign ID
  Serial.printf("Device ID auto-assigned to: %d\n", config.device_id);
}

void moduleTypeCallback(Control* sender, int type) {
  Serial.printf("Module type callback triggered - Type: %d, Value: '%s'\n", type, sender->value.c_str());
  
  // Handle all possible callback types (ESPUI uses various numbers)
  config.module_type = sender->value.toInt();
  Serial.printf("Module type set to: %d (%s)\n", 
                config.module_type, getModuleTypeName(config.module_type).c_str());
}

void deviceNameCallback(Control* sender, int type) {
  if (type == T_VALUE) {
    Serial.printf("Device name callback: '%s'\n", sender->value.c_str());
    strncpy(config.device_name, sender->value.c_str(), sizeof(config.device_name) - 1);
    config.device_name[sizeof(config.device_name) - 1] = '\0';
    Serial.printf("Device name set to: '%s'\n", config.device_name);
  }
}

void masterMacCallback(Control* sender, int type) {
  Serial.printf("Master MAC callback triggered - Type: %d, Value: '%s'\n", type, sender->value.c_str());
  
  if (!config.is_master) {
    if (stringToMac(sender->value, config.master_mac)) {
      Serial.printf("Master MAC set: %s\n", macToString(config.master_mac).c_str());
    } else {
      Serial.println("Invalid MAC address format");
    }
  }
}

void slaveMacCallback(Control* sender, int type) {
  Serial.printf("Slave MAC callback triggered - Type: %d, Value: '%s'\n", type, sender->value.c_str());
  
  if (config.is_master) {
    // Parse multiple MAC addresses (one per line)
    String input = sender->value;
    config.slave_count = 0;
    
    int startPos = 0;
    int endPos = input.indexOf('\n');
    
    while (startPos < input.length() && config.slave_count < 10) {
      String macStr;
      if (endPos == -1) {
        macStr = input.substring(startPos);
        startPos = input.length();
      } else {
        macStr = input.substring(startPos, endPos);
        startPos = endPos + 1;
        endPos = input.indexOf('\n', startPos);
      }
      
      macStr.trim();
      if (macStr.length() > 0) {
        if (stringToMac(macStr, config.slave_macs[config.slave_count])) {
          Serial.printf("Slave MAC %d: %s\n", config.slave_count + 1, macStr.c_str());
          config.slave_count++;
        } else {
          Serial.printf("Invalid MAC format: %s\n", macStr.c_str());
        }
      }
    }
    Serial.printf("Total slaves configured: %d\n", config.slave_count);
  }
}

void wifiSsidCallback(Control* sender, int type) {
  Serial.printf("WiFi SSID callback triggered - Type: %d, Value: '%s'\n", type, sender->value.c_str());
  
  strncpy(config.wifi_ssid, sender->value.c_str(), sizeof(config.wifi_ssid) - 1);
  config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
  Serial.printf("WiFi SSID set to: '%s'\n", config.wifi_ssid);
}

void wifiPasswordCallback(Control* sender, int type) {
  Serial.printf("WiFi Password callback triggered - Type: %d\n", type);
  
  strncpy(config.wifi_password, sender->value.c_str(), sizeof(config.wifi_password) - 1);
  config.wifi_password[sizeof(config.wifi_password) - 1] = '\0';
  Serial.println("WiFi password updated (hidden for security)");
}

void saveConfigCallback(Control* sender, int type) {
  if (type == B_DOWN) {
    Serial.println("Save button pressed - preparing configuration...");
    
    // Force read all form values before saving (ESPUI workaround)
    Serial.println("Current config state before forced updates:");
    Serial.printf("  Is Master: %s\n", config.is_master ? "true" : "false");
    Serial.printf("  Module Type: %d\n", config.module_type);
    Serial.printf("  Device Name: '%s'\n", config.device_name);
    
    // Manual form value reading as backup
    // Note: This is a workaround for ESPUI callback issues
    
    // Ensure configuration is marked as configured
    config.configured = true;
    config.create_ap = (strlen(config.wifi_ssid) == 0); // Use AP if no WiFi configured
    
    // Debug: Print configuration before saving
    Serial.println("Final configuration to save:");
    Serial.printf("  Device Name: '%s'\n", config.device_name);
    Serial.printf("  Is Master: %s\n", config.is_master ? "true" : "false");
    Serial.printf("  Module Type: %d (%s)\n", config.module_type, getModuleTypeName(config.module_type).c_str());
    Serial.printf("  Device ID: %d\n", config.device_id);
    Serial.printf("  WiFi SSID: '%s'\n", config.wifi_ssid);
    
    // Save configuration with full debugging
    saveConfiguration();
    
    // Update UI
    ESPUI.updateButton(sender->id, "Saved! Restarting...");
    Serial.println("Configuration saved - restarting in 5 seconds to see debug output");
    
    // Longer delay to see debug output
    delay(5000);
    ESP.restart();
  }
}

void resetConfigCallback(Control* sender, int type) {
  if (type == B_DOWN) {
    config = DeviceConfig(); // Reset to defaults
    ESPUI.updateButton(sender->id, "Reset! Restarting...");
    Serial.println("Configuration reset - restarting in 3 seconds");
    
    // if (displayPtr) {
    //   displayPtr->clearDisplay();
    //   displayPtr->setCursor(0, 0);
    //   displayPtr->println("Configuration");
    //   displayPtr->println("Reset!");
    //   displayPtr->println("Restarting...");
    //   displayPtr->display();
    // }
    
    delay(3000);
    ESP.restart();
  }
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
      // if (displayPtr) {
      //   displayPtr->clearDisplay();
      //   displayPtr->setCursor(0, 0);
      //   displayPtr->printf("AP: %s\nIP: %s", ap_ssid, WiFi.softAPIP().toString().c_str());
      //   displayPtr->display();
      //   delay(2000);
      // }
    } else {
      Serial.println("AP start failed");
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
    } else {
      Serial.println("\nWiFi connection failed - falling back to AP mode");
      config.create_ap = true;
      setupNetwork(); // Retry with AP mode
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
  if (IS_MASTER) {
    // Master adds all configured slave devices as peers
    for (int i = 0; i < config.slave_count; i++) {
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, config.slave_macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      
      if (esp_now_add_peer(&peerInfo) == ESP_OK) {
        Serial.printf("Master: Slave peer added - %s\n", macToString(config.slave_macs[i]).c_str());
      } else {
        Serial.printf("Master: Failed to add slave peer - %s\n", macToString(config.slave_macs[i]).c_str());
      }
    }
  } else {
    // Slave adds only the master as a peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, config.master_mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      Serial.printf("Slave: Master peer added - %s\n", macToString(config.master_mac).c_str());
    } else {
      Serial.printf("Slave: Failed to add master peer - %s\n", macToString(config.master_mac).c_str());
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
  esp_err_t result = esp_now_send(config.master_mac, (uint8_t *)&message, sizeof(message));
  Serial.printf("Data sent to master: %s\n", result == ESP_OK ? "Success" : "Failed");
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
/*
void updateDisplay() {
  if (!IS_MASTER || !displayPtr || setupMode) return;
  
  int activeCount = 1; // Count this device (master)
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && (millis() - deviceLastSeen[i] <= DEVICE_TIMEOUT) && i != (DEVICE_ID - 1)) {
      activeCount++;
    }
  }
  
  displayPtr->clearDisplay();
  displayPtr->setCursor(0, 0);
  displayPtr->printf("%s - %d\n", config.device_name, DEVICE_ID);
  displayPtr->printf("Active: %d/%d\n", activeCount, config.slave_count + 1);
  
  if (config.create_ap) {
    displayPtr->printf("AP: %s\n%s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
  } else if (WiFi.status() == WL_CONNECTED) {
    displayPtr->printf("WiFi: %s\n%s\n", config.wifi_ssid, WiFi.localIP().toString().c_str());
  } else {
    displayPtr->println("Offline Mode");
  }
  
  displayPtr->println("---");
  
  // Show sensor data from active devices
  int shown = 0;
  for (int i = 0; i < MAX_PEERS && shown < 2; i++) {
    if (deviceActive[i] && deviceReadings[i].module_type > 0 && 
        (millis() - deviceLastSeen[i] <= DEVICE_TIMEOUT)) {
      displayPtr->printf("Dev%d: ", i + 1);
      
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
*/

// ==================== ESPUI FUNCTIONS ====================
void initESPUI() {
  ESPUI.setVerbosity(Verbosity::Quiet);
  
  // Configure ESPUI for better connection stability
  ESPUI.captivePortal = false; // Disable captive portal to reduce connection issues
  
  // Main tab
  uint16_t mainTab = ESPUI.addControl(ControlType::Tab, "Dashboard", "Dashboard");
  
  ESPUI.addControl(ControlType::Separator, "Network Status", "", ControlColor::None, mainTab);
  networkModeLabel = ESPUI.addControl(ControlType::Label, "Network Mode", 
                                     config.create_ap ? "Access Point" : "WiFi Client", 
                                     ControlColor::Emerald, mainTab);
  
  String ipAddr = config.create_ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  ipAddressLabel = ESPUI.addControl(ControlType::Label, "IP Address", ipAddr, ControlColor::Emerald, mainTab);
  activeDevicesLabel = ESPUI.addControl(ControlType::Label, "Active Devices", "1/" + String(config.slave_count + 1), 
                                       ControlColor::Alizarin, mainTab);
  uptimeLabel = ESPUI.addControl(ControlType::Label, "Uptime", "0s", ControlColor::Carrot, mainTab);
  refreshButton = ESPUI.addControl(ControlType::Button, "Refresh Data", "Refresh", 
                                  ControlColor::Peterriver, mainTab, &refreshButtonCallback);
  
  // Sensor Data Tab
  sensorDataTab = ESPUI.addControl(ControlType::Tab, "Sensor Data", "Sensor Data");
  
  // Create sensor data display for this device (master)
  if (MODULE_TYPE > 0) {
    ESPUI.addControl(ControlType::Separator, "This Device (Master)", "", ControlColor::None, sensorDataTab);
    statusLabels[DEVICE_ID - 1] = ESPUI.addControl(ControlType::Label, "Status", "🟢 Online", 
                                                  ControlColor::Emerald, sensorDataTab);
    
    if (MODULE_TYPE == 1) {
      temperatureLabels[DEVICE_ID - 1] = ESPUI.addControl(ControlType::Label, "Temperature", "--°C", 
                                                         ControlColor::Turquoise, sensorDataTab);
      humidityLabels[DEVICE_ID - 1] = ESPUI.addControl(ControlType::Label, "Humidity", "--%", 
                                                      ControlColor::Emerald, sensorDataTab);
    } else if (MODULE_TYPE == 2) {
      lightLabels[DEVICE_ID - 1] = ESPUI.addControl(ControlType::Label, "Light Level", "--%", 
                                                   ControlColor::Sunflower, sensorDataTab);
    }
  }
  
  // Create placeholders for slave devices
  for (int i = 0; i < config.slave_count; i++) {
    String deviceName = "Slave Device " + String(i + 1);
    String macAddr = macToString(config.slave_macs[i]);
    String sectionTitle = deviceName + " (" + macAddr + ")";
    
    ESPUI.addControl(ControlType::Separator, sectionTitle.c_str(), "", ControlColor::None, sensorDataTab);
    
    // We don't know the module type yet, so create all possible labels
    statusLabels[i + 1] = ESPUI.addControl(ControlType::Label, "Status", "🔴 Offline", 
                                          ControlColor::Alizarin, sensorDataTab);
    lastSeenLabels[i + 1] = ESPUI.addControl(ControlType::Label, "Last Seen", "Never", 
                                            ControlColor::Carrot, sensorDataTab);
    temperatureLabels[i + 1] = ESPUI.addControl(ControlType::Label, "Temperature", "--°C", 
                                               ControlColor::Turquoise, sensorDataTab);
    humidityLabels[i + 1] = ESPUI.addControl(ControlType::Label, "Humidity", "--%", 
                                           ControlColor::Emerald, sensorDataTab);
    lightLabels[i + 1] = ESPUI.addControl(ControlType::Label, "Light Level", "--%", 
                                         ControlColor::Sunflower, sensorDataTab);
  }
  
  // Device information tab
  uint16_t deviceTab = ESPUI.addControl(ControlType::Tab, "This Device", "This Device");
  ESPUI.addControl(ControlType::Separator, "Device Info", "", ControlColor::None, deviceTab);
  ESPUI.addControl(ControlType::Label, "Device Name", config.device_name, ControlColor::Wetasphalt, deviceTab);
  ESPUI.addControl(ControlType::Label, "Device Type", IS_MASTER ? "Master" : "Slave", ControlColor::Wetasphalt, deviceTab);
  ESPUI.addControl(ControlType::Label, "Module Type", getModuleTypeName(config.module_type), ControlColor::Wetasphalt, deviceTab);
  ESPUI.addControl(ControlType::Label, "Device ID", String(config.device_id), ControlColor::Wetasphalt, deviceTab);
  ESPUI.addControl(ControlType::Label, "MAC Address", WiFi.macAddress(), ControlColor::Wetasphalt, deviceTab);
  
  if (IS_MASTER) {
    ESPUI.addControl(ControlType::Separator, "Connected Slaves", "", ControlColor::None, deviceTab);
    for (int i = 0; i < config.slave_count; i++) {
      String slaveLabel = "Slave " + String(i + 1);
      ESPUI.addControl(ControlType::Label, slaveLabel.c_str(), macToString(config.slave_macs[i]), 
                      ControlColor::Turquoise, deviceTab);
    }
  } else {
    ESPUI.addControl(ControlType::Label, "Master MAC", macToString(config.master_mac), ControlColor::Turquoise, deviceTab);
  }
  
  // Settings tab
  uint16_t configTab = ESPUI.addControl(ControlType::Tab, "Settings", "Settings");
  ESPUI.addControl(ControlType::Separator, "Configuration", "", ControlColor::None, configTab);
  
  updateIntervalSlider = ESPUI.addControl(ControlType::Slider, "Update Interval (seconds)", 
                                         String(currentUpdateInterval), ControlColor::Alizarin, configTab, 
                                         &updateIntervalCallback);
  ESPUI.addControl(ControlType::Min, "", "5", ControlColor::None, updateIntervalSlider);
  ESPUI.addControl(ControlType::Max, "", "60", ControlColor::None, updateIntervalSlider);
  
  ESPUI.addControl(ControlType::Separator, "Actions", "", ControlColor::None, configTab);
  ESPUI.addControl(ControlType::Button, "Factory Reset", "Reset Device", 
                  ControlColor::Alizarin, configTab, &resetConfigCallback);
  
  ESPUI.begin("ESP32 Sensor Network");
  
  // Add connection stability settings
  Serial.println("ESPUI started with connection stability improvements");
  Serial.println("If you see rapid connect/disconnect after page refresh, wait 10-15 seconds for stabilization");
}


void updateUI() {
  if (!IS_MASTER || setupMode) return;
  
  // Check WebSocket stability - avoid updates during connection instability
  unsigned long currentTime = millis();
  if (currentTime - lastWebSocketEvent < 10000) { // Wait 10 seconds after last WS event
    if (!webSocketStable) {
      Serial.println("WebSocket unstable - skipping UI update to prevent flickering");
      return;
    }
  } else {
    webSocketStable = true; // Mark as stable after 10 seconds of no events
  }
  
  static int lastActiveCount = -1;
  static bool firstRun = true;
  
  int activeCount = 1; // Count this device (master)
  
  // Count active slave devices and update their status
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT) && i != (DEVICE_ID - 1)) {
      activeCount++;
    } else if (deviceActive[i] && (currentTime - deviceLastSeen[i] > DEVICE_TIMEOUT)) {
      deviceActive[i] = false;
    }
  }
  
  // Update main dashboard only if count changed
  if (activeCount != lastActiveCount || firstRun) {
    ESPUI.updateLabel(activeDevicesLabel, String(activeCount) + "/" + String(config.slave_count + 1));
    lastActiveCount = activeCount;
  }
  
  // Update uptime less frequently
  static unsigned long lastUptimeUpdate = 0;
  if (currentTime - lastUptimeUpdate >= 10000 || firstRun) { // Every 10 seconds
    ESPUI.updateLabel(uptimeLabel, formatUptime(currentTime));
    lastUptimeUpdate = currentTime;
  }
  
  // Update sensor data for all devices
  updateSensorDataUI(firstRun);
  
  firstRun = false;
}

void updateSensorDataUI(bool forceUpdate) {
  unsigned long currentTime = millis();
  static float lastTemps[MAX_PEERS] = {-999};
  static float lastHumidity[MAX_PEERS] = {-999};
  static float lastLight[MAX_PEERS] = {-999};
  static bool lastStates[MAX_PEERS] = {false};
  static unsigned long lastSeenUpdateTime[MAX_PEERS] = {0};
  
  // Update master device sensor data (if it has sensors) - less frequently
  if (MODULE_TYPE > 0) {
    int idx = DEVICE_ID - 1;
    bool isActive = deviceActive[idx];
    
    // Only update status if it changed
    if (statusLabels[idx] != 0 && (lastStates[idx] != isActive || forceUpdate)) {
      ESPUI.updateLabel(statusLabels[idx], isActive ? "🟢 Online" : "🔴 Offline");
      lastStates[idx] = isActive;
    }
    
    // Update sensor readings only if values changed significantly
    if (isActive && deviceReadings[idx].module_type > 0) {
      switch (deviceReadings[idx].module_type) {
        case 1: // Temperature & Humidity
          if (temperatureLabels[idx] != 0 && deviceReadings[idx].temperature > -900 && 
              (forceUpdate || abs(lastTemps[idx] - deviceReadings[idx].temperature) > 0.5)) {
            ESPUI.updateLabel(temperatureLabels[idx], String(deviceReadings[idx].temperature, 1) + "°C");
            lastTemps[idx] = deviceReadings[idx].temperature;
          }
          if (humidityLabels[idx] != 0 && deviceReadings[idx].humidity > -900 && 
              (forceUpdate || abs(lastHumidity[idx] - deviceReadings[idx].humidity) > 1.0)) {
            ESPUI.updateLabel(humidityLabels[idx], String(deviceReadings[idx].humidity, 1) + "%");
            lastHumidity[idx] = deviceReadings[idx].humidity;
          }
          break;
        case 2: // Light sensor
          if (lightLabels[idx] != 0 && deviceReadings[idx].light_value > -900 && 
              (forceUpdate || abs(lastLight[idx] - deviceReadings[idx].light_value) > 2.0)) {
            ESPUI.updateLabel(lightLabels[idx], String(deviceReadings[idx].light_value, 1) + "%");
            lastLight[idx] = deviceReadings[idx].light_value;
          }
          break;
      }
    }
  }
  
  // Update slave device data - even less frequently and more selectively
  for (int i = 0; i < MAX_PEERS; i++) {
    if (i == (DEVICE_ID - 1)) continue; // Skip master device
    
    bool wasActive = lastStates[i];
    bool isActive = deviceActive[i] && (currentTime - deviceLastSeen[i] <= DEVICE_TIMEOUT);
    
    // Update device status only if it actually changed
    if (statusLabels[i] != 0 && (wasActive != isActive || forceUpdate)) {
      ESPUI.updateLabel(statusLabels[i], isActive ? "🟢 Online" : "🔴 Offline");
      lastStates[i] = isActive;
    }
    
    // Update last seen time only every 30 seconds to reduce updates
    if (lastSeenLabels[i] != 0 && (currentTime - lastSeenUpdateTime[i] >= 30000 || forceUpdate)) {
      if (isActive) {
        ESPUI.updateLabel(lastSeenLabels[i], getTimeAgo(deviceLastSeen[i]));
      } else {
        ESPUI.updateLabel(lastSeenLabels[i], deviceActive[i] ? "Disconnected" : "Never");
      }
      lastSeenUpdateTime[i] = currentTime;
    }
    
    // Update sensor readings only if device is active AND values changed significantly
    if (isActive && deviceReadings[i].module_type > 0) {
      switch (deviceReadings[i].module_type) {
        case 1: // Temperature & Humidity
          if (temperatureLabels[i] != 0 && deviceReadings[i].temperature > -900 && 
              (forceUpdate || abs(lastTemps[i] - deviceReadings[i].temperature) > 0.5)) {
            ESPUI.updateLabel(temperatureLabels[i], String(deviceReadings[i].temperature, 1) + "°C");
            lastTemps[i] = deviceReadings[i].temperature;
          }
          if (humidityLabels[i] != 0 && deviceReadings[i].humidity > -900 && 
              (forceUpdate || abs(lastHumidity[i] - deviceReadings[i].humidity) > 1.0)) {
            ESPUI.updateLabel(humidityLabels[i], String(deviceReadings[i].humidity, 1) + "%");
            lastHumidity[i] = deviceReadings[i].humidity;
          }
          break;
        case 2: // Light sensor
          if (lightLabels[i] != 0 && deviceReadings[i].light_value > -900 && 
              (forceUpdate || abs(lastLight[i] - deviceReadings[i].light_value) > 2.0)) {
            ESPUI.updateLabel(lightLabels[i], String(deviceReadings[i].light_value, 1) + "%");
            lastLight[i] = deviceReadings[i].light_value;
          }
          break;
      }
    } else if (!isActive && (wasActive || forceUpdate)) {
      // Device just went offline, update readings to show no data (only once)
      if (temperatureLabels[i] != 0 && lastTemps[i] != -999) {
        ESPUI.updateLabel(temperatureLabels[i], "--°C");
        lastTemps[i] = -999;
      }
      if (humidityLabels[i] != 0 && lastHumidity[i] != -999) {
        ESPUI.updateLabel(humidityLabels[i], "--%");
        lastHumidity[i] = -999;
      }
      if (lightLabels[i] != 0 && lastLight[i] != -999) {
        ESPUI.updateLabel(lightLabels[i], "--%");
        lastLight[i] = -999;
      }
    }
  }
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
  if (setupMode) {
    return String(config.device_name);
  }
  return String("Device") + String(id);
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
    
    // Check separator (except for last byte)
    if (i < 5 && str.charAt(i * 3 + 2) != ':') {
      return false;
    }
  }
  
  return true;
}

// ==================== SETUP BUTTON FUNCTION ====================
void checkSetupButton() {
  bool currentButtonState = digitalRead(SETUP_BUTTON_PIN);
  
  // Button pressed (LOW because of INPUT_PULLUP)
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    buttonPressTime = millis();
    Serial.println("Setup button pressed - hold for 3 seconds to enter setup mode");
  }
  
  // Button held down
  if (currentButtonState == LOW && lastButtonState == LOW) {
    unsigned long holdTime = millis() - buttonPressTime;
    
    // Check if held for required time
    if (holdTime >= BUTTON_HOLD_TIME) {
      Serial.println("Setup button held for 3 seconds - entering setup mode");
      
      // Clear configuration and restart in setup mode
      config = DeviceConfig();
      saveConfiguration();
      
      Serial.println("Configuration cleared - restarting in setup mode");
      delay(1000);
      ESP.restart();
    }
  }
  
  // Button released
  if (currentButtonState == HIGH && lastButtonState == LOW) {
    unsigned long holdTime = millis() - buttonPressTime;
    if (holdTime < BUTTON_HOLD_TIME) {
      Serial.printf("Setup button released after %.1f seconds (need 3.0s)\n", holdTime / 1000.0);
    }
  }
  
  lastButtonState = currentButtonState;
}

// ==================== WEBSOCKET EVENT HANDLER ====================
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  lastWebSocketEvent = millis();
  
  switch(type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      webSocketStable = false; // Mark as unstable during connection
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      webSocketStable = false; // Mark as unstable during disconnection
      break;
    case WS_EVT_ERROR:
      Serial.printf("WebSocket client #%u error(%u): %s\n", client->id(), *((uint16_t*)arg), (char*)data);
      webSocketStable = false;
      break;
    case WS_EVT_PONG:
    case WS_EVT_DATA:
      // Data events are normal, don't reset stability
      break;
  }
}