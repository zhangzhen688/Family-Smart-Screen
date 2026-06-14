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
6. [移动端 Web 控制服务](#6-移动端-web-控制服务)
7. [驱动层详解与昇腾310B移植](#7-驱动层详解与昇腾310b移植)
8. [快速上手指南](#8-快速上手指南)
9. [面试高频问题与答案](#9-面试高频问题与答案)

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
     ┌────────┬───────────────┼──────┘      │
     │        │               │             │
┌────▼──┐ ┌──▼──────┐ ┌──────▼──┐ ┌───────▼──────┐
│Device │ │Voice    │ │Camera   │ │Web Server    │
│Server │ │Server   │ │Server   │ │Port 8080     │
│:1234  │ │:1236    │ │:1235    │ │HTTP→JSON-RPC │
│       │ │         │ │         │ │桥接           │
└──┬──┬─┘ └────┬────┘ └────┬────┘ └──────────────┘
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
│   └── camera_shm.h          #   共享内存布局、信号量名称
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
│   │   └── rpc_server.c      #   JSON-RPC服务器入口
│   ├── voice_server/         #   语音服务 (:1236)
│   │   ├── xiaozhi_bridge.c/h #  UDP→RPC桥接
│   │   ├── rpc_server.c      #   JSON-RPC服务器入口
│   │   └── xiaozhi/          #   小智C++源码
│   └── web_server/           #   Web服务 (:8080)
│       ├── main.c            #   路由处理 + 内嵌SPA
│       └── http_server.c/h   #   HTTP/1.1引擎
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

## 9. 面试高频问题与答案

### 架构与设计

**Q1: 为什么选择多进程而不是多线程？**

A: 我们的后端服务（设备、摄像头、语音）使用独立进程而非线程，主要考虑：
1. **故障隔离**：一个服务的崩溃不会拖垮整个系统（比如摄像头拔插不会影响设备控制）
2. **独立部署**：可以选择性启动服务（小智AI可选）
3. **自然边界**：每个服务有独立的地址空间，避免野指针/内存泄漏互相影响
4. **调试友好**：可以单独gdb某个进程，strace追踪系统调用

当然代价是IPC开销（JSON-RPC/TCP），但对于控制面操作（毫秒级延迟可接受）这不是瓶颈。视频数据走共享内存绕过了IPC开销。

**Q2: 为什么用JSON-RPC而不是gRPC/Protobuf/DBus？**

A:
1. **依赖最小化**：JSON-RPC只需要cJSON + libev，总共几千行代码。gRPC需要protobuf编译器、HTTP/2库，在嵌入式平台太重
2. **人类可读**：调试时可以直接 `nc localhost 1234` 发送JSON查看响应
3. **跨语言**：移动端Web可以直接用JS的`fetch()`解析JSON
4. **够用**：控制面QPS很低（每秒几个请求），JSON序列化的开销可忽略

**Q3: 为什么控制面和数据面要分离？**

A: 这是嵌入式系统设计的核心原则。
- 控制面（JSON-RPC）：低频、小数据量（几十字节）、要求可靠性
- 数据面（共享内存）：高频、大数据量（MJPEG帧可达几百KB）、实时性优先

如果将视频帧也走RPC/TCP，640x480@30fps的MJPEG帧需要base64编码+TCP发送，编码开销和协议栈开销会严重降低帧率。分离后视频帧零拷贝直达GUI，控制指令走RPC保证可靠性。

### LVGL显示

**Q4: 如何处理LVGL的线程安全问题？**

A: LVGL v9.x不是线程安全的。我们的策略是**全局互斥锁**：
- 所有LVGL API调用前获取锁，调用后释放
- 主渲染循环持锁调用 `lv_timer_handler()`
- 后台线程（传感器轮询、UDP回调）在更新UI label/image前获取锁
- 锁持有时间极短（<1ms），不会造成UI卡顿

这里有一个关键细节：`lv_timer_handler()` 的返回值是下次调用的建议延迟，我们用 `usleep()` 而不是忙等，避免空转占用CPU。

**Q5: 为什么选择XRGB8888而不是RGB565？**

A:
1. **LVGL内部色深32位**：使用XRGB8888与LVGL内部格式一致，零转换开销
2. **JPEG解码输出BGRX**：libjpeg的`JCS_EXT_BGRX`直接输出XRGB8888兼容格式
3. **透明度硬编码**：Alpha通道固定0xFF，避免预乘Alpha的复杂度和性能开销
4. 代价是内存占用翻倍（每像素4字节 vs 2字节），但800x480x4≈1.5MB在2GB DDR的昇腾310B上完全可以接受

**Q6: 如何彻底解决摄像头预览的画面撕裂？**

A: 三层防护（详见第5.2节）：
1. **Ping-Pong双缓冲（应用层）**：始终渲染到非显示buffer，完成后原子切换 `lv_image_set_src()` + `lv_obj_invalidate()`
2. **SPSC三槽位环形缓冲（IPC层）**：共享内存使用3槽位+POSIX信号量，实现无锁的生产者-消费者同步
3. **精确无效化（显示层）**：同时 `lv_obj_invalidate()` 图像控件和父容器，确保LVGL完全重绘受影响区域

额外措施：`sem_trywait()` 非阻塞读取确保消费者线程永不阻塞，不影响UI刷新率。

### 摄像头 V4L2

**Q7: V4L2的mmap vs read()有什么区别？**

A: `mmap` 模式下，V4L2驱动直接将内核缓冲区映射到用户空间，应用程序直接访问内核内存。`read()` 需要将数据从内核拷贝到用户态缓冲区。
- mmap受益：零拷贝，适合高帧率视频流
- mmap代价：需要管理缓冲区生命周期（QBUF/DQBUF），编程复杂度更高

我们使用4个mmap缓冲区，循环使用，确保驱动有足够缓冲处理USB传输抖动。

**Q8: 为什么共享内存缓冲区用3个槽位而不是2个？**

A: 在SPSC环形缓冲区中，如果只有2个槽位，存在以下风险：
- 槽位0满，生产者写槽位1（此时消费者在读槽位0） ← **安全**
- 槽位1满，生产者要写槽位0（此时消费者在读槽位1） ← **安全**
- 槽位0满，生产者要写槽位1（此时消费者**正在**读槽位1）← **冲突！**

使用3个槽位可以确保生产者和消费者始终操作不同的槽位：
- 最多有1个槽位正在被写、1个正在被读、1个空闲
- 配合信号量的计数语义（`sem_empty`初始值=3），永远不会出现溢出或冲突

这是经典的三槽位SPSC设计，在无锁数据结构中被广泛使用。

**Q9: 如何处理不同摄像头的格式兼容性？**

A: 
1. **多设备回退**：依次尝试by-path节点 → `/dev/video0`~`/dev/video5`
2. **格式协商回退**：优先MJPEG，失败则YUYV
3. **能力检查**：`VIDIOC_QUERYCAP` 确保设备支持 `VIDEO_CAPTURE` + `STREAMING`
4. **Quirk注释**：代码中记录了HIK 1080P摄像头YUYV报告不准确的已知问题

### AI语音

**Q10: 为什么语音部分用UDP而不是TCP？**

A:
1. **实时性优先**：音频数据对延迟敏感，UDP不需要TCP的重传机制
2. **有损可接受**：偶尔丢一个音频包，Opus解码器可以容错
3. **简单**：不需要连接管理，Control Center和Sound App可以独立启停
4. **本地通信**：全部在localhost上，丢包率极低

### Web服务

**Q11: 为什么自研HTTP服务器而不用libmicrohttpd等库？**

A:
1. **极简需求**：只需要5个API路由 + 返回一个内嵌HTML
2. **依赖最小化**：自研HTTP服务器约300行C代码，无需引入额外依赖
3. **完全可控**：可以精确控制内存分配、超时行为
4. **学习价值**：`select()` 多路复用是经典网络编程模型，自研加深理解

### 驱动

**Q12: DHT11的OneWire时序如何保证？**

A: DHT11的OneWire协议对时序要求严格（微秒级）。原驱动通过 `local_irq_save()` 关闭本地中断进入临界区，保证bit-banging时序不被中断打断。代码中还有一个注释掉的版本使用了GPIO边沿中断 + 内核定时器方案，但由于ARM上的中断延迟波动较大，最终选择了轮询方案。

在昇腾310B上，需要重新评估中断延迟。如果轮询方案因CPU频率/调度延迟不满足时序要求，可能需要考虑外接MCU处理时序敏感的OneWire协议。

**Q13: 如何将驱动从i.MX移植到昇騰310B？**

A: 核心工作项（详见7.4节）：
1. 内核版本适配（设备树API可能有变化）
2. GPIO/I2C/PWM编号重新映射
3. 设备树compatible字符串匹配
4. 中断延迟验证（特别是DHT11/SR501等对时序敏感的驱动）
5. 显示后端从i.MX DRM切换到昇騰DRM
6. 摄像头接口可能从USB UVC切换到MIPI CSI（需Media Controller API）

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
