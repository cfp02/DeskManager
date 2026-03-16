#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Keyboard.h>
#include <LittleFS.h>

// Set to 1 to run LED/button/PIR tests once at startup (Serial at 115200)
#define ENABLE_STARTUP_TESTS 0

// Pins from variant: seeed_xiao_rp2040/pins_arduino.h
//   LEDs: PIN_LED_R=17, PIN_LED_G=16, PIN_LED_B=25, LED_BUILTIN=17
//   NeoPixel: PIN_NEOPIXEL=12, NEOPIXEL_POWER=11
//   Digital: D0=26, D1=27, D2=28, D3=29, D4=6, D5=7, D6=0, D7=1, D8=2, D9=4, D10=3

// PIR sensor on GPIO 2 (was D8 on header; wire PIR OUT to that pin)
const int PIR_PIN = D2;

// Reminder LED: use onboard red (PIN_LED_R) or external on D1
const int LED_PIN = PIN_LED_R;

// Blue LED on header: now on D6 (GPIO 0) since GPIO 2 is used for PIR
const int LED_BLUE_PIN = D8;  // P0 on header

// Buttons: one side to pin, other side to GND. Use internal PULLUP so pin is HIGH when
// not pressed and goes LOW when pressed (active LOW). D3=29, D4=6 (D4 is also I2C SDA).
const int CLEAR_BUTTON_PIN = D3;
const int BUTTON2_PIN = D4;

// --- Configurable LED behavior (edit these) ---
// NeoPixel: RGB when you set it (0-255 each). Used e.g. on PIR or as indicator.
const uint8_t CONFIG_NEOPIXEL_R = 255;
const uint8_t CONFIG_NEOPIXEL_G = 0;
const uint8_t CONFIG_NEOPIXEL_B = 0;

// Blue LED (D6/GPIO 0): brightness when "on" (0-255). PIR alternates this LED on/off.
const uint8_t CONFIG_BLUE_LED_ON = 255;

// Onboard RGB LEDs: brightness when "on" (0-255). Button 1 turns off blue, Button 2 turns off red.
const uint8_t CONFIG_ONBOARD_R = 255;
const uint8_t CONFIG_ONBOARD_G = 128;
const uint8_t CONFIG_ONBOARD_B = 255;

// Timing
const unsigned long REMINDER_INTERVAL_MS = 20UL * 60 * 1000;  // 20 min
const unsigned long AWAY_TIMEOUT_MS = 4UL * 60 * 1000;       // 4 min
const unsigned long DEBOUNCE_MS = 200;
// NeoPixel reminder: one full soft green cycle (fade in + fade out) in ms
const unsigned long REMINDER_NEOPIXEL_CYCLE_MS = 2500;

// Macro string: loaded from LittleFS "/macro.txt" at startup, or use this default. Button 2 types it via USB HID.
#define MACRO_STRING_MAX 256
#define MACRO_FILE_PATH  "/macro.txt"
static const char MACRO_DEFAULT[] = "Hello from desk!";  // used if file missing or empty
static char macroString[MACRO_STRING_MAX];

// Volatile: updated in ISRs
volatile unsigned long lastPersonSeen = 0;
volatile bool pirTriggered = false;
volatile bool clearButtonPressed = false;
volatile bool button2Pressed = false;

// Main loop state
unsigned long reminderDueAt = 0;
unsigned long lastClearButtonPress = 0;
unsigned long lastButton2Press = 0;
unsigned long lastPIRProcessedAt = 0;
bool lastClearWasLow = false;
bool lastBtn2WasLow = false;

// NeoPixel object (1 pixel)
static Adafruit_NeoPixel neopixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// Set NeoPixel to (R,G,B), each 0–255. brightness 0–255 (use 255 for full, or e.g. 120 for soft).
void setNeoPixelColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 255) {
  neopixel.setBrightness(brightness);
  neopixel.setPixelColor(0, neopixel.Color(r, g, b));
  neopixel.show();
}

// Onboard RGB (three pins): set to (R,G,B), each 0–255. Hardware is active-LOW so we invert.
void setOnboardRgbColor(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_LED_R, 255 - r);
  analogWrite(PIN_LED_G, 255 - g);
  analogWrite(PIN_LED_B, 255 - b);
}

void onPIR() {
  lastPersonSeen = millis();
  pirTriggered = true;
}

void onClearButton() {
  clearButtonPressed = true;
}

void onButton2() {
  button2Pressed = true;
}

// Load macro string from LittleFS MACRO_FILE_PATH; on failure or empty, use MACRO_DEFAULT.
static void loadMacroString() {
  macroString[0] = '\0';
  if (!LittleFS.begin()) {
    strncpy(macroString, MACRO_DEFAULT, MACRO_STRING_MAX - 1);
    macroString[MACRO_STRING_MAX - 1] = '\0';
    Serial.println("LittleFS not mounted, using default macro");
    return;
  }
  File f = LittleFS.open(MACRO_FILE_PATH, "r");
  if (!f || f.isDirectory()) {
    strncpy(macroString, MACRO_DEFAULT, MACRO_STRING_MAX - 1);
    macroString[MACRO_STRING_MAX - 1] = '\0';
    Serial.println("No macro file, using default");
    if (f) f.close();
    return;
  }
  size_t n = f.read((uint8_t*)macroString, MACRO_STRING_MAX - 1);
  f.close();
  if (n == 0) {
    strncpy(macroString, MACRO_DEFAULT, MACRO_STRING_MAX - 1);
    macroString[MACRO_STRING_MAX - 1] = '\0';
  } else {
    macroString[n] = '\0';
    // trim trailing newline/cr if present
    while (n > 0 && (macroString[n - 1] == '\n' || macroString[n - 1] == '\r')) {
      macroString[--n] = '\0';
    }
    Serial.print("Macro loaded from file (");
    Serial.print(n);
    Serial.println(" chars)");
  }
}

// Type the macro string as USB HID keyboard (character by character with short delay).
static void sendMacroString() {
  if (macroString[0] == '\0') return;
  for (size_t i = 0; macroString[i] != '\0' && i < MACRO_STRING_MAX; i++) {
    if (macroString[i] != '\r')
      Keyboard.write((uint8_t)macroString[i]);
    delay(15);
  }
  Serial.println("Macro sent");
}

// No lights until reminder: keep blue and onboard RGB off. Only the reminder block turns on the green NeoPixel.
void applyLedState() {
  analogWrite(LED_BLUE_PIN, 0);
  setOnboardRgbColor(0, 0, 0);
}

#if ENABLE_STARTUP_TESTS
static void runStartupTests() {
  Serial.println();
  Serial.println("=== Startup tests (LEDs, buttons, PIR) ===");

  // --- LED test: cycle each output ---
  Serial.println("LED test: Onboard R (active-LOW)");
  setOnboardRgbColor(255, 0, 0);
  delay(400);
  setOnboardRgbColor(0, 0, 0);
  delay(200);

  Serial.println("LED test: Onboard G (active-LOW)");
  setOnboardRgbColor(0, 255, 0);
  delay(400);
  setOnboardRgbColor(0, 0, 0);
  delay(200);

  Serial.println("LED test: Onboard B (active-LOW)");
  setOnboardRgbColor(0, 0, 255);
  delay(400);
  setOnboardRgbColor(0, 0, 0);
  delay(200);

  Serial.println("LED test: NeoPixel (power=11, data=12) - 1 pixel, red");
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
  delay(50);
  setNeoPixelColor(255, 0, 0, 80);
  delay(1200);
  setNeoPixelColor(0, 0, 0);
  digitalWrite(NEOPIXEL_POWER, LOW);
  delay(200);

  Serial.println("LED test: Blue on D8. 1st: pin HIGH (anode->pin). 2nd: pin LOW (cathode->pin, sinking).");
  digitalWrite(LED_BLUE_PIN, HIGH);  // Lights if anode to pin, cathode to GND
  delay(700);
  digitalWrite(LED_BLUE_PIN, LOW);
  delay(300);
  digitalWrite(LED_BLUE_PIN, LOW);   // Lights if cathode to pin, anode to 3.3V (sinking)
  delay(700);
  digitalWrite(LED_BLUE_PIN, LOW);
  delay(200);

  Serial.println("LED test: External reminder LED on D1 (if connected)");
  digitalWrite(D1, HIGH);
  delay(400);
  digitalWrite(D1, LOW);
  delay(200);

  Serial.println();
  Serial.println("Button test: Press D3 or D4 (active LOW: pin goes LOW when pressed to GND). Polling + interrupt.");
  unsigned long t0 = millis();
  const unsigned long testDuration = 12000;
  unsigned long lastD3 = 0, lastD4 = 0;
  while (millis() - t0 < testDuration) {
    // Poll pins (active LOW: digitalRead == LOW when pressed)
    bool d3Low = (digitalRead(CLEAR_BUTTON_PIN) == LOW);
    bool d4Low = (digitalRead(BUTTON2_PIN) == LOW);
    if (d3Low && (millis() - lastD3 >= DEBOUNCE_MS)) {
      lastD3 = millis();
      clearButtonPressed = false;
      lastClearButtonPress = millis();
      Serial.println("  -> D3 (clear) pressed -> lighting RED");
      setOnboardRgbColor(255, 0, 0);
      delay(300);
      setOnboardRgbColor(0, 0, 0);
    }
    if (d4Low && (millis() - lastD4 >= DEBOUNCE_MS)) {
      lastD4 = millis();
      button2Pressed = false;
      lastButton2Press = millis();
      Serial.println("  -> D4 (button2) pressed -> lighting GREEN");
      setOnboardRgbColor(0, 255, 0);
      delay(300);
      setOnboardRgbColor(0, 0, 0);
    }
    // Also handle interrupt flags in case they fire
    if (clearButtonPressed && (millis() - lastClearButtonPress >= DEBOUNCE_MS)) {
      clearButtonPressed = false;
      lastClearButtonPress = millis();
      lastD3 = millis();
      Serial.println("  -> D3 (clear) pressed [IRQ] -> lighting RED");
      setOnboardRgbColor(255, 0, 0);
      delay(300);
      setOnboardRgbColor(0, 0, 0);
    }
    if (button2Pressed && (millis() - lastButton2Press >= DEBOUNCE_MS)) {
      button2Pressed = false;
      lastButton2Press = millis();
      lastD4 = millis();
      Serial.println("  -> D4 (button2) pressed [IRQ] -> lighting GREEN");
      setOnboardRgbColor(0, 255, 0);
      delay(300);
      setOnboardRgbColor(0, 0, 0);
    }
    delay(20);
  }
  Serial.println("Button test done.");

  Serial.println();
  Serial.println("PIR test: Wave hand in front of PIR (D6/P0) within 15 sec...");
  t0 = millis();
  const unsigned long pirTestDuration = 15000;
  bool pirSeen = false;
  while (millis() - t0 < pirTestDuration) {
    if (pirTriggered) {
      pirTriggered = false;
      pirSeen = true;
      Serial.println("  -> PIR triggered! Lighting BLUE briefly.");
      setOnboardRgbColor(0, 0, 255);
      delay(500);
      setOnboardRgbColor(0, 0, 0);
      break;
    }
    delay(20);
  }
  if (!pirSeen)
    Serial.println("  -> PIR not triggered (timeout or no motion).");
  Serial.println("PIR test done.");

  Serial.println();
  Serial.println("=== Tests complete. Starting normal timer. ===");
  Serial.println();
}
#endif

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }
  Serial.println("Desk Eye-Rest Timer started");

  Keyboard.begin();
  loadMacroString();

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_NEOPIXEL, OUTPUT);
  pinMode(NEOPIXEL_POWER, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  pinMode(D1, OUTPUT);  // External reminder LED if used
  // Buttons: active LOW (wire other side to GND). Internal pull-up keeps pin HIGH until pressed.
  pinMode(CLEAR_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(D1, LOW);

  digitalWrite(NEOPIXEL_POWER, HIGH);  // NeoPixel power (already OUTPUT from pinMode above)
  neopixel.begin();
  setNeoPixelColor(CONFIG_NEOPIXEL_R, CONFIG_NEOPIXEL_G, CONFIG_NEOPIXEL_B, 100);

  applyLedState();

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPIR, RISING);
  attachInterrupt(digitalPinToInterrupt(CLEAR_BUTTON_PIN), onClearButton, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON2_PIN), onButton2, FALLING);

#if ENABLE_STARTUP_TESTS
  runStartupTests();
#endif
}

void loop() {
  unsigned long now = millis();

  // 1. Handle PIR: when back at desk after away (or first time), start 20 min timer from 0. No lights until reminder.
  if (pirTriggered) {
    pirTriggered = false;
    bool wasAway = (lastPIRProcessedAt == 0) || (now - lastPIRProcessedAt > AWAY_TIMEOUT_MS);
    if (wasAway) {
      reminderDueAt = now + REMINDER_INTERVAL_MS;
      setNeoPixelColor(0, 0, 0);
      Serial.println("PIR: back at desk, 20 min reminder timer started");
    }
    lastPIRProcessedAt = now;
  }

  // 2. Handle clear button (Button 1): reset timer; turn off reminder NeoPixel
  bool clearLow = (digitalRead(CLEAR_BUTTON_PIN) == LOW);
  bool clearJustPressed = clearLow && !lastClearWasLow;
  lastClearWasLow = clearLow;
  if ((clearButtonPressed || clearJustPressed) && (now - lastClearButtonPress >= DEBOUNCE_MS)) {
    clearButtonPressed = false;
    lastClearButtonPress = now;
    reminderDueAt = now + REMINDER_INTERVAL_MS;
    setNeoPixelColor(0, 0, 0);
    Serial.println("Clear button: timer reset, reminder off");
  }

  // Button 2: turn off onboard red LED
  bool btn2Low = (digitalRead(BUTTON2_PIN) == LOW);
  bool btn2JustPressed = btn2Low && !lastBtn2WasLow;
  lastBtn2WasLow = btn2Low;
  if ((button2Pressed || btn2JustPressed) && (now - lastButton2Press >= DEBOUNCE_MS)) {
    button2Pressed = false;
    lastButton2Press = now;
    sendMacroString();
  }

  // 3. Away check: no PIR for 10+ min -> reminder off
  if (lastPersonSeen != 0 && (now - lastPersonSeen > AWAY_TIMEOUT_MS)) {
    if (reminderDueAt != 0) {
      reminderDueAt = 0;
      setNeoPixelColor(0, 0, 0);
      Serial.println("Away: no motion 10+ min, timer cleared");
    }
  } else if (reminderDueAt != 0 && now >= reminderDueAt) {
    // 4. Reminder: only the green NeoPixel soft-flashes
    unsigned long phase = (now - reminderDueAt) % REMINDER_NEOPIXEL_CYCLE_MS;
    unsigned long half = REMINDER_NEOPIXEL_CYCLE_MS / 2;
    uint8_t green;
    if (phase < half)
      green = (uint8_t)((255UL * phase) / half);
    else
      green = (uint8_t)(255 - (255UL * (phase - half)) / half);
    setNeoPixelColor(0, green, 0, 120);
  } else {
    // Before reminder or no active timer: NeoPixel off
    setNeoPixelColor(0, 0, 0);
  }
  applyLedState();
}
