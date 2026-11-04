# ESP32 Keypad Windows App

For the Thai step-by-step guide to each app, firmware build/upload, and Serial Monitor, see [วิธีใช้งาน.txt](./วิธีใช้งาน.txt).

## Start

Double-click `Start-KeypadConfigurator.cmd`. The app checks available COM ports for the keypad and connects automatically when it finds one. If it is not found, connect the keypad with a USB data cable, click **Refresh**, select the COM port that appears for the ESP32 in Device Manager, and click **Connect**. Wait for `Connected to COMx` before saving; the app enables **Save to keypad** and **Reset profile** only after the keypad responds. Choose a profile, assign keys, then click **Save to keypad**. **Reset profile** restores only the selected profile to K1=Z, K2=X, K3=Y, knob volume up/down, and knob press=mute; confirm the prompt to apply it. Wait for `Settings saved to COMx`. The profiles are stored in the ESP32-S3 flash.

The app uses the Windows PowerShell and .NET Framework components already included with Windows. No installer is required. Button key choices include None, A-Z, 0-9, Space, and Windows key. Knob rotation can send keyboard keys or control Windows volume up/down/mute; knob press can send a keyboard key, Windows key, or mute.

The **Live Input** cards show K1-K3 and the knob switch as pressed/released, and briefly light when the knob rotates clockwise or counter-clockwise. This feedback comes from the keypad over USB serial; it does not depend on the assigned action or USB HID connection.

The configurator UI is English-only to keep text rendering consistent on Windows. Click **FN LAYER - Show** to expand the knob's alternate actions, or **FN LAYER - Hide** to collapse them. Hold physical K1 for at least 150 ms, then rotate or press the knob to use the configured Fn action. K2 and K3 always send their regular keys. A shorter K1 tap sends its regular key on release. Fn actions include **Win + Space** to switch Windows input language, **Alt + Tab** to switch apps, and **Alt + F4** to close the active app/game. Fn knob actions are saved per profile.

## Knob switch macro

Run `Start-KnobMacro.cmd` to use the knob switch as a Windows-key macro through the serial connection. Close PlatformIO Serial Monitor and any configurator app holding the COM port, select the keypad's COM port (for example, COM4), and click **Connect**. Each `BUTTON|ENC_SW(GPIO9)=PRESSED` event sends a Windows-key tap directly to the PC, so Native USB HID is not required for this macro. Keep the keypad connected to the serial/USB-to-UART port while using the macro app. Use **Test key** to check the Windows macro on the PC before testing the knob.

## Using on another PC

If the firmware is already flashed to the keypad, connect it with a USB data cable. Windows uses its built-in USB HID drivers for the keys and Start menu action; the configurator app is only needed to change profiles or update firmware. The apps prioritize USB serial devices, so a built-in port such as COM1 is not tried before the keypad.

Downloading the GitHub source alone does not program the ESP32. For a first flash, open the project in PlatformIO, select the `esp32-s3-n8r2` environment, then build and upload. PlatformIO can detect the board's port automatically; when multiple serial ports are present, pass the ESP32's port (for example, `pio run -e esp32-s3-n8r2 -t upload --upload-port COM5`). The app's firmware update option writes only the application image and assumes the bootloader and partition table are already installed.

## Firmware update

Build the PlatformIO project first. Select `.pio/build/esp32-s3-n8r2/firmware.bin` in the app and select an `esptool.exe` compatible with ESP32-S3. When prompted, hold **BOOT**, tap **RESET**, release **BOOT**, and continue. The app writes the application image at `0x10000`; it does not replace the bootloader or partition table.

## USB protocol

The app communicates over USB CDC at 115200 baud while the keypad exposes USB HID. The serial commands are newline-terminated:

- `GET` reads the active profile.
- `SET|field|value` changes a button or encoder action in the active profile.
- `SET|8..10|value` changes the Fn-layer mappings for knob clockwise/counter-clockwise and knob press.
- `PROFILE|index` selects one of three profiles.
- `SAVE` stores all profiles in ESP32 NVS.
- `BUTTON|...=PRESSED/RELEASED` reports keypad and knob-switch state.
- `ENCODER|CW` / `ENCODER|CCW` reports knob rotation.

The Fn layer uses config protocol version 8. Build and upload the current firmware once before using Fn mappings; the app reports no keypad response if connected to older firmware that sends the previous configuration format.

The knob uses GPIO7/GPIO8 for rotation and GPIO9 for its push switch. Choose Volume up for clockwise and Volume down for counter-clockwise to adjust Windows volume, and Mute for the knob press to toggle mute. The three keypad buttons and knob actions are configurable in each profile.

Rotation action values are `-2` for volume up, `-3` for volume down, `-4` for mute, and `0` for no action. The knob press also accepts `-4` for mute.
