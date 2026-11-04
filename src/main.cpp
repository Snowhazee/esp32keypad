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
constexpr size_t kProfileCount = 3;
constexpr int16_t kStartMenuAction = -1;
constexpr int16_t kVolumeUpAction = -2;
constexpr int16_t kVolumeDownAction = -3;
constexpr int16_t kMuteAction = -4;
constexpr int16_t kNoAction = 0;
constexpr int16_t kWindowsKeyAction = 131;
constexpr uint8_t kEncoderPressDefault = KEY_LEFT_GUI;
constexpr uint8_t kProfileConfigVersion = 2;
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
  int16_t encoderPressKey;
};

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

ProfileConfig defaultProfile() {
  return {{'z', 'x', 'y'}, kVolumeUpAction, kVolumeDownAction, kMuteAction};
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
  const uint8_t storedConfigVersion = preferences.getUChar("cfgver", 0);
  const bool migrateEncoderPress = storedConfigVersion < 1;
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
      if (storedConfigVersion < kProfileConfigVersion &&
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
    if (tapOnPress && key == kMuteAction) {
      if (hidMounted) {
        consumerControl.press(CONSUMER_CONTROL_MUTE);
        consumerControl.release();
      }
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
  for (size_t index = 0; index < kButtonCount; index++) {
    updateButton(buttons[index], kButtonPins[index],
                 currentProfile().buttonKeys[index], kButtonNames[index], now);
  }
}

void sendEncoderAction(int16_t action, const char *direction) {
  if (action == kNoAction) {
    return;
  }
  Serial.printf("ENCODER|%s\n", direction);
  if (!hidMounted) {
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
      sendEncoderAction(currentProfile().clockwiseAction, "CW");
      encoderDelta = 0;
    } else if (encoderDelta <= -4) {
      sendEncoderAction(currentProfile().counterClockwiseAction, "CCW");
      encoderDelta = 0;
    }
  }
  encoderState = currentState;
}

void updateEncoderSwitch(uint32_t now) {
  updateButton(encoderButton, kEncoderSwitchPin,
               currentProfile().encoderPressKey, "ENC_SW", now, true);
}

void sendConfiguration() {
  const ProfileConfig &config = currentProfile();
  Serial.printf("CFG|%u|%u|%u|%u|%d|%d|%d\n", activeProfile,
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
        (value == kNoAction || (value >= 32 && value <= 126))) {
      config.buttonKeys[field] = static_cast<uint8_t>(value);
    } else if ((field == 3 || field == 4) &&
               (value == kNoAction ||
                (value >= 32 && value <= 126) ||
                value == kWindowsKeyAction ||
                value == kStartMenuAction ||
                value == kVolumeUpAction ||
                value == kVolumeDownAction ||
                value == kMuteAction)) {
      if (field == 3) {
        config.clockwiseAction = value;
      } else {
        config.counterClockwiseAction = value;
      }
    } else if (field == 5 &&
               ((value >= 0 && value <= 126) ||
                value == kWindowsKeyAction || value == kMuteAction)) {
      config.encoderPressKey = static_cast<int16_t>(value);
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