# 📡 RFID Attendance System — Complete IoT Architecture

A fully modular **RFID-based Attendance & Access Control System** built using:

- Arduino (UI + Hardware Control)
- ESP8266/ESP32 (Networking)
- Python WebSocket Server (Message Router)
- Python DB Client (Business Logic + Database Engine)

Designed to be **fast, scalable, LAN-based, and easy to extend**.

---

# 📁 System Architecture
[Arduino] ⇆ [ESP Client] ⇆ [WebSocket Server] ⇆ [DB Client]


### ✔ Arduino → Handles scanning, LCD, buttons, relay  
### ✔ ESP → Sends/receives messages via WebSocket  
### ✔ WebSocket Server → Pass-through message router  
### ✔ DB Client → Brain of the system (database, decisions, logs)

---

# 🧩 Components Overview

## 🟦 1. Arduino (sketch.ino)
### Responsibilities
- Reads RFID tags using RC522 module  
- UI via I2C LCD  
- Accepts button inputs (UP / OK / DOWN)  
- Controls relay (door lock)  
- Communicates with ESP over Serial  

### Hardware Required
- Arduino UNO / Nano / Mega  
- I2C LCD (16x2 or 20x4)  
- RC522 RFID Reader  
- Relay Module  
- Push Buttons  
- ESP8266 or ESP32  

---

## 🟩 2. ESP Client (esp_client.ino)
### Responsibilities
- Connects to WiFi  
- Maintains WebSocket connection  
- Forwards messages between Arduino ↔ Python server  

---

## 🟥 3. WebSocket Server (server.py)
### Responsibilities
- Accepts multiple connections  
- Forwards every message to all clients  
- No logic — fully acts as a router  

---

## 🟨 4. DB Client (db_client.py)
### Responsibilities
- User database management  
- RFID card mapping  
- Attendance logs  
- Verifying access  
- Sending results back to Arduino  

This is the **brain** of the system.

---

# 🔄 Message Flow

## 🧭 Arduino → DB Client
Examples:
- `SEARCH_CARD`
- `ADD_USER`
- `LOG_ENTRY`
- `GET_USER`
- `MARK_ATTENDANCE`

## 🧭 DB Client → Arduino
Examples:
- `USER_FOUND`
- `ACCESS_GRANTED`
- `ACCESS_DENIED`
- `USER_ADDED`
- `ERROR`

---

# 🧱 Recommended Folder Structure
/rfid-attendance-system
│
├── arduino/
│ └── sketch.ino
│
├── esp/
│ └── esp_client.ino
│
├── server.py
│
├── db_client.py
│
├── userDatabase.xlsx
├── userLogs.xlsx
│
└── README.md


---

# 📚 Required Libraries (Exact + Correct GitHub Links)

Here are only the libraries that must be manually installed.

---

## 🟦 Arduino Libraries (sketch.ino)

### ✔ LiquidCrystal I2C  
- **Author:** Frank de Brabander  
- **GitHub:** https://github.com/fdebrabander/Arduino-LiquidCrystal-I2C-library  
- LCD display support

### ✔ MFRC522 (RFID)  
- **Author:** Miguel Balboa  
- **GitHub:** https://github.com/miguelbalboa/rfid  
- RC522 RFID reader driver

---

## 🟩 ESP32 / ESP8266 Libraries (esp_client.ino)

### ✔ arduinoWebSockets  
- **Author:** Markus Sattler  
- **GitHub:** https://github.com/Links2004/arduinoWebSockets  
- Provides `WebSocketsClient.h`

### ✔ ArduinoJson  
- **Author:** Benoît Blanchon  
- **GitHub:** https://github.com/bblanchon/ArduinoJson  
- JSON encode/decode for messages

---

# 🐍 Python Dependencies

## For WebSocket Server (`server.py`)
pip install websockets


## For DB Client (`db_client.py`)
pip install websockets openpyxl

---

# ⚙️ Installation & Setup

## 🟦 1. Arduino
1. Install required libraries  
2. Open `sketch.ino`  
3. Select board + port  
4. Upload  

---

## 🟩 2. ESP Client
1. Install above libraries  
2. Edit your WiFi and WebSocket IP:
const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASS";
const char* websocket_server = "YOUR_PC_IP"; 
const uint16_t websocket_port = 8765;

3. Upload to ESP32/ESP8266

## 🟥 3. Start WebSocket Server
python server.py

## 🟨 4. Start DB Client
python db_client.py


# ▶️ Start Order (Important)
1️⃣ Run server.py
2️⃣ Run db_client.py
3️⃣ Power/reset ESP
4️⃣ Power/reset Arduino
