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

✔ Arduino → Handles RFID scanning, LCD, buttons, relay  
✔ ESP → Sends/receives messages via WebSocket  
✔ WebSocket Server → Pass-through message router  
✔ DB Client → Brain of the system (database, decisions, logs)

---

# 🧩 Components Overview

## 🟦 1. Arduino
- Reads RFID tags using RC522 module  
- UI via I2C LCD 16x2  
- Accepts button inputs (UP / OK / DOWN)  
- Controls relay (door lock)  
- Communicates with ESP over Serial    

---

## 🟩 2. ESP Client
- Connects to WiFi  
- Maintains WebSocket connection  
- Forwards messages between Arduino ↔ Python server  

---

## 🟥 3. WebSocket Server
- Accepts multiple connections  
- Forwards messages between clients  
- fully acts as a router  

---

## 🟨 4. DB Client
- User database management  
- RFID card mapping  
- Attendance logs  
- Verifying access  
- Sending results back to Arduino  

This is the **brain** of the system.

---

### Hardware Required
| Component                               | Quantity |
|-----------------------------------------|----------|
| Arduino (Nano)                          | 1        |
| ESP32                                   | 1        |
| LCD 16x2                                | 1        |
| IIC/I2C Serial Interface Adapter Module | 1        |
| RC522 RFID Reader                       | 1        |
| Relay Module                            | 1        |
| Push Buttons                            | 3        |
| Resistor 2.2k                           | 1        |
| Resistor 1k                             | 1        |

# 🔄 Message Flow

## 🧭 Arduino → DB Client
Examples:
- `LOG_ENTRY & EXIT`
- `SEARCH_CARD`
- `AUTH_ACCESS`
- `ADD_USER`
- `DELETE_USER`
- `SET_RESET_TIME`

## 🧭 DB Client → Arduino
Examples:
- `TIME_IN, OUT, DURATION & RESET`
- `USER_FOUND`
- `USER_NOT-FOUND`
- `ACCESS_GRANTED`
- `ACCESS_DENIED`
- `USER_ADDED`
- `USER_DELETED`
- `ERROR`

---

# 🧱 Recommended Folder Structure
/rfid-attendance-system<br>
│<br>
├── arduino/<br>
│   └── sketch/<br>
├───────└── sketch.ino/<br>
│<br>
├── esp/<br>
│   └── esp_client.ino<br>
├───────└── esp_client.ino/<br>
│<br>
├── server.py<br>
│<br>
├── db_client.py<br>
│<br>
├── userDatabase.xlsx<br>
├── userLogs.xlsx<br>
│<br>
└── README.md


---

# 📚 Required Libraries

Here are the libraries that must be manually installed.

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
pip install openpyxl

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
2. Edit your WiFi and WebSocket IP:<br>
const char* ssid = "YOUR_WIFI";<br>
const char* password = "YOUR_PASS";<br>
const char* websocket_server_host= "YOUR_PC_IP";<br> 
const uint16_t websocket_port = 8765;<br>
3. Upload

## 🟥 3. Start WebSocket Server
python server.py

## 🟨 4. Start DB Client
python db_client.py


# ▶️ Start Order (Important)
1️⃣ Run server.py<br>
2️⃣ Run db_client.py<br>
3️⃣ Power/reset ESP<br>
4️⃣ Power/reset Arduino
