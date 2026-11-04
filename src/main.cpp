#include <Arduino.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>

namespace {

constexpr uint8_t kButtonPins[] = {4, 5, 6};
constexpr const char *kButtonNames[] = {"K1", "K2", "K3"};
constexpr size_t kButtonCount = sizeof(kButtonPins) / sizeof(kButtonPins[0]);
constexpr uint8_t kEncoderAPin = 7;
constexpr uint8_t kEncoderBPin = 8;
constexpr uint8_t kEncoderSwitchPin = 9;
constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kFnHoldThresholdMs = 150;
constexpr size_t kProfileCount = 3;
constexpr int16_t kStartMenuAction = -1;
constexpr int16_t kVolumeUpAction = -2;
constexpr int16_t kVolumeDownAction = -3;
constexpr int16_t kMuteAction = -4;
constexpr int16_t kSwitchInputLanguageAction = -5;
constexpr int16_t kAltTabAction = -6;
constexpr int16_t kAltF4Action = -7;
constexpr int16_t kNoAction = 0;
constexpr int16_t kWindowsKeyAction = 131;
constexpr uint8_t kEncoderPressDefault = KEY_LEFT_GUI;
constexpr uint8_t kKnobDefaultMigrationVersion = 5;
constexpr uint8_t kProfileConfigVersion = 8;
constexpr size_t kCommandBufferSize = 64;

struct ButtonState {
  bool stablePressed;
  bool lastReading;
  uint32_t lastChangeMs;
  uint8_t pressedKey;
};

struct LegacyProfileConfigV3 {
  uint8_t buttonKeys[kButtonCount];
  uint8_t buttonHoldKeys[kButtonCount];
  uint16_t buttonRepeatMs[kButtonCount];
  uint8_t buttonSequences[kButtonCount][6];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
};

struct LegacyProfileConfigV4 {
  uint8_t buttonKeys[kButtonCount];
  uint16_t buttonRepeatMs[kButtonCount];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
};

struct LegacyProfileConfigV5 {
  uint8_t buttonKeys[kButtonCount];
  uint8_t buttonRepeatEnabled[kButtonCount];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
};

struct ProfileConfigV6 {
  uint8_t buttonKeys[kButtonCount];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
};

struct ProfileConfigV7 {
  uint8_t buttonKeys[kButtonCount];
  uint8_t fnButtonKeys[kButtonCount - 1];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
  int16_t fnClockwiseAction;
  int16_t fnCounterClockwiseAction;
  int16_t fnEncoderPressKey;
};

struct ProfileConfig {
  uint8_t buttonKeys[kButtonCount];
  int16_t fnButtonKeys[kButtonCount - 1];
  int16_t clockwiseAction;
  int16_t counterClockwiseAction;
  int16_t encoderPressKey;
  int16_t fnClockwiseAction;
  int16_t fnCounterClockwiseAction;
  int16_t fnEncoderPressKey;
};

static_assert(sizeof(LegacyProfileConfigV3) == 36);
static_assert(sizeof(LegacyProfileConfigV4) == 16);
static_assert(sizeof(LegacyProfileConfigV5) == 12);
static_assert(sizeof(ProfileConfigV6) == 10);
static_assert(sizeof(ProfileConfigV7) == 18);
static_assert(sizeof(ProfileConfig) == 20);

ButtonState buttons[kButtonCount] = {};
ButtonState encoderButton = {};
USBHIDKeyboard keyboard;
USBHIDConsumerControl consumerControl;
Preferences preferences;
ProfileConfig profiles[kProfileCount];
uint8_t activeProfile = 0;
char commandBuffer[kCommandBufferSize] = {};
size_t commandLength = 0;
uint8_t encoderState = 0;
int8_t encoderDelta = 0;
bool hidMounted = false;
bool hidStatusInitialized = false;
uint32_t fnButtonPressMs = 0;
bool fnLayerActive = false;

ProfileConfig defaultProfile() {
  ProfileConfig config{};
  config.buttonKeys[0] = 'z';
  config.buttonKeys[1] = 'x';
  config.buttonKeys[2] = 'y';
  config.clockwiseAction = kVolumeUpAction;
  config.counterClockwiseAction = kVolumeDownAction;
  config.encoderPressKey = kMuteAction;
  return config;
}

ProfileConfig &currentProfile() {
  return profiles[activeProfile];
}

bool isShortcutAction(int16_t action) {
  return action == kSwitchInputLanguageAction || action == kAltTabAction ||
         action == kAltF4Action;
}

void sendShortcut(int16_t action) {
  uint8_t modifier = KEY_LEFT_ALT;
  uint8_t key = 0;
  if (action == kSwitchInputLanguageAction) {
    modifier = KEY_LEFT_GUI;
    key = ' ';
  } else if (action == kAltTabAction) {
    key = KEY_TAB;
  } else if (action == kAltF4Action) {
    key = KEY_F4;
  } else {
    return;
  }

  keyboard.press(modifier);
  keyboard.press(key);
  delay(10);
  keyboard.release(key);
  keyboard.release(modifier);
}

void sendTapAction(int16_t action) {
  if (!hidMounted) {
    return;
  }
  if (action == kSwitchInputLanguageAction || action == kAltTabAction ||
      action == kAltF4Action) {
    sendShortcut(action);
  } else if (action == kMuteAction) {
    consumerControl.press(CONSUMER_CONTROL_MUTE);
    consumerControl.release();
  } else if (action != kNoAction && hidMounted) {
    keyboard.write(action == kWindowsKeyAction || action == kStartMenuAction
                       ? KEY_LEFT_GUI
                       : static_cast<uint8_t>(action));
  }
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
  const uint8_t storedConfigVersion = preferences.getUChar("cfgver", 0);
  const bool migrateEncoderPress = storedConfigVersion < 1;
  for (size_t index = 0; index < kProfileCount; index++) {
    profiles[index] = defaultProfile();
    char key[3] = {'p', static_cast<char>('0' + index), '\0'};
    const size_t storedLength = preferences.getBytesLength(key);
    if (storedLength == sizeof(ProfileConfig)) {
      preferences.getBytes(key, &profiles[index], sizeof(ProfileConfig));
    } else if (storedLength == sizeof(ProfileConfigV7)) {
      ProfileConfigV7 legacy{};
      preferences.getBytes(key, &legacy, sizeof(legacy));
      memcpy(profiles[index].buttonKeys, legacy.buttonKeys,
             sizeof(legacy.buttonKeys));
      profiles[index].fnButtonKeys[0] = legacy.fnButtonKeys[0];
      profiles[index].fnButtonKeys[1] = legacy.fnButtonKeys[1];
      profiles[index].clockwiseAction = legacy.clockwiseAction;
      profiles[index].counterClockwiseAction =
          legacy.counterClockwiseAction;
      profiles[index].encoderPressKey = legacy.encoderPressKey;
      profiles[index].fnClockwiseAction = legacy.fnClockwiseAction;
      profiles[index].fnCounterClockwiseAction =
          legacy.fnCounterClockwiseAction;
      profiles[index].fnEncoderPressKey = legacy.fnEncoderPressKey;
    } else if (storedLength == sizeof(ProfileConfigV6)) {
      ProfileConfigV6 legacy{};
      preferences.getBytes(key, &legacy, sizeof(legacy));
      memcpy(profiles[index].buttonKeys, legacy.buttonKeys,
             sizeof(legacy.buttonKeys));
      profiles[index].clockwiseAction = legacy.clockwiseAction;
      profiles[index].counterClockwiseAction =
          legacy.counterClockwiseAction;
      profiles[index].encoderPressKey = legacy.encoderPressKey;
    } else if (storedLength == sizeof(LegacyProfileConfigV5)) {
      LegacyProfileConfigV5 legacy{};
      preferences.getBytes(key, &legacy, sizeof(legacy));
      memcpy(profiles[index].buttonKeys, legacy.buttonKeys,
             sizeof(legacy.buttonKeys));
      profiles[index].clockwiseAction = legacy.clockwiseAction;
      profiles[index].counterClockwiseAction =
          legacy.counterClockwiseAction;
      profiles[index].encoderPressKey = legacy.encoderPressKey;
    } else if (storedLength == sizeof(LegacyProfileConfigV4)) {
      LegacyProfileConfigV4 legacy{};
      preferences.getBytes(key, &legacy, sizeof(legacy));
      memcpy(profiles[index].buttonKeys, legacy.buttonKeys,
             sizeof(legacy.buttonKeys));
      profiles[index].clockwiseAction = legacy.clockwiseAction;
      profiles[index].counterClockwiseAction =
          legacy.counterClockwiseAction;
      profiles[index].encoderPressKey = legacy.encoderPressKey;
    } else if (storedLength == sizeof(LegacyProfileConfigV3)) {
      LegacyProfileConfigV3 legacy{};
      preferences.getBytes(key, &legacy, sizeof(legacy));
      memcpy(profiles[index].buttonKeys, legacy.buttonKeys,
             sizeof(legacy.buttonKeys));
      profiles[index].clockwiseAction = legacy.clockwiseAction;
      profiles[index].counterClockwiseAction =
          legacy.counterClockwiseAction;
      profiles[index].encoderPressKey = legacy.encoderPressKey;
    } else {
      continue;
    }

    bool profileChanged = storedLength != sizeof(ProfileConfig);
    if (storedLength == sizeof(ProfileConfig) ||
        storedLength == sizeof(ProfileConfigV7) ||
        storedLength == sizeof(ProfileConfigV6) ||
        storedLength == sizeof(LegacyProfileConfigV5) ||
        storedLength == sizeof(LegacyProfileConfigV4) ||
        storedLength == sizeof(LegacyProfileConfigV3)) {
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
      if (storedConfigVersion < kKnobDefaultMigrationVersion &&
          profiles[index].buttonKeys[0] == 'z' &&
          profiles[index].buttonKeys[1] == 'x' &&
          profiles[index].buttonKeys[2] == 'y' &&
          profiles[index].clockwiseAction == kNoAction &&
          profiles[index].counterClockwiseAction == kNoAction &&
          profiles[index].encoderPressKey == kEncoderPressDefault) {
        profiles[index].clockwiseAction = kVolumeUpAction;
        profiles[index].counterClockwiseAction = kVolumeDownAction;
        profiles[index].encoderPressKey = kMuteAction;
        profileChanged = true;
      }
      if (profiles[index].encoderPressKey < 0 ||
          (profiles[index].encoderPressKey > 126 &&
           profiles[index].encoderPressKey != kWindowsKeyAction &&
           profiles[index].encoderPressKey != kMuteAction)) {
        profiles[index].encoderPressKey = kEncoderPressDefault;
        profileChanged = true;
      }
      for (size_t button = 0; button < kButtonCount; button++) {
        if (profiles[index].buttonKeys[button] > 126 &&
            profiles[index].buttonKeys[button] != kWindowsKeyAction) {
          profiles[index].buttonKeys[button] = kNoAction;
          profileChanged = true;
        }
      }
      for (size_t button = 0; button < kButtonCount - 1; button++) {
        if ((profiles[index].fnButtonKeys[button] < 0 &&
             !isShortcutAction(profiles[index].fnButtonKeys[button])) ||
            (profiles[index].fnButtonKeys[button] > 126 &&
             profiles[index].fnButtonKeys[button] != kWindowsKeyAction)) {
          profiles[index].fnButtonKeys[button] = kNoAction;
          profileChanged = true;
        }
      }
      if ((profiles[index].fnClockwiseAction < 0 &&
           profiles[index].fnClockwiseAction != kStartMenuAction &&
           profiles[index].fnClockwiseAction != kVolumeUpAction &&
           profiles[index].fnClockwiseAction != kVolumeDownAction &&
           profiles[index].fnClockwiseAction != kMuteAction &&
           !isShortcutAction(profiles[index].fnClockwiseAction)) ||
          (profiles[index].fnClockwiseAction > 126 &&
           profiles[index].fnClockwiseAction != kWindowsKeyAction)) {
        profiles[index].fnClockwiseAction = kNoAction;
        profileChanged = true;
      }
      if ((profiles[index].fnCounterClockwiseAction < 0 &&
           profiles[index].fnCounterClockwiseAction != kStartMenuAction &&
           profiles[index].fnCounterClockwiseAction != kVolumeUpAction &&
           profiles[index].fnCounterClockwiseAction != kVolumeDownAction &&
           profiles[index].fnCounterClockwiseAction != kMuteAction &&
           !isShortcutAction(profiles[index].fnCounterClockwiseAction)) ||
          (profiles[index].fnCounterClockwiseAction > 126 &&
           profiles[index].fnCounterClockwiseAction != kWindowsKeyAction)) {
        profiles[index].fnCounterClockwiseAction = kNoAction;
        profileChanged = true;
      }
      if ((profiles[index].fnEncoderPressKey < 0 &&
           !isShortcutAction(profiles[index].fnEncoderPressKey)) ||
          (profiles[index].fnEncoderPressKey > 126 &&
           profiles[index].fnEncoderPressKey != kWindowsKeyAction &&
           profiles[index].fnEncoderPressKey != kMuteAction &&
           !isShortcutAction(profiles[index].fnEncoderPressKey))) {
        profiles[index].fnEncoderPressKey = kNoAction;
        profileChanged = true;
      }
      if (profileChanged) {
        preferences.putBytes(key, &profiles[index], sizeof(ProfileConfig));
      }
    }
  }
  if (storedConfigVersion < kProfileConfigVersion) {
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

void updateButton(ButtonState &button, uint8_t pin, int16_t key,
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
    if (tapOnPress || isShortcutAction(key)) {
      sendTapAction(key);
      button.pressedKey = kNoAction;
      return;
    }
    const uint8_t hidKey =
        key == kWindowsKeyAction ? KEY_LEFT_GUI : key;
    if (hidKey != kNoAction && hidMounted) {
      if (tapOnPress) {
        keyboard.write(hidKey);
        button.pressedKey = kNoAction;
      } else {
        button.pressedKey = hidKey;
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
  const bool fnWasPressed = buttons[0].stablePressed;
  updateButton(buttons[0], kButtonPins[0], kNoAction, kButtonNames[0], now);
  const bool fnPressed = buttons[0].stablePressed;
  if (!fnWasPressed && fnPressed) {
    fnButtonPressMs = now;
    fnLayerActive = false;
  } else if (fnPressed && !fnLayerActive &&
             now - fnButtonPressMs >= kFnHoldThresholdMs) {
    fnLayerActive = true;
  } else if (fnWasPressed && !fnPressed) {
    if (!fnLayerActive) {
      sendTapAction(currentProfile().buttonKeys[0]);
    }
    fnLayerActive = false;
  }

  for (size_t index = 1; index < kButtonCount; index++) {
    updateButton(buttons[index], kButtonPins[index],
                 currentProfile().buttonKeys[index],
                 kButtonNames[index], now);
  }
}

void sendEncoderAction(int16_t action, const char *direction) {
  Serial.printf("ENCODER|%s\n", direction);
  if (action == kNoAction) {
    return;
  }
  if (!hidMounted) {
    return;
  }
  if (isShortcutAction(action)) {
    sendShortcut(action);
    return;
  }
  switch (action) {
    case kVolumeUpAction:
      consumerControl.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
      consumerControl.release();
      break;
    case kVolumeDownAction:
      consumerControl.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
      consumerControl.release();
      break;
    case kMuteAction:
      consumerControl.press(CONSUMER_CONTROL_MUTE);
      consumerControl.release();
      break;
    default:
      keyboard.write(action == kWindowsKeyAction ||
                             action == kStartMenuAction
                         ? KEY_LEFT_GUI
                         : static_cast<uint8_t>(action));
      break;
  }
}

void updateEncoderRotation() {
  static constexpr int8_t kTransitions[] = {
      0, 1, -1, 0,
      -1, 0, 0, 1,
      1, 0, 0, -1,
      0, -1, 1, 0};
  const uint8_t currentState =
      static_cast<uint8_t>((digitalRead(kEncoderAPin) << 1) |
                           digitalRead(kEncoderBPin));
  const uint8_t transition = static_cast<uint8_t>(
      (encoderState << 2) | currentState);
  const int8_t movement = kTransitions[transition];
  if (movement == 0 && currentState != encoderState) {
    encoderDelta = 0;
  } else {
    encoderDelta += movement;
    if (encoderDelta >= 4) {
      sendEncoderAction(fnLayerActive
                            ? currentProfile().fnClockwiseAction
                            : currentProfile().clockwiseAction,
                        "CW");
      encoderDelta = 0;
    } else if (encoderDelta <= -4) {
      sendEncoderAction(fnLayerActive
                            ? currentProfile().fnCounterClockwiseAction
                            : currentProfile().counterClockwiseAction,
                        "CCW");
      encoderDelta = 0;
    }
  }
  encoderState = currentState;
}

void updateEncoderSwitch(uint32_t now) {
  const int16_t action =
      fnLayerActive ? currentProfile().fnEncoderPressKey
                    : currentProfile().encoderPressKey;
  updateButton(encoderButton, kEncoderSwitchPin,
               action, "ENC_SW", now, true);
}

void sendConfiguration() {
  const ProfileConfig &config = currentProfile();
  Serial.printf("CFG|%u|%u|%u|%u|%d|%d|%d|%d|%d|%d|%d|%d\n", activeProfile,
                config.buttonKeys[0], config.buttonKeys[1],
                config.buttonKeys[2], config.clockwiseAction,
                config.counterClockwiseAction, config.encoderPressKey,
                config.fnButtonKeys[0], config.fnButtonKeys[1],
                config.fnClockwiseAction, config.fnCounterClockwiseAction,
                config.fnEncoderPressKey);
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
        (value == kNoAction || (value >= 32 && value <= 126) ||
         value == kWindowsKeyAction)) {
      config.buttonKeys[field] = static_cast<uint8_t>(value);
    } else if (field >= 6 && field <= 7 &&
               (value == kNoAction || (value >= 32 && value <= 126) ||
                value == kWindowsKeyAction || isShortcutAction(value))) {
      config.fnButtonKeys[field - 6] = static_cast<int16_t>(value);
    } else if ((field == 3 || field == 4) &&
               (value == kNoAction ||
                (value >= 32 && value <= 126) ||
                value == kWindowsKeyAction ||
                value == kStartMenuAction ||
                value == kVolumeUpAction ||
                value == kVolumeDownAction ||
                value == kMuteAction || isShortcutAction(value))) {
      if (field == 3) {
        config.clockwiseAction = value;
      } else {
        config.counterClockwiseAction = value;
      }
    } else if ((field == 8 || field == 9) &&
               (value == kNoAction ||
                (value >= 32 && value <= 126) ||
                value == kWindowsKeyAction ||
                value == kStartMenuAction ||
                value == kVolumeUpAction ||
                value == kVolumeDownAction ||
                value == kMuteAction || isShortcutAction(value))) {
      if (field == 8) {
        config.fnClockwiseAction = value;
      } else {
        config.fnCounterClockwiseAction = value;
      }
    } else if ((field == 5 || field == 10) &&
               ((value >= 0 && value <= 126) ||
                value == kWindowsKeyAction || value == kMuteAction ||
                (field == 10 && isShortcutAction(value)))) {
      if (field == 5) {
        config.encoderPressKey = static_cast<int16_t>(value);
      } else {
        config.fnEncoderPressKey = static_cast<int16_t>(value);
      }
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
  pinMode(kEncoderAPin, INPUT_PULLUP);
  pinMode(kEncoderBPin, INPUT_PULLUP);
  encoderState = static_cast<uint8_t>((digitalRead(kEncoderAPin) << 1) |
                                      digitalRead(kEncoderBPin));

  keyboard.begin();
  consumerControl.begin();
  USB.begin();
  Serial.println("USB HID keypad started: Z / X / Y");
}

void loop() {
  const uint32_t now = millis();
  updateHidStatus();
  updateButtons(now);
  updateEncoderRotation();
  updateEncoderSwitch(now);
  updateSerialCommands();
}