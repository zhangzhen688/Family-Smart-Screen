# Family Smart Screen

<p align="center">
  <img src="https://img.shields.io/badge/platform-Ubuntu%20%7C%20ARM%20Linux-blue" alt="Platform">
  <img src="https://img.shields.io/badge/language-C%20%7C%20C%2B%2B-orange" alt="Language">
  <img src="https://img.shields.io/badge/gui-LVGL%20v9.2.2-brightgreen" alt="LVGL">
  <img src="https://img.shields.io/badge/build-CMake%203.15%2B-lightgrey" alt="CMake">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License">
  <img src="https://img.shields.io/badge/AI-XiaoZhi%20Cloud-purple" alt="XiaoZhi AI">
</p>

A smart home control panel built with LVGL and C, featuring XiaoZhi AI voice assistant, V4L2 camera monitoring, JSON-RPC distributed backend services, and mobile web remote control.

## Table of Contents

- [Architecture](#architecture)
- [Features](#features)
- [Screenshots](#screenshots)
- [Quick Start](#quick-start)
- [Project Structure](#project-structure)
- [API Reference](#api-reference)
- [Hardware Drivers](#hardware-drivers)
- [Build Options](#build-options)
- [Tech Stack](#tech-stack)
- [Roadmap](#roadmap)
- [License](#license)

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                    GUI (LVGL + SDL2)                            │
│  7 Pages: Home │ Devices │ Camera │ Album │ Scenes │ Settings  │
│                        │ Voice AI                              │
├────────────────────────────────────────────────────────────────┤
│                 JSON-RPC over TCP (localhost)                   │
├──────────┬──────────┬──────────┬──────────┬────────────────────┤
│ Device   │ Camera   │ Voice    │ Web      │ XiaoZhi            │
│ Server   │ Server   │ Server   │ Server   │ Control Center     │
│ :1234    │ :1235    │ :1236    │ :8080    │ :5678/5679 (UDP)   │
│          │          │          │          │ Sound App (ALSA)   │
├──────────┼──────────┼──────────┼──────────┼────────────────────┤
│ LED x6   │ V4L2     │ RPC      │ HTTP     │ WebSocket Cloud    │
│ DHT11    │ MJPEG    │ Bridge   │ SPA      │ Opus Codec         │
│ SG90     │ Photo    │ UDP IPC  │ API      │ TTS / STT          │
│ AP3216C  │ Record   │          │          │ Device Activation  │
└──────────┴──────────┴──────────┴──────────┴────────────────────┘
```

Three backend servers expose hardware as JSON-RPC services. The GUI processes all interactions through TCP RPC calls. A separate web server embeds a single-page application for mobile remote access. XiaoZhi components communicate with the cloud via WebSocket and with each other over UDP.

## Features

### Local GUI

| Page | Description |
|------|-------------|
| **Home** | Real-time clock, weather bar, temperature/humidity/light sensor cards, six navigation shortcuts |
| **Device Control** | Six independent LED switches (Living Room, Bedroom, Kitchen, Study, Hallway, Bathroom), curtain angle slider (0-180 degrees via SG90 servo), DHT11/AP3216C sensor data refresh |
| **Camera** | V4L2 live preview window, JPEG photo capture saved to disk, video recording |
| **Photo Album** | Browse and manage captured photos, thumbnail grid view |
| **Scene Linkage** | One-tap multi-device automation presets: Home (all on, curtain open), Away (all off, curtain closed), Sleep (bedroom only), Movie (living room dim), Party (all on) |
| **Voice Assistant** | XiaoZhi AI character with dynamic emotion images (idle/listening/speaking/thinking/sad), real-time state bar, chat text area, tap-to-talk interaction, device activation code display |
| **Settings** | Display brightness slider, volume control, sleep timer toggle, system information |

### Mobile Web Remote

- Responsive single-page application served on port 8080
- Real-time sensor dashboard with auto-refresh (3-second interval)
- Toggle switches for all six LED channels
- Curtain position slider
- Camera snapshot view
- Voice assistant status monitoring and control
- Dark theme with gradient accents, mobile-first design

### XiaoZhi AI Voice Assistant

- WebSocket connection to XiaoZhi cloud service (api.tenclass.net)
- Device activation flow with on-screen code display
- Automatic speech recognition (STT) with results displayed in real time
- Text-to-speech (TTS) playback via Opus codec and ALSA
- Emotion state synchronization from cloud LLM responses
- UDP-based inter-process communication between control center, audio app, and GUI

## Quick Start

### Prerequisites

```bash
# Core build dependencies
sudo apt install cmake build-essential libsdl2-dev libfreetype-dev

# XiaoZhi AI dependencies
sudo apt install libwebsocketpp-dev libopus-dev libspeexdsp-dev \
                 libasound2-dev libssl-dev libcurl4-openssl-dev
```

### Build

```bash
git clone https://github.com/zhangzhen688/Family-Smart-Screen.git
cd Family-Smart-Screen
./build.sh                    # Release build
./build.sh clean              # Clean rebuild
```

### Run

```bash
./run.sh                      # Start all 7 services automatically
```

Or start services individually:

```bash
cd build
./backend/device_server &             # Device control    → port 1234
./backend/camera_server &             # Camera capture    → port 1235
./backend/voice_server &              # Voice RPC bridge  → port 1236
./backend/web_server &                # Mobile web UI     → port 8080
./backend/xiaozhi_control_center &    # XiaoZhi cloud     → UDP 5678/5679
./backend/xiaozhi_sound_app &         # Audio capture     → ALSA
./gui/smart_gui                        # Main GUI display
```

### Mobile Access

Connect your phone to the same WiFi network as the host machine, then open:

```
http://<host-ip>:8080
```

Find the host IP address with `hostname -I` or `ip addr show`.

## Project Structure

```
Family-Smart-Screen/
├── CMakeLists.txt                    # Top-level CMake configuration
├── build.sh                          # Build script (release/clean/debug)
├── run.sh                            # One-click startup script
├── inc/
│   ├── common.h                      # Shared types, macros, platform detection
│   └── rpc_protocol.h               # JSON-RPC port numbers and method names
├── libs/
│   ├── cjson/                        # cJSON JSON parser (static library)
│   ├── lvgl/                         # LVGL v9.2.2 embedded GUI library
│   └── jsonrpc/                      # jsonrpc-c framework + libev event loop
├── gui/
│   ├── main.c                        # Application entry point
│   ├── lv_conf.h                     # LVGL configuration (SDL2, FreeType, PNG)
│   ├── rpc_client.h / rpc_client.c   # JSON-RPC TCP client library
│   ├── xiaozhi_ipc.h / xiaozhi_ipc.c # XiaoZhi UDP IPC listener
│   ├── assets/                       # Fonts and emotion images
│   └── ui/
│       ├── ui_main.c                 # Home screen
│       ├── ui_device.c               # Device control
│       ├── ui_camera.c               # Camera preview and capture
│       ├── ui_album.c                # Photo album browser
│       ├── ui_scenes.c               # Scene linkage automation
│       ├── ui_voice.c                # XiaoZhi voice assistant
│       └── ui_settings.c             # System settings
└── backend/
    ├── CMakeLists.txt
    ├── device_server/
    │   ├── rpc_server.c              # JSON-RPC server (port 1234)
    │   ├── dev_led.c / dev_led.h     # LED GPIO driver (stub on x86)
    │   ├── dev_dht11.c / dev_dht11.h # DHT11 temperature/humidity sensor
    │   ├── dev_sg90.c / dev_sg90.h   # SG90 servo PWM driver
    │   └── dev_ap3216c.c / dev_ap3216c.h # AP3216C ambient light sensor
    ├── camera_server/
    │   ├── rpc_server.c              # JSON-RPC server (port 1235)
    │   └── v4l2_capture.c / v4l2_capture.h # V4L2 camera capture module
    ├── voice_server/
    │   ├── rpc_server.c              # JSON-RPC server (port 1236)
    │   ├── xiaozhi_bridge.c / .h     # UDP IPC bridge to control_center
    │   └── xiaozhi/                  # XiaoZhi AI source (adapted from refs)
    │       ├── control_center.cpp    # WebSocket cloud client
    │       ├── sound_app.cpp         # ALSA audio capture/playback
    │       ├── ipc_udp.cpp / .h      # UDP inter-process communication
    │       ├── websocket_client.cpp  # WebSocket wrapper
    │       ├── http.cpp / .h         # Device activation HTTP client
    │       ├── opus.cpp / .h         # Opus audio codec
    │       ├── record.cpp / .h       # ALSA recording
    │       ├── aplay.cpp / .h        # ALSA playback
    │       ├── uuid.cpp / .h         # Device UUID management
    │       ├── json.hpp              # nlohmann/json (single header)
    │       └── cfg.h                 # Port constants
    └── web_server/
        ├── main.c                    # HTTP server + route handlers
        └── http_server.c / .h        # Embedded HTTP/1.1 server engine
```

## API Reference

### Device Server (TCP :1234)

| Method | Parameters | Returns | Description |
|--------|-----------|---------|-------------|
| `led_set` | `[index: int, on: int]` | `{ok: bool}` | Set LED state (0=off, 1=on) |
| `led_get` | `[index: int]` | `{ok: bool, on: bool}` | Query LED state |
| `dht11_read` | `[]` | `{ok: bool, humidity: int, temp: int}` | Read temperature (Celsius) and humidity (%) |
| `sg90_set` | `[angle: int]` | `{ok: bool, angle: int}` | Set curtain servo angle (0-180) |
| `ap3216c_read` | `[]` | `{ok: bool, als: int, ps: int, ir: int}` | Read ambient light, proximity, IR |

### Camera Server (TCP :1235)

| Method | Parameters | Returns | Description |
|--------|-----------|---------|-------------|
| `camera_start` | `[]` | `{ok: bool, width: int, height: int}` | Open device and begin streaming |
| `camera_stop` | `[]` | `{ok: bool}` | Stop streaming and release device |
| `camera_capture_frame` | `[]` | `{ok: bool, data: base64, size: int}` | Grab single JPEG frame |
| `camera_take_photo` | `[filename: str]` | `{ok: bool, path: str}` | Save current frame to disk |
| `camera_start_recording` | `[filename: str]` | `{ok: bool}` | Begin video recording |
| `camera_stop_recording` | `[]` | `{ok: bool, path: str}` | End and save recording |
| `camera_list_photos` | `[]` | `{ok: bool, photos: [str]}` | List saved photo filenames |
| `camera_delete_photo` | `[filename: str]` | `{ok: bool}` | Delete a photo |

### Voice Server (TCP :1236)

| Method | Parameters | Returns | Description |
|--------|-----------|---------|-------------|
| `voice_get_state` | `[]` | `{ok: bool, state: str, text: str}` | Query AI state and last message |
| `voice_send_text` | `[text: str]` | `{ok: bool}` | Send text to TTS or start listening |
| `voice_set_volume` | `[level: int]` | `{ok: bool, volume: int}` | Set output volume (0-100) |

### Web Server (HTTP :8080)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Mobile web control panel (HTML SPA) |
| `/api/status` | GET | Full device status JSON |
| `/api/led` | POST | Control LED (`{"index":0,"on":true}`) |
| `/api/curtain` | POST | Set curtain (`{"angle":90}`) |
| `/api/voice` | POST | Voice command (`{"action":"listen"}`) |
| `/camera/snapshot` | GET | JPEG camera snapshot |

## Hardware Drivers

During development on Ubuntu, all hardware drivers operate in stub mode (`SIMULATOR_LINUX` compile flag), printing debug output and maintaining in-memory state. When deployed to an ARM Linux board, the same code accesses real device nodes directly.

| Peripheral | Interface | Device Node | Driver Module |
|------------|-----------|-------------|---------------|
| LED x6 | GPIO | `/dev/led_0` through `/dev/led_5` | `led_drv.ko` |
| DHT11 | Character Device | `/dev/dht11` | `dht11_drv.ko` |
| SG90 Servo | PWM sysfs | `/sys/class/pwm/pwmchip0/pwm0/` | Kernel PWM |
| AP3216C | I2C | `/dev/ap3216c` | `ap3216c_drv.ko` |
| USB Camera | V4L2 UVC | `/dev/video0` | Kernel uvcvideo |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `SIMULATOR_LINUX` | `ON` | Enable stub drivers and SDL2 display for x86 development |
| `CMAKE_BUILD_TYPE` | `Release` | Build type (`Release`, `Debug`) |

```bash
# ARM target build (cross-compilation)
cmake -B build -DSIMULATOR_LINUX=OFF -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

XiaoZhi AI components (`xiaozhi_control_center`, `xiaozhi_sound_app`) are conditionally built based on available dependencies. The core servers and GUI always build.

## Tech Stack

| Layer | Technology | Version |
|-------|-----------|---------|
| GUI Framework | LVGL | 9.2.2 |
| Display Backend | SDL2 (x86) / DRM/FBDEV (ARM) | - |
| Font Rendering | FreeType | 2.x |
| IPC Protocol | JSON-RPC over TCP | - |
| Event Loop | libev | 4.x |
| JSON Parser | cJSON | 1.x |
| Camera | V4L2 + mmap | - |
| Audio | ALSA + Opus | - |
| AI Cloud | WebSocket (websocketpp) | - |
| Build System | CMake | 3.15+ |
| Compiler | GCC (C11 / C++17) | - |

## Roadmap

- [x] Multi-process backend architecture with JSON-RPC IPC
- [x] Device control (LED, DHT11, SG90, AP3216C) with stub/real driver switching
- [x] V4L2 camera capture with photo and recording
- [x] XiaoZhi AI voice assistant integration (WebSocket, Opus, ALSA)
- [x] Scene linkage automation presets
- [x] Mobile web remote control panel
- [x] Dark theme UI with LVGL
- [ ] Weather API integration (real-time weather data)
- [ ] MJPEG streaming for live camera on web
- [ ] MQTT/OneNet cloud IoT platform support
- [ ] OTA firmware update
- [ ] Voice wake word detection (offline)
- [ ] Bluetooth mesh device networking

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

## Acknowledgments

This project builds upon several excellent open-source works:

- [LVGL](https://lvgl.io/) — Light and Versatile Embedded Graphics Library
- [XiaoZhi Linux](https://github.com/100askTeam/xiaozhi-linux) — AI voice assistant reference implementation by 百问科技
- [cJSON](https://github.com/DaveGamble/cJSON) — Ultralightweight JSON parser in ANSI C
- [jsonrpc-c](https://github.com/hmng/jsonrpc-c) — JSON-RPC framework for C
- [libev](http://software.schmorp.de/pkg/libev.html) — A full-featured and high-performance event loop
