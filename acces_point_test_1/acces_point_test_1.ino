#include <WiFi.h>

// Network credentials
const char* ssid     = "ESP32-AP-Test-Tw";
const char* password = "PAROLA1234";

// Web server port
WiFiServer server(80);

// HTTP request variable
String header;

// GPIO states
String output26State = "off";
String output27State = "off";
int lightSensorValue = 0;  // Variable to store light sensor reading

// GPIO pins
const int output26 = 26;
const int output27 = 27;
const int lightSensor = 25;  // Light sensor pin

// HTML webpage template
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="icon" href="data:,">
    <style>
        html { 
            font-family: Helvetica; 
            display: inline-block; 
            margin: 0px auto; 
            text-align: center;
        }
        .button { 
            background-color: #4CAF50; 
            border: none; 
            color: white; 
            padding: 16px 40px;
            text-decoration: none; 
            font-size: 30px; 
            margin: 2px; 
            cursor: pointer;
        }
        .button2 {
            background-color: #555555;
        }
        .sensor-value {
            font-size: 24px;
            color: #333;
            margin: 20px;
            padding: 10px;
            border: 2px solid #4CAF50;
            border-radius: 5px;
            display: inline-block;
        }
        .value-changed {
            background-color: #fff3cd;
            transition: background-color 0.5s;
        }
    </style>
</head>
<body>
    <h1>ESP32 Web Server</h1>
    
    <div class="sensor-value" id="sensorDisplay">
        <p>Light Sensor Value: <span id="lightValue">%LIGHT_VALUE%</span></p>
    </div>
    
    <p>GPIO 26 - State %STATE26%</p>
    %BUTTON_26%
    
    <p>GPIO 27 - State %STATE27%</p>
    %BUTTON_27%

    <script>
        let lastValue = %LIGHT_VALUE%;
        
        function updateSensorValue() {
            fetch('/sensor')
                .then(response => response.text())
                .then(value => {
                    const newValue = parseInt(value);
                    if (newValue !== lastValue) {
                        const display = document.getElementById('sensorDisplay');
                        const valueSpan = document.getElementById('lightValue');
                        valueSpan.textContent = newValue;
                        
                        // Add highlight effect
                        display.classList.add('value-changed');
                        setTimeout(() => {
                            display.classList.remove('value-changed');
                        }, 500);
                        
                        lastValue = newValue;
                    }
                })
                .catch(error => console.log('Error:', error));
        }

        // Check for updates every 100ms
        setInterval(updateSensorValue, 100);
    </script>
</body>
</html>
)rawliteral";

// Function to process HTML template
String processor(const String& var) {
    if(var == "%STATE26%") {
        return output26State;
    }
    else if(var == "%STATE27%") {
        return output27State;
    }
    else if(var == "%LIGHT_VALUE%") {
        return String(lightSensorValue);
    }
    else if(var == "%BUTTON_26%") {
        if(output26State == "on") {
            return "<p><a href=\"/26/off\"><button class=\"button button2\">OFF</button></a></p>";
        } else {
            return "<p><a href=\"/26/on\"><button class=\"button\">ON</button></a></p>";
        }
    }
    else if(var == "%BUTTON_27%") {
        if(output27State == "on") {
            return "<p><a href=\"/27/off\"><button class=\"button button2\">OFF</button></a></p>";
        } else {
            return "<p><a href=\"/27/on\"><button class=\"button\">ON</button></a></p>";
        }
    }
    return String();
}

// Function to send web page
void sendWebPage(WiFiClient client) {
    lightSensorValue = digitalRead(lightSensor);
    
    String html = MAIN_page;
    
    // Replace all template placeholders
    html.replace("%STATE26%", processor("%STATE26%"));
    html.replace("%STATE27%", processor("%STATE27%"));
    html.replace("%LIGHT_VALUE%", processor("%LIGHT_VALUE%"));
    html.replace("%BUTTON_26%", processor("%BUTTON_26%"));
    html.replace("%BUTTON_27%", processor("%BUTTON_27%"));
    
    // Send the final HTML to client
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html");
    client.println("Connection: close");
    client.println();
    client.println(html);
}

void setup() {
    Serial.begin(115200);
    
    // Initialize GPIO
    pinMode(output26, OUTPUT);
    pinMode(output27, OUTPUT);
    pinMode(lightSensor, INPUT);  // Set light sensor pin as input
    digitalWrite(output26, LOW);
    digitalWrite(output27, LOW);

    // Setup Access Point
    Serial.print("Setting AP (Access Point)...");
    WiFi.softAP(ssid, password);

    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    
    server.begin();
}

void loop() {
    WiFiClient client = server.available();

    if (client) {
        Serial.println("New Client.");
        String currentLine = "";
        
        while (client.connected()) {
            if (client.available()) {
                char c = client.read();
                Serial.write(c);
                header += c;
                
                if (c == '\n') {
                    if (currentLine.length() == 0) {
                        // Check if this is a sensor value request
                        if (header.indexOf("GET /sensor") >= 0) {
                            lightSensorValue = digitalRead(lightSensor);
                            client.println("HTTP/1.1 200 OK");
                            client.println("Content-type:text/plain");
                            client.println("Connection: close");
                            client.println();
                            client.println(lightSensorValue);
                        }
                        // Process GPIO commands and main page request
                        else {
                            if (header.indexOf("GET /26/on") >= 0) {
                                output26State = "on";
                                digitalWrite(output26, HIGH);
                            } else if (header.indexOf("GET /26/off") >= 0) {
                                output26State = "off";
                                digitalWrite(output26, LOW);
                            } else if (header.indexOf("GET /27/on") >= 0) {
                                output27State = "on";
                                digitalWrite(output27, HIGH);
                            } else if (header.indexOf("GET /27/off") >= 0) {
                                output27State = "off";
                                digitalWrite(output27, LOW);
                            }
                            sendWebPage(client);
                        }
                        break;
                    } else {
                        currentLine = "";
                    }
                } else if (c != '\r') {
                    currentLine += c;
                }
            }
        }
        
        header = "";
        client.stop();
        Serial.println("Client disconnected.");
        Serial.println("");
    }
}