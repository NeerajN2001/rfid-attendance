// Required library for 16x2 LCD with I2C adapter
#include <LiquidCrystal_I2C.h>
// Required libraries for RFID (MFRC522)
#include <SPI.h>
#include <MFRC522.h>
#include <avr/pgmspace.h> // Required for F() macro and PROGMEM string functions

// --- LCD Configuration ---
// Check your I2C address; common ones are 0x27 or 0x3F.
LiquidCrystal_I2C lcd(0x27, 16, 2); 

// --- Button Pin Definitions ---
#define UP_PIN 6
#define SELECT_PIN 5
#define DOWN_PIN 4

// --- RFID Configuration (Common default pins) ---
#define RST_PIN 9 	// Configurable reset pin 
#define SS_PIN 10 	// SPI Slave Select (SS) pin
MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522 instance

// Debounce and Timing constants
const int DEBOUNCE_DELAY = 50; // Milliseconds to wait for a stable input
const unsigned long LONG_PRESS_DURATION = 2000; // 2 seconds for 'RETURN' or 'EXIT'
const unsigned long MENU_ENTRY_DURATION = 5000; // 5 seconds for menu entry trigger (now triggers AUTH)
const unsigned long POST_ENTRY_DELAY = 2000; // 2000ms delay after entry to ignore button releases
const unsigned long SERIAL_TIMEOUT = 5000; // 5 seconds to wait for a host reply
const unsigned long SCAN_DISPLAY_TIME = 3000; // 3 seconds to show transaction results

// --- Menu Configuration (Moved to Flash using PROGMEM) ---
// Main Menu
const char menu1[] PROGMEM = "1. Add User";
const char menu2[] PROGMEM = "2. Delete User";
const char menu3[] PROGMEM = "3. Settings";

const char* const menuItems[] PROGMEM = {
  menu1,
  menu2,
  menu3,
};
const int MENU_SIZE = sizeof(menuItems) / sizeof(menuItems[0]);

// Settings Sub-Menu (Kept in RAM for easier access/padding, since they are few)
const char* const settingsMenuItems[] = {
    "1. Set Timer",
    "2. Check Conn",
};
const int SETTINGS_MENU_SIZE = sizeof(settingsMenuItems) / sizeof(settingsMenuItems[0]);


// --- Menu State Variables ---
int currentSelection = 0; // Tracks which item is highlighted (0 to MENU_SIZE - 1)
int menuState = -2; 	// -3: Authentication Mode, -2: Scan/Idle Mode, 0: Main Menu, 1-N: Action State

// --- Debounce/Timing State Variables ---
unsigned long lastDebounceTime = 0;
unsigned long selectPressStart = 0; 
unsigned long entryPressStart = 0; 	
unsigned long menuEntryTime = 0; 	
bool isLongPressReturnActive = false; 

// We track the last *stable* state for each button
int lastUpState = HIGH;
int lastDownState = HIGH;
int lastSelectState = HIGH;

// --- JSON Buffer Size (Optimized for safe margin) ---
const int MAX_REPLY_BUFFER_SIZE = 64; 
const int MAX_SCAN_BUFFER_SIZE = 128; // Larger for transaction replies
const int MAX_NAME_SIZE = 32; // Increased size for user name

// --- GLOBAL FLAGS FOR VISUAL SYNCHRONIZATION ---
// FIX: Flag for mainCardScan display status is now global for cross-function reset.
bool mainScanDisplayDrawn = false;


// --- Function Prototypes (Updated handleAuthentication) ---
void displayMenu(); 
void handleInput();
void handleAuthentication();
void mainCardScan();
void addUser();
void deleteUser();
void settings();
void printUIDHex();
char* extractValue(const char* buffer, const char* key, char* result, int resultSize);


// --- Helper Function for LCD Printing ---
// 1. Both lines are Flash strings (F())
void printScreen(const __FlashStringHelper* line1, const __FlashStringHelper* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// 2. Both lines are RAM strings (char*)
void printScreen(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// 3. Line 1 is Flash, Line 2 is RAM (FIX for Search/Title screens)
void printScreen(const __FlashStringHelper* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// 4. Line 1 is RAM, Line 2 is Flash (FIX for Settings confirmation screen)
void printScreen(const char* line1, const __FlashStringHelper* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}


// Helper to convert UID to Hex string
void printUIDHex() {
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) {
            Serial.print(F("0"));
        }
        Serial.print(mfrc522.uid.uidByte[i], HEX); 
    }
}

// --- Helper Function for Centralized JSON Output (Optimized) ---
void sendSerialJson(const __FlashStringHelper* md, const char* id, const char* un, const char* ut, const __FlashStringHelper* opt, const char* tm) {
  // Start the JSON object and print the mandatory 'md' (Mode) field
  Serial.print(F("{\"md\":\""));
  Serial.print(md);
  Serial.print(F("\"")); // Close quote for 'md' value

  // Include ID if provided
  if (id && id[0] != '\0') {
    Serial.print(F(", \"id\":\""));
    Serial.print(id);
    Serial.print(F("\""));
  }

  // Include user name (un) if provided
  if (un && un[0] != '\0') {
    Serial.print(F(", \"un\":\""));
    Serial.print(un);
    Serial.print(F("\""));
  }

  // Include user title (ut) if provided
  if (ut && ut[0] != '\0') {
    Serial.print(F(", \"ut\":\""));
    Serial.print(ut);
    Serial.print(F("\""));
  }

  // Include option (opt) if provided
  if (opt) {
    Serial.print(F(", \"opt\":\""));
    Serial.print(opt);
    Serial.print(F("\""));
  }

  // Include time (tm) if provided
  if (tm && tm[0] != '\0') {
    Serial.print(F(", \"tm\":\""));
    Serial.print(tm);
    Serial.print(F("\""));
  }

  // Close the JSON object and send newline
  Serial.println(F("}"));
}


// --- Helper Function for JSON Value Extraction ---
// Extracts a value for a given key from a JSON buffer. Returns pointer to value or NULL.
char* extractValue(const char* buffer, const char* key, char* result, int resultSize) {
  char searchKey[10];
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


// --- Setup ---
void setup() {
  Serial.begin(9600); 

  pinMode(UP_PIN, INPUT_PULLUP);
  pinMode(SELECT_PIN, INPUT_PULLUP);
  pinMode(DOWN_PIN, INPUT_PULLUP);
  
  lcd.init();
  lcd.backlight();
  lcd.clear();

  SPI.begin(); 	
  mfrc522.PCD_Init(); 
}

// --- Display Function (16x2 Scrolling) ---
void displayMenu() {
  lcd.clear();
  char buffer[17]; // Temporary RAM buffer to hold Flash string copy

  // --- Line 0: Selected Item ---
  const char *selectedItemAddr = (const char *)pgm_read_word(&menuItems[currentSelection]);
  strcpy_P(buffer, selectedItemAddr); 
  
  lcd.setCursor(0, 0);
  lcd.print(F(">")); // Print selector from Flash
  lcd.print(buffer); // Print the string from RAM
  
  // --- Line 1: Next Item ---
  int nextItemIndex = (currentSelection + 1) % MENU_SIZE;
  const char *nextItemAddr = (const char *)pgm_read_word(&menuItems[nextItemIndex]);

  strcpy_P(buffer, nextItemAddr); 

  lcd.setCursor(1, 1); // Start at col 1 to align text
  lcd.print(buffer); // Print the string from RAM
}


// --- Input Handling (Buttons Only) ---
void handleInput() {
  
  // 0. POST-ENTRY GUARD: Ignore input immediately after entering the menu
  if (menuState == 0 && (millis() - menuEntryTime) < POST_ENTRY_DELAY) {
  	return; 
  }

  int readingUp = digitalRead(UP_PIN);
  int readingDown = digitalRead(DOWN_PIN);
  int readingSelect = digitalRead(SELECT_PIN);

  // --- Debounce for Up/Down (Action on Press) ---
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    
    // Only allow navigation in the Main Menu (menuState == 0)
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

  // --- SELECT Button Logic (Long Press RETURN/EXIT) ---
  
  // 1. Detect SELECT Press (start timer)
  if (readingSelect == LOW && lastSelectState == HIGH) {
      selectPressStart = millis(); 
      isLongPressReturnActive = false; 
  }
  
  // 2. Check for Long Press DURING the hold (IMMEDIATE RETURN/EXIT)
  if (readingSelect == LOW && selectPressStart > 0) {
      if (!isLongPressReturnActive && (millis() - selectPressStart) >= LONG_PRESS_DURATION) {
          isLongPressReturnActive = true;

          if (menuState == 0) {
              menuState = -2; // Exit to Initial State
              // CRITICAL FIX: Force LCD update immediately when entering scan mode
              printScreen(F("Scan your card"), F(""));
          } else {
              menuState = 0; 	// Return to Main Menu
              displayMenu();
          }
          selectPressStart = 0; 
      }
  }

  // 3. Detect SELECT Release (check duration for short press action)
  if (readingSelect == HIGH && lastSelectState == LOW) {
      unsigned long pressDuration = millis() - selectPressStart;
      
      if (pressDuration > DEBOUNCE_DELAY && !isLongPressReturnActive) { 
          
          if (menuState == 0) {
              // --- Main Menu: SHORT PRESS = SELECT ---
              menuState = currentSelection + 1; // Transition to action state (e.g., 1 for addUser)
              // Reset long press timer on successful selection to prevent immediate return
              selectPressStart = 0; 
          } 
          // NOTE: When menuState > 0, the short press is NOT handled here, 
          // but should be handled by the action function (addUser in this case).
      }
      
      lastDebounceTime = millis();
      selectPressStart = 0; 
      isLongPressReturnActive = false; 
  }
  
  // Update the last stable state for all buttons
  lastUpState = readingUp;
  lastDownState = readingDown;
  lastSelectState = readingSelect;
}

// --- Authentication Handler (New) ---
void handleAuthentication() {
    // Auth Sub-States: 
    // 0: Scan Prompt (Default)
    // 1: Wait for Auth Reply
    // 2: Failure Acknowledge (3s delay)
    static int authSubState = 0;
    static char authUserId[17] = ""; 
    static char authSerialReplyBuffer[MAX_REPLY_BUFFER_SIZE];
    static int authSerialReplyIndex = 0;
    static unsigned long authStartTime = 0;
    static bool displayRunOnce = false;

    // --- NEW STATIC VARIABLE FOR LONG PRESS EXIT ---
    static unsigned long authSelectPressStart = 0; 
    
    // Read buttons for Long Press Access Check
    int upReading = digitalRead(UP_PIN);
    int selectReading = digitalRead(SELECT_PIN);
    int downReading = digitalRead(DOWN_PIN); // Reading all pins for state update
    
    // --- 1. Long Press Access Check (Runs in all AUTH states) ---
    bool isHolding = (upReading == LOW && selectReading == LOW);
    bool wasHolding = (entryPressStart != 0);

    if (isHolding) {
        if (!wasHolding) { // If hold just started
            entryPressStart = millis();
            printScreen(F("ENTERING MENU..."), F("")); 
        } else {
            if (millis() - entryPressStart >= MENU_ENTRY_DURATION) {
                // SUCCESS: Long press detected, transition to AUTH scan state
                menuState = -3; // Move to the dedicated Authentication Scan state
                entryPressStart = 0; 
                authSubState = 0; // Ensure substate is set to Scan Prompt
                displayRunOnce = false; // CRITICAL FIX: Force prompt display
                
                // *** CRITICAL SYNCHRONIZATION FIX ***
                // 1. Reset 2-second exit timer completely on 5s success.
                authSelectPressStart = 0; 

                // 2. Manually register button release BEFORE returning, preventing the next cycle's abort check from running.
                lastUpState = HIGH;
                lastSelectState = HIGH;
                
                return; 
            } 
        }
    } else {
        // If we were holding but released before success (failed entry)
        if (wasHolding && menuState != -3) { // Only abort if we haven't successfully transitioned yet
            entryPressStart = 0; // Reset the timer
            menuState = -2; 
            mainScanDisplayDrawn = false; // FIX: Force mainCardScan to redraw immediately
            return;
        }
    }
    
    // --- 2. Authentication Sub-States (Only run if menuState == -3) ---
    if (menuState != -3) {
        // Fallback: If not triggered by long press, run transaction scan
        mainCardScan();
        
        // --- CRITICAL FIX: UPDATE GLOBAL BUTTON STATE IN SCAN/AUTH MODE ---
        lastUpState = upReading;
        lastDownState = downReading;
        lastSelectState = selectReading;
        return;
    }
    
    // --- NEW: LONG PRESS OK EXIT CHECK (Only runs if menuState == -3) ---
    
    // 1. Detect SELECT Press (start timer)
    if (selectReading == LOW && lastSelectState == HIGH) {
        authSelectPressStart = millis(); 
    }
    
    // 2. Check for Long Press DURING the hold (IMMEDIATE EXIT)
    if (selectReading == LOW && authSelectPressStart > 0) {
        if (millis() - authSelectPressStart >= LONG_PRESS_DURATION) {
            // Long Press detected -> EXIT AUTH
            menuState = -2;
            authSubState = 0; // Reset auth state machine
            mainScanDisplayDrawn = false; // Force immediate redraw of main scan screen
            printScreen(F("Scan your card"), F(""));
            authSelectPressStart = 0; // Reset timer
            // CRITICAL: Force the selection to look released globally to prevent handleInput() misfire later
            lastSelectState = HIGH; 
            return;
        }
    }
    
    // 3. Detect SELECT Release (end timer) - This consumes short presses in Auth mode
    if (selectReading == HIGH && lastSelectState == LOW) {
        authSelectPressStart = 0;
    }

    // --- Start Sub-State Logic (Only runs if not exiting) ---
    
    switch (authSubState) {
        case 0: // Auth Scan Prompt
            if (!displayRunOnce) {
                // FIX: Ensure this is the ONLY Auth display in this block
                printScreen(F("Auth. scan card"), F("waiting...."));
                Serial.readStringUntil('\n');
                displayRunOnce = true;
                mfrc522.PCD_Init(); 
                authSerialReplyIndex = 0; 
            }

            if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
                // Store UID
                memset(authUserId, 0, sizeof(authUserId));
                for (byte i = 0; i < mfrc522.uid.size; i++) {
                    sprintf(authUserId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
                }
                
                // Send authentication request
                sendSerialJson(F("auth"), authUserId, NULL, NULL, NULL, NULL);

                // Transition to wait state
                authSubState = 1; 
                displayRunOnce = false;
                authStartTime = millis();
            }
            break;

        case 1: // Wait for Auth Reply
            if (!displayRunOnce) {
                printScreen(F("Scanned Card"), F("Processing...."));
                displayRunOnce = true;
            }

            // Check for Timeout
            if (millis() - authStartTime > SERIAL_TIMEOUT) {
                printScreen(F("Auth Timeout!"), F("Try again."));
                authSubState = 0; 
                displayRunOnce = false;
                delay(SCAN_DISPLAY_TIME); // Blocking delay to show error
                return;
            }
            
            // Non-blocking serial read
            while (Serial.available()) {
                char inChar = Serial.read();
                
                // CRITICAL FIX: Only capture characters that might be part of JSON or the newline terminator
                if (inChar != '\r' && inChar != '\n') {
                    if (authSerialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
                        authSerialReplyBuffer[authSerialReplyIndex++] = inChar;
                        authSerialReplyBuffer[authSerialReplyIndex] = '\0'; 
                    }
                }
                
                // Check for NEWLINE (End of transmission)
                if (inChar == '\n') {
                    char result[12]; // Buffer for "admin" or "not-admin"
                    
                    // --- ATTEMPT TO PARSE ---
                    if (extractValue(authSerialReplyBuffer, "rslt", result, sizeof(result))) {
                        if (strcmp(result, "admin") == 0) {
                            // SUCCESS: Admin granted access
                            menuState = 0; // Enter Main Menu
                            menuEntryTime = millis(); 
                            displayMenu();
                        } else {
                            // FAILURE: Not admin
                            authSubState = 2; // Go to failure acknowledge state
                            authStartTime = millis(); // Start 3s timer
                        }
                    } else {
                        // JSON parsing failed
                        printScreen(F("AUTH JSON ERROR!"), F("Restart host."));
                        Serial.readStringUntil('\n');
                        authSubState = 0; // Reset to scan
                        delay(SCAN_DISPLAY_TIME);
                    }
                    displayRunOnce = false;
                    authSerialReplyIndex = 0;
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
                // 3 seconds passed, revert to main scan loop
                menuState = -2; // Exit authentication state
                displayRunOnce = false;
            }
            break;
    }
    
    // --- CRITICAL FIX: UPDATE GLOBAL BUTTON STATE IN AUTH MODE ---
    lastUpState = upReading;
    lastDownState = downReading;
    lastSelectState = selectReading;
}

// --- Simple Scan Handler (If not in auth state) ---
// --- Simple Scan Handler (If not in auth state) ---
void mainCardScan() {
  // Sub-States: 
  // 0: Scan (Idle)
  // 1: Wait for Host Reply
  // 2: Tap In Result
  // 3: Tap Out / Already Tapped (Stage 1)
  // 4: Tap Out / Already Tapped (Stage 2: Duration/Retry)
  // 5: JSON Error / Timeout
  // 6: User Not Found (New state)
  static int subState = 0; 
  static unsigned long transactionStartTime = 0;
  static char scanReplyBuffer[MAX_SCAN_BUFFER_SIZE]; 
  static int serialReplyIndex = 0;
  // bool displayRunOnce is now GLOBAL (mainScanDisplayDrawn)
  static bool stage2Ready = false; // Flag to check if it's time for the second stage display
  
  // Extracted data buffers (larger for name)
  static char userName[MAX_NAME_SIZE]; 
  static char actionType[5]; // IN, OUT, FAIL
  static char timeValue[9]; // HH:MM:SS
  static char durationValue[9]; // HH:MM:SS
  static char retryValue[9]; // HH:MM:SS

  // --- Sub-State 0: Scan (Idle) ---
  if (subState == 0) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("Scan your card"), F(""));
      Serial.readStringUntil('\n');
      mainScanDisplayDrawn = true;
      mfrc522.PCD_Init(); 
    }

    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      char userId[17];
      memset(userId, 0, sizeof(userId));
      
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        sprintf(userId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
      }
      
      // Send scan request
      sendSerialJson(F("scan"), userId, NULL, NULL, NULL, NULL);

      // Transition to wait state
      subState = 1; 
      mainScanDisplayDrawn = false;
      transactionStartTime = millis();
      serialReplyIndex = 0;
    }
  }

  // --- Sub-State 1: Wait for Host Reply ---
  else if (subState == 1) {
    if (!mainScanDisplayDrawn) {
      // FIX: Display Scan / Processing....
      printScreen(F("Scan"), F("Processing...."));
      mainScanDisplayDrawn = true;
    }

    // Check for Timeout
    if (millis() - transactionStartTime > SERIAL_TIMEOUT) {
      printScreen(F("Timeout!"), F("Try again."));
      subState = 0;
      mainScanDisplayDrawn = false;
      delay(SCAN_DISPLAY_TIME); // Blocking delay to show error
      return;
    }
    
    // Non-blocking serial read
    while (Serial.available()) {
      char inChar = Serial.read();
      
      // Safety check against buffer overflow
      if (serialReplyIndex < MAX_SCAN_BUFFER_SIZE - 1) {
        scanReplyBuffer[serialReplyIndex++] = inChar;
        scanReplyBuffer[serialReplyIndex] = '\0'; 
        
        // If closing brace is found, attempt to parse the full message
        if (inChar == '}') {
          
          // --- NEW CHECK: User Not Found ---
          if (strstr(scanReplyBuffer, "\"rslt\":\"NF\"")) {
            subState = 6; // Go to User Not Found state
            transactionStartTime = millis(); // Start 3s timer
            mainScanDisplayDrawn = false;
            serialReplyIndex = 0;
            return; // Exit state
          }
          
          // --- Attempt to Parse Standard Transaction ---
          if (extractValue(scanReplyBuffer, "nm", userName, sizeof(userName)) &&
              extractValue(scanReplyBuffer, "act", actionType, sizeof(actionType))) {
            
            // Mandatory fields found. Parse optional fields.
            extractValue(scanReplyBuffer, "tm", timeValue, sizeof(timeValue));
            extractValue(scanReplyBuffer, "dur", durationValue, sizeof(durationValue));
            extractValue(scanReplyBuffer, "rtr", retryValue, sizeof(retryValue));

            transactionStartTime = millis(); // Reset timer for result display
            
            // --- Transition to Result State ---
            if (strcmp(actionType, "IN") == 0) {
              subState = 2; // Tap In (Simple 1-stage display)
            } else if (strcmp(actionType, "OUT") == 0 || strcmp(actionType, "FAIL") == 0) {
              subState = 3; // Tap Out / Already Tapped (2-stage display)
            } else {
              // Unknown action
              subState = 5; 
            }
          } else {
            // Failed to find name or action
            subState = 5;
          }
          mainScanDisplayDrawn = false;
          serialReplyIndex = 0;
          return; // Exit state after successful parse or failure
        } 
      }
    }
  }

  // --- Sub-State 2: Tap In Result (1-Stage Display) ---
  else if (subState == 2) {
    if (!mainScanDisplayDrawn) {
      char line2[17];
      snprintf(line2, sizeof(line2), "IN %s", timeValue);
      printScreen(userName, line2);
      mainScanDisplayDrawn = true;
    }

    // Wait for SCAN_DISPLAY_TIME, then reset to idle
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
    }
  }

  // --- Sub-State 3: Tap Out / Already Tapped (Stage 1) ---
  else if (subState == 3) {
    if (!mainScanDisplayDrawn) {
      char line2[17];
      
      if (strcmp(actionType, "OUT") == 0) {
        snprintf(line2, sizeof(line2), "OUT %s", timeValue);
      } else { // FAIL
        strcpy(line2, "Already Tapped");
      }
      
      printScreen(userName, line2);
      mainScanDisplayDrawn = true;
      stage2Ready = false; // Prepare for stage 2
    }

    // Wait for SCAN_DISPLAY_TIME, then transition to Stage 2
    if (!stage2Ready && (millis() - transactionStartTime > SCAN_DISPLAY_TIME)) {
      subState = 4; // Move to Stage 2
      mainScanDisplayDrawn = false;
      transactionStartTime = millis(); // Reset timer for stage 2 display
      stage2Ready = true; // Prevents re-entry into this check
    }
  }

  // --- Sub-State 4: Tap Out / Already Tapped (Stage 2) ---
  else if (subState == 4) {
    if (!mainScanDisplayDrawn) {
      char line1[17];
      char line2[17];

      if (strcmp(actionType, "OUT") == 0) {
        // Tap Out: Show Duration
        strcpy(line1, "Duration");
        strcpy(line2, durationValue);
      } else { // FAIL (Already Tapped)
        // Already Tapped: Show Retry Time
        strcpy(line1, "Try after");
        strcpy(line2, retryValue);
      }
      
      printScreen(line1, line2);
      mainScanDisplayDrawn = true;
    }

    // Wait for SCAN_DISPLAY_TIME, then reset to idle
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
      stage2Ready = false;
    }
  }

  // --- Sub-State 5: JSON Error / Unknown Action ---
  else if (subState == 5) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("JSON Error!"), F("Reset host system."));
      mainScanDisplayDrawn = true;
    }
    // Simple 3-second delay, then reset
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; 
      mainScanDisplayDrawn = false;
    }
  }
  
  // --- NEW Sub-State 6: User Not Found ---
  else if (subState == 6) {
    if (!mainScanDisplayDrawn) {
      printScreen(F("User not found"), F("Please Register"));
      mainScanDisplayDrawn = true;
    }
    // Wait for SCAN_DISPLAY_TIME, then reset to idle
    if (millis() - transactionStartTime > SCAN_DISPLAY_TIME) {
      subState = 0; // Return to Scan (Idle) mode
      mainScanDisplayDrawn = false;
    }
  }
}

// ================= CHAR TABLE FOR NAME SCROLLING =================
const char CHAR_TABLE[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789"
  " ";

const int CHAR_TABLE_SIZE = sizeof(CHAR_TABLE) - 1;


// ===================== ADD USER FUNCTION =========================
void addUser() {

  // Substates:
  // 0 = Scan ID
  // 1 = Wait search reply
  // 2 = Enter name
  // 3 = Select title
  // 4 = Wait add reply
  // 5 = Acknowledge added
  // 6 = ID already exists

  static int subState = 0;
  static char userId[17] = "";
  static char userName[MAX_NAME_SIZE];
  static char userTitle[6] = "admin";

  static int nameCharIndex = 0;
  static int okPressCount = 0;
  static bool isTitleAdmin = true;
  static bool displayUpdateNeeded = true;

  static unsigned long lastCharUpdateTime = 0;
  const unsigned long CHAR_SCROLL_DELAY = 150;

  static char serialReplyBuffer[MAX_REPLY_BUFFER_SIZE];
  static int serialReplyIndex = 0;

  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;

  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);



  // ================================================================
  // EXIT WHEN MENUSTATE CHANGES
  // ================================================================
  if (menuState != 1) {

    subState = 0;
    memset(userId, 0, sizeof(userId));
    memset(userName, 0, sizeof(userName));
    strcpy(userTitle, "admin");

    nameCharIndex = 0;
    okPressCount = 0;
    isTitleAdmin = true;
    displayUpdateNeeded = true;

    serialReplyIndex = 0;
    memset(serialReplyBuffer, 0, sizeof(serialReplyBuffer));

    localLastUpState = HIGH;
    localLastDownState = HIGH;
    localLastSelectState = HIGH;

    return;
  }



  // ================================================================
  // SUBSTATE 0: Scan ID
  // ================================================================
  if (subState == 0) {

    if (displayUpdateNeeded) {
      printScreen(F("Scan User ID"), F("Waiting..."));
      displayUpdateNeeded = false;
      serialReplyIndex = 0;
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



  // ================================================================
  // SUBSTATE 1: Wait for Search Reply
  // ================================================================
  else if (subState == 1) {

    while (Serial.available()) {
      char c = Serial.read();

      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = c;
        serialReplyBuffer[serialReplyIndex] = '\0';
      }

      if (strstr(serialReplyBuffer, "\"rslt\":\"F\"")) {
        subState = 6;
        displayUpdateNeeded = true;
        serialReplyIndex = 0;
        break;
      } else if (strstr(serialReplyBuffer, "\"rslt\":\"NF\"")) {
        subState = 2;
        displayUpdateNeeded = true;
        serialReplyIndex = 0;
        break;
      }
    }
  }



  // ================================================================
  // SUBSTATE 2: ENTER NAME (A-Z, a-z, 0-9, space)
  // ================================================================
  else if (subState == 2) {

    // static vars unique to this state
    static bool nameInit = true;
    static int charIndex = 0;
    static char currentLetter;

    if (nameInit) {
      nameInit = false;
      charIndex = 0;
      currentLetter = CHAR_TABLE[charIndex];
      displayUpdateNeeded = true;
    }

    char line1[17];
    char line2[17];


    // --- Letter scroll ---
    if (millis() - lastCharUpdateTime > CHAR_SCROLL_DELAY) {

      bool scrolled = false;

      if (upReading == LOW) {
        charIndex--;
        if (charIndex < 0) charIndex = CHAR_TABLE_SIZE - 1;
        scrolled = true;
      } else if (downReading == LOW) {
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


    // --- Select pressed ---
    if (selectReading == HIGH && localLastSelectState == LOW) {

      if (okPressCount == 1) {
        // final name done
        nameInit = true;
        subState = 3;
        okPressCount = 0;
        displayUpdateNeeded = true;
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


    // --- Display ---
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



  // ================================================================
  // SUBSTATE 3: SELECT TITLE
  // ================================================================
  else if (subState == 3) {

    if ((upReading == LOW && localLastUpState == HIGH) ||
        (downReading == LOW && localLastDownState == HIGH)) {

      isTitleAdmin = !isTitleAdmin;
      strcpy(userTitle, isTitleAdmin ? "admin" : "user");
      displayUpdateNeeded = true;
    }

    if (selectReading == HIGH && localLastSelectState == LOW) {

      printScreen(F("Adding user"), F("Processing..."));
      sendSerialJson(F("add"), userId, userName, userTitle, NULL, NULL);

      subState = 4;
      displayUpdateNeeded = true;
      serialReplyIndex = 0;
      return;
    }

    if (displayUpdateNeeded) {
      char line2[17];
      snprintf(line2, sizeof(line2), "Title: <%s>", userTitle);
      printScreen(F("Select Title"), line2);
      displayUpdateNeeded = false;
    }
  }



  // ================================================================
  // SUBSTATE 4: WAIT FOR ADD REPLY
  // ================================================================
  else if (subState == 4) {

    while (Serial.available()) {
      char c = Serial.read();

      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = c;
        serialReplyBuffer[serialReplyIndex] = '\0';
      }

      if (strstr(serialReplyBuffer, "\"rslt\":\"A\"")) {
        subState = 5;
        displayUpdateNeeded = true;
        serialReplyIndex = 0;
        break;
      }
    }
  }



  // ================================================================
  // SUBSTATE 5: USER ADDED
  // ================================================================
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



  // ================================================================
  // SUBSTATE 6: ID EXISTS
  // ================================================================
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



  // ================================================================
  // UPDATE LOCAL BUTTON STATES
  // ================================================================
  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}



void deleteUser() {
  // Static variables for state management and data storage
  // subState definitions:
  // 0: Scan
  // 1: Wait for Search Reply
  // 2: Confirmation (Delete? Yes/No)
  // 3: Wait for Delete Reply
  // 4: Acknowledge Not Found
  // 5: Acknowledge Deleted (Wait for OK)
  // 6: Finalize Cancelled (Auto-return)
  static int subState = 0; 
  static char scannedUserId[17] = ""; 
  static bool displayUpdateNeeded = true;
  static char serialReplyBuffer[MAX_REPLY_BUFFER_SIZE]; // Buffer for incoming serial response
  static int serialReplyIndex = 0;
  
  // Local button state tracking for single-press actions
  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;
  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);

  // --- State Check (Used for cleanup after global long-press exit) ---
  if (menuState != 2) { 
    subState = 0; 
    memset(scannedUserId, 0, sizeof(scannedUserId));
    serialReplyIndex = 0;
    displayUpdateNeeded = true; 
    localLastUpState = HIGH;
    localLastDownState = HIGH;
    localLastSelectState = HIGH;
    return; 
  }

  // --- SUB-STATE 0: Scan RFID ID ---
  if (subState == 0) {
    if (displayUpdateNeeded) {
      // FIX: Display "Delete user" and "Scan your card"
      printScreen(F("Delete user"), F("Scan your card"));
      displayUpdateNeeded = false;
      serialReplyIndex = 0; // Clear any old reply data
      mfrc522.PCD_Init();
    }
    
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      
      // Store UID
      memset(scannedUserId, 0, sizeof(scannedUserId));
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        sprintf(scannedUserId + (i * 2), "%02X", mfrc522.uid.uidByte[i]);
      }
      
      // 1. Send search request to serial monitor (Optimized)
      sendSerialJson(F("search"), scannedUserId, NULL, NULL, NULL, NULL);
      
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      
      subState = 1; // Move to Wait for Search Reply
      displayUpdateNeeded = true; 
    }
  }
  
  // --- SUB-STATE 1: Wait for Serial Search Reply ---
  else if (subState == 1) {
    if (displayUpdateNeeded) {
      // FIX: Use Flash, RAM overload
      printScreen(F("Searching..."), scannedUserId);
      displayUpdateNeeded = false;
    }
    
    // Non-blocking serial read and simplified search
    while (Serial.available()) {
      char inChar = Serial.read();
      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = inChar;
        serialReplyBuffer[serialReplyIndex] = '\0'; 
        
        // Check for positive result
        if (strstr(serialReplyBuffer, "\"rslt\":\"F\"")) {
          subState = 2; // Found -> Go to Confirmation
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          break; 
        } 
        // Check for negative result
        else if (strstr(serialReplyBuffer, "\"rslt\":\"NF\"")) {
          subState = 4; // Not Found -> Go to Acknowledge Not Found
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          break;
        }
      } else {
        // Buffer full, treat as not found
        subState = 4; // Go to Acknowledge Not Found
        displayUpdateNeeded = true;
        break;
      }
    }
  }
  
  // --- SUB-STATE 2: Confirmation (User Found) ---
  else if (subState == 2) {
    if (displayUpdateNeeded) {
      // FIX: Display "User found!" and "Del>ok cancle>up"
      printScreen(F("User found!"), F("Del>ok cancle>up"));
      displayUpdateNeeded = false;
    }
    
    // Check for OK (Short Select Release)
    if (selectReading == HIGH && localLastSelectState == LOW) {
      
      // 1. Send delete request (Optimized)
      sendSerialJson(F("delete"), scannedUserId, NULL, NULL, NULL, NULL);
      
      // 2. Transition to wait for delete reply
      subState = 3; 
      displayUpdateNeeded = true; 
      serialReplyIndex = 0; // Clear buffer for delete reply
    } 
    // Check for Up or Down (Press) to Cancel
    else if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
      subState = 6; // Move to Finalize Cancelled
      displayUpdateNeeded = true; 
    }
  }

  // --- SUB-STATE 3: Wait for Serial Delete Reply ---
  else if (subState == 3) {
    if (displayUpdateNeeded) {
      // FIX: Display "Deleting user" and "Processing...."
      printScreen(F("Deleting user"), F("Processing...."));
      displayUpdateNeeded = false;
    }
    
    // Non-blocking serial read and simplified search
    while (Serial.available()) {
      char inChar = Serial.read();
      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = inChar;
        serialReplyBuffer[serialReplyIndex] = '\0'; 
        
        // Check for success result: {"md":"delete", "rslt":"DL"}
        if (strstr(serialReplyBuffer, "\"rslt\":\"DL\"")) {
          subState = 5; // Success -> Go to Acknowledge Deleted
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          break; 
        } 
      }
    }
  }

  // --- SUB-STATE 4: Acknowledge Not Found (Wait for OK) ---
  else if (subState == 4) {
    if (displayUpdateNeeded) {
      // FIX: Display "User not found" and "press ok to exit"
      printScreen(F("User not found"), F("Press ok to exit"));
      displayUpdateNeeded = false;
    }

    // Check for OK (Short Select Release)
    if (selectReading == HIGH && localLastSelectState == LOW) {
        
        // Reset all states and return to Main Menu
        subState = 0; 
        menuState = 0; 
        displayMenu();
        displayUpdateNeeded = true; 
    }
  }

  // --- SUB-STATE 5: Acknowledge Deleted (Wait for OK) ---
  else if (subState == 5) {
    if (displayUpdateNeeded) {
      // FIX: Display "User Deleted" and "Press ok to exit"
      printScreen(F("User Deleted"), F("Press ok to exit"));
      displayUpdateNeeded = false;
    }

    // Check for OK (Short Select Release)
    if (selectReading == HIGH && localLastSelectState == LOW) {
        
        // Reset all states and return to Main Menu
        subState = 0; 
        menuState = 0; 
        displayMenu();
        displayUpdateNeeded = true; 
    }
  }

  // --- SUB-STATE 6: Finalize Cancelled and Return ---
  else if (subState == 6) {
    
    if (displayUpdateNeeded) {
      printScreen(F("Canceling..."), F("Returning..."));
      
      // TEMPORARY BLOCKING DELAY (500ms): Ensures the message is seen
      delay(500); 
      
      // Reset all states and return to Main Menu
      subState = 0; 
      menuState = 0; 
      displayMenu();
      displayUpdateNeeded = true; 
    }
  }

  // --- UPDATE LOCAL BUTTON STATES ---
  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}

void settings() {
  // Static variables for state management and data storage
  // subState definitions: 
  // 0: Select Setting Option
  // 11: Enter Reset Time (Group editing: H/M/S)
  // 12: Confirm Reset Time
  // 13: Wait for Reset Time Reply
  // 14: Acknowledge Update (Wait for OK)
  // 21: Check Connection (Wait for Reply)
  // 22: Acknowledge Connection (Wait for OK)
  static int subState = 0; 
  static int settingsSelection = 0; 
  
  // Time entry variables
  static int hours = 0;
  static int minutes = 0;
  static int seconds = 0;
  static int timeGroupIndex = 0; // 0=Hours, 1=Minutes, 2=Seconds
  static bool displayUpdateNeeded = true;
  static char serialReplyBuffer[MAX_REPLY_BUFFER_SIZE]; 
  static int serialReplyIndex = 0; 	

  // Local state tracking for button releases within this function
  static bool inputReady = false; 
  static int localLastUpState = HIGH;
  static int localLastDownState = HIGH;
  static int localLastSelectState = HIGH;
  int upReading = digitalRead(UP_PIN);
  int downReading = digitalRead(DOWN_PIN);
  int selectReading = digitalRead(SELECT_PIN);
  
  // --- State Check (Used only for cleanup after global long-press exit) ---
  if (menuState != 3) { 
    subState = 0; 
    settingsSelection = 0; 
    hours = 0; minutes = 0; seconds = 0;
    timeGroupIndex = 0;
    
    displayUpdateNeeded = true; 
    inputReady = false;
    localLastUpState = HIGH;
    localLastDownState = HIGH;
    localLastSelectState = HIGH;
    memset(serialReplyBuffer, 0, sizeof(serialReplyBuffer));
    serialReplyIndex = 0;
    return; 
  }
  
  // --- SUB-STATE 0: Select Setting Option ---
  if (subState == 0) {
    
    // 1. Navigation
    if (upReading == LOW && localLastUpState == HIGH) {
        settingsSelection = (settingsSelection == 0) ? (SETTINGS_MENU_SIZE - 1) : (settingsSelection - 1);
        displayUpdateNeeded = true;
    } else if (downReading == LOW && localLastDownState == HIGH) {
        settingsSelection = (settingsSelection + 1) % SETTINGS_MENU_SIZE;
        displayUpdateNeeded = true;
    }
    
    // 2. Selection
    if (selectReading == HIGH && localLastSelectState == LOW) {
        // Clear LCD immediately for clean transition
        lcd.clear();
        
        // Manual button state reset to swallow the residual release in the next state
        localLastUpState = HIGH;
        localLastDownState = HIGH;
        localLastSelectState = HIGH;

        switch(settingsSelection) {
            case 0: // Set Timer selected
                subState = 11; 
                hours = 0; minutes = 0; seconds = 0;
                timeGroupIndex = 0;
                displayUpdateNeeded = true;
                inputReady = false; // CRITICAL: Skip input on first cycle
                break;
            case 1: // Check Connection selected
                subState = 21; 
                displayUpdateNeeded = true;
                inputReady = false; // CRITICAL: Skip input on first cycle
                break;
            default:
                break;
        }
        return; 
    }

    // 3. Display (Scrolling Menu)
    if (displayUpdateNeeded) {
        char line1[17];
        char line2[17];
        
        // Line 0: Selected item
        // Format: > 1. Set Timer (Padded with spaces)
        snprintf(line1, sizeof(line1), "> %s               ", settingsMenuItems[settingsSelection]);
        line1[16] = '\0'; // Truncate to 16 characters

        // Line 1: Next item (scrolling effect)
        // Format:   2. Check Conn (Padded with spaces)
        int nextItemIndex = (settingsSelection + 1) % SETTINGS_MENU_SIZE;
        snprintf(line2, sizeof(line2), "  %s               ", settingsMenuItems[nextItemIndex]);
        line2[16] = '\0'; // Truncate to 16 characters
        
        lcd.setCursor(0, 0);
        lcd.print(line1);

        lcd.setCursor(0, 1);
        lcd.print(line2);
        
        displayUpdateNeeded = false;
    }
  }
  
  // ----------------------------------------------------
  // --- RESET TIME LOGIC (SUB-STATES 11-14) ---
  // ----------------------------------------------------

  // --- SUB-STATE 11: Enter Reset Time (Group Editing) ---
  else if (subState == 11) {
    
    // Input Guard: Skip the first cycle to swallow residual OK press
    if (!inputReady) {
        inputReady = true;
        // Proceed to display
    } else {
        // --- Input Handling: Up/Down to Scroll Group Value ---
        if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
          int* targetValue = nullptr;
          int minVal = 0;
          int maxVal = 0;

          if (timeGroupIndex == 0) { // Hours
            targetValue = &hours;
            maxVal = 23;
          } else if (timeGroupIndex == 1) { // Minutes
            targetValue = &minutes;
            maxVal = 59;
          } else if (timeGroupIndex == 2) { // Seconds
            targetValue = &seconds;
            maxVal = 59;
          }

          if (targetValue != nullptr) {
            if (upReading == LOW && localLastUpState == HIGH) {
              *targetValue = (*targetValue == maxVal) ? minVal : *targetValue + 1;
            } else if (downReading == LOW && localLastDownState == HIGH) {
              *targetValue = (*targetValue == minVal) ? maxVal : *targetValue - 1;
            }
          }
          displayUpdateNeeded = true;
        }
        
        // --- Input Handling: Select (OK) to Lock Group ---
        if (selectReading == HIGH && localLastSelectState == LOW) {
          timeGroupIndex++;
          displayUpdateNeeded = true;

          if (timeGroupIndex > 2) {
            // All groups entered, move to confirmation
            subState = 12;
            displayUpdateNeeded = true;
          }
        }
    }

    // --- Conditional Display ---
    if (displayUpdateNeeded) {
      char line2[17];
      
      // Format time string: HH:MM:SS (with selector indicator)
      snprintf(line2, sizeof(line2), "%s%02d:%s%02d:%s%02d", 
               (timeGroupIndex == 0) ? ">" : " ", hours,
               (timeGroupIndex == 1) ? ">" : " ", minutes,
               (timeGroupIndex == 2) ? ">" : " ", seconds);
      
      printScreen(F("Set Reset Time"), line2); 
      
      displayUpdateNeeded = false;
    }
  }

  // --- SUB-STATE 12: Confirm Reset Time ---
  else if (subState == 12) {
    if (displayUpdateNeeded) {
        char line1[17];
        snprintf(line1, sizeof(line1), "Confirm: %02d:%02d:%02d", hours, minutes, seconds);
        // FIX: Use RAM, Flash overload
        printScreen(line1, F("OK=Yes Up/Dn=No"));
        displayUpdateNeeded = false;
    }

    // Check for OK (Short Select Release) -> YES
    if (selectReading == HIGH && localLastSelectState == LOW) {
        subState = 13; // Move to Wait for Reply
        displayUpdateNeeded = true;
        inputReady = false; // Reset for next state
    }
    // Check for Up or Down (Press) to Cancel -> NO
    else if ((upReading == LOW && localLastUpState == HIGH) || (downReading == LOW && localLastDownState == HIGH)) {
        subState = 0; // Return to settings menu selection
        displayUpdateNeeded = true;
    }
  }

  // --- SUB-STATE 13: Wait for Reset Time Reply ---
  else if (subState == 13) {
    if (displayUpdateNeeded) {
      char timeString[9];
      snprintf(timeString, sizeof(timeString), "%02d:%02d:%02d", hours, minutes, seconds);
               
      // 1. Print Final JSON to Serial Monitor (Optimized)
      sendSerialJson(F("rst_time"), NULL, NULL, NULL, NULL, timeString);
      
      // 2. Display waiting message (FIX: Reset Time / Updating....)
      printScreen(F("Reset Time"), F("Updating...."));
      displayUpdateNeeded = false;
      serialReplyIndex = 0; // Clear buffer
    }
    
    // Non-blocking serial read and check for reply
    while (Serial.available()) {
      char inChar = Serial.read();
      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = inChar;
        serialReplyBuffer[serialReplyIndex] = '\0'; 
        
        // Check for success result: {"md":"rst_time", "rslt":"D"}
        if (strstr(serialReplyBuffer, "\"rslt\":\"D\"")) {
          subState = 14; // Success -> Go to Acknowledge Update
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          inputReady = false; // Reset for next state
          break; 
        } 
      }
    }
  }

  // --- SUB-STATE 14: Acknowledge Update (Wait for OK) ---
  else if (subState == 14) {
    if (displayUpdateNeeded) {
      // FIX: Settings Updated / Press ok to exit
      printScreen(F("Settings Updated"), F("Press ok to exit"));
      displayUpdateNeeded = false;
    }

    // Check for OK (Short Select Release)
    if (selectReading == HIGH && localLastSelectState == LOW) {
        
        // Reset all states and return to Main Menu
        subState = 0; 
        menuState = 0; 
        displayMenu();
        
        displayUpdateNeeded = true; 
    }
  }

  // ----------------------------------------------------
  // --- CHECK CONNECTION LOGIC (SUB-STATES 21-22) ---
  // ----------------------------------------------------

  // --- SUB-STATE 21: Wait for Connection Reply ---
  else if (subState == 21) {
    if (displayUpdateNeeded) {
      // 1. Send Check Connection JSON
      sendSerialJson(F("conn"), NULL, NULL, NULL, NULL, NULL);

      // 2. Display checking message (FIX: Checking Conn. / Processing....)
      printScreen(F("Checking Conn."), F("Processing...."));
      displayUpdateNeeded = false;
      serialReplyIndex = 0; // Clear buffer
    }

    // Non-blocking serial read
    while (Serial.available()) {
      char inChar = Serial.read();
      if (serialReplyIndex < MAX_REPLY_BUFFER_SIZE - 1) {
        serialReplyBuffer[serialReplyIndex++] = inChar;
        serialReplyBuffer[serialReplyIndex] = '\0'; 
        
        // Check for success result: {"md":"conn", "rslt":"C"}
        if (strstr(serialReplyBuffer, "\"rslt\":\"C\"")) {
          subState = 22; // Success -> Go to Acknowledge
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          inputReady = false; // Reset for next state
          break; 
        } 
        // Check for failure result: {"md":"conn", "rslt":"NC"}
        else if (strstr(serialReplyBuffer, "\"rslt\":\"NC\"")) {
          subState = 22; // Failure -> Go to Acknowledge
          displayUpdateNeeded = true;
          serialReplyIndex = 0;
          inputReady = false; // Reset for next state
          break;
        }
      }
    }
  }

  // --- SUB-STATE 22: Acknowledge Connection (Wait for OK) ---
  else if (subState == 22) {
    if (displayUpdateNeeded) {
      // Check which status we received (by checking the buffer content)
      bool isConnected = strstr(serialReplyBuffer, "\"rslt\":\"C\"");

      if (isConnected) {
        // FIX: Connected! / press ok to exit
        printScreen(F("Connected!"), F("Press ok to exit"));
      } else {
        // FIX: Not Connected! / press ok to exit
        printScreen(F("Not Connected!"), F("Press ok to exit"));
      }
      displayUpdateNeeded = false;
    }

    // Check for OK (Short Select Release)
    if (selectReading == HIGH && localLastSelectState == LOW) {
        
        // Reset all states and return to Main Menu
        subState = 0; 
        menuState = 0; 
        displayMenu();
        
        displayUpdateNeeded = true; 
    }
  }


  // --- UPDATE LOCAL BUTTON STATES ---
  localLastUpState = upReading;
  localLastDownState = downReading;
  localLastSelectState = selectReading;
}


// --- Main Loop (The State Machine) ---
void loop() {
  // 1. Check for user input (navigation/selection/return) 
  if (menuState > -1) { // Only check input if we are in Main Menu or an Action State
    handleInput();
  }

  // 2. Handle the current state by calling the corresponding function
  switch (menuState) {
    case -3:
    case -2:
      handleAuthentication();
      break;
      
    case 0:
      // State 0: Main Menu. Loop just waits for input from handleInput().
      break;

    case 1:
      // State 1: Option 1 (Add User) - All logic is now self-contained in this function
      addUser();
      break;

    case 2:
      // State 2: Option 2 (Delete User)
      deleteUser();
      break;

    case 3:
      // State 3: Option 3 (Settings)
      settings();
      break;

    default:
      menuState = -2; // Default to Scan/Idle mode
      break;
  }

  delay(10);
}