/* Licensed under the GNU GPL 3.0 */

#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_AS7341.h>
#include <Servo.h>
#include "HX711.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define SS_PIN 23
#define RST_PIN 22
#define PBY_PIN 33
#define PBN_PIN 32
#define LOADCELL_DOUT_PIN 36
#define LOADCELL_SCK_PIN 37
#define SERVOR_PIN 28
#define SERVOC_PIN 29

#define LCD_COLS 20
#define LCD_ROWS 4
#define SCROLL_INTERVAL 300 // milliseconds

MFRC522 mfrc522(SS_PIN, RST_PIN);                // RFID
Adafruit_AS7341 as7341;                          // Spectrometer
HX711 scale;                                     // Load Cell
Servo servo180;                                  // 180 Servo
Servo servoCont;                                 // Cont. Servo
LiquidCrystal_I2C lcd(0x27, LCD_ROWS, LCD_COLS); // LCD

const int STOP_PULSE = 1500;    // µs (servo stop pulse)
const int FORWARD_PULSE = 0;    // µs (rotates CW)
const int REVERSE_PULSE = 3000; // µs (rotates CCW)
const int ROTATE_90_MS = 3200;  // ms needed for ~90° rotation at this pulse


int ROTATE_90_MS_CCW[] = { 2900, 2700, 2300, 2500 };   // ms needed for ~90° rotation at this pulse
int ROTATE_90_MS_CW[] = { 3000, 2800, 2500, 2250 };  // ms needed for ~90° rotation at this pulse

const int OPEN_ANGLE = 60;
const int CLOSE_ANGLE = 130;

enum MachineState
{
  LFCARD,
  POINTSANDBOTTLECONFIRMATION,
  PREDROPBOTTLE,
  DROPBOTTLE,
  BOTTLEACCEPT,
  BOTTLEREJECT,
  POSTBOTTLE,
  DROPAGAIN
};
enum BottleType
{
  ALUMINUM,
  PET,
  GLASS,
  REJECT
};
enum ServoType
{
  CONT,
  ROT
};
struct UltrasonicSensor
{
  uint8_t trigPin;
  uint8_t echoPin;
};

String machineID = "vend1";
String currentCard = "";
String cardName = "";
int cardPoints = 0;

UltrasonicSensor sensors[3] = {
    // vcc/5v => 5v
    // gnd => gnd
    {42, 43}, // [0]: trigPin=42, echoPin=43
    {44, 45}, // [1]: trigPin=44, echoPin=45
    {46, 47}  // [2]: trigPin=46, echoPin=47
};

BottleType currentType = REJECT;
MachineState currentState = LFCARD;

bool enteredState = false;

bool hasResponse = false;
int servoPos = 0;

struct CardDataState
{
  bool shownWelcome;
  bool shownPoints;
  bool requestSent;
  bool gotName;
  bool gotPoints;
  String responseBuffer;
};

CardDataState cardState;

unsigned long stateEntryMillis = 0;
bool stateInitDone = false;

// Simple debounce
unsigned long lastButtonPressMillis = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 50;

const long HX711_MIN_DETECT = 50;   
const long HX711_MAX_DETECT = 3000; 
const long GLASS_WEIGHT = 1000;     
const int METAL_THRESHOLD = 250;    
const long BIN_FULL_DISTANCE_CM = 6;

const bool DISABLE_SERIAL = false;
const bool DISABLE_SERIAL_VERIF = false;
const bool DISABLE_RFID = false;
const bool DISABLE_LOADCELL = true;
const bool DISABLE_SPECTROMETER = false;
const bool DISABLE_IPS = false;
const bool DISABLE_ULTRASONIC = false;

bool buttonPressed(uint8_t pin)
{
  if (pin != 33)
  {
    int state = digitalRead(pin);
    if (state == LOW)
    {
      unsigned long now = millis();
      if (now - lastButtonPressMillis > BUTTON_DEBOUNCE_MS)
      {
        lastButtonPressMillis = now;
        delay(5);
        return true;
      }
    }
    return false;
  }
  else
  {
    return analogRead(A1) < 800;
  }
}

bool isBinFull(int sensorIndex)
{
  if (DISABLE_ULTRASONIC)
    return false;
  return (readUltrasonic(sensors[sensorIndex]) < BIN_FULL_DISTANCE_CM);
}

long readUltrasonic(UltrasonicSensor &sensor)
{
  digitalWrite(sensor.trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(sensor.trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(sensor.trigPin, LOW);

  unsigned long duration = pulseIn(sensor.echoPin, HIGH, 30000UL);
  long distance = (long)(duration * 0.034 / 2); // cm
  if (duration == 0)
    return 9999;
  return distance;
}

void printOnce(const char *msg)
{
  if (!enteredState)
  {
    Serial.println(msg);
    enteredState = true;
  }
}

// ---------- LCD Line Handlers ----------
// ---------- Scroll State ----------
struct ScrollState
{
  String text;
  uint8_t col;
  unsigned long lastUpdate;
  int index;
  bool active;
};

ScrollState scrollStates[LCD_ROWS];
void lcdClearScreen()
{
  lcd.clear();

  // Reset all scroll states
  for (int i = 0; i < LCD_ROWS; i++)
  {
    scrollStates[i].active = false;
    scrollStates[i].text = "";
    scrollStates[i].index = 0;
  }
}
void lcdPrintHelper(String text, uint8_t line, uint8_t col,
                    bool scrolling = false, bool wrap = false)
{

  if (line >= LCD_ROWS || col >= LCD_COLS)
    return;

  int remainingSpace = LCD_COLS - col;

  // --- Normal Print ---
  if (!scrolling && !wrap)
  {
    lcd.setCursor(col, line);
    lcd.print(text.substring(0, remainingSpace));
    scrollStates[line].active = false;
    return;
  }

  // --- Wrap Mode ---
  if (wrap)
  {
    uint8_t currentLine = line;
    uint8_t currentCol = col;

    for (int i = 0; i < text.length(); i++)
    {
      lcd.setCursor(currentCol, currentLine);
      lcd.print(text[i]);

      currentCol++;
      if (currentCol >= LCD_COLS)
      {
        currentCol = 0;
        currentLine++;
        if (currentLine >= LCD_ROWS)
          break;
      }
    }
    scrollStates[line].active = false;
    return;
  }

  // --- Scrolling Mode (Non-blocking) ---
  ScrollState &state = scrollStates[line];

  if (!state.active || state.text != text)
  {
    state.text = text;
    state.col = col;
    state.lastUpdate = millis();
    state.index = 0;
    state.active = true;
  }

  if (millis() - state.lastUpdate >= SCROLL_INTERVAL)
  {
    state.lastUpdate = millis();

    lcd.setCursor(col, line);

    if (state.index <= text.length() - remainingSpace)
    {
      lcd.print(text.substring(state.index,
                               state.index + remainingSpace));
      state.index++;
    }
    else
    {
      state.index = 0;
    }
  }
}

void stopServo()
{
  servoCont.writeMicroseconds(STOP_PULSE);
}
void setServoPos(int toServoPos)
{
  Serial.println("Rotating servo from " + String(servoPos) + " to " + String(toServoPos));
  if (toServoPos == servoPos)
    return;
  while (servoPos != toServoPos)
  {
    if (servoPos < toServoPos)
    {
      Serial.println("Rotating CW");
      servoCont.writeMicroseconds(FORWARD_PULSE);
      delay(ROTATE_90_MS_CW[servoPos]);
      stopServo();
      servoPos++;
    }
    else
    {
      Serial.println("Rotating CCW");
      servoCont.writeMicroseconds(REVERSE_PULSE);
      delay(ROTATE_90_MS_CCW[servoPos]);
      stopServo();
      servoPos--;
    }
  }
}
void rotateToAngle(int angle)
{
  servo180.write(angle);
}

void setup()
{
  lcd.init();
  lcd.backlight();
  lcdClearScreen();
  Serial.begin(115200);
  Serial1.begin(115200);
  while (!Serial);
  SPI.begin();
  pinMode(SS_PIN, OUTPUT);
  mfrc522.PCD_Init();

  pinMode(PBY_PIN, INPUT_PULLUP);
  pinMode(PBN_PIN, INPUT_PULLUP);
  randomSeed(analogRead(A0));
  while (!Serial1);

  lcdPrintHelper("Starting...", 1, 0, false, true);
  Serial.println("Waiting for WiFi...");
  Serial1.println("#ISREADY");
  while (true)
  {
    if (DISABLE_SERIAL || DISABLE_SERIAL_VERIF)
      break;
    if (Serial1.available() > 0)
    {
      String response = Serial1.readStringUntil('\n');
      response.trim();
      if (response.startsWith("#READY"))
      {
        break;
      }
      else if (response.startsWith("#NOTREADY"))
      {
        Serial.println("Wifi not ready, retrying...");
        delay(1000);
        Serial1.println("#ISREADY");
      }
      else
      {
        Serial.println("Unexpected response: " + response);
      }
    }
  }
  lcdClearScreen();
  Serial1.println("#CLEARID");
  // cardPoints = random(0, 67);

  servo180.attach(SERVOR_PIN);
  servo180.write(CLOSE_ANGLE);
  servoCont.attach(SERVOC_PIN);

  if (!DISABLE_LOADCELL)
  {
    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  }
  if (!DISABLE_SPECTROMETER) {
    if (!as7341.begin())
    {
      Serial.println("Could not find AS7341");
      while (1)
      {
        delay(10);
      }
    }
    as7341.setATIME(100);
    as7341.setASTEP(999);
    as7341.setGain(AS7341_GAIN_256X);
  }
  for (int i = 0; i < 3; i++)
  {
    pinMode(sensors[i].trigPin, OUTPUT);
    pinMode(sensors[i].echoPin, INPUT);
  }
}

void loop()
{
  static MachineState lastState = (MachineState)-1;
  if (currentState != lastState)
  {
    enteredState = false;
    stateInitDone = false;
    lastState = currentState;
    stateEntryMillis = millis();
  }

  switch (currentState)
  {
  case LFCARD:
  {
    printOnce("Welcome to SustainoVend! Please scan your ID to continue...");
    lcdPrintHelper("Welcome to", 0, 5, false, true);
    lcdPrintHelper("SustainoVend!", 1, 4, false, true);
    lcdPrintHelper("Please scan your ID to continue...", 3, 0, true, false);
    // Look for new cards
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
    {
      // Show UID on serial monitor
      lcdClearScreen();
      String content = "";
      for (byte i = 0; i < mfrc522.uid.size; i++)
      {
        if (mfrc522.uid.uidByte[i] < 0x10)
          content += "0";
        content += String(mfrc522.uid.uidByte[i], HEX);
      }
      content.toUpperCase();
      Serial.println();
      Serial.print("Message : ");
      Serial.println(content);
      currentCard = content;
      lcdPrintHelper("Your ID is: " + currentCard, 0, 0, false, true);

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      // delay(2500);
      lcdClearScreen();
      hasResponse = false;
      Serial1.println("#CLEARID");
      // clear serial
      while (Serial1.available() > 0)
      {
        Serial1.read();
      }
      currentState = POINTSANDBOTTLECONFIRMATION;
    }
    if (DISABLE_RFID)
    {
      currentCard = "ABAB4709";
      currentState = POINTSANDBOTTLECONFIRMATION;
    }
    break;
  }

  case POINTSANDBOTTLECONFIRMATION:
  {
    if (!stateInitDone)
    {
      cardState.shownWelcome = false;
      cardState.shownPoints = false;
      cardState.requestSent = false;
      cardState.gotName = false;
      cardState.gotPoints = false;
      cardState.responseBuffer = "";
      cardName = "";
      cardPoints = 0;
      hasResponse = false;
      stateInitDone = true;
    }

    if (DISABLE_SERIAL)
    {
      cardName = "Test User";
      cardPoints = random(0, 67);
      hasResponse = true;
    }

    if (!DISABLE_SERIAL)
    {
      if (!cardState.requestSent)
      {
        Serial1.println("#LOADID-" + currentCard);
        cardState.requestSent = true;
      }

      while (!hasResponse && Serial1.available() > 0)
      {
        char c = (char)Serial1.read();
        cardState.responseBuffer += c;

        if (c == '\n')
        {
          cardState.responseBuffer.trim();
          if (cardState.responseBuffer.startsWith("#NAME-"))
          {
            cardName = cardState.responseBuffer.substring(6);
            if (cardName.length() == 0)
              cardName = "Guest";
            cardState.gotName = true;
          }
          else if (cardState.responseBuffer.startsWith("#POINTS-"))
          {
            cardPoints = cardState.responseBuffer.substring(8).toInt();
            cardState.gotPoints = true;
          }
          cardState.responseBuffer = "";

          if (cardState.gotName && cardState.gotPoints)
          {
            hasResponse = true;
            break;
          }
        }
      }

      if (!hasResponse)
      {
        if (millis() - stateEntryMillis < 5000)
        {
          printOnce("Loading card data...");
          lcdPrintHelper("Loading card data...", 1, 0, false, true);
        }
        else
        {
          hasResponse = true;
          if (!cardState.gotName)
            cardName = "Guest";
          if (!cardState.gotPoints)
            cardPoints = 0;
        }
      }
      else if (!cardState.gotName)
      {
        cardName = "Guest";
      }
    }

    if (hasResponse && !cardState.shownWelcome)
    {
      char msg[100];
      const char *name = cardName.c_str();
      snprintf(msg, sizeof(msg), "Welcome, %s!", name);
      Serial.println(msg);
      lcdClearScreen();
      lcdPrintHelper(msg, 1, 0, false, true);
      delay(2000);
      lcdClearScreen();
      cardState.shownWelcome = true;
    }

    if (hasResponse && cardState.shownWelcome && !cardState.shownPoints)
    {
      char msg[100];
      snprintf(msg, sizeof(msg), "You have %d points.", cardPoints);
      Serial.println(msg);
      lcdClearScreen();
      lcdPrintHelper(msg, 1, 0, false, true);
      delay(2000);
      lcdClearScreen();
      cardState.shownPoints = true;
    }

    if ((hasResponse && cardState.shownWelcome && cardState.shownPoints) || DISABLE_SERIAL)
    {
      lcdPrintHelper("Do you want to drop a bottle?", 1, 0, false, true);

      if (buttonPressed(PBY_PIN))
      {
        lcdClearScreen();
        Serial.println("Please drop a bottle");
        lcdPrintHelper("Please drop a bottle", 1, 0, false, true);
        rotateToAngle(CLOSE_ANGLE);
        currentState = PREDROPBOTTLE;
      }
      else if (buttonPressed(PBN_PIN))
      {
        lcdClearScreen();
        Serial.println("Thank you.");
        lcdPrintHelper("Thank you.", 1, 0, false, true);
        delay(2500);
        lcdClearScreen();
        currentState = LFCARD;
        Serial1.println("#CLEARID");
      }
    }

    break;
  }

  case PREDROPBOTTLE:
  {
    if (!stateInitDone)
    {
      if (!DISABLE_LOADCELL)
      {
        scale.set_scale(20.f);
        scale.tare();
      }
      stateInitDone = true;
    }

    printOnce("Waiting for bottle on load cell...");

    if (DISABLE_LOADCELL)
    {
      
      int timer = 6000;
      while (timer > 0)
      {
        delay(1000);
        timer -= 1000;
        lcdClearScreen();
        lcdPrintHelper("Please drop a bottle", 1, 0, false, true);
        lcdPrintHelper(String(timer / 1000) + "s remaining", 2, 0, false, true);
      }
      currentState = DROPBOTTLE;
      break;
    }

    if (!scale.is_ready())
    {
      Serial.println("HX711 not found.");
      lcdClearScreen();
      lcdPrintHelper("Load cell not ready", 1, 0, false, true);
      delay(500);
      break;
    }
    else
    {
      lcdClearScreen();
      Serial.println("Please drop a bottle");
      lcdPrintHelper("Please drop a bottle", 1, 0, false, true);
    }

    long weight = scale.get_units(10);
    Serial.print("HX711 reading (raw avg): ");
    Serial.println(weight);

    if (weight > HX711_MAX_DETECT)
    {
      Serial.println("Object too heavy.");
      lcdClearScreen();
      lcdPrintHelper("Object too heavy.", 1, 0, false, true);
      delay(2000);
      lcdClearScreen();
      currentState = BOTTLEREJECT;
      break;
    }

    if (weight > (HX711_MIN_DETECT + 10) && weight < HX711_MAX_DETECT)
    {
      lcdClearScreen();
      Serial.println("Bottle detected.");
      lcdPrintHelper("Bottle detected.", 1, 0, false, true);
      delay(1500);
      currentState = DROPBOTTLE;
      break;
    }

    if (millis() - stateEntryMillis > 15000)
    {
      Serial.println("No bottle detected. Returning to card scan.");
      lcdClearScreen();
      lcdPrintHelper("No bottle detected.", 1, 0, false, true);
      delay(2000);
      lcdClearScreen();
      currentState = LFCARD;
      break;
    }

    Serial.println("Waiting for bottle on load cell...");
    lcdClearScreen();
    lcdPrintHelper("Please place bottle", 1, 0, false, true);
    delay(500);
    break;
  }

  case DROPBOTTLE:
  {
    lcdClearScreen();
    printOnce("Analyzing object...");
    lcdPrintHelper("Analyzing object...", 1, 0, false, true);

    bool metally = analogRead(A0) < METAL_THRESHOLD;
    Serial.println("1");

    uint16_t spectrometer[12]; 
    if (!as7341.readAllChannels(spectrometer) && !DISABLE_SPECTROMETER)
    {
       Serial.println("Error reading all channels! Retrying...");
       delay(500);
       break;
    }

    long weight = 0;
    if (scale.is_ready())
    {
      weight = scale.get_units(10);
      Serial.print("HX711 reading (raw avg): ");
      Serial.println(weight);
    }
    else if (!DISABLE_LOADCELL)
    {
      Serial.println("HX711 not found.");
      break;
    }

    // sense
    String typeStr = "";
    currentType = REJECT;
    Serial.println(spectrometer[11]);
    Serial.println(analogRead(A0));
    Serial.println(metally);
    if (!DISABLE_SPECTROMETER)
    {
      if (metally)
      {
        currentType = ALUMINUM;
        typeStr = "Aluminum Can";
      }
      else if (spectrometer[11] >= 3500 && spectrometer[11] <= 6000)
      {
        currentType = PET;
        typeStr = "Plastic (PET) Bottle";
      }
      else if (spectrometer[11] <= 3500 && spectrometer[11] >= 1500)
      {
        currentType = GLASS;
        typeStr = "Glass Bottle";
      }
    }
    else
    {
      Serial.println("2");
      if (metally)
      {
        currentType = ALUMINUM;
        typeStr = "Aluminum Can";
      }
      else if (weight <= GLASS_WEIGHT)
      {
        currentType = PET;
        typeStr = "Plastic (PET) Bottle";
      }
      else
      {
        currentType = GLASS;
        typeStr = "Glass Bottle";
      }
    }
    //else
    //{
    //  int randomChoice = 0; // 0 = Aluminum, 1 = PET, 2 = Glass
    //  if (randomChoice == 0)
    //  {
    //    currentType = ALUMINUM;
    //    typeStr = "Aluminum Can";
    //  }
    //  else if (randomChoice == 1)
    //  {
    //    currentType = PET;
    //    typeStr = "Plastic (PET) Bottle";
    //  }
    //  else if (randomChoice == 2)
    //  {
    //    currentType = GLASS;
    //    typeStr = "Glass Bottle";
    //  }
    //}

    Serial.println("3");
    if (currentType != REJECT)
    {
      Serial.println("Detected Bottle Type: ");
      Serial.println(typeStr);
      lcdClearScreen();
      lcdPrintHelper("Detected Bottle Type: ", 1, 0, false, true);
      lcdPrintHelper(typeStr, 2, 0, false, true);
      currentState = BOTTLEACCEPT;
    }
    else
    {
      Serial.println("Invalid Object");
      lcdClearScreen();
      lcdPrintHelper("Invalid Object", 1, 0, false, true);
      currentState = BOTTLEREJECT;
    }
    delay(1500);
    break;
  }

  case BOTTLEACCEPT:
  {
    printOnce("Accepting bottle and routing to bin...");
    lcdClearScreen();
    lcdPrintHelper("Accepting bottle and routing to bin...", 1, 0, false, true);
    
    switch (currentType)
    {
    case PET:
      if (isBinFull(0))
      {
        Serial.println("Bin is full, cannot proceed.");
        lcdClearScreen();
        lcdPrintHelper("Bin is full, cannot proceed.", 1, 0, false, true);
        currentState = BOTTLEREJECT;
        break;
      }
      setServoPos(3);
      break;
    case ALUMINUM:
      if (isBinFull(1))
      {
        Serial.println("Bin is full, cannot proceed.");
        lcdClearScreen();
        lcdPrintHelper("Bin is full, cannot proceed.", 1, 0, false, true);
        currentState = BOTTLEREJECT;
        break;
      }
      setServoPos(2);
      break;
    case GLASS:
      if (isBinFull(2))
      {
        Serial.println("Bin is full, cannot proceed.");
        lcdClearScreen();
        lcdPrintHelper("Bin is full, cannot proceed.", 1, 0, false, true);
        currentState = BOTTLEREJECT;
        break;
      }
      setServoPos(1);
      break;
    default:
      currentState = BOTTLEREJECT;
      setServoPos(0);
      break;
    }
    rotateToAngle(OPEN_ANGLE);
    if (currentState != BOTTLEREJECT)
      currentState = POSTBOTTLE;
    delay(2000);
    break;
  }

  case BOTTLEREJECT:
  {
    rotateToAngle(OPEN_ANGLE);
    printOnce("Please claim your item from the rejection bin below.");
    lcdClearScreen();
    lcdPrintHelper("Please claim your item from the rejection bin below.", 1, 0, false, true);
    delay(1500);
    currentType = REJECT;
    currentState = POSTBOTTLE;
    break;
  }

  case POSTBOTTLE:
  {
    rotateToAngle(CLOSE_ANGLE);
    printOnce("Adding points and asking if user wants to continue...");
    int addedPoints = 0;
    if (currentType != REJECT)
    {
      if (currentType == PET)
      {
        addedPoints = 2;
      }
      else if (currentType == ALUMINUM)
      {
        addedPoints = 3;
      }
      else if (currentType == GLASS)
      {
        addedPoints = 4;
      }
      cardPoints += addedPoints;
      if (!DISABLE_SERIAL)
      {
        Serial1.println("#ADD-" + String(addedPoints));
      }
      Serial.print("Added Points: ");
      lcdClearScreen();
      lcdPrintHelper("Added Points: ", 1, 0, false, true);
      lcdPrintHelper(String(addedPoints), 2, 0, false, true);
      delay(1500);
      Serial.println(addedPoints);
      Serial.print("New Total: ");
      lcdClearScreen();
      lcdPrintHelper("New Total: ", 1, 0, false, true);
      lcdPrintHelper(String(cardPoints), 2, 0, false, true);
      delay(1500);
      Serial.println(cardPoints);
    }

    Serial.println("Do you want to drop another bottle?");
    lcdClearScreen();
    lcdPrintHelper("Do you want to drop another bottle?", 1, 0, false, true);
    currentState = DROPAGAIN;
    
    setServoPos(0);
    if (!DISABLE_LOADCELL)
    {
      scale.set_scale(20.f);
      scale.tare();
    }
    break;
  }
  case DROPAGAIN:
  {
    if (buttonPressed(PBY_PIN))
    { // yes
      lcdClearScreen();
      Serial.println("Please drop a bottle:");
      lcdPrintHelper("Please drop a bottle:", 1, 0, false, true);
      currentState = PREDROPBOTTLE;
    }
    else if (buttonPressed(PBN_PIN))
    { // no
      lcdClearScreen();
      Serial.println("Thank you.");
      lcdPrintHelper("Thank you.", 1, 0, false, true);
      delay(2500);
      lcdClearScreen();
      currentState = LFCARD;
      Serial1.println("#CLEARID");
    }
    break;
  }

  default:
    currentState = LFCARD;
    break;
  }
}