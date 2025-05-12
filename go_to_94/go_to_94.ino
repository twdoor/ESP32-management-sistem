#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================== CONFIGURATION ====================
// Set to true for master device, false for slave
const bool IS_MASTER = true;

// Device IDs - must be unique for each device in your network
const uint8_t DEVICE_ID = 1; // 1 for master, 2+ for slaves

// Update interval in milliseconds
const unsigned long UPDATE_INTERVAL = 10000;

// ==================== DISPLAY SETTINGS ====================
#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels
#define OLED_ADDRESS 0x3C // I2C address of the OLED display

// ==================== ESP-NOW SETTINGS ====================
// Maximum number of peers in the network
#define MAX_PEERS 20

// Known device MAC addresses - add all your devices here
// Format: {device_id, MAC address}
typedef struct {
  uint8_t id;
  uint8_t address[6];
} device_info;

// Add all device MACs here - master should be first
device_info devices[] = {
  {1, {0xd4, 0x8a, 0xfc, 0x9f, 0x2f, 0x98}}, // Master
  {2, {0x94, 0xb5, 0x55, 0xf9, 0xff, 0xf0}}  // Slave 1
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

// Variable to store transmission status
String lastTransmitStatus = "None";

// ==================== DISPLAY INITIALIZATION ====================
// Only create display object if we're the master
#ifdef IS_MASTER
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#endif

// ==================== FUNCTION DECLARATIONS ====================
void initESPNow();
void registerPeers();
void getSensorReadings(float &temp, float &hum, float &pres, float &batt);
void sendReadings();
void updateDisplay();
void handleReceivedData(const sensor_message &reading);
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len);

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

  // Initialize I2C
  Wire.begin();
  
  // If master, initialize display
  if (IS_MASTER) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
      Serial.println(F("SSD1306 allocation failed"));
      // Continue without display
    } else {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(WHITE);
      display.setCursor(0, 0);
      display.println("ESP32 Master");
      display.println("Initializing...");
      display.display();
    }
  }

  // Initialize ESP-NOW
  initESPNow();
}

// ==================== MAIN LOOP ====================
unsigned long lastSendTime = 0;

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
    
    // Send readings to all peers
    sendReadings();
    
    // Update last send time
    lastSendTime = currentTime;
  }
  
  // If master, update display
  if (IS_MASTER) {
    updateDisplay();
  }
  
  // Small delay to prevent CPU hogging
  delay(100);
}

// ==================== ESP-NOW FUNCTIONS ====================
void initESPNow() {
  // Set device as WiFi Station
  WiFi.mode(WIFI_STA);
  
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
      Serial.println(devices[i].id);
    } else {
      Serial.print("Failed to add peer: Device ");
      Serial.println(devices[i].id);
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

void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
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
  }
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplay() {
  if (!IS_MASTER) return; // Only master updates display
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("ESP32 Network - ");
  display.println(DEVICE_ID);
  
  // Calculate how many active devices we have
  int activeCount = 0;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (deviceActive[i]) activeCount++;
  }
  
  // Display active device count
  display.print("Active: ");
  display.print(activeCount);
  display.print("/");
  display.println(NUM_DEVICES);
  
  // Display last transmission status
  display.print("Last Tx: ");
  display.println(lastTransmitStatus);
  
  // We'll show the master's data plus up to 2 slaves on the display
  // Master's data first
  int deviceIndex = DEVICE_ID - 1;
  display.println("-------------------");
  display.print("ID ");
  display.print(deviceReadings[deviceIndex].sender_id);
  display.print(": ");
  display.print(deviceReadings[deviceIndex].temperature, 1);
  display.print("C ");
  display.print(deviceReadings[deviceIndex].humidity, 1);
  display.print("% ");
  display.print(deviceReadings[deviceIndex].pressure, 1);
  display.println("hPa");
  
  // Then other active devices
  int shownDevices = 1;
  for (int i = 0; i < MAX_PEERS && shownDevices < 3; i++) {
    if (deviceActive[i] && i != deviceIndex) {
      display.print("ID ");
      display.print(deviceReadings[i].sender_id);
      display.print(": ");
      display.print(deviceReadings[i].temperature, 1);
      display.print("C ");
      display.print(deviceReadings[i].humidity, 1);
      display.println("%");
      shownDevices++;
    }
  }
  
  display.display();
}