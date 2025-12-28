/*
 * ==================================================================================
 * PROJECT: RFID Attendance System with Serial-JSON Integration
 * HARDWARE: Arduino Uno/Nano, MFRC522 RFID, 16x2 I2C LCD, Buzzer, LEDs, Reed Switch
 * * DESCRIPTION:
 * This system manages user attendance via RFID tags. It communicates with a host 
 * system using JSON over Serial for data persistence (Add, Delete, Scan, WiFi).
 * It features a multi-level menu system, non-blocking indicator updates, and 
 * an automated door lock mechanism controlled by a Reed switch sensor.
 * ==================================================================================
 */

#include <LiquidCrystal_I2C.h>  // Library for I2C LCD control
#include <SPI.h>                // SPI communication for the RFID module
#include <MFRC522.h>            // Library for MFRC522 RFID reader
#include <avr/pgmspace.h>       // Enables storing constant strings in Flash (SRAM saving)

// ----------------------------------------------------------------------------------
// --- HARDWARE CONFIGURATION & PIN DEFINITIONS ---
// ----------------------------------------------------------------------------------

// LCD setup: 0x27 is the standard I2C address for most 1602 modules
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Output Indicator Pins
#define BUZZER_PIN A1
#define ACCEPT_PIN 4    // Connects to Green LED and Door Lock Relay/Transistor
#define REJECT_PIN 9    // Connects to Red LED
#define WIFI_LED 8      // Connects to Green/WiFi Status LED

// Status Input Pins
#define WIFI_STATUS_PIN A2  // Pulled LOW by external host (ESP8266/ESP32) when WiFi is active

// Navigation Button Pins (Using Internal Pullups)
#define UP_PIN 7
#define SELECT_PIN 6
#define DOWN_PIN 5
#define REED_PIN A3   // Magnetic Reed Switch: Detects if the door is physically open/closed

// RFID MFRC522 Pins
#define RST_PIN A0                    // Reset pin for RFID module
#define SS_PIN 10                     // Slave Select (SS) pin for SPI communication
MFRC522 mfrc522(SS_PIN, RST_PIN);     // Initialize MFRC522 instance

// ----------------------------------------------------------------------------------
// --- SYSTEM CONSTANTS & TIMING ---
// ----------------------------------------------------------------------------------

const int DEBOUNCE_DELAY = 50;                    // Stability window for button reads (ms)
const unsigned long LONG_PRESS_DURATION = 2000;   // Time held for 'Back' or 'Exit' actions
const unsigned long MENU_ENTRY_DURATION = 5000;   // Time held to trigger the Auth/Admin menu
const unsigned long POST_ENTRY_DELAY = 2000;      // Cooldown after entering menu to prevent accidental clicks
const unsigned long SERIAL_TIMEOUT = 5000;        // Max wait time for a JSON response from host
const unsigned long SCAN_DISPLAY_TIME = 3000;     // Duration transaction results stay on screen

// ----------------------------------------------------------------------------------
// --- MENU STRUCTURE (Stored in PROGMEM to save SRAM) ---
// ----------------------------------------------------------------------------------

// Main Menu Strings
const char menu1[] PROGMEM = "1. Add User";
const char menu2[] PROGMEM = "2. Delete User";
const char menu3[] PROGMEM = "3. Settings";
const char* const menuItems[] PROGMEM = {menu1, menu2, menu3,};
const int MENU_SIZE = sizeof(menuItems) / sizeof(menuItems[0]);

// Settings Sub-Menu Strings (Padded for visual alignment on LCD)
const char setting1[] PROGMEM = "1. Set Timer";
const char setting2[] PROGMEM = "2. WiFi     ";
const char* const settingsMenuItems[] PROGMEM = {setting1, setting2,};
const int SETTINGS_MENU_SIZE = sizeof(settingsMenuItems) / sizeof(settingsMenuItems[0]);

// ----------------------------------------------------------------------------------
// --- GLOBAL STATE VARIABLES ---
// ----------------------------------------------------------------------------------

// State Management
int currentSelection = 0;     // Currently highlighted menu index
int menuState = -2;           // -3: Auth Scan, -2: Main Idle, 0: Menu, 1+: Sub-processes

// --- Debounce/Timing State Variables ---
unsigned long lastDebounceTime = 0;
unsigned long selectPressStart = 0; 
unsigned long entryPressStart = 0;
unsigned long menuEntryTime = 0; 
bool isLongPressReturnActive = false; 
int lastUpState = HIGH;
int lastDownState = HIGH;
int lastSelectState = HIGH;

// Shared Buffer Management (Optimized for ATmega328P memory constraints)
const int MAX_GLOBAL_BUFFER_SIZE = 128; 
char g_serial_buffer[MAX_GLOBAL_BUFFER_SIZE];     // Shared buffer for incoming Serial JSON
int g_serial_index = 0;
const int MAX_EXTRACT_BUFFER = 64;
char g_temp_extract_buffer[MAX_EXTRACT_BUFFER];   // Shared buffer for JSON key-value extraction
const int MAX_NAME_SIZE = 32;

// Door/Security Logic
bool doorForceUnlock = false;
bool doorOpened = false;
unsigned long doorLockTimer = 0;

// Indicators & WiFi
bool wifiConnected = false;
bool requestDirectWifiEntry = false;
char wifiChosenSSID[28] = {0};
char wifiChosenPass[28] = {0};
bool mainScanDisplayDrawn = false;
unsigned long buzzerEndTime = 0;
unsigned long acceptLedEndTime = 0;
unsigned long rejectLedEndTime = 0;
unsigned long doubleBeepTimer = 0;
bool doubleBeepStage = false;
bool doubleBeepActive = false;

// ----------------------------------------------------------------------------------
// --- Function Prototypes ---
// ----------------------------------------------------------------------------------

void displayMenu(); 
void handleInput();
void handleAuthentication();
void mainCardScan();
void addUser();
void deleteUser();
void settings();
void printUIDHex();
char* extractValue(const char* buffer, const char* key, char* result, int resultSize);
void sendSerialJson(const __FlashStringHelper* md, const char* id, const char* un, const char* ut, const __FlashStringHelper* opt, const char* tm);
void sendWifiJson(const char* ssid, const char* pass);

// ----------------------------------------------------------------------------------
// --- INDICATOR & DOOR LOGIC (NON-BLOCKING) ---
// ----------------------------------------------------------------------------------

// Triggers a single short beep and pulse.
void beepOnce() {tone(BUZZER_PIN, 2000, 150);}

// Triggers two short beeps using a state-based timer to avoid blocking loop().
void beepTwice() {
  tone(BUZZER_PIN, 2000, 150);
  doubleBeepTimer = millis() + 250;
  doubleBeepStage = false;
  doubleBeepActive = true;
}

// Activates the unlock relay (ACCEPT_PIN) and starts the smart door auto-lock sequence.
void pulseAccept() {
  digitalWrite(ACCEPT_PIN, HIGH);
  doorForceUnlock = true;
  doorOpened = false;
  doorLockTimer = 0;
}

// Pulses the Reject indicator for 1 second.
void pulseReject() {
  digitalWrite(REJECT_PIN, HIGH);
  rejectLedEndTime = millis() + 1000;
}

/*
Updates LED and Buzzer states based on timers. 
Called continuously in main loop().
*/
void indicatorUpdate() {
  unsigned long now = millis();
  
  if (doubleBeepActive) {
    if (!doubleBeepStage && now >= doubleBeepTimer) {
      tone(BUZZER_PIN, 2000, 150);  // second beep
      doubleBeepStage = true;
      doubleBeepTimer = now + 150;  // wait until second beep ends
    }
    else if (doubleBeepStage && now >= doubleBeepTimer) {
      doubleBeepActive = false;
    }
  }
  
  if (now > rejectLedEndTime) {
    digitalWrite(REJECT_PIN, LOW);
  }
  
  // WiFi Indicator: Solid if connected, Blinking if searching
  static unsigned long wifiTimer = 0;
  static bool wifiBlink = false;
  
  int s = digitalRead(WIFI_STATUS_PIN);
  wifiConnected = (s == LOW);
  
  if (wifiConnected) {
    digitalWrite(WIFI_LED, HIGH);
    wifiTimer = millis();
    wifiBlink = true;
  }
  else {
    if (millis() - wifiTimer > 1000) {
      wifiTimer = millis();
      wifiBlink = !wifiBlink;
    }
    digitalWrite(WIFI_LED, wifiBlink ? HIGH : LOW);
  }
}

/* Door Lock Logic:
 * 1. Door is unlocked by system.
 * 2. System waits for Reed Switch to detect door opening.
 * 3. Once opened, system waits for Reed Switch to detect door closing.
 * 4. Once closed, locks the door after a 3-second delay.
 */
void doorUpdate() {
  int reedState = digitalRead(REED_PIN); // HIGH = closed, LOW = open
  unsigned long now = millis();
  
  if (doorForceUnlock) {
    if (!doorOpened && reedState == LOW) {
      doorOpened = true;
    }
    if (doorOpened && reedState == HIGH) {
      if (doorLockTimer == 0) {
        doorLockTimer = now + 3000;
      }
    }
    if (doorLockTimer > 0 && now >= doorLockTimer) {
      digitalWrite(ACCEPT_PIN, LOW);  // LOCK DOOR
      doorForceUnlock = false;
      doorOpened = false;
      doorLockTimer = 0;
    }
  }
}

// ----------------------------------------------------------------------------------
// --- LCD & SERIAL ---
// ----------------------------------------------------------------------------------

void printScreen(const __FlashStringHelper* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}
void printScreen(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}
void printScreen(const __FlashStringHelper* line1, const __FlashStringHelper* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}
void printScreen(const char* line1, const __FlashStringHelper* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// Helper to convert UID to Hex string (prints to Serial - unchanged)
void printUIDHex() {
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) {
      Serial.print(F("0"));
    }
    Serial.print(mfrc522.uid.uidByte[i], HEX); 
  }
}

// Constructs and sends a standardized JSON packet to the Serial port.
void sendSerialJson(const __FlashStringHelper* md, const char* id, const char* un, const char* ut, const __FlashStringHelper* opt, const char* tm) {
  Serial.print(F("{\"md\":\""));
  Serial.print(md);
  Serial.print(F("\""));
  
  if (id && id[0] != '\0') {
    Serial.print(F(", \"id\":\""));
    Serial.print(id);
    Serial.print(F("\""));
  }
  if (un && un[0] != '\0') {
    Serial.print(F(", \"un\":\""));
    Serial.print(un);
    Serial.print(F("\""));
  }
  if (ut && ut[0] != '\0') {
    Serial.print(F(", \"ut\":\""));
    Serial.print(ut);
    Serial.print(F("\""));
  }
  if (opt) {
    Serial.print(F(", \"opt\":\""));
    Serial.print(opt);
    Serial.print(F("\""));
  }
  if (tm && tm[0] != '\0') {
    Serial.print(F(", \"tm\":\""));
    Serial.print(tm);
    Serial.print(F("\""));
  }
  Serial.println(F("}"));
}

// Specialized WiFi JSON packet.
void sendWifiJson(const char* ssid, const char* pass) {
  Serial.print(F("{\"md\":\"wifi\",\"SSID\":\""));
  Serial.print(ssid);
  Serial.print(F("\",\"PASS\":\""));
  Serial.print(pass);
  Serial.println(F("\"}"));
}

// Lightweight JSON parser: Searches for a key and extracts the value into a result buffer.
char* extractValue(const char* buffer, const char* key, char* result, int resultSize) {
  char searchKey[32];
  snprintf(searchKey, sizeof(searchKey), "\"%s\":\"", key);
  
  char* start = strstr(buffer, searchKey);
  if (!start) return NULL;
  
  start += strlen(searchKey);
  char* end = strchr(start, '"');
  
  if (end && (end - start) < resultSize) {
    int len = end - start;
    strncpy(result, start, len);
    result[len] = '\0';
    return result;
  }
  return NULL;
}


// ----------------------------------------------------------------------------------
// --- MAIN SYSTEM INITIALIZATION ---
// ----------------------------------------------------------------------------------

void setup() {
  Serial.begin(9600); 
  
  pinMode(UP_PIN, INPUT_PULLUP);
  pinMode(SELECT_PIN, INPUT_PULLUP);
  pinMode(DOWN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(ACCEPT_PIN, OUTPUT);
  pinMode(REJECT_PIN, OUTPUT);
  pinMode(WIFI_STATUS_PIN, INPUT_PULLUP);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(REED_PIN, INPUT_PULLUP);
  
  digitalWrite(ACCEPT_PIN, LOW);
  digitalWrite(REJECT_PIN, LOW);
  digitalWrite(WIFI_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  SPI.begin(); 
  
  mfrc522.PCD_Init(); 
}


// ----------------------------------------------------------------------------------
// --- MENU DISPLAY & NAVIGATION ---
// ----------------------------------------------------------------------------------

// Renders the current menu items with a cursor on the LCD.
void displayMenu() {
  lcd.clear();
  char buffer[17];
  
  // --- Line 0 (Selected Item) ---
  const char *selectedItemAddr = (const char *)pgm_read_word(&menuItems[currentSelection]);
  strcpy_P(buffer, selectedItemAddr);
  lcd.setCursor(0, 0);
  lcd.print(F(">"));
  lcd.print(buffer);
  
  // --- Line 1 (Next Item - non-selected) ---
  int nextItemIndex = (currentSelection + 1) % MENU_SIZE;
  const char *nextItemAddr = (const char *)pgm_read_word(&menuItems[nextItemIndex]);
  strcpy_P(buffer, nextItemAddr); 
  lcd.setCursor(0, 1); 
  lcd.print(F(" "));
  lcd.print(buffer);
}

// Handles tactile input for menu navigation and exits.
void handleInput() {
  if (menuState == 0 && (millis() - menuEntryTime) < POST_ENTRY_DELAY) return;
  int readingUp = digitalRead(UP_PIN);
  int readingDown = digitalRead(DOWN_PIN);
  int readingSelect = digitalRead(SELECT_PIN);
  
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (menuState == 0) {
      if (readingUp == LOW && lastUpState == HIGH) {
        currentSelection = (currentSelection == 0) ? (MENU_SIZE - 1) : (currentSelection - 1);
        displayMenu();
        lastDebounceTime = millis();
      }
      if (readingDown == LOW && lastDownState == HIGH) {
        currentSelection = (currentSelection + 1) % MENU_SIZE;
        displayMenu();
        lastDebounceTime = millis();
      }
    }
  }
  
  // SELECT Button Logic (Long Press RETURN/EXIT)
  if (readingSelect == LOW && lastSelectState == HIGH) {
    selectPressStart = millis(); 
    isLongPressReturnActive = false; 
  }
  if (readingSelect == LOW && selectPressStart > 0) {
    if (!isLongPressReturnActive && (millis() - selectPressStart) >= LONG_PRESS_DURATION) {
      isLongPressReturnActive = true;
      if (menuState == 0) {
        menuState = -2; // Exit to Initial State
        printScreen(F("Scan your card"), F(""));
      }
      else {
        menuState = 0; // Return to Main Menu
        displayMenu();
      }
      selectPressStart = 0; 
    }
  }
  if (readingSelect == HIGH && lastSelectState == LOW) {
    unsigned long pressDuration = millis() - selectPressStart;
    if (pressDuration > DEBOUNCE_DELAY && !isLongPressReturnActive) { 
      if (menuState == 0) {
        menuState = currentSelection + 1; // Transition to action state
        selectPressStart = 0;
      }
    }
    lastDebounceTime = millis();
    selectPressStart = 0; 
    isLongPressReturnActive = false; 
  }
  lastUpState = readingUp;
  lastDownState = readingDown;
  lastSelectState = readingSelect;
}

// ----------------------------------------------------------------------------------
// --- AUTHENTICATION & SCAN PROCESSES ---
// ----------------------------------------------------------------------------------

void handleAuthentication() {
  static int authSubState = 0;
  static char authUserId[17] = "";
  static unsigned long authStartTime = 0;
  static bool displayRunOnce = false;
  static unsigned long authSelectPressStart = 0; 
  
  int upReading = digitalRead(UP_PIN);
  int selectReading = digitalRead(SELECT_PIN);
  int downReading = digitalRead(DOWN_PIN);
  
  bool isHolding = (upReading == LOW && selectReading == LOW);
  bool wasHolding = (entryPressStart != 0);

  if (isHolding) {
    if (!wasHolding) {
      entryPressStart = millis();
      printScreen(F("ENTERING MENU..."), F("")); 
    }
    else {
      if (millis() - entryPressStart >= MENU_ENTRY_DURATION) {
        if (menuState == -2 && !wifiConnected) {
          menuState = 3; // Settings
          requestDirectWifiEntry = true;
        }
        else {
          menuState = -3; // Authentication Scan state
          entryPressStart = 0;
          authSubState = 0;
          displayRunOnce = false;
          authSelectPressStart = 0;
          lastUpState = HIGH;
          lastSelectState = HIGH;
        }
        return;
      }
    }
  }
  else {
    if (wasHolding && menuState != -3) {
      entryPressStart = 0;
      menuState = -2; 
      mainScanDisplayDrawn = false;
      return;
    }
  }
  
  if (menuState != -3) {
    mainCardScan();
    lastUpState = upReading;
    lastDownState = downReading;
    lastSelectState = selectReading;
    return;
  }
  
  // AUTH mode long-press-ok exit
  if (selectReading == LOW && lastSelectState == HIGH) {
    authSelectPressStart = millis(); 
  }
  if (selectReading == LOW && authSelectPressStart > 0) {
    if (millis() - authSelectPressStart >= LONG_PRESS_DURATION) {
      menuState = -2;
      authSubState = 0;
      mainScanDisplayDrawn = false;
      printScreen(F("Scan your card"), F(""));
      authSelectPressStart = 0;
      lastSelectState = HIGH; 
      return;
    }
  }
  if (selectReading == HIGH && lastSelectState == LOW) {
    authSelectPressStart = 0;
  }

  switch (authSubState) {
    case 0: // Waiting for Admin Scan
      if (!displayRunOnce) {
        printScreen(F("Auth. scan card"), F("waiting...."));
        while (Serial.available()) Serial.read();
        displayRunOnce = true;
        mfrc522.PCD_Init(); 
        g_serial_index = 0;
      }

      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        memset(authUserId, 0, sizeof(authUserId));
        for (byte i = 0; i < mfrc522.uid.size; i++) {
          sprintf(authUserId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
        }
        sendSerialJson(F("auth"), authUserId, NULL, NULL, NULL, NULL);
        authSubState = 1; 
        displayRunOnce = false;
        authStartTime = millis();
      }
      break;

    case 1: // Processing JSON Response
      if (!displayRunOnce) {
        printScreen(F("Scanned Card"), F("Processing...."));
        displayRunOnce = true;
      }
      if (millis() - authStartTime > SERIAL_TIMEOUT) {
        printScreen(F("Auth Timeout!"), F("Try again."));
        authSubState = 0;
        displayRunOnce = false;
        delay(SCAN_DISPLAY_TIME);
        return;
      }
      while (Serial.available()) {
        char inChar = Serial.read();
        if (inChar != '\r' && inChar != '\n') {
          if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
            g_serial_buffer[g_serial_index++] = inChar;
            g_serial_buffer[g_serial_index] = '\0'; 
          }
        }
        if (inChar == '\n') {
          if (extractValue(g_serial_buffer, "rslt", g_temp_extract_buffer, sizeof(g_temp_extract_buffer))) {
            if (strcmp(g_temp_extract_buffer, "admin") == 0) {
              menuState = 0; // Enter Main Menu
              menuEntryTime = millis(); 
              displayMenu();
            } else {
              authSubState = 2; // Not admin
              authStartTime = millis();
            }
          } else {
            printScreen(F("AUTH JSON ERROR!"), F("Restart host."));
            Serial.readStringUntil('\n');
            authSubState = 0; // Reset to scan
            delay(SCAN_DISPLAY_TIME);
          }
          displayRunOnce = false;
          g_serial_index = 0; // Reset global index
          return;
        }
      }
      break;

    case 2: // Failure Acknowledge (3s delay)
      if (!displayRunOnce) {
        printScreen(F("User - not admin"), F("reverting..."));
        displayRunOnce = true;
      }
      if (millis() - authStartTime > SCAN_DISPLAY_TIME) {
        menuState = -2; // Exit authentication state
        displayRunOnce = false;
      }
      break;
  }
  lastUpState = upReading;
  lastDownState = downReading;
  lastSelectState = selectReading;
}

// Simple Scan Handler (If not in auth state - Updated to use shared buffers)
void mainCardScan() {
  static int subState = 0; 
  static unsigned long transactionStartTime = 0;
  static bool stage2Ready = false;
  static char userName[MAX_NAME_SIZE]; 
  static char actionType[8]; // IN, OUT, FAIL
  static char timeValue[16]; // HH:MM:SS or larger
  static char durationValue[16];
  static char retryValue[16];
  
  if (subState == 0) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("Scan your card"), F(""));
      while (Serial.available()) Serial.read();
      mainScanDisplayDrawn = true;
      mfrc522.PCD_Init(); 
    }
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      char userId[33];
      memset(userId, 0, sizeof(userId));
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        sprintf(userId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
      }
      sendSerialJson(F("scan"), userId, NULL, NULL, NULL, NULL);
      subState = 1; 
      mainScanDisplayDrawn = false;
      transactionStartTime = millis();
      g_serial_index = 0;
    }
  }
  
  else if (subState == 1) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("Scan"), F("Processing...."));
      mainScanDisplayDrawn = true;
    } 
    if (millis() - transactionStartTime > SERIAL_TIMEOUT) {
      printScreen(F("Timeout!"), F("Try again."));
      subState = 0;
      mainScanDisplayDrawn = false;
      delay(SCAN_DISPLAY_TIME);
      return;
    }
    
    while (Serial.available()) {
      char inChar = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = inChar;
        g_serial_buffer[g_serial_index] = '\0'; 
        
        if (inChar == '}') {
          // Check for User Not Found
          if (strstr(g_serial_buffer, "\"rslt\":\"NF\"")) {
            subState = 6; // User Not Found
            transactionStartTime = millis();
            mainScanDisplayDrawn = false;
            g_serial_index = 0;
            return;
          }
          // Parse full response
          if (extractValue(g_serial_buffer, "nm", userName, sizeof(userName)) && extractValue(g_serial_buffer, "act", actionType, sizeof(actionType))) {
            extractValue(g_serial_buffer, "tm", timeValue, sizeof(timeValue));
            extractValue(g_serial_buffer, "dur", durationValue, sizeof(durationValue));
            extractValue(g_serial_buffer, "rtr", retryValue, sizeof(retryValue));
            transactionStartTime = millis();
            
            if (strcmp(actionType, "IN") == 0) {
              subState = 2;
              beepOnce();
              pulseAccept(); // ADDED: Activate pin for IN (Success)
            }
            else if (strcmp(actionType, "OUT") == 0) {
              subState = 3;
              beepOnce();
              pulseAccept();
            }
            else if (strcmp(actionType, "FAIL") == 0) {
              subState = 3; // Use state 3 for dual-display failure
              beepTwice();
              pulseReject();
              stage2Ready = false;
            }
            else {
              subState = 5;
              beepTwice();
              pulseReject();
            }
          }
          else {
            subState = 5;
            beepTwice();
            pulseReject();
          }
          mainScanDisplayDrawn = false;
          g_serial_index = 0;
          return;
        } 
      }
    }
  }
  
  else if (subState == 2) {
    if (!mainScanDisplayDrawn) {
      char line2[17];
      snprintf(line2, sizeof(line2), "IN %s", timeValue);
      printScreen(userName, line2);
      mainScanDisplayDrawn = true;
    }
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
    }
  }
  
  else if (subState == 3) {
    if (!mainScanDisplayDrawn) {
      char line2[17];
      if (strcmp(actionType, "OUT") == 0) {
        snprintf(line2, sizeof(line2), "OUT %s", timeValue);
      }
      else { // FAIL or already tapped
        strcpy(line2, "Already Tapped");
      }
      printScreen(userName, line2);
      mainScanDisplayDrawn = true;
      stage2Ready = false;
    }
    // If this is a single display message (e.g., simple error), move to subState 4 immediately for the next screen
    if (!stage2Ready && (millis() - transactionStartTime > SCAN_DISPLAY_TIME)) {
      subState = 4;
      mainScanDisplayDrawn = false;
      transactionStartTime = millis();
      stage2Ready = true;
    }
  }
  
  else if (subState == 4) {
    if (!mainScanDisplayDrawn) {
      char line1[17], line2[17];
      if (strcmp(actionType, "OUT") == 0) {
        strcpy(line1, "Duration");
        strncpy(line2, durationValue, 16);
        line2[16] = '\0';
      }
      else if (strcmp(actionType, "FAIL") == 0) {
        strcpy(line1, "Try after");
        strncpy(line2, retryValue, 16);
        line2[16] = '\0';
      }
      else {
        strcpy(line1, "Wait for next");
        strcpy(line2, "entry cycle");
      }
      printScreen(line1, line2);
      mainScanDisplayDrawn = true;
    }
    
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
      stage2Ready = false;
    }
  }
  
  else if (subState == 5) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("JSON Error!"), F("Reset host system."));
      mainScanDisplayDrawn = true;
    }
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
    }
  }
  
  else if (subState == 6) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("User not found"), F("Please Register"));
      mainScanDisplayDrawn = true;
    }
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
    }
  }
}

// ----------------------------------------------------------------------------------
// --- MENU FUNCTIONS & NAVIGATION ---
// ----------------------------------------------------------------------------------

// CHAR TABLE FOR NAME SCROLLING
const char CHAR_TABLE[] =
  "*ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "!@#$%^&*"
  "0123456789"
  " ";
const int CHAR_TABLE_SIZE = sizeof(CHAR_TABLE) - 1;

void addUser() {
  static int subState = 0;
  static char userId[33] = "";
  static char userName[MAX_NAME_SIZE];
  static char userTitle[6] = "admin";
  static int nameCharIndex = 0;
  static int okPressCount = 0;
  static bool isTitleAdmin = true;
  static bool displayUpdateNeeded = true;
  static unsigned long lastCharUpdateTime = 0;
  const unsigned long CHAR_SCROLL_DELAY = 150;
  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;
  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);

  if (menuState != 1) {
    subState = 0;
    memset(userId, 0, sizeof(userId));
    memset(userName, 0, sizeof(userName));
    nameCharIndex = 0;
    strcpy(userTitle, "admin");
    okPressCount = 0;
    isTitleAdmin = true;
    displayUpdateNeeded = true;
    g_serial_index = 0;
    memset(g_serial_buffer, 0, sizeof(g_serial_buffer));
    localLastUpState = HIGH;
    localLastDownState = HIGH;
    localLastSelectState = HIGH;
    return;
  }

  if (subState == 0) {
    if (displayUpdateNeeded) {
      printScreen(F("Scan User ID"), F("Waiting..."));
      displayUpdateNeeded = false;
      g_serial_index = 0;
      mfrc522.PCD_Init();
    }

    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      memset(userId, 0, sizeof(userId));
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        sprintf(userId + i * 2, "%02X", mfrc522.uid.uidByte[i]);
      }
      sendSerialJson(F("search"), userId, NULL, NULL, NULL, NULL);
      printScreen(F("Searching..."), userId);
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      subState = 1;
      displayUpdateNeeded = true;
    }
  }

  else if (subState == 1) {
    while (Serial.available()) {
      char c = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = c;
        g_serial_buffer[g_serial_index] = '\0';
      }
      if (c == '\n' || strstr(g_serial_buffer, "}")) {
        if (strstr(g_serial_buffer, "\"rslt\":\"F\"")) {
          subState = 6;
          displayUpdateNeeded = true;
          g_serial_index = 0;
          break;
        }
        else if (strstr(g_serial_buffer, "\"rslt\":\"NF\"")) {
          subState = 2;
          displayUpdateNeeded = true;
          g_serial_index = 0;
          break;
        }
      }
    }
  }

  else if (subState == 2) {
    static bool nameInit = true;
    static int charIndex = 0;
    static char currentLetter;

    if (nameInit) {
      charIndex = 0;
      currentLetter = CHAR_TABLE[charIndex];
      nameCharIndex = 0;
      memset(userName, 0, sizeof(userName));
      okPressCount = 0;
      displayUpdateNeeded = true;
      nameInit = false;
    }

    char line1[17];
    char line2[17];

    if (millis() - lastCharUpdateTime > CHAR_SCROLL_DELAY) {
      bool scrolled = false;
      if (upReading == LOW) {
        charIndex--;
        if (charIndex < 0) charIndex = CHAR_TABLE_SIZE - 1;
        scrolled = true;
      }
      else if (downReading == LOW) {
        charIndex++;
        if (charIndex >= CHAR_TABLE_SIZE) charIndex = 0;
        scrolled = true;
      }
      if (scrolled) {
        currentLetter = CHAR_TABLE[charIndex];
        lastCharUpdateTime = millis();
        okPressCount = 0;
        displayUpdateNeeded = true;
      }
    }

    if (selectReading == HIGH && localLastSelectState == LOW) {
      if (currentLetter == '*') {
        nameInit = true;
        subState = 3;
        displayUpdateNeeded = true;
        localLastSelectState = HIGH;
        return;
      }
      if (okPressCount == 1) { // Second press on the same char
        nameInit = true;
        subState = 3;
        displayUpdateNeeded = true;
        localLastSelectState = HIGH;
        return;
      }
      if (nameCharIndex < MAX_NAME_SIZE - 1) {
        userName[nameCharIndex++] = currentLetter;
        userName[nameCharIndex] = '\0';
        charIndex = 0;
        currentLetter = CHAR_TABLE[charIndex];
        okPressCount = 1;
        displayUpdateNeeded = true;
      }
    }

    if (displayUpdateNeeded) {
      snprintf(line1, sizeof(line1), "Name: %s", userName);
      snprintf(line2, sizeof(line2), "%c Char %d: <%c>",
      okPressCount == 0 ? '>' : '!',
      nameCharIndex + 1,
      currentLetter);
      printScreen(line1, line2);
      displayUpdateNeeded = false;
    }
  }

  else if (subState == 3) {
    if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
      isTitleAdmin = !isTitleAdmin;
      strcpy(userTitle, isTitleAdmin ? "admin" : "user");
      displayUpdateNeeded = true;
    }
    if (selectReading == HIGH && localLastSelectState == LOW) {
      printScreen(F("Adding user"), F("Processing..."));
      sendSerialJson(F("add"), userId, userName, userTitle, NULL, NULL);
      subState = 4;
      displayUpdateNeeded = true;
      g_serial_index = 0;
      return;
    }
    if (displayUpdateNeeded) {
      char line2[17];
      snprintf(line2, sizeof(line2), "Title: <%s>", userTitle);
      printScreen(F("Select Title"), line2);
      displayUpdateNeeded = false;
    }
  }

  else if (subState == 4) {
    while (Serial.available()) {
      char c = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = c;
        g_serial_buffer[g_serial_index] = '\0';
      }
      if (c == '\n' || strstr(g_serial_buffer, "}")) {
        if (strstr(g_serial_buffer, "\"rslt\":\"A\"")) {
          subState = 5;
          displayUpdateNeeded = true;
          g_serial_index = 0;
          break;
        }
      }
    }
  }

  else if (subState == 5) {
    if (displayUpdateNeeded) {
      printScreen(F("User added"), F("Press OK to exit"));
      displayUpdateNeeded = false;
    }
    if (selectReading == HIGH && localLastSelectState == LOW) {
      subState = 0;
      menuState = 0;
      displayMenu();
      displayUpdateNeeded = true;
    }
  }

  else if (subState == 6) {
    if (displayUpdateNeeded) {
      printScreen(F("ID exists"), F("Press OK to exit"));
      displayUpdateNeeded = false;
    }
    if (selectReading == HIGH && localLastSelectState == LOW) {
      subState = 0;
      menuState = 0;
      displayMenu();
      displayUpdateNeeded = true;
    }
  }

  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}

void deleteUser() {
  static int subState = 0; 
  static char scannedUserId[33] = "";
  static bool displayUpdateNeeded = true;
  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;
  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);

  if (menuState != 2) { 
    subState = 0; 
    memset(scannedUserId, 0, sizeof(scannedUserId));
    g_serial_index = 0;
    displayUpdateNeeded = true; 
    localLastUpState = HIGH;
    localLastDownState = HIGH;
    localLastSelectState = HIGH;
    return; 
  }

  if (subState == 0) {
    if (displayUpdateNeeded) {
      printScreen(F("Delete user"), F("Scan your card"));
      displayUpdateNeeded = false;
      g_serial_index = 0;
      mfrc522.PCD_Init();
    }
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      memset(scannedUserId, 0, sizeof(scannedUserId));
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        sprintf(scannedUserId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
      }
      sendSerialJson(F("search"), scannedUserId, NULL, NULL, NULL, NULL);
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      subState = 1;
      displayUpdateNeeded = true; 
    }
  }

  else if (subState == 1) {
    if (displayUpdateNeeded) { 
      printScreen(F("Searching..."), scannedUserId);
      displayUpdateNeeded = false; 
    }
    while (Serial.available()) {
      char inChar = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = inChar;
        g_serial_buffer[g_serial_index] = '\0'; 
        if (inChar == '\n' || strstr(g_serial_buffer, "}")) {
          if (strstr(g_serial_buffer, "\"rslt\":\"F\"")) {
            subState = 2; displayUpdateNeeded = true; g_serial_index = 0; break; 
          }
          else if (strstr(g_serial_buffer, "\"rslt\":\"NF\"")) {
            subState = 4; displayUpdateNeeded = true; g_serial_index = 0; break;
          }
        }
      }
      else {
        subState = 4; displayUpdateNeeded = true; break;
      }
    }
  }

  else if (subState == 2) {
    if (displayUpdateNeeded) { printScreen(F("User found!"), F("Del>ok cancle>up")); displayUpdateNeeded = false; }
    if (selectReading == HIGH && localLastSelectState == LOW) {
      sendSerialJson(F("delete"), scannedUserId, NULL, NULL, NULL, NULL);
      subState = 3; displayUpdateNeeded = true; g_serial_index = 0;
    }
    else if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
      subState = 6; displayUpdateNeeded = true;
    }
  }

  else if (subState == 3) {
    if (displayUpdateNeeded) { printScreen(F("Deleting user"), F("Processing....")); displayUpdateNeeded = false; }
    while (Serial.available()) {
      char inChar = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = inChar;
        g_serial_buffer[g_serial_index] = '\0'; 
        if (inChar == '\n' || strstr(g_serial_buffer, "}")) {
          if (strstr(g_serial_buffer, "\"rslt\":\"DL\"")) {
            subState = 5; displayUpdateNeeded = true; g_serial_index = 0; break; 
          } 
        }
      }
    }
  }

  else if (subState == 4) {
    if (displayUpdateNeeded) { printScreen(F("User not found"), F("Press ok to exit")); displayUpdateNeeded = false; }
    if (selectReading == HIGH && localLastSelectState == LOW) {
      subState = 0; menuState = 0; displayMenu(); displayUpdateNeeded = true; 
    }
  }

  else if (subState == 5) {
    if (displayUpdateNeeded) { printScreen(F("User Deleted"), F("Press ok to exit")); displayUpdateNeeded = false; }
      if (selectReading == HIGH && localLastSelectState == LOW) {
        subState = 0; menuState = 0; displayMenu(); displayUpdateNeeded = true; 
      }
  }

  else if (subState == 6) {
    if (displayUpdateNeeded) {
      printScreen(F("Canceling..."), F("Returning..."));
      delay(500);
      subState = 0; menuState = 0; displayMenu(); displayUpdateNeeded = true;
    }
  }

  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}

void settings() {
  static int subState = 0; 
  static int settingsSelection = 0; 
  static int hours = 0;
  static int minutes = 0;
  static int seconds = 0;
  static int timeGroupIndex = 0;
  static bool displayUpdateNeeded = true;
  static bool inputReady = false; 
  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;
  
  // WiFi Specific Statics
  static char nets[3][17]; // Reduced to 3 networks, 16 char max SSID + NULL (51 bytes)
  static int netsCount = 0;
  static int selectedNet = 0;
  static unsigned long scanStart = 0;
  
  // Static state variables for manual entry flow
  static char manualSSID[33];
  static char passwordBuff[33];
  
  static unsigned long lastCharUpdateTime = 0;
  const unsigned long CHAR_SCROLL_DELAY = 150;

  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);

  /* Settings Session Flag
   * Tracks if settings was entered via the long press (Auth path). This flag should persist
   * throughout the settings menu session (menuState 3).
   * static bool enteredDirectlyThisSession = false;
  **/
  
  // Check if the global request flag is set on entry to the Settings parent state (subState 0)
  if (menuState == 3 && requestDirectWifiEntry && subState == 0) {
    enteredDirectlyThisSession = true;
    requestDirectWifiEntry = false;
  }

  if (menuState != 3) { 
    // Reset settings state when exiting the menu entirely
    subState = 0; settingsSelection = 0; hours = minutes = seconds = 0;
    timeGroupIndex = 0; displayUpdateNeeded = true; inputReady = false;
    localLastUpState = HIGH; localLastDownState = HIGH; localLastSelectState = HIGH;
    g_serial_index = 0; g_serial_buffer[0] = '\0'; 
    // Reset session flag when leaving menuState 3
    enteredDirectlyThisSession = false;
    return; 
  }
  
  // FULL RESET of WiFi buffers whenever entering WiFi settings
  static bool wifiInit = true;

  if (menuState == 3 && wifiInit && subState == 0) {
    // Clear previously-entered WiFi SSID & Password & Buffers
    memset(wifiChosenSSID, 0, sizeof(wifiChosenSSID));
    memset(wifiChosenPass, 0, sizeof(wifiChosenPass));
    memset(manualSSID, 0, sizeof(manualSSID));
    memset(passwordBuff, 0, sizeof(passwordBuff));

    wifiInit = false;
  }

  // When leaving WiFi settings and returning again → reset flag
  if (subState == 0) {
    wifiInit = true;
  }


  if (subState == 0) {
    if (enteredDirectlyThisSession) {
      settingsSelection = 1;
      subState = 30;    
      displayUpdateNeeded = true;
      inputReady = false;
      localLastUpState = HIGH;
      localLastDownState = HIGH;
      localLastSelectState = HIGH;
      return;
    }
        
    // Display logic runs only if we are still in subState 0
    if (upReading == LOW && localLastUpState == HIGH) { settingsSelection = (settingsSelection == 0) ? (SETTINGS_MENU_SIZE - 1) : (settingsSelection - 1); displayUpdateNeeded = true; }
    else if (downReading == LOW && localLastDownState == HIGH) { settingsSelection = (settingsSelection + 1) % SETTINGS_MENU_SIZE; displayUpdateNeeded = true; }

    if (selectReading == HIGH && localLastSelectState == LOW) {
      lcd.clear();
      localLastUpState = HIGH; localLastDownState = HIGH; localLastSelectState = HIGH;
      switch(settingsSelection) {
        case 0: subState = 11; hours = 0; minutes = 0; seconds = 0; timeGroupIndex = 0; displayUpdateNeeded = true; inputReady = false; break;
        case 1: 
          subState = 30; 
          displayUpdateNeeded = true; 
          inputReady = false; 
          break;
      }
      return;
    }

    if (displayUpdateNeeded) {
      char line1[17];
      char line2[17];
      char selectedTextRam[17];
      char nextTextRam[17];
      
      // 1. Get and format selected item (Line 1: > ItemName)
      const char *selectedItemAddr = (const char *)pgm_read_word(&settingsMenuItems[settingsSelection]);
      strcpy_P(selectedTextRam, selectedItemAddr);
      
      // 2. Get and format next item (Line 2: ItemName)
      int nextItemIndex = (settingsSelection + 1) % SETTINGS_MENU_SIZE;
      const char *nextItemAddr = (const char *)pgm_read_word(&settingsMenuItems[nextItemIndex]);
      strcpy_P(nextTextRam, nextItemAddr);
      
      // Align text to column 1
      // Selected line: Starts with '>', text starts column 1
      snprintf(line1, sizeof(line1), ">%s", selectedTextRam);
      line1[16] = '\0';
      
      // Next line: Starts with ' ', text starts column 1
      snprintf(line2, sizeof(line2), " %s", nextTextRam); 
      line2[16] = '\0';
      
      printScreen(line1, line2);
      displayUpdateNeeded = false;
    }
  }
  // NEW SUBSTATE: WAIT FOR BUTTON RELEASE
  else if (subState == 30) {
    if (displayUpdateNeeded) {
      printScreen(F("Ready..."), F("Release SELECT"));
      displayUpdateNeeded = false;
    }
    // Check if the SELECT button is released
    if (selectReading == HIGH) {
      localLastSelectState = HIGH;
      subState = 31; // Go to WiFi scan
      displayUpdateNeeded = true;
    }
  }
  
  // Reset time flow (11-14)
  else if (subState == 11) {

    // time setting logic
    if (!inputReady) { inputReady = true; } 
    else {
      if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
        int* targetValue = NULL; int minVal = 0, maxVal = 0;
        if (timeGroupIndex == 0) { targetValue = &hours; maxVal = 23; }
        else if (timeGroupIndex == 1) { targetValue = &minutes; maxVal = 59; }
        else if (timeGroupIndex == 2) { targetValue = &seconds; maxVal = 59; }
        if (targetValue) {
          if (upReading == LOW && localLastUpState == HIGH) *targetValue = (*targetValue == maxVal) ? minVal : *targetValue + 1;
          else if (downReading == LOW && localLastDownState == HIGH) *targetValue = (*targetValue == minVal) ? maxVal : *targetValue - 1;
        }
        displayUpdateNeeded = true;
      }
      if (selectReading == HIGH && localLastSelectState == LOW) {
        timeGroupIndex++;
        displayUpdateNeeded = true;
        if (timeGroupIndex > 2) { subState = 12; displayUpdateNeeded = true; }
      }
    }
    if (displayUpdateNeeded) {
      char line2[17];
      snprintf(line2, sizeof(line2), "%s%02d:%s%02d:%s%02d", (timeGroupIndex==0)?">":" ", hours, (timeGroupIndex==1)?">":" ", minutes, (timeGroupIndex==2)?">":" ", seconds);
      printScreen(F("Set Reset Time"), line2); displayUpdateNeeded = false;
    }
  }

  else if (subState == 12) {
    if (displayUpdateNeeded) {
      char line1[17]; snprintf(line1, sizeof(line1), "Confirm: %02d:%02d:%02d", hours, minutes, seconds);
      printScreen(line1, F("OK=Yes Up/Dn=No")); displayUpdateNeeded = false;
    }
    if (selectReading == HIGH && localLastSelectState == LOW) { subState = 13; displayUpdateNeeded = true; inputReady = false; }
    else if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) { subState = 0; displayUpdateNeeded = true; }
  }

  else if (subState == 13) {
    if (displayUpdateNeeded) {
      char timeString[9]; snprintf(timeString, sizeof(timeString), "%02d:%02d:%02d", hours, minutes, seconds);
      sendSerialJson(F("rst_time"), NULL, NULL, NULL, NULL, timeString);
      printScreen(F("Reset Time"), F("Updating...."));
      displayUpdateNeeded = false;
      g_serial_index = 0; // Use global index
    }
    while (Serial.available()) {
      char inChar = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) { // Use global buffer
        g_serial_buffer[g_serial_index++] = inChar;
        g_serial_buffer[g_serial_index] = '\0'; 
        if (inChar == '\n' || strstr(g_serial_buffer, "}")) {
          if (strstr(g_serial_buffer, "\"rslt\":\"D\"")) { subState = 14; displayUpdateNeeded = true; g_serial_index = 0; inputReady = false; break; }
        }
      }
    }
  }

  else if (subState == 14) {
    if (displayUpdateNeeded) { printScreen(F("Settings Updated"), F("Press ok to exit")); displayUpdateNeeded = false; }
    if (selectReading == HIGH && localLastSelectState == LOW) { subState = 0; menuState = 0; displayMenu(); displayUpdateNeeded = true; }
  }

  // WiFi flow (31-33)
  else if (subState == 31) {
    // 1. Initialization and Scanning
    if (displayUpdateNeeded && netsCount == 0) {
      g_serial_index = 0; 
      g_serial_buffer[0] = '\0';
      for (int i = 0; i < 3; i++) nets[i][0] = '\0'; 
      netsCount = 0; 
      selectedNet = 0;
      scanStart = millis();
      sendSerialJson(F("scan_wifi"), NULL, NULL, NULL, NULL, NULL);
      printScreen(F("Scanning WiFi"), F("Please wait..."));
      displayUpdateNeeded = false; 
    }

    // 2. Handle Incoming Serial Data
    while (Serial.available()) {
      char inChar = Serial.read();
      if (g_serial_index < (int)sizeof(g_serial_buffer) - 1) {
        g_serial_buffer[g_serial_index++] = inChar;
        g_serial_buffer[g_serial_index] = '\0';
      }
      if (inChar == '}') {
        if (extractValue(g_serial_buffer, "nets", g_temp_extract_buffer, MAX_EXTRACT_BUFFER)) {
          char *p = g_temp_extract_buffer; 
          int idx = 0; char tmp[17]; int tpos = 0;
          while (*p && idx < 3) {
            if (*p == ';') { 
              tmp[tpos] = '\0'; 
              strncpy(nets[idx], tmp, 16); 
              nets[idx][16] = '\0'; 
              idx++; tpos = 0; 
            }
            else { if (tpos < 16) tmp[tpos++] = *p; }
            p++;
          }
          if (tpos > 0 && idx < 3) { 
            tmp[tpos] = '\0'; 
            strncpy(nets[idx], tmp, 16); 
            nets[idx][16] = '\0'; 
            idx++; 
          }
          netsCount = idx;
          displayUpdateNeeded = true; 
        }
        g_serial_index = 0; 
        break;
      }
    }

    if (netsCount == 0 && (millis() - scanStart > SERIAL_TIMEOUT)) {
      subState = 34; // Manual entry
      displayUpdateNeeded = true;
      return;
    }

    // 3. Navigation Logic
    if (netsCount > 0) {
      if (upReading == LOW && localLastUpState == HIGH) {
        selectedNet = (selectedNet == 0) ? (netsCount - 1) : (selectedNet - 1);
        displayUpdateNeeded = true;
      }
      else if (downReading == LOW && localLastDownState == HIGH) {
        selectedNet = (selectedNet + 1) % netsCount;
        displayUpdateNeeded = true;
      }

      if (selectReading == HIGH && localLastSelectState == LOW) {
        strncpy(wifiChosenSSID, nets[selectedNet], sizeof(wifiChosenSSID) - 1);
        wifiChosenSSID[sizeof(wifiChosenSSID) - 1] = '\0';
        
        subState = 32;                  // Move to Password Entry
        displayUpdateNeeded = true; 
        inputReady = false;             // Reset flag to force wait-for-release in next state
        
        // Reset state trackers so state 32
        localLastSelectState = HIGH; 
        selectReading = HIGH;
        return;
      }

      if (displayUpdateNeeded) {
        char line1[17], line2[17];
        int next = (selectedNet + 1) % netsCount;
        snprintf(line1, sizeof(line1), ">%s", nets[selectedNet]);
        snprintf(line2, sizeof(line2), " %s", nets[next]);
        printScreen(line1, line2);
        displayUpdateNeeded = false;
      }
    }
  }

  else if (subState == 32) {
    // password char-scroll
    static bool pwdInit = true;
    static int charIndex = 0;
    static char currentLetter;
    static int pwdPos = 0;

    // 1. RELEASE GUARD: Wait for button release from SSID selection
    if (!inputReady) {
      if (selectReading == HIGH) {  // Button lifted
        inputReady = true;       
        pwdInit = true;             // Force a clean start
      }
      return;
    }

    // 2. INITIALIZATION
    if (pwdInit) { 
      charIndex = 0; 
      currentLetter = CHAR_TABLE[charIndex]; 
      pwdPos = 0; 
      memset(passwordBuff, 0, sizeof(passwordBuff)); 
      displayUpdateNeeded = true; 
      pwdInit = false;

      // Final guard: ensures we don't process a press on the exact frame we initialize
      localLastSelectState = HIGH;
    }

    // 3. SCROLLING LOGIC
    if (millis() - lastCharUpdateTime > CHAR_SCROLL_DELAY) {
      bool scrolled = false;
      if (upReading == LOW) { 
        charIndex--; if (charIndex < 0) charIndex = CHAR_TABLE_SIZE - 1; 
        scrolled = true; 
      }
      else if (downReading == LOW) { 
        charIndex++; if (charIndex >= CHAR_TABLE_SIZE) charIndex = 0; 
        scrolled = true; 
      }
      if (scrolled) {
        currentLetter = CHAR_TABLE[charIndex];
        lastCharUpdateTime = millis();
        displayUpdateNeeded = true;
      }
    }

    // 4. SELECTION LOGIC
    if (selectReading == HIGH && localLastSelectState == LOW) {
      if (currentLetter == '*') {
        // Save and move to connection
        strncpy(wifiChosenPass, passwordBuff, sizeof(wifiChosenPass) - 1);
        wifiChosenPass[sizeof(wifiChosenPass) - 1] = '\0';
        
        subState = 33; // CONNECTING
        displayUpdateNeeded = true;
        pwdInit = true; // Reset for next time
        return; 
      }

      // Add character to password
      if (pwdPos < (int)sizeof(passwordBuff) - 1) { 
        passwordBuff[pwdPos++] = currentLetter; 
        passwordBuff[pwdPos] = '\0'; 
        charIndex = 0; // Reset scroll to start of table
        currentLetter = CHAR_TABLE[charIndex];
        displayUpdateNeeded = true;
      }
    }

    // 5. RENDER DISPLAY
    if (displayUpdateNeeded) {
      char line1[17], line2[17];
      snprintf(line1, sizeof(line1), "PW: %s", passwordBuff);
      line1[16] = '\0';
      snprintf(line2, sizeof(line2), "P:%d Chr:<%c>", pwdPos + 1, currentLetter);
      line2[16] = '\0';
      
      printScreen(line1, line2);
      displayUpdateNeeded = false;
    }
  }

  else if (subState == 33) {
    // Connection attempt
    static unsigned long wifiTxTime = 0;

    if (displayUpdateNeeded) {
      sendWifiJson(wifiChosenSSID, wifiChosenPass);
      printScreen(F("WiFi"), F("Connecting..."));
      g_serial_index = 0; g_serial_buffer[0] = '\0';
      wifiTxTime = millis();
      displayUpdateNeeded = false;
    }

    while (Serial.available()) {
      char c = Serial.read();
      if (g_serial_index < MAX_GLOBAL_BUFFER_SIZE - 1) {
        g_serial_buffer[g_serial_index++] = c;
        g_serial_buffer[g_serial_index] = '\0';
      }
      if (c == '}') {
        if (strstr(g_serial_buffer, "\"md\":\"wifi\"") && strstr(g_serial_buffer, "\"rslt\":\"C\"")) {
          printScreen(F("wifi connected"), F("Press ok to exit"));
          subState = 36; return;
        } else {
          printScreen(F("error connecting"), F("Press ok to exit"));
          subState = 36; return;
        }
      }
    }
    if (millis() - wifiTxTime > SERIAL_TIMEOUT) {
      printScreen(F("error connecting"), F("Press ok to exit"));
      subState = 36; displayUpdateNeeded = true; return;
    }
  }

  else if (subState == 34) {
    // manual SSID entry
    static bool ssidInit = true;
    static int charIndex = 0;
    static char currentLetter = CHAR_TABLE[0];
    static int ssidPos = 0;
    static int okCountSS = 0; // Use okPressCount logic

    if (ssidInit) { 
      memset(manualSSID, 0, sizeof(manualSSID)); 
      memset(wifiChosenSSID, 0, sizeof(wifiChosenSSID)); 
      charIndex=0; 
      currentLetter=CHAR_TABLE[charIndex]; 
      ssidPos=0; 
      displayUpdateNeeded=true; 
      ssidInit=false; 
      okCountSS = 0;
      }

    // Add continuous scrolling
    if (millis() - lastCharUpdateTime > CHAR_SCROLL_DELAY) {
      bool scrolled = false;
      if (upReading == LOW) { 
        charIndex--; if (charIndex<0) charIndex=CHAR_TABLE_SIZE-1; currentLetter=CHAR_TABLE[charIndex]; scrolled=true; 
      }
      else if (downReading == LOW) { 
        charIndex++; if (charIndex>=CHAR_TABLE_SIZE) charIndex=0; currentLetter=CHAR_TABLE[charIndex]; scrolled=true; 
      }
      if (scrolled) {
        lastCharUpdateTime = millis();
        okCountSS = 0; 
        displayUpdateNeeded = true;
      }
    }


    if (selectReading == HIGH && localLastSelectState == LOW) {
      if (currentLetter == '*') {
        // Confirm entry: Move to password screen (subState 35)
        strncpy(wifiChosenSSID, manualSSID, sizeof(wifiChosenSSID)-1); wifiChosenSSID[sizeof(wifiChosenSSID)-1]='\0';
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;
        subState = 35; // Go to password input
        displayUpdateNeeded = true; 
        return;
      }
      
      if (okCountSS == 1) {
        strncpy(wifiChosenSSID, manualSSID, sizeof(wifiChosenSSID)-1); wifiChosenSSID[sizeof(wifiChosenSSID)-1]='\0';
        
        // Reset button states before state transition
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;
        subState = 35; // Go to password input
        displayUpdateNeeded = true; 
        return;
      }

      if (ssidPos < (int)sizeof(wifiChosenSSID) - 1) { 
        manualSSID[ssidPos++] = currentLetter; manualSSID[ssidPos] = '\0'; 
      }
      
      charIndex = 0; currentLetter = CHAR_TABLE[charIndex]; 
      okCountSS = 1;
      displayUpdateNeeded = true;
    }

    if (displayUpdateNeeded) {
      char line1[17], line2[17];
      snprintf(line1, sizeof(line1), "SSID: %s", manualSSID);
      snprintf(line2, sizeof(line2), "%c Char %d: <%c>",
      okCountSS == 0 ? '>' : '!', // Use > if choosing char, ! if confirming/ready to exit
      ssidPos + 1,
      currentLetter);
      line2[16]='\0';
      printScreen(line1, line2);
      displayUpdateNeeded=false;
    }
  }

  else if (subState == 35) {
    // manual password entry
    static bool pwdInit = true;
    static int charIndex = 0;
    static char currentLetter = CHAR_TABLE[0];
    static int pwdPos = 0;
    static int okCount = 0;

    if (pwdInit) { 
      memset(passwordBuff, 0, sizeof(passwordBuff)); 
      memset(wifiChosenPass, 0, sizeof(wifiChosenPass)); 
      charIndex=0; 
      currentLetter=CHAR_TABLE[charIndex]; 
      pwdPos=0; 
      displayUpdateNeeded=true; 
      pwdInit=false; 
      okCount = 0;
    }

    // Character scrolling and selection logic
    if (millis() - lastCharUpdateTime > CHAR_SCROLL_DELAY) {
      bool scrolled = false;
      if (upReading == LOW) { 
        charIndex--; if (charIndex<0) charIndex=CHAR_TABLE_SIZE-1; currentLetter=CHAR_TABLE[charIndex]; scrolled=true; 
      }
      else if (downReading == LOW) { 
        charIndex++; if (charIndex>=CHAR_TABLE_SIZE) charIndex=0; currentLetter=CHAR_TABLE[charIndex]; scrolled=true; 
      }
      if (scrolled) {
        lastCharUpdateTime = millis();
        okCount = 0; 
        displayUpdateNeeded = true;
      }
    }

    if (selectReading == HIGH && localLastSelectState == LOW) {
      if (currentLetter == '*') {
        // Password entry complete: Move to connection attempt (subState 33)
        strncpy(wifiChosenPass, passwordBuff, sizeof(wifiChosenPass)-1); wifiChosenPass[sizeof(wifiChosenPass)-1]='\0';
        
        // Reset button states before state transition
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;

        subState = 33; displayUpdateNeeded=true; return; 
      }

      // If pressing 'OK' on a character
      if (okCount == 1) {
        strncpy(wifiChosenPass, passwordBuff, sizeof(wifiChosenPass)-1); wifiChosenPass[sizeof(wifiChosenPass)-1]='\0';
        // Reset button states before state transition
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;
        subState = 33; displayUpdateNeeded=true; return;
      }

      if (pwdPos < (int)sizeof(wifiChosenPass) - 1) { passwordBuff[pwdPos++] = currentLetter; passwordBuff[pwdPos] = '\0'; }
      charIndex = 0; currentLetter = CHAR_TABLE[charIndex]; 
      okCount = 1;
      displayUpdateNeeded = true;
    }

    if (displayUpdateNeeded) {
      char line1[17], line2[17];
      snprintf(line1, sizeof(line1), "Pass: %s", passwordBuff);
      line1[16]='\0';
      snprintf(line2, sizeof(line2), "%c Char %d: <%c>", 
        okCount == 0 ? '>' : '!', // Use > if choosing char, ! if confirming/ready to exit
        pwdPos + 1, currentLetter);
      line2[16]='\0';
      printScreen(line1, line2); displayUpdateNeeded=false;
    }
  }

  else if (subState == 36) {
    if (displayUpdateNeeded) { displayUpdateNeeded=false; } 
      if (selectReading == HIGH && localLastSelectState == LOW) {
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;
        
        // Check if we entered directly and adjust exit state
        if (enteredDirectlyThisSession) {
          enteredDirectlyThisSession = false;
          subState = 0;
          menuState = -2; // Go to Scan/Idle state
        }
        else {
          // Normal exit path (back to Settings Menu)
          subState = 0; 
          menuState = 3; 
          settingsSelection = 0; 
          displayUpdateNeeded = true;
        }
        return;
    }
  }

  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}

void loop() {
  if (menuState > -1) { handleInput(); }
  switch (menuState) {
    case -3:
    case -2:
    handleAuthentication();
    break;
    case 0:
    // Main menu idle
    break;
    case 1:
    addUser();
    break;
    case 2:
    deleteUser();
    break;
    case 3:
    settings();
    break;
    default:
    menuState = -2;
    break;
  }

  indicatorUpdate();
  doorUpdate();
  delay(10);
}
