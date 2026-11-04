# ESP32 Multi-Display Message System

A Mac-hosted Flask web server that lets you publish text or GIF animations to multiple ESP32 devices with different display types (OLED, TFT, dot matrix).

## Features

- **Multi-device support**: Manage multiple ESP32 devices independently
- **Text messaging**: Send text messages (up to 300 characters) to specific devices
- **GIF animation support**: Upload GIFs converted server-side to appropriate formats for each display
- **Auto-registration**: Devices automatically register when they first connect
- **Real-time polling**: ESP32 devices poll the server every 2 seconds for updates
- **Device management UI**: Web interface to view, edit, and delete devices
- **GIF shows**: Sequence GIFs, waits, built-in effects (digital rain, equalizer, radar, ...) and styled text (marquee, flash words, wavy) on one display or across several, in sync
- **Multiple display types**: Supports SSD1306 OLED, ST7735 TFT, and 7219MAX dot matrix displays
- **Dark mode web interface**: Clean, responsive UI for publishing messages and animations

## Supported Hardware

### Displays
- **SSD1306**: 128×64 monochrome OLED (I²C)
- **ST7735**: 240×320 color TFT (SPI)
- **7219MAX**: 32×8 dot matrix (SPI)

### Board
- Elegoo ESP32 (model ESP-EL-SM-012) or equivalent

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
- Required libraries (varies by display type - see sketch/README.md)

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

Choose the appropriate sketch from the `sketch/` directory based on your display type:
- `sketch/esp-ssd1306/` for OLED displays
- `sketch/esp-st7735/` for TFT displays
- `sketch/esp-7219max/` for dot matrix displays

**Before uploading**, configure each device:

1. Copy the `secrets.h.example` file in the sketch directory to `secrets.h`
2. Fill in your credentials:
   ```cpp
   const char* WIFI_SSID = "YourNetworkName";
   const char* WIFI_PASSWORD = "YourPassword";
   const char* SERVER_IP = "192.168.1.183";  // Your Mac's IP
   const char* DEVICE_NAME = "living-room";  // Unique device name
   ```
3. **Important**: Each ESP32 must have a unique `DEVICE_NAME`
4. Upload the sketch to your ESP32

See `sketch/README.md` for detailed hardware connections and library requirements.

## Configuration

The server can be configured via environment variables. Copy `.env.example` to `.env` and customize:

- `HOST`: Server host (default: `0.0.0.0`)
- `PORT`: Server port (default: `5050`)
- `DEBUG`: Enable debug mode (default: `False`)
- `MAX_GIF_BYTES`: Maximum GIF upload size in bytes (default: `5242880`)
- `MAX_FRAMES`: Maximum number of frames to process (default: `60`)
- `DEVICE_TIMEOUT_SECONDS`: Time before device is considered offline (default: `10`)
- `DATA_DIR`: Where `devices.json`, `shows.json` and the `gifs/` library are kept (default: `.`)
- `LOG_LEVEL`: Logging level (`DEBUG`, `INFO`, `WARNING`, `ERROR`, default: `INFO`)

## Web Interface

### Main Page (`/`)
- Select a device from the dropdown
- Publish text messages to the selected device
- Upload GIF animations for the selected device
- View device connection status

### Device Management (`/devices`)
- View all registered devices with online/offline status
- See screen type and last seen time for each device
- Click "Edit Message" to jump to the message editor for a specific device
- Delete devices (use with caution - this removes all data for that device)

## API Endpoints

### Device Management

#### `GET /api/devices`
List all registered devices.

**Response**:
```json
{
  "devices": [
    {
      "name": "living-room",
      "screen_type": "ssd1306",
      "last_seen": "2026-11-02T11:00:00.000000",
      "online": true
    }
  ]
}
```

#### `GET /api/devices/<name>`
Get detailed info for a specific device.

#### `POST /api/devices`
Manually create a new device.

**Request body**:
```json
{
  "name": "kitchen",
  "screen_type": "st7735"
}
```

#### `DELETE /api/devices/<name>`
Delete a device.

### Message Endpoints

#### `GET /api/message?device=<name>&screen=<type>`
Returns the current message for the specified device. If `device` is not provided, defaults to "default".

**Response**:
```json
{
  "text": "Hello from your Mac!",
  "version": 1
}
```

#### `POST /api/message?device=<name>`
Publish a new text message to the specified device.

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

### Animation Endpoints

#### `POST /api/animation?device=<name>`
Upload a GIF for animation for the specified device.

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

#### `GET /api/animation?device=<name>`
Returns animation metadata for the specified device.

**Response**:
```json
{
  "version": 1,
  "frame_count": 24,
  "frame_ms": 120,
  "frame_delays": [120, 120, "... one entry per frame"]
}
```

`frame_delays` holds each frame's display time in milliseconds, taken from the uploaded GIF. `frame_ms` is their mean, for firmware that only supports a single delay. GIF delays shorter than 10 ms (including 0) are raised to 10 ms. Animations stored before this was added fall back to 120 ms per frame. The same two fields appear in the other animation metadata endpoints below.

#### `GET /api/animation/frame/<index>?device=<name>`
Returns a single monochrome animation frame as binary data (1,024 bytes, 128×64 pixels, 1-bit packed).

#### `GET /api/animation/jpeg?device=<name>`
Metadata for the color (ST7735) frames. The URL is a historical name: the frames are raw RGB332, one byte per pixel, not JPEG. Adds `width`, `height`, `x`, `y` (the first GIF's size and position), `format` (`"rgb332"`) and `frame_bytes`.

#### `GET /api/animation/jpeg/frame/<index>?device=<name>`
One RGB332 frame as binary data. `index` counts across all of the device's GIFs.

Both metadata endpoints include `program` whenever something is playing (see Shows below).

### Shows

A show has one **lane per display**. A lane is an ordered list of steps, played in order and then looped:

- **GIF**: a library GIF, repeated for the step's number of seconds (cut off when time runs out)
- **Wait**: the display shows its text message
- **Effect**: drawn by the device itself at the display's full native resolution. Each moves by the show clock, so they match across displays:

  | `effect` | Looks like |
  |---|---|
  | `rain` | digital rain |
  | `static` | TV static |
  | `equalizer` | audio-visualizer bars |
  | `scanner` | a Cylon / Knight Rider sweep |
  | `radar` | a rotating radar sweep with blips |
  | `testpattern` | color bars (patterns on mono displays) |
  | `snow`, `confetti` | falling flakes / colored bits |
  | `bounce` | the device name bouncing around like a DVD logo |

- **Text**: text the device draws in a `style`: `marquee` (scrolling), `flash` (one word at a time, as big as fits) or `wave` (the text filling the screen, each letter bobbing). Up to 100 plain-ASCII characters; anything else becomes `?`. Leave `text` blank to use the display's own message as it is when the show starts, and a lane can use up to 8 different texts.

All lanes start together and share one cycle (the longest lane). Shorter lanes show their text for the rest. Use waits to stagger a GIF from one display to the next. Devices are told how far into the cycle the show is each time they poll, so displays stay in step to within a poll's network jitter. Sending a message to a display takes it out of the show.

A lane can hold at most 24 steps, 16 different GIFs and `MAX_FRAMES` frames in total. If the GIFs add up to more frames than that, they are thinned evenly (keeping their lengths) and the response says so.

Build shows at `/shows`, or through the API:

- `GET /api/gifs`, `POST /api/gifs` (multipart `gif`), `GET /api/gifs/<id>/file`, `DELETE /api/gifs/<id>` (refused while a show uses it)
- `GET /api/shows`, `POST /api/shows`, `GET|PUT|DELETE /api/shows/<id>`
- `POST /api/shows/<id>/play` renders every lane and starts them together; `POST /api/shows/<id>/stop`

```json
{
  "name": "Chase",
  "lanes": [
    {"device": "oled", "steps": [
      {"type": "gif", "gif": "<id>", "seconds": 5},
      {"type": "wait", "seconds": 5}]},
    {"device": "tft", "steps": [
      {"type": "wait", "seconds": 5},
      {"type": "effect", "effect": "rain", "seconds": 5},
      {"type": "text", "style": "marquee", "text": "PARTY TIME", "seconds": 8}]}
  ]
}
```

`POST /api/animation?device=<name>` still uploads a GIF and plays it on one device straight away; the GIF is also added to the library.

The `program` the devices read:

```json
{
  "position_ms": 1250,
  "cycle_ms": 10000,
  "steps": [{"clip": 0, "start_ms": 0, "duration_ms": 5000},
            {"clip": -1, "start_ms": 5000, "duration_ms": 5000}],
  "clips": [{"gif": "<id>", "first_frame": 0, "frame_count": 24, "width": 80, "height": 60, "x": 80, "y": 130}],
  "texts": ["PARTY TIME"]
}
```

`clip` is an index into `clips`, `-1` for a wait, `-2` for an effect (then `effect` is its number, see `EFFECTS` in `partytime/config.py`), or `-3` for text (then `style` is `1` marquee, `2` flash, `3` wave, and `text` is an index into `texts`).

### Status & Health

#### `GET /api/status?device=<name>`
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

- `device_seen` is `true` if the device has polled the server within the last 10 seconds (configurable via `DEVICE_TIMEOUT_SECONDS`)

#### `GET /health`
Health check endpoint for monitoring.

**Response**:
```json
{
  "status": "ok",
  "timestamp": "2026-11-02T11:00:00.000000"
}
```

## GIF Processing

GIFs are stored once in the library. When a show plays, each lane's GIFs are rendered for that display:
1. Each frame is converted to RGBA and composited onto black, cropped to the visible content
2. **Color (RGB332)**: scaled to fit 240×320 within a per-frame pixel budget, so the whole lane fits the ST7735's RAM cache. The ST7735 then draws each frame at the largest whole-number scale that fits the screen (nearest neighbour), centered, so small frames still fill it
3. **Monochrome**: 1-bit, dithered, packed MSB first (128×64), with the top 16 rows left black for the dancing text
4. At most `MAX_FRAMES` frames are read from each GIF

Rendered frames live in `devices.json`. Saved shows are in `shows.json` and the original GIFs in `gifs/`.

## Project Layout

```
app.py                 entry point (python app.py)
partytime/
  config.py            environment settings and tuning constants
  registry.py          device records, persistence
  library.py           GIF library
  shows.py             saved shows and their validation
  program.py           renders a lane into frames + timeline
  playback.py          starts and stops shows on devices
  protocol.py          what devices are told (metadata, program)
  media/               GIF decoding, pixel encoding
  routes/              one blueprint per area (pages, messages, animation, devices, gifs, shows)
templates/, static/    web UI
sketch/                firmware (see sketch/README.md)
tests/                 python -m unittest discover tests
```

## Multi-Device Architecture

### Device Registration
- Devices auto-register on their first poll to the server
- Each device has independent message and animation state
- Device registry persists in `devices.json`

### Backward Compatibility
- Old API endpoints (`/api/message`, `/api/animation`) without device parameters default to the "default" device
- This ensures existing single-device setups continue to work

### Screen Type Handling
- Devices advertise their screen type via the `screen` query parameter
- Different screen types use different animation endpoints (ST7735 uses `/api/animation/jpeg`; the others use `/api/animation`)
- ESP32 sketches choose the appropriate endpoint based on their display type

## Troubleshooting

### ESP32 not connecting to server
- Verify Mac and ESP32 are on the same network
- Check that the server is running on port 5050
- Ensure firewall allows connections on port 5050
- Verify Wi-Fi credentials in ESP32 sketch
- Check the Serial Monitor (115200 baud) for connection errors
- Confirm SERVER_IP in the sketch matches your Mac's IP address
- Verify DEVICE_NAME is unique across all devices

### Device not appearing in web UI
- Ensure the ESP32 has successfully connected to Wi-Fi
- Check the Serial Monitor for HTTP errors
- Verify the device is polling with the correct device name
- Check server logs for device registration errors

### GIF not displaying correctly
- Ensure GIF is converted to the correct format for your display type
- Check that frame packing matches your display's format
- Verify hardware connections for your specific display type

### Arduino IDE not seeing the board
- Check USB cable (data cables required, not charge-only)
- Verify board selection in Arduino IDE (Elegoo ESP32)
- Install proper USB drivers if needed
- Try a different USB port

## License

This project is provided as-is for educational and personal use.
