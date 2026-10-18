# ESP32 Keypad Windows App

## Start

Double-click `Start-KeypadConfigurator.cmd`. Connect the keypad with a USB data cable, select its COM port, and click **Connect**. Choose a profile, assign keys and encoder actions, then click **Save to keypad**. The profiles are stored in the ESP32-S3 flash.

The app uses the Windows PowerShell and .NET Framework components already included with Windows. No installer is required. The current app configures printable ASCII keys, encoder direction actions, and the encoder push switch.

## Firmware update

Build the PlatformIO project first. Select `.pio/build/esp32-s3-n8r2/firmware.bin` in the app and select an `esptool.exe` compatible with ESP32-S3. When prompted, hold **BOOT**, tap **RESET**, release **BOOT**, and continue. The app writes the application image at `0x10000`; it does not replace the bootloader or partition table.

## USB protocol

The app communicates over USB CDC at 115200 baud while the keypad exposes USB HID. The serial commands are newline-terminated:

- `GET` reads the active profile.
- `SET|field|value` changes a button or encoder action in the active profile.
- `PROFILE|index` selects one of three profiles.
- `SAVE` stores all profiles in ESP32 NVS.

Encoder action `-1` means volume control, `0` means off, and a positive ASCII value sends that key.
