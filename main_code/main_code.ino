#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// ==================== CONFIGURATION ====================
// Set to true for master device, false for slave
const bool IS_MASTER = true;

// Access Point mode configuration (Master only)
const bool CREATE_AP = true;  // Set to true to create an AP, false to connect to existing WiFi

// Device IDs - must be unique for each device in your network
const uint8_t DEVICE_ID = 1; // 1 for master, 2+ for slaves

// Update interval in milliseconds
const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000; // Update display more frequently

// ==================== WIFI & AP SETTINGS ====================
// For connecting to existing WiFi (when CREATE_AP = false)
const char* wifi_ssid = "TP-Link_20C8";
const char* wifi_password = "70458572";

// For creating an Access Point (when CREATE_AP = true)
const char* ap_ssid = "ESP32-SensorHub";  // Name of the AP
const char* ap_password = "sensornetwork";  // Password for the AP (8+ characters or blank for open network)
const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

// Set a hostname for the master device
const char* hostname = "esp32-sensor-hub";

// Create web server object on port 80
WebServer server(80);

// ==================== DISPLAY SETTINGS ====================
#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels
#define OLED_ADDRESS 0x3C // I2C address of the OLED display

// Only declare and use the display object if this is a master
Adafruit_SSD1306* displayPtr = NULL;

// ==================== ESP-NOW SETTINGS ====================
// Maximum number of peers in the network
#define MAX_PEERS 20

// Known device MAC addresses - add all your devices here
// Format: {device_id, MAC address}
typedef struct {
  uint8_t id;
  uint8_t address[6];
  char name[32]; // Friendly name for the device
} device_info;

// Add all device MACs here - master should be first
device_info devices[] = {
  {1, {0xd4, 0x8a, 0xfc, 0x9f, 0x2f, 0x98}, "Master"}, // Master
  {2, {0x94, 0xb5, 0x55, 0xf9, 0xff, 0xf0}, "Outdoor"} // Slave 1
  // Add more slaves as needed
};
const int NUM_DEVICES = sizeof(devices) / sizeof(device_info);

// ==================== DATA STRUCTURES ====================
// Data structure for sending readings
typedef struct sensor_message {
  uint8_t sender_id;       // ID of the sending device
  float temperature;
  float humidity;
  float pressure;
  float battery_voltage;   // Optional battery level monitoring
  unsigned long timestamp; // Milliseconds since boot
} sensor_message;

// Array to store the latest readings from each device
sensor_message deviceReadings[MAX_PEERS];
bool deviceActive[MAX_PEERS] = {false}; // Track which devices are active
unsigned long deviceLastSeen[MAX_PEERS] = {0}; // Last time each device was seen

// Variable to store transmission status
String lastTransmitStatus = "None";

// ==================== FUNCTION DECLARATIONS ====================
void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &pres, float &batt);
void sendReadings();
void updateDisplay();
void handleReceivedData(const sensor_message &reading);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

// WiFi and web server functions (Master only)
void setupWiFi();
void setupAccessPoint();
void initWebServer();
void handleRoot();
void handleData();
void handleNotFound();
String getDeviceName(uint8_t id);
String getTimeAgo(unsigned long timestamp);
String getStatusHTML();

// ==================== SETUP ====================
void setup() {
  // Initialize Serial
  Serial.begin(115200);
  Serial.println();
  Serial.print("Initializing ");
  Serial.println(IS_MASTER ? "MASTER" : "SLAVE");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  // Initialize device array with invalid values
  for (int i = 0; i < MAX_PEERS; i++) {
    deviceReadings[i].sender_id = 0;
    deviceReadings[i].temperature = -999;
    deviceReadings[i].humidity = -999;
    deviceReadings[i].pressure = -999;
    deviceReadings[i].battery_voltage = -999;
    deviceReadings[i].timestamp = 0;
  }

  // Set this device's own reading
  deviceReadings[DEVICE_ID - 1].sender_id = DEVICE_ID;
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();

  // Initialize I2C
  Wire.begin();
  
  // If master, initialize display and WiFi
  if (IS_MASTER) {
    displayPtr = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
    
    if (!displayPtr->begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
      Serial.println(F("SSD1306 allocation failed"));
      // Clean up if initialization failed
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
    
    // Initialize web server
    initWebServer();
  }

  // Initialize ESP-NOW
  initESPNow();
}

// ==================== MAIN LOOP ====================
unsigned long lastSendTime = 0;
unsigned long lastDisplayUpdate = 0;

void loop() {
  unsigned long currentTime = millis();
  
  // Check if it's time to send data
  if (currentTime - lastSendTime >= UPDATE_INTERVAL) {
    // Get sensor readings
    float temp, hum, pres, batt;
    getSensorReadings(temp, hum, pres, batt);
    
    // Update local device readings
    deviceReadings[DEVICE_ID - 1].temperature = temp;
    deviceReadings[DEVICE_ID - 1].humidity = hum;
    deviceReadings[DEVICE_ID - 1].pressure = pres;
    deviceReadings[DEVICE_ID - 1].battery_voltage = batt;
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
    
    // Handle client requests
    server.handleClient();
  }
  
  // Small delay to prevent CPU hogging
  delay(10);
}

// ==================== WIFI/AP FUNCTIONS ====================
void setupWiFi() {
  // Connect to existing WiFi network
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  Serial.print("Connecting to WiFi");
  
  // Wait for connection
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
    
    // Set up mDNS responder
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
  // Create Access Point
  WiFi.mode(WIFI_AP_STA);
  
  // Configure the access point
  WiFi.softAPConfig(ap_ip, ap_gateway, ap_subnet);
  
  // Start the access point
  bool apStarted = false;
  if (strlen(ap_password) >= 8) {
    // Secure access point with password
    apStarted = WiFi.softAP(ap_ssid, ap_password);
  } else {
    // Open access point without password
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
  // ESP-NOW initialization (WiFi mode is already set in WiFi/AP setup for master)
  if (!IS_MASTER) {
    // For slaves, set WiFi mode to station only
    WiFi.mode(WIFI_STA);
  }
  
  // Print MAC address
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  // Register callbacks
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  
  // Register peers
  registerPeers();
}

void registerPeers() {
  // Register all devices as peers (except ourselves)
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == DEVICE_ID) continue; // Skip ourselves
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, devices[i].address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    // Add peer
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
      Serial.print("Peer added successfully: Device ");
      Serial.print(devices[i].id);
      Serial.print(" (");
      Serial.print(devices[i].name);
      Serial.println(")");
    } else {
      Serial.print("Failed to add peer: Device ");
      Serial.print(devices[i].id);
      Serial.print(" (");
      Serial.print(devices[i].name);
      Serial.println(")");
    }
  }
}

// ==================== SENSOR FUNCTIONS ====================
void getSensorReadings(float &temp, float &hum, float &pres, float &batt) {
  // Replace with actual sensor readings
  // For now using dummy values
  temp = 25.0 + (random(100) - 50) / 10.0; // 20.0 - 30.0°C
  hum = 50.0 + (random(200) - 100) / 10.0; // 40.0 - 60.0%
  pres = 1000.0 + (random(100) - 50) / 10.0; // 995.0 - 1005.0 hPa
  
  // Read battery voltage if available
  // Assuming a voltage divider on pin 34 (ADC)
  // Adjust the formula based on your hardware setup
  float adcValue = analogRead(34);
  batt = adcValue * 3.3 * 2 / 4095.0; // Example formula, adjust as needed
}

// ==================== COMMUNICATION FUNCTIONS ====================
void sendReadings() {
  // Prepare message
  sensor_message message;
  message.sender_id = DEVICE_ID;
  message.temperature = deviceReadings[DEVICE_ID - 1].temperature;
  message.humidity = deviceReadings[DEVICE_ID - 1].humidity;
  message.pressure = deviceReadings[DEVICE_ID - 1].pressure;
  message.battery_voltage = deviceReadings[DEVICE_ID - 1].battery_voltage;
  message.timestamp = millis();
  
  // Send to all registered peers
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == DEVICE_ID) continue; // Skip ourselves
    
    esp_err_t result = esp_now_send(devices[i].address, (uint8_t *)&message, sizeof(message));
    
    if (result == ESP_OK) {
      Serial.print("Sent to device ");
      Serial.println(devices[i].id);
    } else {
      Serial.print("Failed to send to device ");
      Serial.println(devices[i].id);
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

// Updated callback function for newer ESP-NOW API
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len == sizeof(sensor_message)) {
    // Cast incoming data to our structure
    sensor_message *incomingData = (sensor_message*)data;
    
    // Process received data
    handleReceivedData(*incomingData);
    
    // Debug output
    Serial.print("Received data from Device ");
    Serial.println(incomingData->sender_id);
    Serial.print("Temperature: ");
    Serial.println(incomingData->temperature);
    Serial.print("Humidity: ");
    Serial.println(incomingData->humidity);
    Serial.print("Pressure: ");
    Serial.println(incomingData->pressure);
    Serial.print("Battery: ");
    Serial.println(incomingData->battery_voltage);
    Serial.println();
  }
}

void handleReceivedData(const sensor_message &reading) {
  // Store data in our array if sender_id is valid
  if (reading.sender_id > 0 && reading.sender_id <= MAX_PEERS) {
    deviceReadings[reading.sender_id - 1] = reading;
    deviceActive[reading.sender_id - 1] = true;
    deviceLastSeen[reading.sender_id - 1] = millis();
  }
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplay() {
  // Only proceed if this is master and display is initialized
  if (!IS_MASTER || displayPtr == NULL) return;
  
  // Calculate how many active devices we have
  int activeCount = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i]) activeCount++;
  }
  
  // Update the display
  displayPtr->clearDisplay();
  displayPtr->setCursor(0, 0);
  displayPtr->print("ESP32 Network - ");
  displayPtr->println(DEVICE_ID);
  
  // Display active device count
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
  
  // We'll show the master's data plus up to 1 slave on the display
  // Master's data first
  int deviceIndex = DEVICE_ID - 1;
  displayPtr->println("-------------------");
  displayPtr->print(getDeviceName(DEVICE_ID));
  displayPtr->print(": ");
  displayPtr->print(deviceReadings[deviceIndex].temperature, 1);
  displayPtr->print("C ");
  displayPtr->print(deviceReadings[deviceIndex].humidity, 1);
  displayPtr->println("%");
  
  // Show one other active device if available
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i] && i != deviceIndex) {
      displayPtr->print(getDeviceName(i+1));
      displayPtr->print(": ");
      displayPtr->print(deviceReadings[i].temperature, 1);
      displayPtr->print("C ");
      displayPtr->print(deviceReadings[i].humidity, 1);
      displayPtr->println("%");
      break; // Only show one other device
    }
  }
  
  displayPtr->display();
}

// ==================== WEB SERVER FUNCTIONS ====================
void initWebServer() {
  // Set up web server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  
  // Start the server
  server.begin();
  Serial.println("HTTP server started");
}

void handleRoot() {
  String html = "<!DOCTYPE html>\n";
  html += "<html lang='en'>\n";
  html += "<head>\n";
  html += "  <meta charset='UTF-8'>\n";
  html += "  <meta name='viewport' content='width=device-width, initial-scale=1.0'>\n";
  html += "  <title>ESP32 Sensor Network</title>\n";
  html += "  <style>\n";
  html += "    body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f4f4f4; }\n";
  html += "    h1 { color: #333; text-align: center; }\n";
  html += "    .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n";
  html += "    .device { margin-bottom: 20px; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }\n";
  html += "    .device h2 { margin-top: 0; color: #0066cc; }\n";
  html += "    .status { display: flex; justify-content: space-between; margin-bottom: 10px; }\n";
  html += "    .reading { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid #eee; }\n";
  html += "    .reading:last-child { border-bottom: none; }\n";
  html += "    .badge { display: inline-block; padding: 3px 8px; border-radius: 12px; font-size: 12px; font-weight: bold; }\n";
  html += "    .badge-success { background-color: #28a745; color: white; }\n";
  html += "    .badge-danger { background-color: #dc3545; color: white; }\n";
  html += "    .refresh-btn { display: block; width: 100%; padding: 10px; background: #0066cc; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 15px; }\n";
  html += "    .footer { text-align: center; margin-top: 20px; font-size: 12px; color: #666; }\n";
  html += "    .mode-info { text-align: center; padding: 5px; background-color: #f8f9fa; margin-bottom: 15px; border-radius: 5px; }\n";
  html += "  </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "  <div class='container'>\n";
  html += "    <h1>ESP32 Sensor Network</h1>\n";
  
  // Display network mode
  html += "    <div class='mode-info'>\n";
  html += "      <p>Network Mode: <strong>";
  html += CREATE_AP ? "Access Point" : "WiFi Client";
  html += "</strong></p>\n";
  html += "      <p>IP Address: <strong>";
  html += CREATE_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  html += "</strong></p>\n";
  html += "    </div>\n";
  
  html += "    <div id='status-container'>\n";
  html += getStatusHTML();
  html += "    </div>\n";
  html += "    <button class='refresh-btn' onclick='refreshData()'>Refresh Data</button>\n";
  html += "  </div>\n";
  html += "  <div class='footer'>ESP32 Sensor Network &copy; " + String(2025) + "</div>\n";
  html += "  <script>\n";
  html += "    function refreshData() {\n";
  html += "      fetch('/data')\n";
  html += "        .then(response => response.text())\n";
  html += "        .then(data => {\n";
  html += "          document.getElementById('status-container').innerHTML = data;\n";
  html += "        })\n";
  html += "        .catch(error => console.error('Error fetching data:', error));\n";
  html += "    }\n";
  html += "    // Auto refresh every 10 seconds\n";
  html += "    setInterval(refreshData, 10000);\n";
  html += "  </script>\n";
  html += "</body>\n";
  html += "</html>\n";
  
  server.send(200, "text/html", html);
}

void handleData() {
  server.send(200, "text/html", getStatusHTML());
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  
  server.send(404, "text/plain", message);
}

String getStatusHTML() {
  String html = "";
  
  // Calculate how many active devices we have
  int activeCount = 0;
  unsigned long currentTime = millis();
  const unsigned long TIMEOUT = 5 * 60 * 1000; // 5 minutes timeout
  
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i]) {
      // Check if device is still active (timeout)
      if (currentTime - deviceLastSeen[i] > TIMEOUT) {
        deviceActive[i] = false;
      } else {
        activeCount++;
      }
    }
  }
  
  // Add network status
  html += "<div class='device'>\n";
  html += "  <h2>Network Status</h2>\n";
  html += "  <div class='status'>\n";
  html += "    <span>Active Devices:</span>\n";
  html += "    <span>" + String(activeCount) + "/" + String(NUM_DEVICES) + "</span>\n";
  html += "  </div>\n";
  html += "  <div class='status'>\n";
  html += "    <span>Last Update:</span>\n";
  html += "    <span>" + String(currentTime / 1000) + " seconds</span>\n";
  html += "  </div>\n";
  html += "</div>\n";
  
  // Add each device's data
  for (int i = 0; i < NUM_DEVICES; i++) {
    uint8_t id = devices[i].id;
    int idx = id - 1; // index in the readings array
    
    if (idx >= 0 && idx < MAX_PEERS) {
      html += "<div class='device'>\n";
      html += "  <h2>" + getDeviceName(id) + " (ID: " + String(id) + ")</h2>\n";
      
      // Device status
      html += "  <div class='status'>\n";
      html += "    <span>Status:</span>\n";
      
      bool isActive = deviceActive[idx] && (currentTime - deviceLastSeen[idx] <= TIMEOUT);
      
      if (isActive) {
        html += "    <span class='badge badge-success'>Online</span>\n";
      } else {
        html += "    <span class='badge badge-danger'>Offline</span>\n";
      }
      
      html += "  </div>\n";
      
      if (isActive) {
        // Last seen
        html += "  <div class='status'>\n";
        html += "    <span>Last Seen:</span>\n";
        html += "    <span>" + getTimeAgo(deviceLastSeen[idx]) + "</span>\n";
        html += "  </div>\n";
        
        // Readings
        html += "  <div class='reading'>\n";
        html += "    <span>Temperature:</span>\n";
        html += "    <span>" + String(deviceReadings[idx].temperature, 1) + " °C</span>\n";
        html += "  </div>\n";
        
        html += "  <div class='reading'>\n";
        html += "    <span>Humidity:</span>\n";
        html += "    <span>" + String(deviceReadings[idx].humidity, 1) + " %</span>\n";
        html += "  </div>\n";
        
        if (deviceReadings[idx].pressure > -900) {
          html += "  <div class='reading'>\n";
          html += "    <span>Pressure:</span>\n";
          html += "    <span>" + String(deviceReadings[idx].pressure, 1) + " hPa</span>\n";
          html += "  </div>\n";
        }
        
        if (deviceReadings[idx].battery_voltage > 0) {
          html += "  <div class='reading'>\n";
          html += "    <span>Battery:</span>\n";
          html += "    <span>" + String(deviceReadings[idx].battery_voltage, 2) + " V</span>\n";
          html += "  </div>\n";
        }
      } else {
        html += "  <div class='status'>\n";
        html += "    <span>No data available</span>\n";
        html += "  </div>\n";
      }
      
      html += "</div>\n";
    }
  }
  
  return html;
}

String getDeviceName(uint8_t id) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == id) {
      return String(devices[i].name);
    }
  }
  return "Unknown Device";
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