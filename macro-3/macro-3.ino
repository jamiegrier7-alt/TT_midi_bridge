#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_NeoPixel.h>
#include <RotaryEncoder.h>

#define NUM_PIXELS   12
#define MAX_FILES    30

// Mapped to scan down Column 1, then Column 2, then Column 3
const uint8_t keyPins[] = {
  1, 4, 7, 10,  // Column 1 (Will report as buttons 1, 2, 3, 4)
  2, 5, 8, 11,  // Column 2 (Will report as buttons 5, 6, 7, 8)
  3, 6, 9, 12   // Column 3 (Will report as buttons 9, 10, 11, 12)
};
bool lastKeyStates[12];

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &SPI1, OLED_DC, OLED_RST, OLED_CS);
Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
RotaryEncoder encoder(PIN_ROTA, PIN_ROTB, RotaryEncoder::LatchMode::FOUR3);

int32_t lastEncoderPos = 0;
bool lastEncoderSwitch = HIGH;
String inputBuffer = ""; 

bool pixelsChanged = false;
bool screenNeedsRefresh = false;

// INTERACTIVE OPERATIONAL MODES
enum OpMode { RUNTIME_MODE, MENU_MODE, NAMING_MODE, SCROLL_MODE };
OpMode currentMode = RUNTIME_MODE;

// 1. Top-Level Main Menu Tracking
int menuSelection = 0;
const int maxMenuItems = 2;
const char* menuItems[] = {
  "1. Load Patch",
  "2. Save Patch"
};

// 2. Horizontal Left-To-Right Character Matrices
String typedName = "";
int currentKeyboardPage = 0;
const char* keyboardPages[4][9] = {
  {"A", "B", "C", "D", "E", "F", "G", "H", "I"}, // Page 1: ABC top, DEF mid, GHI low
  {"J", "K", "L", "M", "N", "O", "P", "Q", "R"}, // Page 2: JKL top, MNO mid, PQR low
  {"S", "T", "U", "V", "W", "X", "Y", "Z", "_"}, // Page 3: STU top, VWX mid, YZ_ low
  {"1", "2", "3", "4", "5", "6", "7", "8", "9"}  // Page 4: 123 top, 456 mid, 789 low (0 auto-handles)
};

// 3. PD Remote Scrolling file array memory tracking
String fileBuffer[MAX_FILES];
int totalFilesReceived = 0;
int fileSelectionIdx = 0;
// Internal Screen Layout Display Renderers
void drawMainMenu();
void drawNamingKeyboard();
void drawFileBrowser();
void restoreOriginalBootScreen();

// Robust FUDI argument splitter (100% identical to yours)
String getFudiArg(String data, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data[i] == ' ' || data[i] == ';' || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// FUDI PROTOCOL ENGINE (100% identical to yours + PD background list catcher)
void parseFudiCommand(String cmd) {
  cmd.trim();
  if (cmd.endsWith(";")) {
    cmd = cmd.substring(0, cmd.length() - 1); 
    cmd.trim();
  }

  int firstSpace = cmd.indexOf(' ');
  String prefix = (firstSpace == -1) ? cmd : cmd.substring(0, firstSpace);
  String arguments = (firstSpace == -1) ? "" : cmd.substring(firstSpace + 1);

  // Background Data Receiver: Intercepts and catches list data from PD at any point
  if (prefix.equals("file_count")) {
    totalFilesReceived = getFudiArg(arguments, 0).toInt();
    if (totalFilesReceived > MAX_FILES) totalFilesReceived = MAX_FILES;
    fileSelectionIdx = 0;
    for (int i = 0; i < MAX_FILES; i++) fileBuffer[i] = ""; 
    return;
  }
  else if (prefix.equals("file_item")) {
    int itemIdx = getFudiArg(arguments, 0).toInt();
    int count = 0; int nameStartIdx = 0;
    for (unsigned int i = 0; i < arguments.length(); i++) {
      if (arguments[i] == ' ') {
        count++;
        if (count == 1) { nameStartIdx = i + 1; break; }
      }
    }
    if (itemIdx >= 0 && itemIdx < MAX_FILES && nameStartIdx < (int)arguments.length()) {
      fileBuffer[itemIdx] = arguments.substring(nameStartIdx);
    }
    if (currentMode == SCROLL_MODE) drawFileBrowser();
    return;
  }

  // Safety Gate: If running local configuration forms, block running canvas updates
  if (currentMode != RUNTIME_MODE) return;

  // 1. cls;
  if (prefix.equals("cls")) {
    display.clearDisplay();
    screenNeedsRefresh = true;
  }
  
  // 2. prn [x] [y] [size] [text];
  else if (prefix.equals("prn")) {
    int x = getFudiArg(arguments, 0).toInt();
    int y = getFudiArg(arguments, 1).toInt();
    int fontSize = getFudiArg(arguments, 2).toInt();
    
    int count = 0;
    int textStartIdx = 0;
    for (unsigned int i = 0; i < arguments.length(); i++) {
      if (arguments[i] == ' ') {
        count++;
        if (count == 3) {
          textStartIdx = i + 1;
          break;
        }
      }
    }
    String textContent = arguments.substring(textStartIdx);

    display.setTextSize(fontSize);
    display.setTextColor(SH110X_WHITE, SH110X_BLACK); 
    display.setCursor(x, y);
    display.print(textContent);
    screenNeedsRefresh = true;
  }
  
  // 3. fdr [x] [y] [width] [height] [percent];
  else if (prefix.equals("fdr")) {
    int x = getFudiArg(arguments, 0).toInt();
    int y = getFudiArg(arguments, 1).toInt();
    int w = getFudiArg(arguments, 2).toInt();
    int h = getFudiArg(arguments, 3).toInt();
    int pct = getFudiArg(arguments, 4).toInt();
    pct = constrain(pct, 0, 100);

    display.drawRect(x, y, w, h, SH110X_WHITE);
    int innerW = w - 4;
    int innerH = h - 4;
    int fillW = (innerW * pct) / 100;
    
    display.fillRect(x + 2, y + 2, innerW, innerH, SH110X_BLACK);
    if (fillW > 0) {
      display.fillRect(x + 2, y + 2, fillW, innerH, SH110X_WHITE);
    }
    screenNeedsRefresh = true;
  }
  
  // 4. rgb [key] [r] [g] [b];
  else if (prefix.equals("rgb")) {
    int fudiKeyIdx = getFudiArg(arguments, 0).toInt();
    int r = getFudiArg(arguments, 1).toInt();
    int g = getFudiArg(arguments, 2).toInt();
    int b = getFudiArg(arguments, 3).toInt();
    
    if (fudiKeyIdx >= 1 && fudiKeyIdx <= 12) {
      int internalPixelMap[] = {0, 3, 6, 9, 1, 4, 7, 10, 2, 5, 8, 11};
      int physicalLedIdx = internalPixelMap[fudiKeyIdx - 1];
      
      pixels.setPixelColor(physicalLedIdx, pixels.Color(r, g, b));
      pixelsChanged = true; 
    }
  }

  // 4b. rgb_clr;
  else if (prefix.equals("rgb_clr")) {
    for (int i = 0; i < NUM_PIXELS; i++) {
      pixels.setPixelColor(i, pixels.Color(0, 0, 0));
    }
    pixelsChanged = true;
  }

  // 5. drw_line [x0] [y0] [x1] [y1];
  else if (prefix.equals("drw_line")) {
    display.drawLine(getFudiArg(arguments, 0).toInt(), getFudiArg(arguments, 1).toInt(), getFudiArg(arguments, 2).toInt(), getFudiArg(arguments, 3).toInt(), SH110X_WHITE);
    screenNeedsRefresh = true;
  }

  // 6. drw_rect [x] [y] [width] [height] [filled];
  else if (prefix.equals("drw_rect")) {
    int x = getFudiArg(arguments, 0).toInt(); int y = getFudiArg(arguments, 1).toInt();
    int w = getFudiArg(arguments, 2).toInt(); int h = getFudiArg(arguments, 3).toInt();
    if (getFudiArg(arguments, 4).toInt() == 1) { display.fillRect(x, y, w, h, SH110X_WHITE); }
    else { display.drawRect(x, y, w, h, SH110X_WHITE); }
    screenNeedsRefresh = true;
  }

  // 7. drw_circ [x_center] [y_center] [radius] [filled];
  else if (prefix.equals("drw_circ")) {
    int cx = getFudiArg(arguments, 0).toInt(); int cy = getFudiArg(arguments, 1).toInt();
    int r = getFudiArg(arguments, 2).toInt();
    if (getFudiArg(arguments, 3).toInt() == 1) { display.fillCircle(cx, cy, r, SH110X_WHITE); }
    else { display.drawCircle(cx, cy, r, SH110X_WHITE); }
    screenNeedsRefresh = true;
  }
}
void drawMainMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
  display.setCursor(0, 0);
  display.print(" ---- CONFIG MENU ---- ");
  display.drawFastHLine(0, 10, 128, SH110X_WHITE);

  for (int i = 0; i < maxMenuItems; i++) {
    display.setCursor(4, 16 + (i * 12));
    if (i == menuSelection) {
      display.setTextColor(SH110X_BLACK, SH110X_WHITE);
      display.print("> "); display.print(menuItems[i]);
    } else {
      display.setTextColor(SH110X_WHITE, SH110X_BLACK);
      display.print("  "); display.print(menuItems[i]);
    }
  }

  // Absolute physical bottom keys: Button 4, Button 8, Button 12
  display.drawFastHLine(0, 52, 128, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(14, 55);  display.print("[ < ]"); // Button 4
  display.setCursor(56, 55);  display.print("[ > ]"); // Button 8
  display.setCursor(94, 55); display.print("[ENT]"); // Button 12
  screenNeedsRefresh = true;
}

void drawNamingKeyboard() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(2, 2);
  display.print("Save As: "); display.print(typedName);
  if ((millis() / 400) % 2 == 0) display.print("_"); 
  display.drawFastHLine(0, 13, 128, SH110X_WHITE);

  // Draws screen text left-to-right to display clean, ordered horizontal rows
  // Row 1: Char 0, 1, 2  | Row 2: Char 3, 4, 5 | Row 3: Char 6, 7, 8
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      int charIdx = (row * 3) + col; 
      int bx = col * 43; 
      int by = 16 + (row * 11);
      
      display.drawRect(bx, by, 41, 10, SH110X_WHITE);
      display.setCursor(bx + 18, by + 1);
      if (currentKeyboardPage == 3 && charIdx == 8) display.print("0");
      else display.print(keyboardPages[currentKeyboardPage][charIdx]);
    }
  }

  // Bottom text matches the absolute physical bottom row: Buttons 4, 8, 12
  display.drawFastHLine(0, 51, 128, SH110X_WHITE);
  display.setCursor(2, 54);  display.print("[P"); display.print(currentKeyboardPage + 1); display.print("]");
  display.setCursor(46, 54); display.print("[DEL]");
  display.setCursor(94, 54); display.print("[ENT]");
  screenNeedsRefresh = true;
}

void drawFileBrowser() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
  display.setCursor(2, 2);
  display.print("LOAD PATCH:");
  display.drawFastHLine(0, 13, 128, SH110X_WHITE);

  if (totalFilesReceived == 0) {
    display.setCursor(16, 30); display.print("Waiting for PD...");
  } else {
    int startIdx = (fileSelectionIdx / 3) * 3;
    for (int i = 0; i < 3; i++) {
      int targetItem = startIdx + i;
      if (targetItem >= totalFilesReceived) break;
      display.setCursor(4, 17 + (i * 11));
      if (targetItem == fileSelectionIdx) {
        display.setTextColor(SH110X_BLACK, SH110X_WHITE);
        display.print("> "); display.print(fileBuffer[targetItem]);
      } else {
        display.setTextColor(SH110X_WHITE, SH110X_BLACK);
        display.print("  "); display.print(fileBuffer[targetItem]);
      }
    }
  }
  
  display.drawFastHLine(0, 52, 128, SH110X_WHITE);
  display.setCursor(2, 55); display.print("[B4]Back"); // Button 4 drops back to menu
  display.setCursor(68, 55); display.print("[ENC]Load");
  screenNeedsRefresh = true;
}

void restoreOriginalBootScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 15);
  display.print("FUDI MODE");
  display.drawFastHLine(10, 35, 108, SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(22, 45);
  display.print("Parser Online");
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(100); 
  for (int i = 0; i < 12; i++) { pinMode(keyPins[i], INPUT_PULLUP); lastKeyStates[i] = HIGH; }
  pinMode(PIN_SWITCH, INPUT_PULLUP);
  pixels.begin(); pixels.setBrightness(50); pixels.show(); 
  display.begin(0x3C, true); restoreOriginalBootScreen();
  lastEncoderPos = encoder.getPosition();
}

void loop() {
  // 1. Scan Keypad Switches
  for (int i = 0; i < 12; i++) {
    bool currentState = digitalRead(keyPins[i]);
    if (currentState != lastKeyStates[i]) {
      int reportedIndex = i + 1;
      
      if (currentState == LOW) { 
        if (currentMode == MENU_MODE) {
          if (reportedIndex == 4) {       // Button 4 (Bottom Left) -> Scroll Up
            menuSelection = (menuSelection == 0) ? maxMenuItems - 1 : menuSelection - 1; 
            drawMainMenu();
          } 
          else if (reportedIndex == 8) {  // Button 8 (Bottom Middle) -> Scroll Down
            menuSelection = (menuSelection + 1) % maxMenuItems; 
            drawMainMenu();
          } 
          else if (reportedIndex == 12) { // Button 12 (Bottom Right) -> Enter
            if (menuSelection == 0) { currentMode = SCROLL_MODE; totalFilesReceived = 0; drawFileBrowser(); Serial.println("request_files ;"); } 
            else if (menuSelection == 1) { currentMode = NAMING_MODE; typedName = ""; drawNamingKeyboard(); }
          }
        } 
        else if (currentMode == NAMING_MODE) {
          if (reportedIndex == 4) { currentKeyboardPage = (currentKeyboardPage + 1) % 4; drawNamingKeyboard(); } // Button 4: Page Toggle
          else if (reportedIndex == 8) { if (typedName.length() > 0) typedName.remove(typedName.length() - 1); drawNamingKeyboard(); } // Button 8: Delete
          else if (reportedIndex == 12) { // Button 12: Confirm / Save Name to PD
            Serial.print("save_patch "); Serial.print(typedName); Serial.println(" ;");
            currentMode = RUNTIME_MODE; display.clearDisplay(); restoreOriginalBootScreen();
          } 
          else { 
            // Re-mapped matrix router to decode down-scanned keys into left-to-right arrays
            int charIdx = -1;
            if (reportedIndex == 1) charIdx = 0;      // Row 1, Col 1 -> Slot 1 (A / 1)
            else if (reportedIndex == 5) charIdx = 1; // Row 1, Col 2 -> Slot 2 (B / 2)
            else if (reportedIndex == 9) charIdx = 2; // Row 1, Col 3 -> Slot 3 (C / 3)
            else if (reportedIndex == 2) charIdx = 3; // Row 2, Col 1 -> Slot 4 (D / 4)
            else if (reportedIndex == 6) charIdx = 4; // Row 2, Col 2 -> Slot 5 (E / 5)
            else if (reportedIndex == 10) charIdx = 5;// Row 2, Col 3 -> Slot 6 (F / 6)
            else if (reportedIndex == 3) charIdx = 6; // Row 3, Col 1 -> Slot 7 (G / 7)
            else if (reportedIndex == 7) charIdx = 7; // Row 3, Col 2 -> Slot 8 (H / 8)
            else if (reportedIndex == 11) charIdx = 8;// Row 3, Col 3 -> Slot 9 (I / 9)

            if (charIdx != -1) {
              if (currentKeyboardPage == 3 && charIdx == 8) typedName += "0";
              else typedName += keyboardPages[currentKeyboardPage][charIdx];
              drawNamingKeyboard();
            }
          }
        } 
        else if (currentMode == SCROLL_MODE && reportedIndex == 4) { currentMode = MENU_MODE; drawMainMenu(); } 
        else { if (currentMode == RUNTIME_MODE) { Serial.print("btn "); Serial.print(reportedIndex); Serial.println(" 1 ;"); } }
      } else {
        if (currentMode == RUNTIME_MODE) { Serial.print("btn "); Serial.print(reportedIndex); Serial.println(" 0 ;"); }
      }
      lastKeyStates[i] = currentState; delay(5);
    }
  }

  // 2. Scan Encoder Turn (Active only in Scroll list mode or standard Runtime control)
  encoder.tick();
  int32_t currentEncoderPos = encoder.getPosition();
  if (currentEncoderPos != lastEncoderPos) {
    int32_t diff = currentEncoderPos - lastEncoderPos;
    if (currentMode == SCROLL_MODE && totalFilesReceived > 0) {
      fileSelectionIdx += (diff > 0) ? 1 : -1;
      if (fileSelectionIdx < 0) fileSelectionIdx = totalFilesReceived - 1;
      if (fileSelectionIdx >= totalFilesReceived) fileSelectionIdx = 0;
      drawFileBrowser();
    } else if (currentMode == RUNTIME_MODE) {
      Serial.print("enc "); Serial.print(diff); Serial.println(" ;"); 
    }
    lastEncoderPos = currentEncoderPos;
  }

  // 3. Scan Encoder Click (Exclusively toggles menu layer window on and off)
  bool currentEncoderSwitch = digitalRead(PIN_SWITCH);
  if (currentEncoderSwitch != lastEncoderSwitch) {
    if (currentEncoderSwitch == LOW) {
      if (currentMode == RUNTIME_MODE) { currentMode = MENU_MODE; menuSelection = 0; drawMainMenu(); } 
      else if (currentMode == SCROLL_MODE && totalFilesReceived > 0 && fileBuffer[fileSelectionIdx] != "") {
        Serial.print("load_patch "); Serial.print(fileBuffer[fileSelectionIdx]); Serial.println(" ;");
        currentMode = RUNTIME_MODE; display.clearDisplay(); restoreOriginalBootScreen();
      } else {
        currentMode = RUNTIME_MODE; display.clearDisplay(); restoreOriginalBootScreen();
      }
    }
    lastEncoderSwitch = currentEncoderSwitch; delay(5);
  }

  // 4. Keyboard Editor Blinking Timer Layer
  if (currentMode == NAMING_MODE) {
    static unsigned long lastCursorUpdate = 0;
    if (millis() - lastCursorUpdate > 400) { drawNamingKeyboard(); lastCursorUpdate = millis(); }
  }

  // 5. Read incoming Serial stream data from PD
  while (Serial.available() > 0) {
    char inChar = (char)Serial.read();
    if (inChar == '\n') {
      inputBuffer.trim(); if (inputBuffer.length() > 0) parseFudiCommand(inputBuffer); inputBuffer = "";
    } else { inputBuffer += inChar; }
  }

  if (pixelsChanged) { pixels.show(); pixelsChanged = false; }
  if (screenNeedsRefresh) { display.display(); screenNeedsRefresh = false; }
  delayMicroseconds(300); 
}
