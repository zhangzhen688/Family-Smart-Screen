# Smart Screen — Family Smart Home Panel

LVGL-based smart home control center with XiaoZhi AI voice assistant, V4L2 camera, JSON-RPC distributed backend, and mobile web remote control.

## Architecture

```
GUI (LVGL + SDL2)  ←──JSON-RPC/TCP──→  device_server :1234
  7 pages                               camera_server :1235
  Home/Device/Camera                    voice_server  :1236
  Album/Scenes/Settings                 web_server    :8080 (HTTP)
                                        xiaozhi_control_center (WebSocket→Cloud)
                                        xiaozhi_sound_app      (ALSA Audio)
```

## Features

### Local LVGL GUI (800x480 touchscreen)
| Page | Features |
|------|----------|
| **Home** | Clock, weather bar, temp/humidity/light sensor cards, 6 shortcuts |
| **Devices** | 6 LED switches, curtain angle slider, DHT11/AP3216C sensor refresh |
| **Camera** | V4L2 live preview, photo capture, video recording |
| **Album** | Photo browser and management |
| **Scenes** | One-tap automation: Home/Away/Sleep/Movie/Party |
| **Voice AI** | XiaoZhi character with emotions, activation codes, tap-to-talk |
| **Settings** | Brightness, volume, sleep timer, about info |

### Mobile Web Remote Control
- Open `http://<IP>:8080` on any phone browser
- Temp/humidity/light cards with live refresh
- LED toggles, curtain slider
- Camera snapshot
- Voice assistant status/control
- Dark theme, responsive design

### XiaoZhi AI Voice
- Voice wake-up and conversation
- Cloud STT/TTS via WebSocket
- Character emotions (idle/listening/speaking/thinking/sad)
- Device activation flow (show code → activate on website)

## Quick Start

### Install Dependencies
```bash
# Core
sudo apt install cmake build-essential libsdl2-dev libfreetype-dev

# XiaoZhi AI
sudo apt install libwebsocketpp-dev libopus-dev libspeexdsp-dev \
                 libasound2-dev libssl-dev libcurl4-openssl-dev
```

### Build & Run
```bash
git clone https://github.com/YOUR_USER/smart-screen.git
cd smart-screen
./build.sh
./run.sh
```

### Mobile Access
Phone and PC on same WiFi, open browser:
```
http://<YOUR_IP>:8080
```
(Find IP with `hostname -I`)

## Project Structure

```
smart_screen/
├── CMakeLists.txt          # Top-level CMake
├── build.sh / run.sh       # Build & run scripts
├── inc/
│   ├── common.h            # Types, macros, platform detection
│   └── rpc_protocol.h      # JSON-RPC protocol (ports, methods)
├── libs/
│   ├── cjson/              # JSON parser
│   ├── lvgl/               # LVGL v9.2.2
│   └── jsonrpc/            # jsonrpc-c + libev
├── gui/                    # LVGL Frontend
│   ├── main.c              # Entry point
│   ├── lv_conf.h           # LVGL config (SDL2, FreeType, PNG)
│   ├── rpc_client.c/h      # JSON-RPC TCP client
│   ├── xiaozhi_ipc.c/h     # XiaoZhi UDP IPC listener
│   └── ui/                 # 7 UI pages
├── backend/                # Backend services
│   ├── device_server/      # LED/DHT11/SG90/AP3216C (port 1234)
│   ├── camera_server/      # V4L2 camera (port 1235)
│   ├── voice_server/       # JSON-RPC bridge (port 1236)
│   ├── web_server/         # HTTP server + mobile web UI (port 8080)
│   └── voice_server/xiaozhi/  # XiaoZhi AI source (from refs)
└── refs/                   # Reference projects
```

## Hardware Drivers

Stub drivers on Ubuntu (`#ifdef SIMULATOR_LINUX`). Real `/dev` nodes on ARM:

| Device | Interface | Node |
|--------|-----------|------|
| LED x6 | GPIO | `/dev/led_0` ~ `/dev/led_5` |
| DHT11 | Char device | `/dev/dht11` |
| SG90 | PWM sysfs | `/sys/class/pwm/pwmchip0/` |
| AP3216C | I2C | `/dev/ap3216c` |
| Camera | V4L2 UVC | `/dev/video0` |

## Tech Stack

| Layer | Technology |
|-------|-----------|
| GUI | LVGL v9.2.2 + SDL2 + FreeType |
| IPC | JSON-RPC over TCP + UDP |
| Camera | V4L2 + mmap + MJPEG |
| Voice AI | WebSocket + Opus + ALSA + XiaoZhi Cloud |
| Web Remote | Embedded HTTP (C) + Vanilla JS SPA |
| Build | CMake + GCC |
| Platform | Ubuntu (dev) / ARM Linux (deploy) |

## Acknowledgments

- [LVGL](https://lvgl.io/)
- [XiaoZhi Linux](https://github.com/100askTeam/xiaozhi-linux) by 百问科技
- [cJSON](https://github.com/DaveGamble/cJSON)
- [jsonrpc-c](https://github.com/hmng/jsonrpc-c)
- [libev](http://software.schmorp.de/pkg/libev.html)
