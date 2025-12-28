/*
 * ==================================================================================
 * PROJECT: ESP32 WiFi & WebSocket Bridge for RFID Attendance
 * HARDWARE: ESP32 (acting as a Serial-to-WebSocket Gateway)
 * * DESCRIPTION:
 * This firmware allows an Arduino to communicate with a remote Python server.
 * 1. It listens for JSON commands from Arduino via Serial (9600 baud).
 * 2. It manages local WiFi connections using non-volatile storage (Preferences).
 * 3. It routes packets to a central Database Client via WebSockets.
 * 4. It emulates an "open-drain" status line to signal the Arduino when online.
 * ==================================================================================
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ----------------------------------------------------------------------------------
// --- NETWORK & SERVER CONFIGURATION ---
// ----------------------------------------------------------------------------------

const char* websocket_server_host = "192.168.31.164"; // Target Python server IP
const uint16_t websocket_server_port = 8765;          // Target WebSocket port
const char* client_name = "esp_client";               // Identity of this ESP32
const char* target_name = "db_client";                // Target identity for data routing

// ----------------------------------------------------------------------------------
// --- HARDWARE & STORAGE DEFINITIONS ---
// ----------------------------------------------------------------------------------
/* GPIO23 is used to signal WiFi status to the Arduino.
 * We use "Open-Drain" emulation:
 * - Not Connected: Pin is INPUT (High Impedance), Arduino pulls it HIGH via pull-up.
 * - Connected: Pin is OUTPUT LOW, pulling the Arduino line LOW.
 */
const int WIFI_STATUS_GPIO = 23; // GPIO23 -> Arduino WIFI_STATUS_PIN (A2)

// NVS (Non-Volatile Storage) Keys
const char* PREF_SSID_KEY = "wifi_ssid";
const char* PREF_PASS_KEY = "wifi_pass";
const char* PREF_NAMESPACE = "rfid_att";

// Runtime variables
String storedSSID = "";
String storedPASS = "";
int prevWiFiStatus = WL_DISCONNECTED;

WebSocketsClient webSocket;
Preferences preferences;

// Serial Communication State
String arduinoCommandBuffer = "";
bool newCommandReceived = false;

// ----------------------------------------------------------------------------------
// --- GPIO STATUS HELPERS (OPEN-DRAIN EMULATION) ---
// ----------------------------------------------------------------------------------

/* Signals "Connected" to the Arduino.
 * We set the pin to OUTPUT and drive it LOW. 
 */
void setStatusConnected() {
  pinMode(WIFI_STATUS_GPIO, OUTPUT);
  digitalWrite(WIFI_STATUS_GPIO, LOW);
}

/* Signals "Disconnected" to the Arduino.
 * We set the pin to INPUT (High Impedance). 
 * The Arduino's internal pull-up will then pull the line to 5V/3.3V (HIGH).
 */
void setStatusNotConnected() {
  pinMode(WIFI_STATUS_GPIO, INPUT); // high-impedance -> Arduino pull-up reads HIGH
}

// ----------------------------------------------------------------------------------
// --- WIFI & CREDENTIAL MANAGEMENT ---
// ----------------------------------------------------------------------------------

/* Persists WiFi credentials to the ESP32's internal flash memory.
 */
void saveCredentials(const String& newSsid, const String& newPass) {
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putString(PREF_SSID_KEY, newSsid);
  preferences.putString(PREF_PASS_KEY, newPass);
  preferences.end();

  storedSSID = newSsid;
  storedPASS = newPass;
}

/* Loads saved credentials from flash. 
 * Defaults to "wifi"/"12345678" if memory is empty.
 */
void loadCredentials() {
  preferences.begin(PREF_NAMESPACE, true); // read-only
  storedSSID = preferences.getString(PREF_SSID_KEY, "");
  storedPASS = preferences.getString(PREF_PASS_KEY, "");
  preferences.end();

  if (storedSSID == "") {
    storedSSID = "wifi";
    storedPASS = "12345678";
  }
}

/* Performs a local WiFi scan and returns results to the Arduino in JSON format.
*/
void handleScanWifi() {
  while (Serial.available()) Serial.read();
  WiFi.mode(WIFI_STA);
  delay(100);

  int n = WiFi.scanNetworks(false, false);
  String nets = "";
  int added = 0;

  // Compile up to 6 SSIDs into a semicolon-separated string
  for (int i = 0; i < n && added < 6; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (ssid.length() > 16) ssid = ssid.substring(0, 16); // Truncate for LCD display

    if (nets.length() > 0) nets += ";";
    nets += ssid;
    added++;
  }

  // Format the response for the Arduino
  StaticJsonDocument<128> doc;
  doc["md"] = "scan_wifi";
  doc["nets"] = nets;

  String out;
  serializeJson(doc, out);
  Serial.println(out);  // Send back to Arduino

  WiFi.scanDelete();    // Free memory
}

/* Attempt to connect to WiFi. Blocking for up to 10 seconds.
 */
bool connectToWiFi() {
  if (storedSSID.length() == 0) return false;

  WiFi.disconnect(true);
  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime < 10000)) {
    delay(500);
  }
  
  return (WiFi.status() == WL_CONNECTED);
}

// ----------------------------------------------------------------------------------
// --- WEBSOCKET EVENT HANDLING ---
// ----------------------------------------------------------------------------------

/* Core WebSocket logic: Handles connection, registration, and data routing.
 */
void sendWebSocketMessage(const char* jsonPayload) {
  if (webSocket.isConnected()) {
    webSocket.sendTXT(jsonPayload);
  }
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      setStatusNotConnected();     // Drop line to inform Arduino
      break;
    
    case WStype_CONNECTED: {
      setStatusConnected();        // Pull line LOW to inform Arduino
      
      // Immediately register this client with the WebSocket server
      StaticJsonDocument<256> reg_doc;
      reg_doc["type"] = "register";
      reg_doc["name"] = client_name;
      String output;
      serializeJson(reg_doc, output);
      webSocket.sendTXT(output);
      break;
    }
    
    case WStype_TEXT: {
      // Message received from Server
      StaticJsonDocument<512> in_doc;
      DeserializationError err = deserializeJson(in_doc, payload, length);
      
      if (err) return;
      
      // Filter: Check if the message is specifically from the Database client
      if (in_doc.containsKey("from") && String((const char*)in_doc["from"].as<const char*>()) == String(target_name)) {
        if (in_doc.containsKey("msg")) {
          // Extract the actual command/response and forward it to the Arduino via Serial
          String replyJson;
          serializeJson(in_doc["msg"], replyJson);
          Serial.print(replyJson);
          Serial.print('\n');        // Terminate with newline for Arduino's parser
        }
      }
      break;
    }
    
    case WStype_BIN:
    case WStype_PING:
    case WStype_PONG:
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
    break;
  }
}

// ----------------------------------------------------------------------------------
// --- SERIAL COMMAND PROCESSING ---
// ----------------------------------------------------------------------------------

/* Reads incoming characters from the Arduino until a newline is found.
 */
void readArduinoSerial() {
  while (Serial.available()) {
    char incomingChar = Serial.read();
    if (incomingChar == '\n') {
      newCommandReceived = true;
      return;
    }
    else {
      arduinoCommandBuffer += incomingChar;
    }
  }
}

/* Parses JSON from Arduino and decides if it's a local command or needs forwarding.
 */
void processArduinoCommand() {
  if (!newCommandReceived) return;

  String cleaned = arduinoCommandBuffer;
  cleaned.trim();
  arduinoCommandBuffer = "";
  newCommandReceived = false;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, cleaned);

  if (err) return;

  const char* md = doc["md"].as<const char*>();

  // CASE 1: Request to update local WiFi settings
  if (md != nullptr && strcmp(md, "wifi") == 0) {
    const char* newSsid = doc["SSID"].as<const char*>();
    const char* newPass = doc["PASS"].as<const char*>();
    
    StaticJsonDocument<128> reply_doc;
    reply_doc["md"] = "wifi";

    if (newSsid != nullptr && strlen(newSsid) > 0) {
      saveCredentials(newSsid, newPass != nullptr ? newPass : "");
      
      if (connectToWiFi()) {
        reply_doc["rslt"] = "C"; // Connected
      }
      else {
        // Note: ESP32 doesn't easily distinguish between "not found" and "wrong password"
        // For simplicity based on prompt, we reply "NC" for failed attempts.
        reply_doc["rslt"] = "NC"; // Not connected
      }
    }
    else {
      // Should not happen if Arduino side is working, but safety check
      reply_doc["rslt"] = "ERR";
    }
    
    String replyJson;
    serializeJson(reply_doc, replyJson);
    Serial.print(replyJson);
    Serial.print('\n');
    return;
  }

    // CASE 2: Request to scan for nearby networks
    if (md != nullptr && strcmp(md, "scan_wifi") == 0) {
      handleScanWifi();
      return;
    }

  // CASE 3: Standard Data Request (forwarded to the server)
  else {
    if (webSocket.isConnected()) {
      // Build routing document
      StaticJsonDocument<512> routing_doc;
      routing_doc["to"] = target_name;

      // Copy original received JSON object into routing_doc["msg"]
      routing_doc["msg"] = doc.as<JsonObject>(); 
      
      String ws_output;
      serializeJson(routing_doc, ws_output);
      sendWebSocketMessage(ws_output.c_str());
    }
  }
}

// ----------------------------------------------------------------------------------
// --- MAIN EXECUTION BLOCKS ---
// ----------------------------------------------------------------------------------

void setup() {
  Serial.begin(9600);
  delay(10);
  
  loadCredentials();           // Retrieve SSID/Pass from Flash
  setStatusNotConnected();     // Default state: Offline
  connectToWiFi();             // Attempt connection

  // Sync Hardware Pin with current connection status
  prevWiFiStatus = WiFi.status();
  if (prevWiFiStatus == WL_CONNECTED) setStatusConnected();
  else setStatusNotConnected();
  if (WiFi.status() == WL_CONNECTED) {
    webSocket.begin(websocket_server_host, websocket_server_port, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
  }
}

void loop() {
  // Keep WebSocket heartbeats and events running
  webSocket.loop();

  // Check for incoming data from Arduino
  readArduinoSerial();
  processArduinoCommand();

  // Watchdog for WiFi status changes
  int cur = WiFi.status();
  if (cur != prevWiFiStatus) {
    if (cur == WL_CONNECTED) {
      setStatusConnected();
      // When connection is re-established, try to start WebSocket
      webSocket.begin(websocket_server_host, websocket_server_port, "/");
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
    }
    else {
      setStatusNotConnected();
    }
    prevWiFiStatus = cur;
  }
}
