#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// --- WiFi Settings ---
const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASS";

// --- Server Settings ---
const char* websocket_server_host = "YOUR_PC_IP"; // REPLACE with the IP of your Python server (e.g., your laptop's IP)
const uint16_t websocket_server_port = 8765;
const char* client_name = "esp_client"; // Consistent client name
const char* target_name = "db_client"; // Target is the Python client 'db_client'

WebSocketsClient webSocket;

// --- Serial Communication Configuration ---
// Serial (Default 115200) is used for debug output to the computer.
// Serial2 is used for Arduino communication (9600 baud)
#define ARDUINO_RX_PIN 16 // ESP32 pin connected to Arduino TX
#define ARDUINO_TX_PIN 17 // ESP32 pin connected to Arduino RX
// -----------------------------------------

// Buffer to store incoming JSON command from Arduino
String arduinoCommandBuffer = "";
bool newCommandReceived = false;

// Function to send a WebSocket message
void sendWebSocketMessage(const char* jsonPayload) {
    if (webSocket.isConnected()) {
        webSocket.sendTXT(jsonPayload);
        Serial.printf("[WS] SENT: %s\n", jsonPayload);
    } else {
        Serial.println("[WS] ERROR: Cannot send, WebSocket disconnected.");
    }
}

// Function to handle WebSocket events
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] DISCONNECTED: %s\n", client_name);
      break;
    case WStype_CONNECTED: {
      Serial.printf("[WS] CONNECTED: to %s\n", payload);
      
      // 1. Send registration message upon successful connection
      StaticJsonDocument<256> reg_doc;
      reg_doc["type"] = "register";
      reg_doc["name"] = client_name;
      
      String output;
      serializeJson(reg_doc, output);
      webSocket.sendTXT(output);
      Serial.printf("[WS] Sent registration: %s\n", output.c_str());
      break;
    }
    case WStype_TEXT: { 
      // Parse the incoming JSON message to check sender/type
      StaticJsonDocument<512> in_doc;
      DeserializationError error = deserializeJson(in_doc, payload);

      if (error) {
        Serial.printf("[WS] ERROR: JSON deserialization failed: %s\n", error.f_str());
        return;
      }
      
      // Check if this is a forwarded message from db_client
      if (in_doc["from"] == target_name) {
        
        // --- PASS: Forward ONLY the inner 'msg' JSON back to Arduino ---
        String replyJson;
        // Extracts the inner message (e.g., {"md":"scan", "nm":"User Name", ...})
        serializeJson(in_doc["msg"], replyJson);
        
        // Send JSON string + newline terminator to Arduino
        Serial2.print(replyJson);
        Serial2.print('\n'); 
        
        // Debug output: Confirms the raw JSON was successfully forwarded to Arduino
        Serial.printf("[ARDUINO] ⬅️ Sent Reply JSON: %s\n", replyJson.c_str());
      }
      
      // Other messages (e.g., server status or error)
      else if (in_doc.containsKey("status") || in_doc.containsKey("error")) {
        Serial.printf("[SERVER] STATUS: %s\n", in_doc["message"].as<const char*>() ? in_doc["message"].as<const char*>() : in_doc["error"].as<const char*>());
      }
      
      break;
    } 
    case WStype_BIN:
      // Removed unnecessary debug prints for binary data, ping, pong, and fragments
      break;
    case WStype_PING:
    case WStype_PONG:
      break;
    case WStype_ERROR:
      Serial.println("[WS] ERROR: Protocol error occurred.");
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

// Function to process incoming serial data from Arduino
void readArduinoSerial() {
    while (Serial2.available()) {
        char incomingChar = Serial2.read();

        // Assuming Arduino sends the full JSON command followed by a newline character
        if (incomingChar == '\n') {
            // Newline received, command is complete
            newCommandReceived = true;
            // Debug print: Shows the raw command received from Arduino
            Serial.printf("[ARDUINO] ➡️ Received CMD: %s\n", arduinoCommandBuffer.c_str());
        } else {
             // Append character to buffer
            arduinoCommandBuffer += incomingChar;
        }
    }
}

void setup() {
  // Debug Serial (USB)
  Serial.begin(115200); 
  delay(10);
  Serial.println("\n--- ESP32 DEBUG SERIAL READY ---");

  // Arduino Serial (RX/TX pins)
  Serial2.begin(9600, SERIAL_8N1, ARDUINO_RX_PIN, ARDUINO_TX_PIN);
  Serial.printf("Arduino Serial (Serial2) initialized at 9600 baud (RX:%d, TX:%d).\n", ARDUINO_RX_PIN, ARDUINO_TX_PIN);
  
  // --- Connect to WiFi ---
  Serial.printf("\nConnecting to %s", ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // --- WebSocket Initialization ---
  webSocket.begin(websocket_server_host, websocket_server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();
  readArduinoSerial(); // Check for incoming commands from Arduino

  // If a full command was received from Arduino, process and send it to db_client
  if (newCommandReceived) {
    if (webSocket.isConnected()) {
        
        StaticJsonDocument<512> routing_doc;
        routing_doc["to"] = target_name;
        
        String cleaned_command = arduinoCommandBuffer;
        cleaned_command.trim(); 
        
        // Deserialize the Arduino command string into the 'msg' object
        // This is the "get" and "wrap" part.
        DeserializationError error = deserializeJson(routing_doc["msg"], cleaned_command);

        if (error) {
            Serial.printf("[ARDUINO] ERROR: Failed to parse JSON from Arduino: %s. Command: %s\n", error.f_str(), cleaned_command.c_str());
        } else {
            // Send the fully routed message over WebSocket
            // This is the "sent" part.
            String ws_output;
            serializeJson(routing_doc, ws_output);
            sendWebSocketMessage(ws_output.c_str());
        }
    } else {
        Serial.println("[WS] WARNING: WebSocket not connected. Cannot send Arduino command.");
    }
    
    // Reset the buffer and flag for the next command
    arduinoCommandBuffer = "";
    newCommandReceived = false;
  }
}