# ESP32 OLED Message Display

A Mac-hosted Flask web server that lets you publish text or GIF animations to an Elegoo ESP32, which displays them on a 128×64 SSD1306 OLED screen.

## Features

- **Text messaging**: Send text messages (up to 300 characters) from a web interface to the ESP32
- **GIF animation support**: Upload GIFs that are converted server-side to 1-bit monochrome frames for the OLED
- **Real-time polling**: ESP32 polls the server every 2 seconds for updates
- **Dark mode web interface**: Clean, responsive UI for publishing messages and animations

## Hardware

- **Board**: Elegoo ESP32 (model ESP-EL-SM-012)
- **Display**: 128×64 I²C SSD1306 OLED
- **I²C Wiring**:
  - SDA: GPIO 21
  - SCL: GPIO 22

## Software Requirements

### Mac (Server)
- Python 3.x
- Virtual environment (recommended)
- Dependencies:
  - Flask
  - Pillow (PIL)
  - python-dotenv

### ESP32
- Arduino IDE
- Required libraries:
  - `WiFi.h`
  - `HTTPClient.h`
  - `Wire.h`
  - `Adafruit_GFX.h`
  - `Adafruit_SSD1306.h`
  - `Arduino_JSON.h`

## Setup

### 1. Mac Server Setup

```bash
# Clone the repository
cd esp32-message-demo

# Create and activate virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# (Optional) Copy environment configuration and customize
cp .env.example .env
# Edit .env to change default settings if needed

# Run the server
python app.py
```

The server will start on `http://0.0.0.0:5050`, accessible from your local network.

### 2. ESP32 Setup

The ESP32 sketch is located in `sketch/esp-ssd1306.ino`.

**Before uploading**, you must configure the Wi-Fi and server settings. You have two options:

**Option 1: Edit the sketch directly (simpler)**
Edit `sketch/esp-ssd1306.ino` and update:
- `WIFI_SSID`: Your Wi-Fi network name
- `WIFI_PASSWORD`: Your Wi-Fi password
- `SERVER_IP`: The IP address of your Mac (where the Flask server is running)

**Option 2: Use a secrets file (recommended for security)**
1. Copy `sketch/secrets.h.example` to `sketch/secrets.h`
2. Fill in your credentials in `secrets.h`
3. Add `#include "secrets.h"` at the top of `esp-ssd1306.ino`
4. Remove or comment out the hardcoded credentials in the sketch

**Upload steps**:
1. Open the Arduino IDE
2. Install required libraries via Library Manager:
   - WiFi
   - Adafruit GFX Library
   - Adafruit SSD1306
   - ArduinoJson
3. Select your board (Elegoo ESP32 or equivalent)
4. Open `sketch/esp-ssd1306.ino`
5. Configure your credentials (using either option above)
6. Upload to your ESP32

## Configuration

The server can be configured via environment variables. Copy `.env.example` to `.env` and customize:

- `HOST`: Server host (default: `0.0.0.0`)
- `PORT`: Server port (default: `5050`)
- `DEBUG`: Enable debug mode (default: `False`)
- `MAX_GIF_BYTES`: Maximum GIF upload size in bytes (default: `5242880`)
- `MAX_FRAMES`: Maximum number of frames to process (default: `60`)
- `DEVICE_TIMEOUT_SECONDS`: Time before device is considered offline (default: `10`)
- `LOG_LEVEL`: Logging level (`DEBUG`, `INFO`, `WARNING`, `ERROR`, default: `INFO`)

## API Endpoints

### `GET /`
Returns the web interface for publishing messages and uploading GIFs.

### `GET /health`
Health check endpoint for monitoring.

**Response**:
```json
{
  "status": "ok",
  "timestamp": "2026-11-02T11:00:00.000000"
}
```

### `GET /api/message?device_id=<id>`
Returns the current message JSON.

**Response**:
```json
{
  "text": "Hello from your Mac!",
  "version": 1
}
```

### `POST /api/message`
Publish a new text message.

**Request body**:
```json
{
  "text": "Your message here"
}
```

**Response**:
```json
{
  "text": "Your message here",
  "version": 2
}
```

**Constraints**:
- `text` must be a string
- Cannot be empty
- Maximum 300 characters

### `GET /api/status`
Returns the current message and device connection status.

**Response**:
```json
{
  "message": {
    "text": "Hello from your Mac!",
    "version": 1
  },
  "device_seen": true
}
```

- `device_seen` is `true` if the ESP32 has polled the server within the last 10 seconds (configurable via `DEVICE_TIMEOUT_SECONDS`)

### `POST /api/animation`
Upload a GIF for animation.

**Request**: `multipart/form-data` with a `gif` field containing the GIF file.

**Constraints**:
- Maximum file size: 5 MB
- Maximum frames: 60
- Must be a valid GIF

**Response**:
```json
{
  "version": 1,
  "frame_count": 24
}
```

### `GET /api/animation`
Returns animation metadata.

**Response**:
```json
{
  "version": 1,
  "frame_count": 24,
  "frame_ms": 120
}
```

- `frame_ms` is fixed at 120ms per frame (not the GIF's original frame delays)

### `GET /api/animation/frame/<index>`
Returns a single animation frame as binary data.

**Response**: 1,024 bytes of packed 1-bit image data (128×64 pixels, 8 pixels per byte, MSB first)

**Error**: 404 if frame index is out of range

## GIF Processing

When a GIF is uploaded:
1. Each frame is converted to RGBA and composited onto a black background
2. Frames are resized to fit within 128×64 while preserving aspect ratio
3. Images are centered on the canvas
4. Each frame is converted to 1-bit monochrome (grayscale threshold at 128)
5. Pixels are packed row by row, 8 pixels per byte, MSB first
6. Maximum 60 frames are processed (additional frames are ignored)

**Note**: Animations are stored in memory only and are lost when the server restarts.

## ESP32 Implementation Notes

The provided sketch (`sketch/esp-ssd1306.ino`) implements the following logic:

### Message Polling
- Polls `/api/message` every 2 seconds with `device_id=esp32-demo`
- Compares `version` field to detect changes
- Displays text when `version` changes
- Shows "Server: ONLINE/OFFLINE" status on the OLED

### Animation Polling
- Polls `/api/animation` every 2 seconds
- Compares `version` field to detect new animations
- When a new animation is detected:
  - Fetches each frame via `/api/animation/frame/<index>`
  - Displays frames at the server-specified `frame_ms` interval (default 120ms)
  - Loops the animation continuously
- Animation display takes priority over text display when frames are available

### I²C Configuration
```cpp
const int OLED_SDA = 21;
const int OLED_SCL = 22;
Wire.begin(OLED_SDA, OLED_SCL);
```

### Serial Debug
- Uses `Serial.begin(115200)` for debugging
- Outputs Wi-Fi connection status and ESP32 IP address
- Monitor the Serial Monitor for connection issues

## Troubleshooting

### ESP32 not connecting to server
- Verify Mac and ESP32 are on the same network
- Check that the server is running on port 5050
- Ensure firewall allows connections on port 5050
- Verify Wi-Fi credentials in ESP32 sketch
- Check the Serial Monitor (115200 baud) for connection errors
- Confirm SERVER_IP in the sketch matches your Mac's IP address

### GIF not displaying correctly
- Ensure GIF is converted to 1-bit monochrome on the server
- Check that frame packing matches ESP32's bitmap format
- Verify I²C connection (SDA on GPIO 21, SCL on GPIO 22)

### Arduino IDE not seeing the board
- Check USB cable (data cables required, not charge-only)
- Verify board selection in Arduino IDE (Elegoo ESP32)
- Install proper USB drivers if needed
- Try a different USB port

## License

This project is provided as-is for educational and personal use.
