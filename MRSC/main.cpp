#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <LiquidCrystal.h>
#include <Adafruit_MCP23017.h>
#include <Adafruit_PWMServoDriver.h>

// How many MCP23017 & PCA9685 boards are connected, needs to be an equal amount
const uint8_t BOARD_COUNT = 4;

// Keypad & Display
const uint8_t KEYPAD_PIN = 0; // A0

// MCP's - Addresses start from the base 0x20
const uint8_t MCP_ADDRESSES[BOARD_COUNT] = { 0 /* 0x20 */, 1 /* 0x21 */, 2 /* 0x22 */, 3 /* 0x23 */ };

// PCA Servo Drivers
const uint16_t SERVO_MIN = 90;
const uint16_t SERVO_MAX = 440;
const uint8_t SERVO_FREQ = 50;
const uint8_t PCA_ADDRESSES[BOARD_COUNT] = { 0x40, 0x41, 0x42, 0x43 };

// Board group
struct {
  Adafruit_MCP23017 *mcp = nullptr; // MCP23017
  Adafruit_PWMServoDriver *pca = nullptr; // PCA9685
  uint16_t states = 0;
} boards[BOARD_COUNT];

enum class Type : uint8_t {
  POINT,      // Full sweep from limit to limit
  SEMAPHORE,  // Full sweep from limit to limit with optional simulation of semaphore drop bounce and pull up hesitate
  SWEEP,      // Similar to POINT but with different speeds, can be used for gates etc.
  ONOFF       // 0v or 5v to allow trigger of other components, e.g N-Channel MOSFET
};

// These are ms delay times between each degree move
uint8_t pointSpeeds[] = { 25, 30, 35, 40 };
uint8_t sweepSpeeds[] = { 15, 25, 35, 45 };

// Servo config
struct {
  uint8_t limit1; // Limit 1
  uint8_t limit2; // Limit 2
  Type type;
  bool swap; // A convenience option, can swap the direction without needing to change wires
  uint8_t speed;
  bool hesitate; // Simulate hesitate
  bool bounce; // Simulate bounce
  uint32_t unused; // Bit of space in case there's future updates
} servos[BOARD_COUNT * 16];

#define MENUSIZE(items) sizeof(items) / 16
const char yesNoMenu[][16] PROGMEM = { "Yes", "No" };
const char servoSetupMenu[][16] PROGMEM = { "Setup/Change", "Swap Direction", "Centre", "Cancel" };
const char servoSetupTypeMenu[][16] PROGMEM = { "Point", "Semaphore", "Sweep", "On/Off" };
const char servoPointSpeedMenu[][16] PROGMEM = { "Speed 1", "Speed 2", "Speed 3", "Speed 4" };
const char servoSweepSpeedMenu[][16] PROGMEM = { "Speed 1", "Speed 2", "Speed 3", "Speed 4" };

uint8_t board = 0;
uint8_t pin = 0;
void checkForChange();

namespace Keypad {
  enum class Key : uint8_t {
    NONE    = 0,
    LEFT    = 1,
    RIGHT   = 2,
    UP      = 3,
    DOWN    = 4,
    SELECT  = 5
  };

  Key getKey(bool blocking = true) {
    Key key = Key::NONE;
    uint16_t adc;

    do {
      adc = analogRead(KEYPAD_PIN);
      if (adc < 50) {
        key = Key::RIGHT;
      } else if (adc < 250) {
        key = Key::UP;
      } else if (adc < 450) {
        key = Key::DOWN;
      } else if (adc < 650) {
        key = Key::LEFT;
      } else if (adc < 850) {
        key = Key::SELECT;
      }
    } while (blocking && adc < 1000);

    return key;
  }
}

namespace Display {
  LiquidCrystal LCD(8, 9, 4, 5, 6, 7);

  void initialize() {
    LCD.begin(16, 2);
  }

  auto _change = [](int8_t) { };
  template<typename onChange = decltype(_change)>
  uint8_t showMenu(const __FlashStringHelper *title, const char menu[][16], uint8_t count, int8_t index = 0, onChange change = _change) {
    Keypad::Key oldKey = Keypad::Key::SELECT;
    Keypad::Key newKey = Keypad::Key::NONE;

    count--;
    if (index > count) {
      index = count;
    }

    LCD.clear();
    LCD.print(title);
    LCD.setCursor(0, 1);
    LCD.print((__FlashStringHelper*)&menu[index]);

    uint8_t len = strlen_P((const char*)title);
    int8_t scroll = len - 16;
    uint32_t wait = millis() + 1000;
    uint8_t i = 0;
    bool ltr = true;

    delay(100);

    do {
      if (len > 16 && millis() > wait) { // Scroll the title
        ltr ? i++ : i--;
        if (i == scroll || i == 0) { // Change scroll direction
          ltr = !ltr;
          wait = millis() + 1000; // Wait for 1 second before starting to scroll again
        } else {
          wait = millis() + 500;
        }
        
        LCD.setCursor(0, 0);
        LCD.print((const __FlashStringHelper *)((char*)title + i));
      }

      newKey = Keypad::getKey();
      if (oldKey != newKey) {
        if (newKey == Keypad::Key::DOWN) {
          if (++index > count) {
            index = 0;
          }
        } else if (newKey == Keypad::Key::UP) {
          if (--index < 0) {
            index = count;
          }
        }
        LCD.setCursor(0, 1);
        LCD.print((__FlashStringHelper*)&menu[index]);
        LCD.print(F("                "));

        oldKey = newKey;

        if (oldKey == Keypad::Key::NONE) {
          change(index);
        }
      }

      checkForChange();
    } while (newKey != Keypad::Key::SELECT);

    return index;
  }
}

namespace Switches {
  Adafruit_MCP23017* initialize(uint8_t address) {
    Adafruit_MCP23017 *mcp = new Adafruit_MCP23017;
    mcp->begin(address);
    // We want input pullups on all 16 pins
    for (uint8_t i = 0; i < 16; i++) {
      mcp->pullUp(i, HIGH);
    }

    return mcp;
  }

  bool hasChanged(uint8_t &board, uint8_t &pin) {
    for (uint8_t b = 0; b < BOARD_COUNT; b++) { // Loop the MCP23017 boards
      if (boards[b].mcp == nullptr) { // No MCP board to query
        continue;
      }
      uint16_t pins = boards[b].mcp->readGPIOAB();
      if (pins == boards[b].states) { // Any changes?
        continue;
      }
      for (uint8_t p = 0; p < 16; p++) { // Check all 16 pins
        if (bitRead(boards[b].states, p) != bitRead(pins, p)) {
          bitToggle(boards[b].states, p);
          board = b;
          pin = p;
          return true;
        }
      }
    }

    return false;
  }
}

namespace Servos {
  Adafruit_PWMServoDriver* initialize(uint8_t address) {
    Adafruit_PWMServoDriver *pca = new Adafruit_PWMServoDriver(address);

    pca->begin();
    pca->setOscillatorFrequency(27000000);
    pca->setPWMFreq(SERVO_FREQ);

    return pca;
  }

  uint8_t select(uint8_t &board, uint8_t &pin, uint8_t start = 0) {
    Display::LCD.clear();
    Display::LCD.print(F("Select Servo #"));
    
    int8_t servo = start;

    Display::LCD.setCursor(0, 1);
    Display::LCD.print(servo);

    Keypad::Key key;
    uint32_t held = 0;    

    do {
      key = Keypad::getKey(false);
      if (held == 0 || millis() > held + 500) {
        if (key == Keypad::Key::DOWN) {
          if (--servo < 0) {
            servo = (BOARD_COUNT * 16) - 1;
          }
          Display::LCD.setCursor(0, 1);
          Display::LCD.print(servo);
          Display::LCD.print(F("  "));
        } else if (key == Keypad::Key::UP) {
          if (++servo > (BOARD_COUNT * 16) - 1) {
            servo = 0;
          }
          Display::LCD.setCursor(0, 1);
          Display::LCD.print(servo);
          Display::LCD.print(F("  "));
        }
        if (held == 0) {
          held = millis();
        } else {
          delay(100);
        }
      }
      if (key == Keypad::Key::NONE) {
        held = 0;
      }
    } while (key != Keypad::Key::SELECT);

    board = servo / 16;
    pin = servo % 16;

    delay(100);

    return servo;
  }

  void setAngle(uint8_t board, uint8_t pin, uint8_t angle, uint16_t msDelay, bool off) {
    uint16_t pulseLength = map(angle, 0, 180, SERVO_MIN, SERVO_MAX);
    boards[board].pca->setPWM(pin, 0, pulseLength);
    delay(msDelay);
    if (off) { // There's no real load on the servo so we can disable it after it's moved. Saves a bit of power and stops it hunting
      boards[board].pca->setPWM(pin, 0, 4096);
    }
  }

  void sweep(uint8_t board, uint8_t pin, uint8_t from, uint8_t to, uint8_t delay) {
    if (from < to) {
      for (uint8_t i = from; i <= to; i++) {
        setAngle(board, pin, i, delay, i == to);
      }
    } else {
      for (uint8_t i = from; i >= to; i--) {
        setAngle(board, pin, i, delay, i == to);
      }
    }
  }

  void change(uint8_t board, uint8_t pin, uint8_t servo) {
    if (bitRead(boards[board].states, pin) ^ servos[servo].swap) {
      switch (servos[servo].type) {
        case Type::POINT: {
          sweep(board, pin, servos[servo].limit1, servos[servo].limit2, pointSpeeds[servos[servo].speed]);
        } break;
        case Type::SEMAPHORE: {
          // TODO, speed on semaphores?
          sweep(board, pin, servos[servo].limit1, servos[servo].limit2, 20);
          if (servos[servo].bounce) {
            int8_t bounce = (servos[servo].limit2 - servos[servo].limit1) / 5;
            sweep(board, pin, servos[servo].limit2, servos[servo].limit2 - bounce, 15);
            sweep(board, pin, servos[servo].limit2 - bounce, servos[servo].limit2, 15);
            bounce /= 2;
            sweep(board, pin, servos[servo].limit2, servos[servo].limit2 - bounce, 15);
            sweep(board, pin, servos[servo].limit2 - bounce, servos[servo].limit2, 15);
          }
        } break;
        case Type::SWEEP: {
          sweep(board, pin, servos[servo].limit1, servos[servo].limit2, sweepSpeeds[servos[servo].speed]);
        } break;
        case Type::ONOFF: {
          boards[board].pca->setPWM(pin, 4096, 0);
        } break;
      }
    } else {
      switch (servos[servo].type) {
        case Type::POINT: { // TODO hesitate for points too, simulate lever pull?
          sweep(board, pin, servos[servo].limit2, servos[servo].limit1, pointSpeeds[servos[servo].speed]);
        } break;
        case Type::SEMAPHORE: {
          if (servos[servo].hesitate) {
            int8_t half = (servos[servo].limit2 - servos[servo].limit1) / 2;
            sweep(board, pin, servos[servo].limit2, servos[servo].limit2 - half, 35);
            delay(200);
            sweep(board, pin, servos[servo].limit2 - half, servos[servo].limit1, 35);
          } else {
            sweep(board, pin, servos[servo].limit2, servos[servo].limit1, 35);
          }
        } break;
        case Type::SWEEP: {
          sweep(board, pin, servos[servo].limit2, servos[servo].limit1, sweepSpeeds[servos[servo].speed]);
        } break;
        case Type::ONOFF: {
          boards[board].pca->setPWM(pin, 0, 4096);
        } break;
      }
    }
  }

  void jog(uint8_t limit, uint8_t board, uint8_t pin, uint8_t &angle) {
    Display::LCD.clear();
    Display::LCD.print(F("Setup Limit "));
    Display::LCD.print(limit);
    Display::LCD.setCursor(0, 1);
    Display::LCD.print(F("Use L/R to Jog"));

    setAngle(board, pin, angle, 100, true);

    Keypad::Key key;
    uint32_t held = 0;
    do {
      key = Keypad::getKey(false);
      if (!held || millis() > held + 500) {
        if (key == Keypad::Key::LEFT) {
          setAngle(board, pin, --angle, 100, true);
        } else if (key == Keypad::Key::RIGHT) {
          setAngle(board, pin, ++angle, 100, true);
        }
        if (!held) {
          held = millis();
        }
      }
      if (key == Keypad::Key::NONE) {
        held = 0;
      }
    } while (key != Keypad::Key::SELECT);

    delay(100);
  }

  void setup(uint8_t board, uint8_t pin, uint8_t servo) {
    if ((uint8_t)servos[servo].type == 0xFF || !Display::showMenu(F("Existing Setup - Overwrite?"), yesNoMenu, MENUSIZE(yesNoMenu), 1)) {
      servos[servo].type = (Type)Display::showMenu(F("Setup Type"), servoSetupTypeMenu, MENUSIZE(servoSetupTypeMenu));
      // Defaults
      servos[servo].swap = false;
      servos[servo].speed = 0;
      servos[servo].hesitate = false;
      servos[servo].bounce = false;

      if (servos[servo].type != Type::ONOFF) {
        do {
          Display::LCD.clear();
          Display::LCD.print(F("Centring"));
          delay(1000);

          servos[servo].limit1 = 90;
          jog(1, board, pin, servos[servo].limit1);

          servos[servo].limit2 = 90;
          jog(2, board, pin, servos[servo].limit2);
        } while (Display::showMenu(F("Limits Correct? - Test Switch"), yesNoMenu, MENUSIZE(yesNoMenu)) == 1);
      }
    }

    Display::showMenu(F("Swap Direction? - Test Switch"), yesNoMenu, MENUSIZE(yesNoMenu), 1, [&](uint8_t selected) {
      servos[servo].swap = !selected;
    });

    switch (servos[servo].type) {
      case Type::POINT: {
        Display::showMenu(F("Select Speed - Test Switch"), servoPointSpeedMenu, MENUSIZE(servoPointSpeedMenu), 0, [&](uint8_t selected) {
          servos[servo].speed = selected;
        });
      } break;
      case Type::SEMAPHORE: {
        Display::showMenu(F("Simulate Bounce? - Test Switch"), yesNoMenu, MENUSIZE(yesNoMenu), 0, [&](uint8_t selected) {
          servos[servo].bounce = !selected;
        });
        Display::showMenu(F("Simulate Hesitate? - Test Switch"), yesNoMenu, MENUSIZE(yesNoMenu), 0, [&](uint8_t selected) {
          servos[servo].hesitate = !selected;
        });
      } break;
      case Type::SWEEP: {
        Display::showMenu(F("Select Speed - Test Switch"), servoSweepSpeedMenu, MENUSIZE(servoSweepSpeedMenu), 0, [&](uint8_t selected) {
          servos[servo].speed = selected;
        });
      } break;
      default: break;
    }

    Display::LCD.clear();
    Display::LCD.print(F("Setup Complete"));
    delay(1000);
  }
}

void running() {
  Display::LCD.clear();
  Display::LCD.print(F("Running... Press"));
  Display::LCD.setCursor(0, 1);
  Display::LCD.print(F("Select to Setup"));
}

void setup() {
  Serial.begin(9600);

  Wire.begin();
  Wire.setClock(400000);

  EEPROM.get(0, servos);
  Display::initialize();

  auto detect = [](uint8_t addr) -> bool {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
  };
  
  for (uint8_t c = 0; c < BOARD_COUNT; c++) {
    // Setup the MCP23017 & PCA9685 boards
    if (detect(MCP23017_ADDRESS | MCP_ADDRESSES[c]) && detect(PCA_ADDRESSES[c])) {
      #if __cplusplus >= 201402L
      boards[c] = {
        Switches::initialize(MCP_ADDRESSES[c]),
        Servos::initialize(PCA_ADDRESSES[c]),
      };
      #else
      boards[c].mcp = Switches::initialize(MCP_ADDRESSES[c]);
      boards[c].pca = Servos::initialize(PCA_ADDRESSES[c]);
      #endif
      // Get current switch positions
      boards[c].states = boards[c].mcp->readGPIOAB();
    }
  }

  running();
}

// cppcheck-suppress unusedFunction
void loop() {
  if (Keypad::getKey() == Keypad::Key::SELECT) {
    uint8_t servo = 0;
    uint8_t menu = UINT8_MAX;
    do {
      servo = Servos::select(board, pin, servo);
      if (menu == UINT8_MAX) {
        menu = Display::showMenu(F("Setup Servos"), servoSetupMenu, MENUSIZE(servoSetupMenu));
      }
      if (menu == 0) { // Setup
        Servos::setup(board, pin, servo);
        EEPROM.put(0, servos);
        menu = Display::showMenu(F("Setup/Change Another?"), yesNoMenu, MENUSIZE(yesNoMenu));
        servo++; // If we continue to setup another then auto inc the servo #
      } else if (menu == 1) { // Swap
        servos[servo].swap = !servos[servo].swap;
        EEPROM.put(0, servos);
        Display::LCD.clear();
        Display::LCD.print(F("Swapped"));
        delay(1000);
      } else if (menu == 2) { // Centre
        Servos::setAngle(board, servo, 90, 100, true);
      }
    } while (menu == 0);
    
    running();
  }

  checkForChange();
}

void checkForChange() {
  if (Switches::hasChanged(board, pin)) {
    // TODO, sleep and wake PWM boards?
    uint8_t servo = (board * 16) + pin;
    if ((uint8_t)servos[servo].type != 0xFF) { // Do we have a valid type? Only change if so. EEPROM defaults are 0xFF
      Servos::change(board, pin, servo);
    }
  }
}
