# 家庭智慧屏项目完全指南

> **适用人群：** 刚接手项目的开发者、准备面试的技术面试官、写简历的项目参与者
>
> **技术栈：** C11/C++17 · LVGL 9.2 · V4L2 · JSON-RPC · POSIX共享内存 · FreeType · libev · 昇腾310B

---

## 目录

1. [项目概览与架构](#1-项目概览与架构)
2. [简历技术亮点](#2-简历技术亮点)
3. [LVGL 智慧屏显示系统](#3-lvgl-智慧屏显示系统)
4. [AI 小智语音助手](#4-ai-小智语音助手)
5. [V4L2 摄像头系统](#5-v4l2-摄像头系统)
6. [移动端 Web 控制服务](#6-移动端-web-控制服务) (含 6.6 WebRTC 实时视频)
7. [驱动层详解与昇腾310B移植](#7-驱动层详解与昇腾310b移植)
8. [快速上手指南](#8-快速上手指南)
9. [面试真题库：嵌入式Linux应用与驱动](#9-面试真题库嵌入式linux应用与驱动)
   - 9.1 [项目相关面试题（Q1-Q16）](#91-项目相关面试题)
   - 9.2 [V4L2 视频驱动](#92-v4l2-视频驱动面试题)
   - 9.3 [设备树与驱动模型](#93-设备树device-tree与驱动模型)
   - 9.4 [字符设备驱动](#94-字符设备驱动)
   - 9.5 [中断与并发控制](#95-中断与并发控制)
   - 9.6 [内存管理与DMA](#96-内存管理与dma)
   - 9.7 [C++ 嵌入式高频考点](#97-c-嵌入式高频考点)
   - 9.8 [多线程与同步](#98-多线程与同步)
   - 9.9 [LVGL 显示与图形](#99-lvgl-显示与图形面试题)
   - 9.10 [IPC 与网络通信](#910-ipc-进程间通信与网络)
   - 9.11 [综合场景题](#911-综合场景题)

---

## 1. 项目概览与架构

### 1.1 项目简介

家庭智慧屏是一个**全栈嵌入式智能家居控制终端**，运行在 ARM Linux（昇腾310B）平台上。它集成了：

- **7大UI页面**（主屏、设备控制、摄像头、相册、AI语音、场景联动、设置）
- **4个后端微服务**（设备服务、摄像头服务、语音服务、Web服务）
- **7种硬件驱动**（LED/蜂鸣器/继电器、DHT11温湿度、AP3216C光感、GT9147触摸屏、SG90舵机、SR501人体红外、HC06蓝牙）

### 1.2 架构全景图

```
                         ┌──────────────────────────────────────┐
                         │          Smart GUI (LVGL 9.2)        │
                         │  SDL2(模拟器) / DRM+FBDEV(ARM)       │
                         │  7 Pages · FreeType中文字体           │
                         │  Ping-Pong双缓冲 · 30fps预览         │
                         └────┬──────┬──────┬───────────────────┘
                              │      │      │
          JSON-RPC/TCP        │      │      │  POSIX共享内存
          (localhost)         │      │      │  (零拷贝视频帧)
                              │      │      │
     ┌────────┬───────────────┼──────┘      │                │
     │        │               │             │                │
┌────▼──┐ ┌──▼──────┐ ┌──────▼──┐ ┌───────▼──────┐ ┌───────▼──────┐
│Device │ │Voice    │ │Camera   │ │Web Server    │ │WebRTC Server │
│Server │ │Server   │ │Server   │ │Port 8080     │ │Port 8081/8082│
│:1234  │ │:1236    │ │:1235    │ │HTTP→JSON-RPC │ │C++17         │
│       │ │         │ │Relay:8081│ │桥接           │ │libdatachannel│
└──┬──┬─┘ └────┬────┘ └────┬────┘ └──────────────┘ └──────────────┘
   │  │        │           │
   │  │   ┌────▼─────┐ ┌───▼──────────────┐
   │  │   │UDP IPC   │ │V4L2 UVC Camera   │
   │  │   │:5678/5679│ │MJPEG 640x480     │
   │  │   └────┬─────┘ │mmap + 4 buffers  │
   │  │        │       └──────────────────┘
   │  │  ┌─────▼──────┐
   │  │  │XiaoZhi     │
   │  │  │Control     │
   │  │  │Center      │──WebSocket──→ XiaoZhi Cloud
   │  │  │+ Sound App │   (api.tenclass)
   │  │  └────────────┘
   │  │
   ▼  ▼
┌──────────────────────┐
│ 硬件驱动层            │
│ /dev/led_*  (GPIO)   │
│ /dev/dht11  (OneWire)│
│ /dev/ap3216c (I2C)   │
│ /dev/input/eventX     │
│ /dev/sg90   (PWM)    │
│ /dev/sr501  (GPIO INT)│
│ /dev/ttySACx (UART)  │
└──────────────────────┘
```

### 1.3 三种IPC通信机制

| 通道 | 协议 | 用途 | 性能特点 |
|------|------|------|----------|
| **A: JSON-RPC/TCP** | JSON-RPC 2.0 over TCP | 控制面：设备操作、摄像头启停、语音指令 | 文本协议、易调试、低吞吐 |
| **B: POSIX共享内存+信号量** | SPSC环形缓冲区 | 数据面：摄像头MJPEG帧实时传输 | 零拷贝、低延迟、高吞吐 |
| **C: UDP IPC** | JSON over UDP | AI语音组件间异步通信 | 轻量、异步、适合状态同步 |

**设计原则：** "控制走RPC，数据走共享内存" —— 将高频率的大块视频数据与低频的控制指令分离到不同的IPC通道。

**为什么不用 MQTT / ZeroMQ / nanomsg？**
- MQTT 需要 Broker（如 mosquitto），增加部署复杂度；适合跨设备通信而非本地 IPC
- ZeroMQ / nanomsg 功能强大但依赖较重，JSON-RPC + cJSON 组合更轻量可控
- 本项目的 IPC 范围是**单板内部多进程通信**，TCP/UDP localhost 足够

**POSIX 共享内存 vs System V 共享内存：**
- POSIX (`shm_open`) 使用文件接口命名，与文件系统集成更好
- System V (`shmget`) 使用 key 命名，需要 `ftok()` 转换
- POSIX 信号量 (`sem_open`) 天然支持命名，与共享内存配合更自然
- 项目选择 POSIX 是为了更好的可移植性和更简洁的 API

### 1.4 编译模式：一套代码双平台

整个项目通过**单一编译宏 `SIMULATOR_LINUX`** 控制双平台行为：

```
SIMULATOR_LINUX=ON  →  x86 Ubuntu模拟器：SDL2显示 + 硬件stub
SIMULATOR_LINUX=OFF →  ARM昇腾310B：DRM/FBDEV显示 + 真实硬件驱动
```

所有驱动层代码使用 `#ifdef SIMULATOR_LINUX` 分支：
- **模拟器模式：** 返回随机但合理的模拟数据，无需真实硬件即可开发调试UI
- **目标模式：** 访问 `/dev/*` 设备节点、sysfs PWM、V4L2设备

---

## 2. 简历技术亮点

### 2.1 架构设计（适合写简历的条目）

> **基于JSON-RPC微服务架构，实现显示层与硬件控制层的完全解耦**
>
> - 将系统拆分为4个独立进程（GUI/设备/摄像头/语音），各服务通过JSON-RPC over TCP通信
> - 控制面与数据面分离：高频视频帧走POSIX共享内存（零拷贝），低频控制指令走RPC
> - 单编译宏（`SIMULATOR_LINUX`）实现x86模拟器与ARM目标板的一键切换
> - 驱动层采用统一接口抽象（`#ifdef`双模式），所有硬件在PC上可模拟调试

**面试关键词：** 微服务架构、进程间通信、控制面/数据面分离、平台抽象层

### 2.2 LVGL智慧屏

> **基于LVGL 9.2实现800x480智慧屏显示系统，解决摄像头预览画面撕裂与显示不全问题**
>
> - 使用32位色深（XRGB8888）+ SDL2/DRM双后端，支持模拟器与真机无缝切换
> - 实现Ping-Pong双缓冲 + `lv_obj_invalidate` 精确刷新策略，消除摄像头预览的画面撕裂
> - 集成FreeType + HarmonyOS Sans字体，实现中英文混排显示
> - 摄像头JPEG解码→双线性缩放→XRGB8888图像描述符的完整渲染管线，画面尺寸动态适配

**面试关键词：** LVGL、双缓冲、画面撕裂、XRGB8888、FreeType、渲染管线

### 2.3 AI小智

> **集成小智AI语音助手，通过UDP IPC + WebSocket实现云端语音交互**
>
> - 设计UDP IPC协议（4端口）连接GUI、语音控制中心、音频采集/播放模块
> - 实现9状态状态机（空闲→连接→监听→说话→激活→错误），支持多状态emoji表情切换
> - JSON-RPC桥接层将云端WebSocket AI能力暴露为本地RPC接口
> - 支持中文字符实时渲染（FreeType），激活码、STT文本、TTS播放全流程

**面试关键词：** 语音助手、状态机、UDP IPC、WebSocket、Opus编解码

### 2.4 摄像头V4L2

> **基于V4L2+libjpeg实现MJPEG摄像头采集，解决画面撕裂与显示不全问题**
>
> - 使用V4L2 mmap内存映射（4缓冲区），避免用户态/内核态数据拷贝
> - 设计SPSC三槽位共享内存环形缓冲区 + POSIX命名信号量实现帧同步
> - Ping-Pong双缓冲渲染策略：写入非显示缓冲区→原子交换→`lv_obj_invalidate`触发重绘，彻底消除撕裂
> - 动态分辨率适配：摄像头输出分辨率与LVGL显示区域独立计算，双线性缩放填充
> - 处理UVC摄像头quirks（HIK 1080P相机YUYV/MJPEG格式协商异常），支持多设备回退

**面试关键词：** V4L2、mmap、SPSC、信号量、双缓冲、画面撕裂、UVC

### 2.5 移动端Web控制

> **自研嵌入式HTTP服务器，实现手机端智慧屏远程控制**
>
> - 纯C实现select()复用的HTTP/1.1服务器，无第三方Web框架依赖
> - 单页应用(SPA)内嵌为C字符串，零文件系统依赖
> - RESTful API设计（`/api/status`、`/api/led`、`/api/curtain`等）
> - HTTP→JSON-RPC桥接，Web请求转换为后端RPC调用
> - 响应式布局（`@media`查询），支持iOS Home Screen Web App

**面试关键词：** 嵌入式HTTP、SPA、RESTful、JSON-RPC桥接

---

## 3. LVGL 智慧屏显示系统

### 3.1 显示初始化流程

**代码位置：** [gui/main.c](gui/main.c)

```c
// 步骤 1: LVGL 核心初始化
lv_init();

// 步骤 2: 文件系统初始化（FreeType字体 + 图片资源）
lv_fs_stdio_init();

// 步骤 3: 显示后端初始化
//   模拟器: lv_sdl_window_create(800, 480)  — SDL2双缓冲
//   ARM:    lv_linux_drm_create()           — DRM/DRI硬件加速
//          lv_linux_fbdev_create()          — Framebuffer回退
lv_display_init();

// 步骤 4: 输入设备
//   模拟器: lv_sdl_mouse_create()
//   ARM:    lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1")
lv_input_init();

// 步骤 5: 主循环（互斥锁保护）
while (1) {
    lvgl_lock();                        // pthread互斥锁
    uint32_t delay_ms = lv_timer_handler(); // LVGL定时器 + 渲染
    lvgl_unlock();
    usleep(min(delay_ms, 5) * 1000);
}
```

### 3.2 关键LVGL配置

**代码位置：** [gui/lv_conf.h](gui/lv_conf.h)

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `LV_COLOR_DEPTH` | 32 | XRGB8888，每通道8位 |
| `LV_MEM_SIZE` | 2MB | LVGL内部堆大小 |
| `LV_SDL_RENDER_MODE` | `DIRECT` | 直接渲染模式，减少flush回调开销 |
| `LV_SDL_BUF_COUNT` | 2 | SDL双缓冲 |
| `LV_USE_FREETYPE` | 1 | 启用FreeType矢量字体 |
| `LV_FREETYPE_CACHE_SIZE` | 64KB | 字形缓存 |

### 3.3 UI页面架构（7大页面）

| 页面 | 文件 | 核心功能 |
|------|------|----------|
| 主屏 | [gui/ui/ui_main.c](gui/ui/ui_main.c) | 时钟、天气卡片、6个快捷入口按钮 |
| 设备控制 | [gui/ui/ui_device.c](gui/ui/ui_device.c) | 6路LED开关、窗帘滑块、温湿度/光感显示 |
| 摄像头 | [gui/ui/ui_camera.c](gui/ui/ui_camera.c) | 实时预览、拍照、录像 |
| 相册 | [gui/ui/ui_album.c](gui/ui/ui_album.c) | 扫描`./photos/`目录、BMP缩略图浏览 |
| AI语音 | [gui/ui/ui_voice.c](gui/ui/ui_voice.c) | 角色表情、状态栏、对话文本、点击对话 |
| 场景联动 | [gui/ui/ui_scenes.c](gui/ui/ui_scenes.c) | 5种智能家居预设场景 |
| 设置 | [gui/ui/ui_settings.c](gui/ui/ui_settings.c) | 亮度/音量调节、休眠定时器 |

### 3.4 线程安全设计

LVGL本身不是线程安全的。我们使用**单一全局互斥锁**保护所有LVGL API调用：

```c
// 主线程调用
lvgl_lock();
lv_timer_handler();    // LVGL内部渲染
lvgl_unlock();

// 其他线程（UDP回调、传感器轮询）调用
lvgl_lock();
lv_label_set_text(label, text);   // 更新UI
lvgl_unlock();
```

**代码位置：** [gui/main.c:30-33](gui/main.c#L30-L33) 定义了 `lvgl_lock()` / `lvgl_unlock()`，在 [gui/ui/ui_voice.c](gui/ui/ui_voice.c) 的UDP回调中频繁使用。

### 3.5 FreeType中文渲染

**代码位置：** [gui/ui/ui_voice.c:80-113](gui/ui/ui_voice.c#L80-L113)

```c
// 加载HarmonyOS Sans中文字体
g_font_chat = lv_freetype_font_create(
    "assets/HarmonyOS_Sans_SC_Regular.ttf",
    LV_FREETYPE_FONT_RENDER_MODE_BITMAP,  // 位图渲染模式
    26,                                     // 字号
    LV_FREETYPE_FONT_STYLE_NORMAL
);
```

然后将字体通过LVGL style绑定到label对象上，实现中英文混排。

---

## 4. AI 小智语音助手

### 4.1 整体架构

```
┌─────────┐  UDP :5678  ┌──────────────┐  WebSocket  ┌──────────────┐
│ GUI     │────────────→│ Control      │────────────→│ XiaoZhi      │
│ (LVGL)  │←────────────│ Center       │←────────────│ Cloud        │
│         │  UDP :5679  │ (C++)        │             │ (ASR/TTS/NLU)│
└─────────┘             └──────┬───────┘             └──────────────┘
                               │ UDP :5676/:5677
                        ┌──────▼───────┐
                        │ Sound App    │
                        │ (C++)        │
                        │ ALSA + Opus  │
                        └──────────────┘
```

### 4.2 通信协议：4端口UDP IPC

**代码位置：** [backend/voice_server/xiaozhi/cfg.h](backend/voice_server/xiaozhi/cfg.h)

| 端口 | 方向 | 消息类型 | 说明 |
|------|------|----------|------|
| 5676 | Sound App → Control Center | 二进制音频 | 麦克风采集的Opus编码音频 |
| 5677 | Control Center → Sound App | 二进制音频 | 云端返回的TTS音频 |
| 5678 | GUI → Control Center | JSON控制 | `{"type":"listen","state":"start"}` |
| 5679 | Control Center → GUI | JSON状态 | 状态变更、STT文本、情绪表情、IoT数据 |

### 4.3 状态机（9状态）

**代码位置：** [backend/voice_server/xiaozhi_bridge.h](backend/voice_server/xiaozhi_bridge.h)

```
IDLE(空闲) → CONNECTING(连接中) → LISTENING(监听中) → SPEAKING(说话中)
                                                      ↓
                                              ACTIVATING(激活中) → ERROR(错误)
```

每个状态对应不同的emoji表情：

```c
// gui/ui/ui_voice.c:132-142
switch (state) {
    case 3: emoji = img_idle;    break;  // 待机 → 调皮表情
    case 5: emoji = img_listen;  break;  // 监听 → 倾听表情
    case 6: emoji = img_speak;   break;  // 说话 → 开心表情
    case 9: emoji = img_worry_s; break;  // 错误 → 担心表情
    default: emoji = img_think;  break;  // 其他 → 思考表情
}
```

### 4.4 点击对话流程

**代码位置：** [gui/ui/ui_voice.c:182-193](gui/ui/ui_voice.c#L182-L193)

1. 用户点击屏幕 → `screen_tap_cb`
2. 发送UDP `{"type":"listen","state":"start","mode":"auto"}` 到Control Center
3. Control Center通过WebSocket连接云端，开始ASR
4. 云端返回STT文本 → Control Center → UDP :5679 → GUI更新对话文本
5. NLU处理后返回TTS → Sound App播放 → GUI显示Speaking状态
6. 对话结束 → 回到Standby状态

### 4.5 Voice Server桥接层

**代码位置：** [backend/voice_server/rpc_server.c](backend/voice_server/rpc_server.c)

Voice Server作为JSON-RPC服务器（端口1236），将RPC调用翻译为UDP消息：

```c
// RPC方法: voice_send_text
//   如果带text参数 → 发送TTS请求到Control Center
//   如果不带参数   → 启动监听模式
//   返回: {ok: true}
```

---

## 5. V4L2 摄像头系统

### 5.1 数据流全景

```
USB Camera (UVC)
      │
      ▼
┌──────────────────────────────────────────────────┐
│ V4L2 Capture (v4l2_capture.c)                    │
│   - open(/dev/video0)                            │
│   - VIDIOC_S_FMT → MJPEG 640x480                 │
│   - VIDIOC_REQBUFS → 4个mmap缓冲区                │
│   - VIDIOC_STREAMON                               │
│   - 循环: DQBUF → 拷贝 → QBUF                     │
└────────────────┬─────────────────────────────────┘
                 │ camera_capture_frame()
                 ▼
┌──────────────────────────────────────────────────┐
│ Camera Streamer (camera_streamer.c) — 独立线程    │
│   sem_wait(empty) → 写入slot → sem_post(full)     │
│                                                  │
│  POSIX共享内存 (/camera_shm)                      │
│  ┌──────┬─────────┬─────────┬─────────┐          │
│  │Header│ Slot 0  │ Slot 1  │ Slot 2  │          │
│  │ 64B  │ 16B+1MB │ 16B+1MB │ 16B+1MB │          │
│  └──────┴─────────┴─────────┴─────────┘          │
│  信号量: /camera_sem_empty(3), /camera_sem_full(0)│
└────────────────┬─────────────────────────────────┘
                 │ sem_trywait() 非阻塞读取
                 ▼
┌──────────────────────────────────────────────────┐
│ Camera Reader (camera_reader.c) — LVGL定时器线程   │
│   sem_trywait(full) → 读取slot → sem_post(empty)  │
│   拷贝到本地buffer后立即释放slot                    │
└────────────────┬─────────────────────────────────┘
                 │
                 ▼
┌──────────────────────────────────────────────────┐
│ UI Camera Pipeline (ui_camera.c)                  │
│   1. JPEG校验 (SOI marker 0xFFD8)                 │
│   2. libjpeg解码 → BGRX (JCS_EXT_BGRX)           │
│   3. 双线性缩放 → 显示分辨率                        │
│   4. 写入Ping-Pong缓冲区的"非活跃"buffer             │
│   5. lv_image_set_src() 原子切换                   │
│   6. lv_obj_invalidate() 触发重绘                  │
└──────────────────────────────────────────────────┘
```

### 5.2 画面撕裂解决方案

画面撕裂的根因：**摄像头帧到达时刻与LVGL屏幕刷新时刻不同步**，如果直接在显示缓冲区上更新图像，就会出现"上半帧是新画面、下半帧是旧画面"的撕裂现象。

我们的解决方案包含**三层防护**：

#### 第一层：Ping-Pong双缓冲（应用层）

**代码位置：** [gui/ui/ui_camera.c:43-46](gui/ui/ui_camera.c#L43-L46)

```c
// 两个独立的图像描述符 + 像素缓冲区，交替使用
static lv_image_dsc_t  g_dsc[2];        // LVGL图像描述符
static uint8_t        *g_rgb[2];        // 像素数据
static int             g_cur = 0;       // 当前显示的buffer索引

// 每一帧都写入到"另一个"buffer
int next = g_cur ^ 1;                    // 切换(0→1, 1→0)
scale_bilinear(..., g_rgb[next], ...);   // 写入非活跃buffer
lv_image_set_src(preview_img, &g_dsc[next]); // 原子切换
lv_obj_invalidate(preview_img);          // 触发重绘
g_cur = next;                            // 更新索引
```

关键点：
- **缩放结果始终写入非显示的buffer**，避免"画到一半被显示"导致的撕裂
- `lv_image_set_src()` 只是一个指针赋值，不涉及像素拷贝
- `lv_obj_invalidate()` 告诉LVGL该区域需要重绘，LVGL会在下一次 `lv_timer_handler()` 渲染周期读取新的buffer

#### 第二层：SPSC三槽位环形缓冲区（IPC层）

**代码位置：** [inc/camera_shm.h](inc/camera_shm.h)

```
生产者(Streamer线程)                 消费者(GUI的LVGL定时器)
       │                                    │
  sem_wait(empty)                    sem_trywait(full) // 不阻塞!
       │                                    │
  写入slot[write_idx]                 读取slot[read_idx]
       │                                    │
  write_idx = (idx+1)%3              read_idx = (idx+1)%3
       │                                    │
  sem_post(full)                     sem_post(empty)
```

设计要点：
- **3个槽位**：2个不够（生产者和消费者可能同时访问相邻槽位），3个保证SPSC无锁安全
- **`sem_trywait()` 非阻塞读取**：消费者在LVGL定时器中调用，如果没帧就跳过，不阻塞UI渲染
- **64字节缓存行对齐**：Header结构体填充到64字节，避免ARM上的伪共享（false sharing）

#### 第三层：精确的控件无效化（显示层）

**代码位置：** [gui/ui/ui_camera.c:191-193](gui/ui/ui_camera.c#L191-L193)

```c
lv_image_set_src(preview_img, &g_dsc[next]);
lv_obj_invalidate(preview_img);   // 无效化图像控件本身
lv_obj_invalidate(preview_ctnr);  // 同时无效化父容器
```

两个 `lv_obj_invalidate()` 分别针对：
1. `preview_img`：标记图像数据已变更
2. `preview_ctnr`：标记容器区域需要重绘（覆盖圆角、边框等）

如果不调用第二个invalidate，父容器的装饰元素可能使用旧的像素缓存。

### 5.3 显示不全问题解决

显示不全的根因：**摄像头分辨率 ≠ LVGL显示区域尺寸**。

**代码位置：** [gui/ui/ui_camera.c:234-248](gui/ui/ui_camera.c#L234-L248)

```c
// 运行时动态获取显示区域尺寸
g_view_w = lv_obj_get_content_width(preview_ctnr);
g_view_h = lv_obj_get_content_height(preview_ctnr);

// 为显示区域精确尺寸分配缓冲区
for (int i = 0; i < 2; i++) {
    alloc_dsc(i, g_view_w, g_view_h);  // 不是摄像头分辨率!
}

// 双线性缩放：摄像头分辨率 → 显示分辨率
scale_bilinear(g_decode_buf,      // 源：摄像头分辨率
               g_cam_w, g_cam_h, g_cam_w * 4,
               g_rgb[next],       // 目标：显示分辨率
               g_view_w, g_view_h, g_view_w * 4);
```

关键设计决策：
- **图像描述符尺寸 = 显示区域尺寸，不是摄像头尺寸**。如果图像描述符大于显示区域，LVGL会自动缩放导致模糊；如果小于，会有黑边。精确匹配就能避免LVGL的二次处理。
- **首帧隐藏**：在第一个有效帧解码完成前，图像控件保持隐藏，避免短暂的白屏/黑屏闪烁

```c
// gui/ui/ui_camera.c:196-201
if (placeholder && !lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(preview_ctnr);
}
```

### 5.4 V4L2初始化流程

**代码位置：** [backend/camera_server/v4l2_capture.c](backend/camera_server/v4l2_capture.c)

```c
// 1. 设备发现（多设备回退）
const char *paths[] = {
    CAM_DEVICE_NODE,     // 首选：by-path符号链接
    "/dev/video0", ..., "/dev/video5",  // 回退
    NULL
};

// 2. 能力检查
VIDIOC_QUERYCAP → 验证 V4L2_CAP_VIDEO_CAPTURE & V4L2_CAP_STREAMING

// 3. 格式协商（优先MJPEG）
VIDIOC_S_FMT → V4L2_PIX_FMT_MJPEG 640x480
// 如果失败，回退到YUYV

// 4. 缓冲区管理（4个mmap缓冲区）
VIDIOC_REQBUFS(count=4, memory=MMAP)
VIDIOC_QUERYBUF + mmap() × 4
VIDIOC_QBUF × 4  // 入队

// 5. 启动流
VIDIOC_STREAMON
```

**UVC Quirk处理：** 注释中记录了一个已知问题——某HIK 1080P摄像头在 `VIDIOC_S_FMT` 时声称支持YUYV格式，但实际硬件只输出MJPEG。解决方案是显式请求MJPEG格式，确保像素格式元数据准确。

**摄像头问题排查工具链（实战经验）：**
```
1. v4l2-ctl --list-devices          # 列出所有V4L2设备
2. v4l2-ctl -d /dev/video0 --all    # 查看设备能力、支持格式
3. v4l2-ctl --list-formats-ext      # 详细格式枚举（分辨率+帧率）
4. ffplay /dev/video0               # 快速验证采集是否正常
5. v4l2-compliance -d /dev/video0   # V4L2协议兼容性检查
6. dmesg | grep uvcvideo            # 内核UVC驱动日志
7. cat /sys/kernel/debug/usb/devices # USB设备树
```

**常见V4L2故障与解决方法：**

| 现象 | 可能原因 | 排查方法 |
|------|---------|---------|
| `VIDIOC_DQBUF` timeout | 摄像头已拔出、USB带宽不足 | `lsusb` 确认设备存在，检查USB hub带宽 |
| `VIDIOC_REQBUFS` 返回 -ENOMEM | 系统内存碎片化严重 | `cat /proc/buddyinfo` 检查内存碎片 |
| MJPEG帧解码失败 | 帧不完整（USB丢包） | 检查JPEG SOI(0xFFD8)/EOI(0xFFD9) marker |
| 帧率低于预期 | USB等时传输带宽不足 | 降低分辨率或帧率重试 |
| 不同摄像头行为不一致 | UVC固件实现差异 | 建立quirks表，逐设备适配 |

### 5.5 捕获循环

```c
// 阻塞式出队
VIDIOC_DQBUF → 拷贝帧数据 → malloc返回给调用者
VIDIOC_QBUF   // 重新入队
```

使用 `mmap` 而非 `read()`：
- 避免内核态到用户态的数据拷贝
- 适合高帧率场景

---

## 6. 移动端 Web 控制服务

### 6.1 架构：HTTP→JSON-RPC桥接

```
┌─────────────────┐     HTTP      ┌──────────────────┐    JSON-RPC    ┌──────────────┐
│ 手机浏览器       │──────────────→│  Web Server       │──────────────→│ Backend      │
│ (SPA 单页应用)   │←──────────────│  Port 8080        │←──────────────│ Servers      │
└─────────────────┘     JSON      └──────────────────┘    TCP         └──────────────┘
```

Web Server的角色是**HTTP到JSON-RPC的协议转换器**。移动端发HTTP请求，Web Server翻译为JSON-RPC调用后端服务，将结果格式化为HTTP JSON响应。

### 6.2 HTTP服务器实现

**代码位置：** [backend/web_server/http_server.c](backend/web_server/http_server.c)

核心技术选型：
- **纯C实现，零外部依赖**（除cJSON外）
- **`select()` I/O多路复用**：单线程处理多个连接
- **路由注册机制：** `http_route("METHOD /path", handler)`
- **支持前缀匹配：** `"GET /api/*"` 匹配所有 `/api/` 下的路径

### 6.3 API接口一览

**代码位置：** [backend/web_server/main.c:386-397](backend/web_server/main.c#L386-L397)

| 方法 | 路径 | 功能 | 请求参数 |
|------|------|------|----------|
| GET | `/` | 返回移动端SPA页面 | - |
| GET | `/api/status` | 获取全部传感器+设备状态 | - |
| POST | `/api/led` | 控制LED开关 | `{"index":0..5,"on":true/false}` |
| POST | `/api/curtain` | 控制舵机角度 | `{"angle":0..180}` |
| POST | `/api/voice` | 触发语音监听 | `{"action":"listen"}` |
| GET | `/camera/snapshot` | 获取单帧JPEG | - |

### 6.4 内嵌SPA设计

**代码位置：** [backend/web_server/main.c:36-163](backend/web_server/main.c#L36-L163)

整个手机控制页面约125行HTML+CSS+JS，编译为C字符串常量 `INDEX_HTML`，直接嵌入可执行文件：

```c
static const char *INDEX_HTML =
    "<!DOCTYPE html><html lang=\"zh-CN\">"
    // ... 完整的单页应用
    "</html>";
```

设计亮点：
- **零文件系统依赖**：不需要部署静态文件
- **响应式布局**：`@media(min-width:768px)` 适配手机/平板
- **iOS Web App支持**：`apple-mobile-web-app-capable` 元标签
- **3秒轮询更新**：`setInterval(loadStatus, 3000)` 获取最新传感器数据
- **暗色主题**：`#0a0e27` 背景 + `#16213e` 卡片 + 紫青渐变

### 6.5 摄像头快照Base64传输

**代码位置：** [backend/web_server/main.c:314-369](backend/web_server/main.c#L314-L369)

由于JSON-RPC不适合传输二进制数据，摄像头单帧通过base64编码在RPC响应中传输：

```
Camera Server                  Web Server                  Browser
     │                             │                          │
     │ JPEG帧 → base64编码          │                          │
     │ ──RPC响应{data:"/9j/4A..."}─→│                          │
     │                             │ base64解码 → 二进制JPEG    │
     │                             │ ──HTTP Content-Type:     │
     │                             │   image/jpeg─────────────→│
```

### 6.6 WebRTC 远程实时视频（新增）

**代码位置：** [backend/webrtc_server/](backend/webrtc_server/) | [backend/camera_server/camera_relay.c](backend/camera_server/camera_relay.c) | [inc/webrtc_protocol.h](inc/webrtc_protocol.h)

在原有的摄像头快照（base64 单帧）基础上，新增了基于 WebRTC 的**低延迟实时视频流**功能。

```
UVC Camera
    │ V4L2 DQBUF (MJPEG 640x480 @ 30fps)
    ▼
camera_streamer 线程
    │
    ├──→ 共享内存 (/camera_shm) ──→ GUI LVGL 预览（不变）
    │
    └──→ camera_relay (TCP :8081)     ← 帧中继：4字节长度前缀 + JPEG
              │
              ▼
         webrtc_server (C++17)         ← 新进程，libdatachannel
              │ Data Channel (binary)
              ▼
         手机浏览器 WebRTC
         (createImageBitmap → <canvas>)
```

**信令通道：**
```
Browser ←── HTTP POST /api/webrtc/* ──→ web_server ←── TCP :8082 ──→ webrtc_server
```

**关键技术点：**

1. **MJPEG 直通（无 H.264 编码器）**：摄像头输出 MJPEG → Data Channel 直传二进制 JPEG，浏览器端 `createImageBitmap()` GPU 解码。避免了嵌入式平台上 H.264 软件编码的高 CPU 开销。

2. **camera_relay 帧中继**：在 `camera_streamer` 线程中，帧写入共享内存后立即通过 TCP 广播给 webrtc_server。非阻塞 send（`MSG_DONTWAIT`），慢客户端自动丢帧/断开，不影响主预览管线。

3. **libdatachannel**：轻量级 C++17 WebRTC 库（~200KB），内置 ICE/libjuice，无需 Google libwebrtc 的重型依赖。

4. **条件编译**：libdatachannel 不可用时自动跳过 webrtc_server 构建，Web 端回退到 `/camera/snapshot` 快照模式。

**信令 API 接口：**

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/api/webrtc/offer` | 浏览器发送 SDP offer，返回 SDP answer |
| POST | `/api/webrtc/ice` | 浏览器发送 ICE candidate |

**二进制帧中继协议（TCP :8081）：**
```
[4 bytes: frame_size (uint32_t, big-endian)]
[frame_size bytes: JPEG data]
```

**依赖：**
| 依赖 | 用途 | 来源 |
|------|------|------|
| libdatachannel (v0.22.x) | WebRTC PeerConnection + DataChannel | 源码编译 → `libs/libdatachannel/` |
| rtc headers | C++ API 头文件 | `libs/rtc/`（37个头文件） |
| OpenSSL | TLS/DTLS 加密 | 系统包 `libssl-dev` |

**目录结构（新增文件）：**
```
inc/webrtc_protocol.h                     # 共享协议常量
backend/camera_server/camera_relay.{c,h}   # 帧中继 TCP 服务器
backend/webrtc_server/
├── main.cpp                               # 入口：组装所有组件
├── frame_source.{h,cpp}                   # 从 relay 读取 MJPEG 帧
├── peer_manager.{h,cpp}                   # libdatachannel PeerConnection 封装
└── signaling_server.{h,cpp}               # 信令 TCP 服务器 (JSON行协议)
libs/rtc/                                  # libdatachannel C++ 头文件
libs/libdatachannel/                       # libdatachannel.so (ARM32)
```

---

## 7. 驱动层详解与昇腾310B移植

### 7.1 驱动总览

**目录：** [driver/](driver/)

| 驱动 | 硬件 | 协议 | 设备节点 | 数据接口 |
|------|------|------|----------|----------|
| `ap3216c` | 光感/接近/红外传感器 | I2C (0x1e) | `/dev/ap3216` | read 6字节 → IR,光感,距离 |
| `dht11` | 温湿度传感器 | OneWire GPIO | `/dev/dht11` | read 5字节 → 湿度,温度,校验 |
| `gt9147` | 5点电容触摸屏 | I2C + GPIO中断 | `/dev/input/eventX` | Linux input子系统(ABS_MT) |
| `led_beep_jdq` | LED/蜂鸣器/继电器 | GPIO输出 | `/dev/<name>` | read/write 1字节 |
| `sg90` | SG90舵机 | PWM | `/dev/sg90` | write 1字节(角度0-180) |
| `sr501` | PIR人体红外 | GPIO中断 | `/dev/sr501` | read 1字节 + SIGIO异步通知 |
| `hc06_uart` | HC06蓝牙 | UART串口 | `/dev/ttySACx` | termios用户态读写 |

### 7.2 驱动设计模式分析

#### 字符设备驱动模板（以AP3216C为例）

**代码位置：** [driver/ap3216c/ap3216_i2c_driver.c](driver/ap3216c/ap3216_i2c_driver.c)

```c
// 标准Linux字符设备驱动结构
static const struct file_operations ap3216c_fops = {
    .owner   = THIS_MODULE,
    .open    = ap3216c_open,     // 打开设备时软复位+使能芯片
    .read    = ap3216c_read,     // I2C读取6字节→copy_to_user
    .release = ap3216c_release,
};

// probe时：注册misc设备 + 创建设备节点
probe() {
    i2c_check_functionality(I2C_FUNC_SMBUS_WORD_DATA);
    misc_register(&ap3216c_miscdev);
    device_create(ap3216_class, ..., "/dev/ap3216");
}
```

#### 中断+异步通知驱动（以SR501为例）

**代码位置：** [driver/sr501/sr501_drv_dtb.c](driver/sr501/sr501_drv_dtb.c)

SR501采用**GPIO双边沿中断 + 定时器消抖 + 异步通知**的组合模式：

```c
// 中断处理：仅重置定时器
sr501_handler() { mod_timer(&sr501_timer, jiffies + HZ/50); }

// 定时器回调：读取GPIO电平，发送SIGIO
timeout_handler() {
    int val = gpio_get_value(sr501_gpio);
    kill_fasync(&sr501_fasync_queue, SIGIO, POLL_IN);  // 异步通知用户态
}

// 用户态通过fcntl(F_SETFL, O_ASYNC)启用异步通知
```

#### PWM舵机驱动（以SG90为例）

**代码位置：** [driver/sg90/sg90_drv.c](driver/sg90/sg90_drv.c)

角度→脉宽转换公式：

```
pulse_ns = 500000 + angle * 100000 / 9
// 0°   → 500us  (0.5ms)
// 90°  → 1500us (1.5ms)
// 180° → 2500us (2.5ms)
```

周期固定为20ms（50Hz标准舵机频率）。

### 7.3 驱动抽象层设计（适配昇腾310B）

**代码位置：** [backend/device_server/dev_led.c](backend/device_server/dev_led.c) 等文件

每个硬件设备对应一个抽象层，使用 `#ifdef SIMULATOR_LINUX` 实现双平台：

```c
// 模式 1：模拟器模式（x86开发调试）
#ifdef SIMULATOR_LINUX
int led_set(int index, int on) {
    LOG_STUB("led_set(%d, %d)", index, on);  // 仅打印日志
    if (index >= 0 && index < LED_COUNT) {
        g_led_state[index] = on;             // 维护内存状态
    }
    return 0;
}

// 模式 2：真实ARM目标
#else
int led_set(int index, int on) {
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/led_%d", index);
    int fd = open(dev, O_WRONLY);
    write(fd, &on, 1);                       // 直接控制GPIO
    close(fd);
    return 0;
}
#endif
```

这种抽象层的优势：
- **PC开发调试无需硬件**，所有功能通过stub模拟
- **接口完全一致**，上层代码无需任何改动
- **一键切换**，只需CMake参数 `-DSIMULATOR_LINUX=OFF`

### 7.4 昇腾310B交叉编译迁移方案

**SDK位置：** `Ascend310B-sdk/`

昇腾310B的交叉编译需要以下适配：

#### 1. 工具链配置

```cmake
# toolchain.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER Ascend310B-sdk/toolchain/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER Ascend310B-sdk/toolchain/bin/aarch64-linux-gnu-g++)
set(CMAKE_SYSROOT Ascend310B-sdk/toolchain/sysroot)
```

#### 2. 驱动内核源码适配

昇腾310B使用不同的内核版本和硬件平台，驱动的设备树匹配字符串需要适配：

```c
// 原：NXP i.MX平台
static const struct of_device_id ap3216c_of_match[] = {
    { .compatible = "alientek,ap3216c" },  // 需要改为昇腾310B的设备树节点
    { }
};
```

#### 3. GPIO/I2C/PWM子系统差异

昇腾310B使用Ascend AI处理器 + 华为自研外设控制器，GPIO/I2C的编号体系可能不同：

| 原平台 (i.MX) | 昇腾310B等效 | 说明 |
|---------------|-------------|------|
| `/dev/led_*` | 需重新映射GPIO编号 | GPIO引脚编号体系不同 |
| `/dev/dht11` | OneWire GPIO时序要求高，需验证中断延迟 | 昇腾310B中断响应时间需实测 |
| `/sys/class/pwm/pwmchip0/` | 需确认PWM控制器驱动存在 | 不是所有昇腾方案都有硬件PWM |

#### 4. 摄像头适配

昇腾310B有专用的ISP和视频输入接口：
- USB UVC摄像头：与x86平台兼容，`/dev/video*` 直接可用
- MIPI CSI摄像头：需要使用昇腾的Media Controller API，与V4L2标准API不同

#### 5. 显示适配

昇腾310B方案的显示输出可能通过HDMI或MIPI DSI：
- DRM后端：`/dev/dri/card0` 通常可用
- FBDEV后端：`/dev/fb0` 作为备用方案
- HDMI音频：可能需要额外的ALSA配置

### 7.5 驱动编译结构规划

```
driver/
├── ap3216c/       → ap3216c_drv.ko
├── dht11/         → dht11_drv.ko
├── gt9147/        → gt9147_drv.ko
├── led_beep_jdq/  → led_drv.ko
├── sg90/          → sg90_drv.ko
├── sr501/         → sr501_drv.ko
├── hc06_uart/     → (用户态程序，无需编译为ko)
└── Makefile       → 顶层交叉编译Makefile
```

顶层Makefile使用昇腾SDK工具链：
```makefile
KDIR := /path/to/Ascend310B/kernel/source
CROSS_COMPILE := $(SDK_PATH)/toolchain/bin/aarch64-linux-gnu-
obj-m += ap3216c/ dht11/ gt9147/ led_beep_jdq/ sg90/ sr501/

all:
    make -C $(KDIR) M=$(PWD) modules ARCH=arm64
```

---

## 8. 快速上手指南

### 8.1 目录结构

```
smart_screen/
├── inc/                      # 共享头文件
│   ├── common.h              #   平台检测、LED数量、摄像头默认参数
│   ├── rpc_protocol.h        #   RPC端口号、方法名、错误码
│   ├── camera_shm.h          #   共享内存布局、信号量名称
│   └── webrtc_protocol.h     #   WebRTC中继/信令端口与帧协议
├── gui/                      # LVGL前端
│   ├── main.c                #   入口：显示初始化 + 事件循环
│   ├── lv_conf.h             #   LVGL配置
│   ├── camera_reader.c/h     #   共享内存JPEG帧消费者
│   ├── xiaozhi_ipc.c/h       #   UDP IPC（端口5679）
│   ├── rpc_client.c/h        #   JSON-RPC TCP客户端库
│   └── ui/                   #   7个UI页面
│       ├── ui_main.c/h       #   主屏（时钟、天气、快捷入口）
│       ├── ui_device.c/h     #   设备控制
│       ├── ui_camera.c       #   摄像头预览
│       ├── ui_album.c        #   相册
│       ├── ui_voice.c        #   AI语音
│       ├── ui_settings.c     #   设置
│       └── ui_scenes.c       #   场景联动
├── backend/                  # 后端微服务
│   ├── device_server/        #   设备服务 (:1234)
│   │   ├── dev_led.c/h       #   LED GPIO抽象
│   │   ├── dev_dht11.c/h     #   DHT11温湿度抽象
│   │   ├── dev_sg90.c/h      #   SG90舵机抽象
│   │   ├── dev_ap3216c.c/h   #   AP3216C光感抽象
│   │   └── rpc_server.c      #   JSON-RPC服务器入口
│   ├── camera_server/        #   摄像头服务 (:1235)
│   │   ├── v4l2_capture.c/h  #   V4L2采集引擎
│   │   ├── camera_streamer.c/h # 共享内存生产者
│   │   ├── camera_relay.c/h   #   WebRTC帧中继 (TCP :8081)
│   │   └── rpc_server.c      #   JSON-RPC服务器入口
│   ├── voice_server/         #   语音服务 (:1236)
│   │   ├── xiaozhi_bridge.c/h #  UDP→RPC桥接
│   │   ├── rpc_server.c      #   JSON-RPC服务器入口
│   │   └── xiaozhi/          #   小智C++源码
│   ├── web_server/           #   Web服务 (:8080)
│   │   ├── main.c            #   路由处理 + 内嵌SPA
│   │   └── http_server.c/h   #   HTTP/1.1引擎
│   └── webrtc_server/        #   WebRTC服务 (C++17, :8081/:8082)
│       ├── main.cpp          #   入口：组装所有组件
│       ├── frame_source.cpp  #   MJPEG帧读取
│       ├── peer_manager.cpp  #   PeerConnection封装
│       └── signaling_server.cpp # 信令TCP服务器
├── driver/                   # 内核驱动
│   ├── ap3216c/              #   光感驱动
│   ├── dht11/                #   温湿度驱动
│   ├── gt9147/               #   触摸屏驱动
│   ├── led_beep_jdq/         #   LED/蜂鸣器/继电器
│   ├── sg90/                 #   舵机驱动
│   ├── sr501/                #   人体红外驱动
│   └── hc06_uart/            #   蓝牙串口(用户态)
├── libs/                     # 第三方库(git submodule)
│   ├── lvgl/                 #   LVGL 9.2.2
│   ├── cjson/                #   超轻量JSON解析
│   ├── jsonrpc/              #   JSON-RPC C实现(libev)
│   ├── rtc/                  #   libdatachannel C++ 头文件 (v0.22.x)
│   ├── libdatachannel/       #   libdatachannel.so (ARM交叉编译)
│   └── MQTT/                 #   MQTT客户端
├── Ascend310B-sdk/           # 昇腾SDK（不纳入版本管理）
│   ├── toolchain/            #   交叉编译工具链
│   ├── hdmi_sample/          #   HDMI输出示例
│   └── audio_sample/         #   音频示例
├── docs/                     # 文档
├── build/                    # 构建输出（不纳入版本管理）
├── photos/                   # 照片存储
├── CMakeLists.txt            # 顶层CMake
├── build.sh                  # x86模拟器构建脚本
└── run.sh                    # 启动脚本（依次启动7个服务）
```

### 8.2 构建与运行

#### x86模拟器开发模式

```bash
# 构建
./build.sh clean

# 手动构建（需要先安装依赖）
sudo apt install libsdl2-dev libfreetype-dev libjpeg-dev cmake
cmake -B build -DSIMULATOR_LINUX=ON -DCMAKE_BUILD_TYPE=Release
make -C build -j$(nproc)

# 运行（自动启动全部服务 + GUI）
./run.sh
```

#### ARM昇腾310B生产模式

```bash
# 交叉编译
cmake -B build_arm \
    -DSIMULATOR_LINUX=OFF \
    -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
make -C build_arm -j$(nproc)

# 编译驱动模块
cd driver
make CROSS_COMPILE=<ascend-toolchain-prefix>
```

### 8.3 核心代码阅读路径

对于首次接触项目的开发者，建议按以下顺序阅读：

```
1. inc/common.h          — 理解平台检测和全局宏定义
2. inc/rpc_protocol.h    — 理解IPC协议（端口、方法名）
3. inc/camera_shm.h      — 理解视频共享内存协议
4. gui/main.c            — 理解GUI启动流程
5. gui/lv_conf.h         — 理解LVGL配置
6. gui/camera_reader.c   — 理解消费者端帧读取
7. gui/ui/ui_camera.c    — 理解摄像头渲染管线（重点）
8. backend/camera_server/v4l2_capture.c    — 理解V4L2采集
9. backend/camera_server/camera_streamer.c — 理解共享内存生产者
10. backend/device_server/ — 理解硬件抽象层
11. gui/ui/ui_voice.c      — 理解AI语音UI
12. backend/voice_server/  — 理解语音桥接层
13. backend/web_server/    — 理解移动端Web控制
```

---

## 9. 面试真题库：嵌入式Linux应用与驱动

> 以下面试题综合了 2024-2026 年大厂嵌入式面试真题、CSDN/牛客网面经，以及本项目涉及的核心技术领域。每道题提供参考答案要点，面试时可据此展开。

---

### 9.1 项目相关面试题

**Q1: 为什么选择多进程而不是多线程？**

A: 本项目后端服务使用独立进程，原因：
1. **故障隔离**：一个服务崩溃不影响其他（摄像头拔插不会影响设备控制）
2. **独立部署**：可选择启动服务（小智AI可选）
3. **独立地址空间**：避免野指针/内存泄漏互相影响
4. **调试友好**：可单独 gdb、strace 每个进程
5. **自然语言边界**：C 服务 + C++ 服务（XiaoZhi/WebRTC），无需混编

代价是 IPC 开销（JSON-RPC/TCP），但对控制面毫秒级延迟可接受。视频数据走共享内存绕过 IPC 开销。

**Q2: 为什么用 JSON-RPC 而不是 gRPC/Protobuf/DBus？**

A:
1. **依赖最小化**：cJSON + libev 共几千行；gRPC 需 protobuf 编译器、HTTP/2，嵌入式太重
2. **人类可读**：`nc localhost 1234` 即可调试
3. **跨语言**：Web 端 JS 的 `fetch()` + `.json()` 天然支持
4. **够用**：控制面 QPS < 10，JSON 序列化开销可忽略

**Q3: 控制面和数据面分离的设计哲学？**

A:
- 控制面：低频（几KB/s）、小数据（几十字节）、要求可靠 → JSON-RPC/TCP
- 数据面：高频（30fps×50KB=1.5MB/s）、大数据（MJPEG帧）、实时优先 → POSIX 共享内存

video 帧走 RPC→base64 编码+TCP 发送→编码开销+协议栈开销严重降帧率。分离后零拷贝达 GUI。

**Q4: 画面撕裂的三层防护设计？**

A:
1. **Ping-Pong 双缓冲**（应用层）：缩放结果始终写"另一"buffer → `lv_image_set_src()` 原子切换 → `lv_obj_invalidate()`
2. **SPSC 三槽位环形缓冲**（IPC层）：3槽位无锁安全，`sem_trywait()` 非阻塞读取不卡UI
3. **双 `lv_obj_invalidate()`**（显示层）：图像控件 + 父容器同时无效化

**Q5: 为什么 LVGL 图像描述符尺寸 = 显示区域尺寸？**

A: 如果 img desc 尺寸 ≠ 显示区域 → LVGL 自动缩放（模糊/黑边）。精确匹配避免 LVGL 二次处理，保证像素级精度。

**Q6: V4L2 mmap 零拷贝是如何做到的？**

A: `VIDIOC_QUERYBUF` 获取内核缓冲区物理地址 → `mmap()` 映射到用户空间虚拟地址 → 用户直接读写内核页缓存，无需 `copy_to_user`/`copy_from_user`。代价是需手动管理 QBUF/DQBUF 生命周期。

**Q7: 3 槽位 SPSC 优于 2 槽位的原因？**

A: 2 槽位：生产者写 slot[0] 时消费者可能正在读 slot[0] → 冲突。3 槽位：最多 1 写 + 1 读 + 1 空闲，配合 `sem_empty`(3) / `sem_full`(0) 计数，永不冲突。

**Q8: 为什么语音走 UDP 而不是 TCP？**

A: 实时性优先（音频延迟敏感）、有损可接受（Opus 容错）、无需连接管理、localhost 丢包率极低。

**Q9: 自研 HTTP 服务器的设计取舍？**

A: 极简需求（5路由+1 SPA）、依赖最小化（300行C）、完全可控（内存/超时）、学习价值（`select()` 多路复用）。不适用于高并发场景。

**Q10: SIMULATOR_LINUX 宏的双平台策略？**

A: 单宏控所有平台差异：ON → x86 SDL2 + 硬件 stub；OFF → ARM DRM/FBDEV + 真硬件。`#ifdef` 分支仅出现在驱动抽象层（`dev_led.c` 等），上层代码接口一致。

**Q11: UVC 摄像头 quirk 如何处理？**

A: HIK 1080P 相机 `VIDIOC_S_FMT` 报告 YUYV 但实际输出 MJPEG。方案：显式设 MJPEG 格式 → 信任实际数据而非元数据 → 多设备回退链（by-path → `/dev/video0..5`）→ 格式回退（MJPEG → YUYV）。

**Q12: 为什么 WebRTC 用 Data Channel 而非 Video Track？**

A: 摄像头输出 MJPEG（JPEG帧），浏览器原生 video track 需 H.264/VP8。不用 data channel 则需 MJPEG→YUV→H.264 软件编码，ARM 上 CPU 开销高。data channel 直传 MJPEG + 浏览器 `createImageBitmap()` GPU 解码，零编码开销。

**Q13: DHT11 OneWire 时序在 ARM 上如何保证？**

A: `local_irq_save()` 关本地中断入临界区保证 bit-banging 时序。备选方案（注释掉）：GPIO 边沿中断 + 内核定时器，因 ARM 中断延迟波动被放弃。昇腾310B 需重新评估中断延迟。

**Q14: 驱动从 i.MX 到昇騰310B 的移植要点？**

A: 内核版本适配、GPIO/I2C/PWM 编号重映射、设备树 compatible 匹配、中断延迟验证（DHT11/SR501）、显示后端（i.MX DRM → 昇騰 DRM）、摄像头接口（USB UVC → 可能 MIPI CSI+Media Controller）。

**Q15: camera_relay 帧中继为什么用非阻塞 send？**

A: `MSG_DONTWAIT` 非阻塞写：慢客户端自动丢帧（EWOULDBLOCK/EAGAIN → 跳过当前帧）。超3秒断连。保证生产者线程永不阻塞，GUI 30fps 预览不受影响。

**Q16: 这套 IPC 架构有什么局限性？**

A:
- JSON-RPC 不适合大二进制传输（视频帧需 base64 → 33% 开销）
- `select()` 单线程 HTTP 服务器不支持高并发
- 共享内存 SPSC 仅单消费者（GUI），WebRTC 需独立 relay 扩展
- 无服务发现/健康检查机制（需手动管理进程生命周期）

---

### 9.2 V4L2 视频驱动面试题

**Q17: 描述 V4L2 应用层标准采集流程。**

A: `open()` → `VIDIOC_QUERYCAP`（能力）→ `VIDIOC_S_FMT`（格式）→ `VIDIOC_REQBUFS`（申请缓冲区）→ `VIDIOC_QUERYBUF` + `mmap()` × N → `VIDIOC_QBUF` × N（入队）→ `VIDIOC_STREAMON` → 循环 `VIDIOC_DQBUF`（出队）→ 处理数据 → `VIDIOC_QBUF`（重新入队）→ `VIDIOC_STREAMOFF` → `munmap` → `close()`。

**Q18: V4L2 框架的核心层级有哪些？**

A:
- `video_device`：暴露给用户空间的 `/dev/videoX` 节点
- `v4l2_device`：整个 V4L2 设备的管理实体，管理所有 subdev
- `v4l2_subdev`：子设备抽象（sensor、CSI 接收器、ISP），每个 subdev 有独立的 `v4l2_subdev_ops`
- `media_device` / Media Controller：描述 entity（节点）、pad（端口）、link（连接）的拓扑关系
- `videobuf2`（VB2）：缓冲区管理框架，支持 mmap/userptr/DMABUF 三种模式

**Q19: V4L2 的 mmap 实现原理？零拷贝如何实现？**

A: 驱动层分配物理连续内存 → `vmalloc`/`dma_alloc_coherent` → `remap_pfn_range`（或 `vm_insert_page`）在 `mmap` 回调中建页表映射到用户空间 → 用户指针直指内核物理页。无 `copy_to_user`/`copy_from_user`。

**Q20: 像素格式 V4L2_PIX_FMT_MJPEG vs YUYV vs NV12 的区别？**

A:
- **MJPEG**：帧内 JPEG 压缩，每帧独立，带宽低（~50KB @ 640x480），解码开销
- **YUYV**：YUV 4:2:2 打包格式，每 2 像素 4 字节，未压缩，带宽高（614KB）
- **NV12**：YUV 4:2:0 半平面格式，Y 全分辨率 + UV 交错半分辨率，硬件编解码器常用

**Q21: 摄像头无数据/黑屏如何排查？**

A:
1. `v4l2-ctl --list-devices` → 确认设备存在
2. `v4l2-ctl -d /dev/videoX --all` → 检查当前格式/分辨率
3. `ffplay /dev/videoX` → 快速验证采集
4. I2C 通信检查：`i2cdetect -y <bus>` 确认 sensor 地址
5. MIPI 信号检查（示波器/逻辑分析仪）
6. 上电时序：sensor 的 `powerdown`/`reset` 引脚需按 datasheet 时序
7. `dmesg | grep -i video` → 内核驱动日志

**Q22: 为一个新 sensor 编写 V4L2 驱动需要实现哪些关键结构？**

A:
- `v4l2_subdev_ops`：实现 `s_stream`、`s_fmt`、`g_fmt` 等
- `v4l2_ctrl_ops`：曝光/增益/白平衡等控制
- `media_entity_operations`：注册到 media 拓扑
- 设备树中描述 sensor 节点（`compatible`、I2C 地址、时钟、reset GPIO 等）
- probe 中：I2C 探测 → `v4l2_subdev_init` → 注册到 `v4l2_device` → 创建 media entity

---

### 9.3 设备树（Device Tree）与驱动模型

**Q23: 设备树是什么？为什么要用设备树？**

A: 设备树（.dts/.dtsi → .dtb）是硬件描述数据结构。**本质：硬件描述与驱动代码解耦**。没有设备树时，板级信息硬编码在内核中（`arch/arm/mach-xxx/board-xxx.c`），每块板需要重新编译内核。有设备树后，同内核 + 不同 dtb 支持不同硬件。

**Q24: `compatible` 属性的作用？驱动如何匹配设备树节点？**

A: `compatible` 是设备树节点的"型号标识"字符串，如 `compatible = "alientek,ap3216c"`。驱动通过 `struct of_device_id` 声明匹配表，内核在 probe 时遍历匹配：
```
platform_match():
  1. driver_override 强制匹配
  2. OF match: of_match_table 比较 compatible
  3. ACPI match (x86)
  4. id_table 按名称匹配
  5. driver->name 与 device->name 直接比较
```

**Q25: 驱动中如何解析设备树属性？**

A:
```c
of_property_read_u32(np, "reg", &value);     // 读 u32
of_property_read_string(np, "label", &str);   // 读字符串
of_get_named_gpio(np, "reset-gpios", 0);      // 读 GPIO
of_irq_get(np, 0);                             // 读中断号
of_address_to_resource(np, 0, &res);          // 读内存资源
of_get_child_count(np);                        // 子节点数量
```

**Q26: 设备树不生效如何调试？**

A:
1. `dtc -I dtb -O dts xxx.dtb` → 反编译检查内容
2. `cat /sys/firmware/devicetree/base/` → 运行时设备树
3. 驱动 probe 加 `dev_info(&pdev->dev, "compatible matched\n")`
4. 检查 `compatible` 字符串是否完全一致（含逗号后空格）
5. status = "okay" 而非 "disabled"

**Q27: platform_driver 的 probe 函数一般做什么？**

A:
1. 从设备树获取资源（GPIO、IRQ、内存映射地址）
2. `devm_kzalloc` 申请私有数据结构
3. 初始化硬件：GPIO 方向、I2C 配置、时钟使能等
4. 注册字符设备：`alloc_chrdev_region` → `cdev_init` → `cdev_add`
5. 创建设备节点：`class_create` → `device_create`
6. 注册中断处理函数（如有）
7. 将私有数据存入 `platform_set_drvdata`

---

### 9.4 字符设备驱动

**Q28: 字符设备注册完整流程。**

A:
```c
// 1. 申请设备号
alloc_chrdev_region(&dev, 0, 1, "mydev");  // 或 register_chrdev_region
// 2. 初始化 cdev
cdev_init(&my_cdev, &my_fops);
// 3. 注册到内核
cdev_add(&my_cdev, dev, 1);
// 4. 创建设备节点
cls = class_create("myclass");
device_create(cls, NULL, dev, NULL, "mydev%d", 0);
```
旧接口 `register_chrdev()` 一次占 256 个次设备号，新接口按需分配。

**Q29: file_operations 中关键接口的作用？**

A:
| 接口 | 作用 | 注意事项 |
|------|------|---------|
| `open` | 设备初始化、私有数据分配 | 返回值 0 或 -errno |
| `read` | 数据从内核→用户 | 必须用 `copy_to_user` |
| `write` | 数据从用户→内核 | 必须用 `copy_from_user` |
| `ioctl`/`unlocked_ioctl` | 设备自定义控制 | 新内核用 `unlocked_ioctl` |
| `mmap` | 内存映射 | `remap_pfn_range` 建映射 |
| `poll` | 多路复用支持 | `poll_wait` 注册等待队列 |
| `release` | 关闭设备时的清理 | 与 `open` 配对 |

**Q30: `copy_to_user` / `copy_from_user` 为什么不能用 `memcpy` 替代？**

A: 用户态指针可能是非法地址、未映射、或被换出到 swap。`copy_to_user` 在异常表中注册了地址 → 如果缺页/权限错误，会返回 `-EFAULT` 而非 kernel panic。`memcpy` 在内核态直接解引用用户态指针 → 安全漏洞 + 潜在内核崩溃。

**Q31: 阻塞 I/O vs 非阻塞 I/O 在驱动中如何区分？**

A: 检查 `file->f_flags & O_NONBLOCK`：
- 阻塞模式：资源不可用时 `wait_event_interruptible()` 睡眠直到条件满足
- 非阻塞模式：资源不可用时立即返回 `-EAGAIN`（或 `-EWOULDBLOCK`）

**Q32: 驱动的 `.poll` 实现原理？**

A: `.poll` 不阻塞！它只是：
1. `poll_wait(filp, &wait_queue, poll_table)` → 把当前进程注册到等待队列
2. 返回当前资源状态位掩码（`POLLIN | POLLRDNORM` 或 `POLLOUT | POLLWRNORM`）

真正的阻塞发生在 VFS 层的 `do_poll()` 中，它会调用 `.poll` 检查状态 → 不满足则 `schedule()` 让出 CPU → 被唤醒后重新检查。

---

### 9.5 中断与并发控制

**Q33: Linux 中断上半部和下半部的区别？**

A:
- **上半部（Top Half）**：注册在 `request_irq` 中的 ISR，运行在中断上下文。**不能睡眠**、不能调 `copy_to_user`、不能调 `mutex_lock`。越快越好。
- **下半部（Bottom Half）**：将耗时操作推迟到更安全上下文的机制：
  - **tasklet**：软中断上下文，不能睡眠，同一 tasklet 不会同时在多 CPU 执行
  - **workqueue**：内核线程上下文，**可以睡眠**、可以调 mutex、可以调 `copy_to_user`
  - **threaded IRQ**：`request_threaded_irq()` 将整个 ISR 运行在线程上下文

**Q34: spin_lock 和 mutex 的使用场景分别是什么？**

A:
| 特性 | spin_lock | mutex |
|------|-----------|-------|
| 可睡眠上下文 | ✅ (中断不可用) | ✅ |
| 中断上下文 | ✅ `spin_lock_irqsave` | ❌ 不能在 ISR 中用 |
| 持有期间可睡眠 | ❌ (自旋忙等) | ✅ (睡眠等) |
| 临界区长短 | 极短（<100μs级） | 可较长（ms级） |
| 实现 | 原子操作 + 忙等 | 基于 `atomic` + 等待队列 |

面试记忆口诀：**spin 不睡 mutex 不中断**。

**Q35: 优先级反转是什么？如何解决？**

A: 低优先级任务持锁 → 中优先级任务抢占 CPU → 高优先级任务等锁 → 实际被中优先级的任务阻塞。解决方案：**优先级继承（Priority Inheritance）**——低优先级任务持锁时临时提升到等待锁的最高优先级任务，释放锁后恢复原优先级。Linux 内核的 `rt_mutex` 实现了优先级继承。

**Q36: 原子操作（atomic_t）和 volatile 的区别？**

A:
- `volatile` 是编译器指令：告诉编译器每次从内存读，不优化。不保证原子性（多核上其他核的修改仍然可见性问题），不用来做同步！
- `atomic_t`：利用硬件原子指令（LDREX/STREX on ARM），保证读-修改-写的原子性。有内存屏障语义。
- 正确用法：ISR 和主线程共享的 flag 用 `volatile` + 小心设计；计数器/引用计数用 `atomic_t`。

**Q37: `request_irq` 返回 -EBUSY 是什么原因？**

A: 该中断号已被其他驱动占用且未设置 `IRQF_SHARED`。解决：
1. 检查中断号是否正确（`of_irq_get`）
2. 考虑使用共享中断：加 `IRQF_SHARED` 标志
3. 确保其他驱动也已正确设置 `IRQF_SHARED`

---

### 9.6 内存管理与DMA

**Q38: kmalloc / vmalloc / dma_alloc_coherent 区别？**

A:
| 函数 | 连续性 | 适用场景 |
|------|--------|---------|
| `kmalloc` | 物理连续 | 小内存分配（<128KB），DMA 缓冲区 |
| `vmalloc` | 虚拟连续（物理可能不连续） | 大内存分配（>128KB），不需要 DMA |
| `dma_alloc_coherent` | 物理连续 + 一致性 | DMA 缓冲区（无需手动 flush cache） |

**Q39: ARM 上的 Cache 一致性问题？**

A: CPU 写数据进 cache，DMA 控制器直接从物理内存读 → 数据不一致。解决方案：
- `dma_alloc_coherent`：分配无 cache 或自动维护一致性的内存
- DMA Streaming Mapping：`dma_map_single()` + `dma_unmap_single()` 在传输前后手动 flush/invalidate cache
- 对于 SHM + Cache 场景：MAP_SHARED 的 mmap 区域需配置为 Write-through 或定期 sync

**Q40: 1GB 物理内存能否 malloc(1.2GB)？为什么？**

A: 可以（在某些情况下）。`malloc` 分配的是**虚拟地址空间**，与物理内存无直接关系。Linux 采用**延迟分配（Lazy Allocation）**：`malloc` 时只分配虚拟地址，实际物理页在首次 `memset`/写入时通过缺页中断分配。但如果持续写入超过物理内存 + swap 总量 → OOM Killer。

**Q41: 内存碎片如何检测？**

A:
```bash
cat /proc/buddyinfo          # buddy 分配器各 order 空闲块数量
cat /proc/pagetypeinfo       # 按迁移类型分组的页面信息
echo m > /proc/sysrq-trigger # 输出内存信息到 dmesg
```
`/proc/buddyinfo` 中 order 10（4MB）的块数量 = 0 表示物理内存碎片严重。

---

### 9.7 C++ 嵌入式高频考点

**Q42: RAII 是什么？为什么在嵌入式开发中重要？**

A: Resource Acquisition Is Initialization —— 资源获取即初始化。利用 C++ 对象生命周期（构造/析构）自动管理资源（内存、文件句柄、锁、GPIO 等）。嵌入式通常**禁用异常**（`-fno-exceptions`），RAII 成为唯一可靠的资源管理手段：
```cpp
class GpioGuard {
    int pin_;
public:
    explicit GpioGuard(int pin) : pin_(pin) { gpio_request(pin); }
    ~GpioGuard() { gpio_free(pin_); }
    GpioGuard(const GpioGuard&) = delete;  // RAII 对象通常禁止拷贝
};
```


**Q43: 智能指针类型及使用场景？**

A:
- `std::unique_ptr`：独占所有权，零额外开销，嵌入式首选
- `std::shared_ptr`：共享所有权 + 引用计数（原子操作，有开销），引用计数操作线程安全，但管理对象访问不保证线程安全
- `std::weak_ptr`：不增加引用计数，用于打破 `shared_ptr` 循环引用

**Q44: shared_ptr 循环引用如何解决？**

A: 用 `weak_ptr` 打破循环：父对象持有子对象的 `shared_ptr`，子对象对父对象只持有 `weak_ptr`。`weak_ptr::lock()` 检查对象是否仍存活。典型场景：树/图数据结构、观察者模式中的回调注册。

**Q45: constexpr 在嵌入式中的应用？**

A: 编译期计算，零运行时开销：
```cpp
constexpr uint32_t GPIO_BASE = 0x50000000;
constexpr uint32_t gpio_reg(int port, int offset) {
    return GPIO_BASE + port * 0x1000 + offset;
}
constexpr uint32_t LED_CTL = gpio_reg(2, 0x04);  // 编译时计算，无运行时开销
```
适合寄存器地址、位掩码计算、查找表、CRC表等。

**Q46: new/malloc 的区别？placement new 是什么？**

A:
- `new`：运算符，类型安全，调用构造函数，失败抛 `std::bad_alloc`（或返回 nullptr with `nothrow`）
- `malloc`：C 库函数，返回 `void*`，不调用构造/析构，失败返回 NULL
- Placement new：`new (ptr) T(args)` 在已分配的内存上构造对象，不分配新内存。用于内存池、共享内存上构造 C++ 对象

**Q47: C++ 内存模型中的 memory order？**

A:
- `memory_order_relaxed`：仅保证原子性，无顺序约束（计数器适用）
- `memory_order_acquire`：后续读写不会被重排到此操作之前
- `memory_order_release`：之前的读写不会被重排到此操作之后
- `memory_order_acq_rel`：同时具有 acquire 和 release 语义
- `memory_order_seq_cst`：全局顺序一致性（默认，最强但最慢）

嵌入式常用 `relaxed` 做无锁统计，`acquire-release` 做无锁队列。

---

### 9.8 多线程与同步

**Q48: 线程与进程的区别？嵌入式何时用线程何时用进程？**

A:
- 进程：资源分配单位，独立地址空间，IPC 开销大，隔离性好
- 线程：CPU 调度单位，共享地址空间，通信开销小，需同步保护
- **线程**：需要共享大量数据、频繁通信、低延迟场景
- **进程**：需要强隔离、不同语言/生命周期、独立崩溃域（本项目选择）

**Q49: 死锁的四个必要条件？如何预防？**

A: 必要条件：互斥、持有并等待、不可剥夺、循环等待。打破任一即可。预防方案：
1. 固定加锁顺序（所有线程以相同顺序获取多把锁）
2. `std::lock(m1, m2, m3)` / `std::scoped_lock` 一次获取多锁
3. 尝试锁 `try_lock` + 回退重试
4. 超时锁 `try_lock_for`

**Q50: 条件变量的正确用法？为什么需要 while 而非 if？**

A:
```cpp
std::unique_lock<std::mutex> lk(mtx);
cv.wait(lk, []{ return ready; });  // ← 等价于 while(!ready) cv.wait(lk);
```
用 `if` 而不是 `while` 的问题：**虚假唤醒（spurious wakeup）**——条件变量可能在没有 `notify` 调用的情况下醒来，`while` 可以重新检查条件。

**Q51: std::atomic 能替代 mutex 吗？**

A: 部分场景可以（无锁编程），但不能完全替代：
- 适合：计数器、flag、单生产者单消费者（SPSC/MSPC 无锁队列）
- 不适合：复杂数据结构的原子更新、多变量一致性、需要等待的场景
- 关键限制：`std::atomic` 保护的是单个变量的原子性，不保护代码块的临界区

**Q52: 线程局部存储 thread_local 的使用场景？**

A:
- 每线程独立的日志缓冲区、错误码 `errno`
- 线程特定的上下文数据，避免全局变量加锁
- 注意：`thread_local` 变量初始化在第一次使用时，析构在线程退出时，有构造/析构开销
- FreeRTOS 无 `thread_local` 支持，需用 `vTaskSetThreadLocalStoragePointer()`

---

### 9.9 LVGL 显示与图形面试题

**Q53: LVGL 双缓冲的工作流程？**

A: LVGL 渲染到 back buffer → `flush_cb` 通知渲染完成 → LVGL 进行下一轮渲染 — 同时硬件显示 front buffer → 在 VSYNC 信号触发时交换（或拷贝）buffer → 周而复始。

**Q54: 画面撕裂的根因和解决方案？**

A: 根因：LCD 控制器（读取）与 CPU/GPU（写入）的读写竞争。显示扫描线正在读取上半帧时 CPU 已写入下半帧 → 上下半帧不一致 → 可见水平"撕裂线"。

解决方案优先级：
1. **硬件 TE 信号**：LCD 刷完一帧后产生 TE 脉冲，CPU 在 TE 中断中更新 frame buffer
2. **DRM Page Flip**：`drmModePageFlip()` 在 VSYNC 原子切换 framebuffer 指针
3. **双/三缓冲**：软件层面维护多个 buffer + 仅完整帧后切换
4. **中间 buffer + memcpy**：先渲染到 malloc buffer → 整帧拷贝到显存

**Q55: DRM stride/pitch 是什么？为什么要对齐？**

A: `stride`（也称 pitch）是每行像素数据的实际字节长度，可能大于 `width × bpp/8`。如 280px 宽 ARGB8888 = 1120 字节，可能对齐到 1152 字节（288px 倍数）。因为 GPU/DMA 硬件为优化访问效率要求行数据对齐到 32/64/128 字节边界。LVGL 用 `lv_draw_buf_t` 的 stride 字段处理对齐。

**Q56: LVGL v9 的渲染模式有哪些？各适用什么场景？**

A:
- `DIRECT`：LVGL 直接渲染到 framebuffer，性能最好，需硬件支持双缓冲切换，每渲染一个"块"就 flush 一次
- `FULL`：全屏刷新，简单但性能差，每帧完整重绘所有像素
- `PARTIAL`：仅刷新 dirty area，推荐模式，需提供 screen-size buffer

**Q57: 嵌入式显示 Cache 导致花屏怎么办？**

A: 原因：framebuffer 被 cache 住，CPU 写入未同步到物理内存，LCD 控制器读到的还是旧数据。解决方案：
1. 显存区域的 MPU 属性设为 Write-through 或 Non-cacheable
2. 每次渲染完 `SCB_CleanInvalidateDCache_by_Addr()`（ARMv7-M）
3. 使用 `dma_alloc_coherent` 分配显存（自动维护一致性）
4. 减少 cache 区域影响：仅 framebuffer 所在内存区域 non-cacheable

---

### 9.10 IPC 进程间通信与网络

**Q58: 嵌入式常用 IPC 及适用场景？**

A:
| IPC | 适用场景 | 特点 |
|-----|---------|------|
| 共享内存 (SHM) | 大块数据高频交换（视频帧） | 零拷贝，需额外同步 |
| 消息队列 (MQ) | 异步任务分发 | 有长度限制，SystemV/POSIX 两种 |
| 管道 (pipe/FIFO) | 父子进程/无关进程简单字节流 | 单向，适合命令行管道 |
| Unix Domain Socket | 本地 C/S 架构 | 类似 TCP API，支持 SCM_RIGHTS 传 fd |
| 信号量 (sem) | 资源计数、生产者-消费者 | 内核持久化（named sem），注意 `sem_unlink` |
| 信号 (signal) | 异步事件通知 | SIGIO（异步 I/O）、SIGCHLD（子进程退出） |

**Q59: TCP 三次握手和四次挥手？**

A: 
- 三次握手：SYN → SYN-ACK → ACK。为什么是三次？防止已失效的连接请求到达服务端造成资源浪费。
- 四次挥手：FIN → ACK → FIN → ACK。为什么是四次？TCP 是全双工的，双方各需独立关闭发送通道。TIME_WAIT（2MSL）原因：保证最后的 ACK 到达对端 + 让旧连接的数据包在网络中消失。

**Q60: select/poll/epoll 的区别？**

A:
| 特性 | select | poll | epoll |
|------|--------|------|-------|
| fd 上限 | FD_SETSIZE(1024) | 无限制 | 无限制 |
| 数据结构 | fd_set bitmap | pollfd 数组 | 红黑树 + 就绪链表 |
| 扫描方式 | O(n) 全量扫描 | O(n) 全量扫描 | O(1) 事件驱动 |
| 适用场景 | fd < 100 少量连接 | fd < 1000 | > 1000 高并发 |

本项目用 `select()`：fd 数 ≤ 4（web_server 最大连接少），代码简洁。

**Q61: WebSocket 与 HTTP 的关系？**

A: WebSocket 通过 HTTP Upgrade 机制建立：客户端发 `Upgrade: websocket` → 服务端 `101 Switching Protocols` → TCP 连接升级为全双工 WebSocket。与 HTTP 的区别：
- HTTP：请求-响应，服务器不能主动推送
- WebSocket：全双工，服务器可主动推消息，帧开销仅 2-14 字节

本项目 XiaoZhi 用 WebSocket 连云端 AI（STT/TTS 全双工），移动端 Web 控制用 HTTP（请求-响应够用）。

---

### 9.11 综合场景题

**Q62: 设计一个远程 OTA 固件升级系统。**

A: 关键模块：
1. **版本检查**：设备定期 HTTP GET `/api/version` 获取最新版本号
2. **下载**：HTTP Range 请求分段下载 + MD5/SHA256 校验
3. **双分区**（A/B 分区）：下载到备用分区，不影响当前系统运行
4. **切换**：bootloader 检查分区标志 → 切换到新分区
5. **回退**：启动失败（看门狗超时未更新标志）→ bootloader 回退到旧分区
6. **断点续传**：结合 flash 擦写次数和网络中断风险

**Q63: 用户态 open("/dev/led") → write(fd, &on, 1) 的完整内核调用路径？**

A: 用户态 → `sys_open()` → `get_unused_fd()`（分配 fd） → `do_sys_open()` → VFS `path_openat()` → 遍历路径 → `do_last()` → 调用 `cdev_file_open()` → `f_op->open()` → 返回 fd。然后 `sys_write()` → VFS `vfs_write()` → `f_op->write()` → 驱动中的 `copy_from_user()` → 硬件操作（GPIO 输出）。返回用户态。

**Q64: 系统启动后 /dev 节点未生成，排查思路？**

A:
1. `dmesg | grep <drivername>` → 检查驱动 probe 是否成功
2. `cat /sys/devices/platform/xxx/of_node/compatible` → 检查设备树是否有对应节点且 status="okay"
3. 驱动 init 函数是否被调用？加 `printk` 验证
4. `device_create` 是否被执行？class_create 是否成功？
5. 设备号冲突？`cat /proc/devices` 检查
6. udev/mdev 是否运行？`ps aux | grep udev`

**Q65: 设计一个低功耗的传感器采集系统。**

A:
1. **硬件唤醒链**：传感器 → GPIO 中断唤醒 SoC → I2C/SPI 读取 → 处理 → 发送结果 → 重新睡眠
2. **批量上报**：积累多条数据后一起发送（减少 radio 唤醒次数）
3. **时钟门控**：不用的外设关闭时钟
4. **DRAM 自刷新**：睡眠时 DRAM 进入 self-refresh 模式
5. **Tickless 内核**：`CONFIG_NO_HZ_IDLE` 避免不必要的定时中断唤醒
6. 权衡：深度睡眠（suspend-to-RAM）延迟高（ms 级），浅睡眠（WFI）延迟低（μs 级）

---

> **面试备考建议：**
> 1. 不要死记硬背，围绕"用户态调用 → VFS → 驱动 → 硬件" 的完整数据流路径来组织答案
> 2. 每个知识点至少准备一个本项目中对应的实际例子
> 3. V4L2/设备树/中断/内存管理是嵌入式 Linux 驱动的四大高频考点
> 4. C++ 方面 RAII/智能指针/多线程同步是最常被问到的
> 5. 遇到不会的问题可以坦诚说"这部分没深入"然后引导到自己擅长的领域

---

## 附录：核心技术词汇中英对照

| 中文 | English |
|------|---------|
| 智慧屏 | Smart Screen |
| 多进程微服务 | Multi-process Microservices |
| JSON-RPC | JSON Remote Procedure Call |
| 共享内存环形缓冲 | Shared Memory Ring Buffer |
| SPSC无锁队列 | Single-Producer Single-Consumer Lock-Free Queue |
| Ping-Pong双缓冲 | Ping-Pong Double Buffering |
| 画面撕裂 | Screen Tearing |
| 双线性缩放 | Bilinear Scaling |
| V4L2内存映射 | V4L2 Memory Mapping (mmap) |
| 非阻塞信号量 | Non-blocking Semaphore (sem_trywait) |
| 平台抽象层 | Platform Abstraction Layer (HAL) |
| 单页应用 | Single Page Application (SPA) |
| 响应式布局 | Responsive Layout |
