#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// ==================== CONFIGURATION ====================
const bool IS_MASTER = true;
const bool CREATE_AP = true; 

const uint8_t DEVICE_ID = 1; 
const uint8_t MODULE_TYPE = 2;


const unsigned long UPDATE_INTERVAL = 10000;
const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;

// ==================== WIFI & AP SETTINGS ====================
const char* wifi_ssid = "YOUR_WIFI_SSID";
const char* wifi_password = "YOUR_WIFI_PASSWORD";

const char* ap_ssid = "ESP32-SensorHub";
const char* ap_password = "sensornetwork";
const IPAddress ap_ip(192, 168, 4, 1);
const IPAddress ap_gateway(192, 168, 4, 1);
const IPAddress ap_subnet(255, 255, 255, 0);

const char* hostname = "esp32-sensor-hub";
WebServer server(80);

// ==================== DISPLAY SETTINGS ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C
Adafruit_SSD1306* displayPtr = NULL;

// ==================== ESP-NOW SETTINGS ====================
#define MAX_PEERS 20

typedef struct {
  uint8_t id;
  uint8_t address[6];
  char name[32];
  uint8_t module_type;


device_info devices[] = {
  {1, {0xd4, 0x8a, 0xfc, 0x9f, 0x2f, 0x98}, "Master", 0},     // Master - no sensors
  {2, {0x94, 0xb5, 0x55, 0xf9, 0xff, 0xf0}, "Outdoor", 1},   // Temp/Humidity sensor
  {3, {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}, "Indoor", 2}     // Light sensor
};
const int NUM_DEVICES = sizeof(devices) / sizeof(device_info);

// ==================== DATA STRUCTURES ====================
typedef struct sensor_message {
  uint8_t sender_id;
  uint8_t module_type;     
  float temperature;       
  float humidity;         
  float light_value;       
  unsigned long timestamp;
} sensor_message;


sensor_message deviceReadings[MAX_PEERS];
bool deviceActive[MAX_PEERS] = {false};
unsigned long deviceLastSeen[MAX_PEERS] = {0};

String lastTransmitStatus = "None";

// ==================== FUNCTION DECLARATIONS ====================
void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &light);
void sendReadings();
void updateDisplay();
void handleReceivedData(const sensor_message &reading);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

void setupWiFi();
void setupAccessPoint();
void initWebServer();
void handleRoot();
void handleData();
void handleNotFound();
String getDeviceName(uint8_t id);
String getModuleTypeName(uint8_t type);
String getTimeAgo(unsigned long timestamp);
String getStatusHTML();

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

  for (int i = 0; i < MAX_PEERS; i++) {
    deviceReadings[i].sender_id = 0;
    deviceReadings[i].module_type = 0;
    deviceReadings[i].temperature = -999;
    deviceReadings[i].humidity = -999;
    deviceReadings[i].light_value = -999;
    deviceReadings[i].timestamp = 0;
  }

  deviceReadings[DEVICE_ID - 1].sender_id = DEVICE_ID;
  deviceReadings[DEVICE_ID - 1].module_type = MODULE_TYPE;
  deviceActive[DEVICE_ID - 1] = true;
  deviceLastSeen[DEVICE_ID - 1] = millis();


  Wire.begin();
  
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
   
    if (CREATE_AP) {
      setupAccessPoint();
    } else {
      setupWiFi();
    }
    
    initWebServer();
  }
  initESPNow();
}

// ==================== MAIN LOOP ====================
unsigned long lastSendTime = 0;
unsigned long lastDisplayUpdate = 0;

void loop() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastSendTime >= UPDATE_INTERVAL && MODULE_TYPE > 0) {
    float temp, hum, light;
    getSensorReadings(temp, hum, light);
    
    deviceReadings[DEVICE_ID - 1].temperature = temp;
    deviceReadings[DEVICE_ID - 1].humidity = hum;
    deviceReadings[DEVICE_ID - 1].light_value = light;
    deviceReadings[DEVICE_ID - 1].timestamp = currentTime;
    deviceLastSeen[DEVICE_ID - 1] = currentTime;
    
    sendReadings();
    
    lastSendTime = currentTime;
  }
  
  if (IS_MASTER) {
    if (currentTime - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
      if (displayPtr != NULL) {
        updateDisplay();
      }
      lastDisplayUpdate = currentTime;
    }
    
    server.handleClient();
  }
  
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
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == DEVICE_ID) continue;
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, devices[i].address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
      Serial.print("Peer added successfully: Device ");
      Serial.print(devices[i].id);
      Serial.print(" (");
      Serial.print(devices[i].name);
      Serial.print(" - ");
      Serial.print(getModuleTypeName(devices[i].module_type));
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
void getSensorReadings(float &temp, float &hum, float &light) {
  temp = -999;
  hum = -999;
  light = -999;
  
  switch (MODULE_TYPE) {
    case 0:
      break;
      
    case 1: {
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
      int rawValue = analogRead(A0);
      light = (rawValue / 4095.0) * 100.0; 
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
  if (MODULE_TYPE == 0) return;
  
  sensor_message message;
  message.sender_id = DEVICE_ID;
  message.module_type = MODULE_TYPE;
  message.temperature = deviceReadings[DEVICE_ID - 1].temperature;
  message.humidity = deviceReadings[DEVICE_ID - 1].humidity;
  message.light_value = deviceReadings[DEVICE_ID - 1].light_value;
  message.timestamp = millis();
  
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].id == DEVICE_ID) continue;
    
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

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  if (len == sizeof(sensor_message)) {
    sensor_message *incomingData = (sensor_message*)data;
    
    handleReceivedData(*incomingData);
    
    Serial.print("Received data from Device ");
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
    Serial.println();
  }
}

void handleReceivedData(const sensor_message &reading) {
  if (reading.sender_id > 0 && reading.sender_id <= MAX_PEERS) {
    deviceReadings[reading.sender_id - 1] = reading;
    deviceActive[reading.sender_id - 1] = true;
    deviceLastSeen[reading.sender_id - 1] = millis();
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

// ==================== WEB SERVER FUNCTIONS ====================
void initWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  
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
  html += "    .badge-info { background-color: #17a2b8; color: white; }\n";
  html += "    .refresh-btn { display: block; width: 100%; padding: 10px; background: #0066cc; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin-top: 15px; }\n";
  html += "    .footer { text-align: center; margin-top: 20px; font-size: 12px; color: #666; }\n";
  html += "    .mode-info { text-align: center; padding: 5px; background-color: #f8f9fa; margin-bottom: 15px; border-radius: 5px; }\n";
  html += "  </style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "  <div class='container'>\n";
  html += "    <h1>ESP32 Sensor Network</h1>\n";
  
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
  
  int activeCount = 0;
  unsigned long currentTime = millis();
  const unsigned long TIMEOUT = 5 * 60 * 1000;
  
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i]) {
      if (currentTime - deviceLastSeen[i] > TIMEOUT) {
        deviceActive[i] = false;
      } else {
        activeCount++;
      }
    }
  }
  
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
  
  for (int i = 0; i < NUM_DEVICES; i++) {
    uint8_t id = devices[i].id;
    int idx = id - 1;
    
    if (idx >= 0 && idx < MAX_PEERS) {
      html += "<div class='device'>\n";
      html += "  <h2>" + getDeviceName(id) + " (ID: " + String(id) + ")</h2>\n";
      
      html += "  <div class='status'>\n";
      html += "    <span>Type:</span>\n";
      html += "    <span class='badge badge-info'>" + getModuleTypeName(devices[i].module_type) + "</span>\n";
      html += "  </div>\n";
      
      html += "  <div class='status'>\n";
      html += "    <span>Status:</span>\n";
      
      bool isActive = deviceActive[idx] && (currentTime - deviceLastSeen[idx] <= TIMEOUT);
      
      if (isActive) {
        html += "    <span class='badge badge-success'>Online</span>\n";
      } else {
        html += "    <span class='badge badge-danger'>Offline</span>\n";
      }
      
      html += "  </div>\n";
      
      if (isActive && devices[i].module_type > 0) {
        html += "  <div class='status'>\n";
        html += "    <span>Last Seen:</span>\n";
        html += "    <span>" + getTimeAgo(deviceLastSeen[idx]) + "</span>\n";
        html += "  </div>\n";
        
        switch (deviceReadings[idx].module_type) {
          case 1:
            html += "  <div class='reading'>\n";
            html += "    <span>Temperature:</span>\n";
            html += "    <span>" + String(deviceReadings[idx].temperature, 1) + " °C</span>\n";
            html += "  </div>\n";
            html += "  <div class='reading'>\n";
            html += "    <span>Humidity:</span>\n";
            html += "    <span>" + String(deviceReadings[idx].humidity, 1) + " %</span>\n";
            html += "  </div>\n";
            break;
          case 2:
            html += "  <div class='reading'>\n";
            html += "    <span>Light Level:</span>\n";
            html += "    <span>" + String(deviceReadings[idx].light_value, 1) + " %</span>\n";
            html += "  </div>\n";
            break;
        }
      } else if (devices[i].module_type == 0) {
        html += "  <div class='status'>\n";
        html += "    <span>Master device - no sensor data</span>\n";
        html += "  </div>\n";
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