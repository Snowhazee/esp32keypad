#include <Arduino.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

namespace {

constexpr uint8_t kButtonPins[] = {4, 5, 6};
constexpr const char *kButtonNames[] = {"K1", "K2", "K3"};
constexpr size_t kButtonCount = sizeof(kButtonPins) / sizeof(kButtonPins[0]);
constexpr uint8_t kEncoderSwitchPin = 8;
constexpr uint32_t kDebounceMs = 20;
constexpr size_t kProfileCount = 3;
constexpr int16_t kStartMenuAction = -1;
constexpr int16_t kNoAction = 0;
constexpr uint8_t kEncoderPressDefault = KEY_LEFT_GUI;
constexpr uint8_t kProfileConfigVersion = 1;
constexpr size_t kCommandBufferSize = 64;

struct ButtonState {
  bool stablePressed;
  bool lastReading;
  uint32_t lastChangeMs;
  uint8_t pressedKey;
};

struct ProfileConfig {
  uint8_t buttonKeys[kButtonCount];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  uint8_t encoderPressKey;
};

ButtonState buttons[kButtonCount] = {};
ButtonState encoderButton = {};
USBHIDKeyboard keyboard;
Preferences preferences;
ProfileConfig profiles[kProfileCount];
uint8_t activeProfile = 0;
char commandBuffer[kCommandBufferSize] = {};
size_t commandLength = 0;
bool hidMounted = false;
bool hidStatusInitialized = false;

ProfileConfig defaultProfile() {
  return {{'z', 'x', 'y'}, kNoAction, kNoAction, kEncoderPressDefault};
}

ProfileConfig &currentProfile() {
  return profiles[activeProfile];
}

void saveProfiles() {
  for (size_t index = 0; index < kProfileCount; index++) {
    char key[3] = {'p', static_cast<char>('0' + index), '\0'};
    preferences.putBytes(key, &profiles[index], sizeof(ProfileConfig));
  }
  preferences.putUChar("active", activeProfile);
}

void loadProfiles() {
  preferences.begin("keypad", false);
  const bool migrateEncoderPress =
      preferences.getUChar("cfgver", 0) < kProfileConfigVersion;
  for (size_t index = 0; index < kProfileCount; index++) {
    profiles[index] = defaultProfile();
    char key[3] = {'p', static_cast<char>('0' + index), '\0'};
    if (preferences.getBytesLength(key) == sizeof(ProfileConfig)) {
      preferences.getBytes(key, &profiles[index], sizeof(ProfileConfig));
      bool profileChanged = false;
      if (profiles[index].buttonKeys[0] == 'z' &&
          profiles[index].buttonKeys[1] == 'x' &&
          profiles[index].buttonKeys[2] == 'c') {
        profiles[index].buttonKeys[2] = 'y';
        profileChanged = true;
      }
      if (migrateEncoderPress &&
          profiles[index].encoderPressKey == kNoAction) {
        profiles[index].encoderPressKey = kEncoderPressDefault;
        profileChanged = true;
      }
      if (profileChanged) {
        preferences.putBytes(key, &profiles[index], sizeof(ProfileConfig));
      }
    }
  }
  if (migrateEncoderPress) {
    preferences.putUChar("cfgver", kProfileConfigVersion);
  }
  activeProfile = preferences.getUChar("active", 0);
  if (activeProfile >= kProfileCount) {
    activeProfile = 0;
  }
}

void checkPsram() {
  const bool found = psramFound();
  Serial.printf("PSRAM found: %s\nPSRAM size: %u bytes\n",
                found ? "YES" : "NO", ESP.getPsramSize());
  if (!found) {
    return;
  }

  constexpr size_t testSize = 256 * 1024;
  auto *testBuffer = static_cast<uint8_t *>(ps_malloc(testSize));
  if (testBuffer == nullptr) {
    Serial.println("PSRAM allocation: FAILED");
    return;
  }

  for (size_t index = 0; index < testSize; index++) {
    testBuffer[index] = static_cast<uint8_t>((index * 17 + 31) & 0xFF);
  }

  bool testPassed = true;
  for (size_t index = 0; index < testSize; index++) {
    if (testBuffer[index] != static_cast<uint8_t>((index * 17 + 31) & 0xFF)) {
      testPassed = false;
      break;
    }
  }

  Serial.printf("PSRAM write/read test: %s (%u bytes)\n",
                testPassed ? "PASS" : "FAIL", testSize);
  free(testBuffer);
}

void updateHidStatus() {
  const bool connected = static_cast<bool>(USB);
  if (hidStatusInitialized && connected == hidMounted) {
    return;
  }

  hidMounted = connected;
  hidStatusInitialized = true;
  Serial.printf("HID|%s\n", hidMounted ? "CONNECTED" : "NOT_CONNECTED");
}

void updateButton(ButtonState &button, uint8_t pin, uint8_t key,
                  const char *name, uint32_t now, bool tapOnPress = false) {
  const bool reading = digitalRead(pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.lastChangeMs = now;
  }

  if (now - button.lastChangeMs < kDebounceMs) {
    return;
  }

  const bool pressed = reading == LOW;
  if (pressed == button.stablePressed) {
    return;
  }

  button.stablePressed = pressed;
  Serial.printf("BUTTON|%s(GPIO%u)=%s\n", name, pin,
                pressed ? "PRESSED" : "RELEASED");
  if (pressed) {
    if (key != kNoAction && hidMounted) {
      if (tapOnPress) {
        keyboard.write(key);
        button.pressedKey = kNoAction;
      } else {
        button.pressedKey = key;
        keyboard.press(button.pressedKey);
      }
    } else {
      button.pressedKey = kNoAction;
    }
  } else if (button.pressedKey != kNoAction) {
    if (hidMounted) {
      keyboard.release(button.pressedKey);
    }
    button.pressedKey = kNoAction;
  }
}

void updateButtons(uint32_t now) {
  for (size_t index = 0; index < kButtonCount; index++) {
    updateButton(buttons[index], kButtonPins[index],
                 currentProfile().buttonKeys[index], kButtonNames[index], now);
  }
}

void updateEncoderSwitch(uint32_t now) {
  updateButton(encoderButton, kEncoderSwitchPin,
               KEY_LEFT_GUI, "ENC_SW", now, true);
}

void sendConfiguration() {
  const ProfileConfig &config = currentProfile();
  Serial.printf("CFG|%u|%u|%u|%u|%d|%d|%u\n", activeProfile,
                config.buttonKeys[0], config.buttonKeys[1],
                config.buttonKeys[2], config.clockwiseAction,
                config.counterClockwiseAction, config.encoderPressKey);
}

void handleCommand(char *command) {
  char *operation = strtok(command, "|");
  if (operation == nullptr) {
    return;
  }

  if (strcmp(operation, "GET") == 0) {
    sendConfiguration();
    return;
  }

  if (strcmp(operation, "SET") == 0) {
    char *fieldText = strtok(nullptr, "|");
    char *valueText = strtok(nullptr, "|");
    if (fieldText == nullptr || valueText == nullptr) {
      return;
    }
    const int field = atoi(fieldText);
    const int value = atoi(valueText);
    ProfileConfig &config = currentProfile();
    if (field >= 0 && field < static_cast<int>(kButtonCount) &&
        value >= 32 && value <= 126) {
      config.buttonKeys[field] = static_cast<uint8_t>(value);
    } else if (field == 3 && value >= kStartMenuAction && value <= 126) {
      config.clockwiseAction = value;
    } else if (field == 4 && value >= kStartMenuAction && value <= 126) {
      config.counterClockwiseAction = value;
    } else if (field == 5 && value >= 0 && value <= 126) {
      config.encoderPressKey = static_cast<uint8_t>(value);
    } else if (field == 5 && value == KEY_LEFT_GUI) {
      config.encoderPressKey = static_cast<uint8_t>(value);
    }
    sendConfiguration();
    return;
  }

  if (strcmp(operation, "PROFILE") == 0) {
    char *profileText = strtok(nullptr, "|");
    if (profileText != nullptr) {
      const int profile = atoi(profileText);
      if (profile >= 0 && profile < static_cast<int>(kProfileCount)) {
        activeProfile = static_cast<uint8_t>(profile);
        preferences.putUChar("active", activeProfile);
        sendConfiguration();
      }
    }
    return;
  }

  if (strcmp(operation, "SAVE") == 0) {
    saveProfiles();
    Serial.println("SAVED");
  }
}

void updateSerialCommands() {
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      commandBuffer[commandLength] = '\0';
      handleCommand(commandBuffer);
      commandLength = 0;
    } else if (commandLength < kCommandBufferSize - 1) {
      commandBuffer[commandLength++] = value;
    } else {
      commandLength = 0;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) {
    delay(10);
  }

  Serial.println("ESP32-S3 osu! keypad starting");
  checkPsram();
  loadProfiles();

  for (size_t index = 0; index < kButtonCount; index++) {
    pinMode(kButtonPins[index], INPUT_PULLUP);
    buttons[index].lastReading = digitalRead(kButtonPins[index]);
  }

  pinMode(kEncoderSwitchPin, INPUT_PULLUP);
  encoderButton.lastReading = digitalRead(kEncoderSwitchPin);

  keyboard.begin();
  USB.begin();
  Serial.println("USB HID keypad started: Z / X / Y");
}

void loop() {
  const uint32_t now = millis();
  updateHidStatus();
  updateButtons(now);
  updateEncoderSwitch(now);
  updateSerialCommands();
}