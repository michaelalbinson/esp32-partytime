# ESP32 Sketches

This folder contains Arduino sketches for different ESP32/display combinations that connect to the Flask server and display messages/animations.

## Supported Configurations

- **esp-ssd1306**: 128×64 monochrome OLED (I²C)
- **esp-st7735**: 240×320 color TFT (SPI)
- **esp-7219max**: 32×8 dot matrix (SPI)

## Required Libraries

Install these libraries via the Arduino IDE Library Manager:

### Common to all sketches
- **WiFi** (builtin with ESP32 board support)
- **HTTPClient** (builtin with ESP32 board support)
- **ArduinoJson** by Benoit Blanchon

### esp-ssd1306 (OLED)
- **Adafruit GFX Library** by Adafruit
- **Adafruit SSD1306** by Adafruit
- **Wire** (builtin)

### esp-st7735 (TFT)
- **Adafruit GFX Library** by Adafruit
- **Adafruit ST7789** by Adafruit
- **JPEGDEC** by BitBank
- **SPI** (builtin)

### esp-7219max (Dot Matrix)
- **MD_MAX72XX** by majicDesigns
- **SPI** (builtin)

## Configuration

Each sketch has its own `secrets.h.example` file. Copy it to `secrets.h` and fill in your credentials:

```cpp
const char* WIFI_SSID = "YourNetworkName";
const char* WIFI_PASSWORD = "YourPassword";
const char* SERVER_IP = "192.168.1.183";  // Your Mac's IP
const char* DEVICE_NAME = "living-room";  // Unique device name
```

**Important**: Each ESP32 must have a unique `DEVICE_NAME`. This allows the server to manage multiple devices independently.

## Multi-Device Architecture

The server now supports multiple ESP32 devices. Each device:
1. Auto-registers when it first polls the server
2. Has independent message and animation state
3. Can be managed via the web UI at `/devices`

## Hardware Connections

### esp-ssd1306 (OLED)
- **OLED SDA** → GPIO 21
- **OLED SCL** → GPIO 22
- **OLED VCC** → 3.3V
- **OLED GND** → GND

### esp-st7735 (TFT)
- **TFT CS** → GPIO 5
- **TFT DC** → GPIO 2
- **TFT RST** → GPIO 4
- **TFT MOSI** → GPIO 23
- **TFT SCLK** → GPIO 18
- **TFT VCC** → 3.3V or 5V (check your display)
- **TFT GND** → GND

### esp-7219max (Dot Matrix)
- **MATRIX CS** → GPIO 5
- **SPI SCK** → GPIO 18
- **SPI MISO** → GPIO 19
- **SPI MOSI** → GPIO 23
- **VCC** → 5V
- **GND** → GND

## Board Selection

- Board: Elegoo ESP32 (ESP-EL-SM-012) or equivalent
- Upload Speed: 921600 (default) or 115200 if you encounter issues

## Serial Monitor

- Baud rate: 115200
- Use for debugging connection issues and viewing the ESP32's IP address

## Shared timeline code

`timeline.h` and `netlink.h` are identical in all three sketch folders (the Arduino IDE only builds files inside the sketch). `timeline.h` works out what to show at any moment from the server's program and the device's own clock. `netlink.h` is the handoff between the network task and the drawing loop. Edit the copies in `esp-st7735/`, copy them to the other two, and run `sketch/tests/run.sh`, which checks the copies match and runs their unit tests on your computer.

## Networking runs in its own task

Polling the server happens in a FreeRTOS task on core 0 (alongside the WiFi stack), so a slow or failing request can't stall drawing, which stays on core 1. The task never touches the display: it posts what it learns into small mailboxes (`netlink.h`) and the drawing loop picks it up between frames. On the ST7735 the task also preloads the frame cache (a one-pixel progress line grows along the top edge while it does). Frames that aren't cached, and every frame on the SSD1306 and 7219, are still downloaded by the drawing loop and can still hold it up.

Effects (digital rain, static, equalizer, scanner, radar, test pattern, snow, confetti, bouncing name, DVD logo) and the text styles (marquee, flash words, wavy) are drawn by each sketch itself, at that display's full resolution (240×320, 128×64, 32×8). They move by the show clock (`tlStepElapsed`), so every display of a show agrees on where they are.

## Sketch Behavior

All sketches:
1. Connect to Wi-Fi on startup
2. Poll `/api/message?device=<name>&screen=<type>` every 2 seconds for text updates
3. Poll `/api/animation?device=<name>` every 2 seconds for animation updates
4. Play the server's program for the device (GIFs, effects, waits) in step with the other displays; waits and idle time show the text
5. Show server connection status on the display

The server automatically registers devices when they first poll, so no manual setup is required beyond configuring the `DEVICE_NAME` in secrets.h.
