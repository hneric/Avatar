# ESP32-S31-Korvo-1 数字人 AI 聊天项目

> **芯片**: ESP32-S31 (RISC-V 双核 @320MHz)  
> **开发板**: ESP32-S31-Korvo-1 (乐鑫 2026.5 发布)  
> **后端**: xiaozhi-esp32-server-golang  
> **通信协议**: WebSocket / Opus 音频流  
> **显示**: 4.3" LCD (800×480) + LVGL + 数字人动画  

---

## 一、项目概述

### 1.1 功能目标

基于 ESP32-S31-Korvo-1 开发板，实现一个带**数字人虚拟形象**的 AI 语音聊天助手：

| 功能 | 说明 |
|------|------|
| 🎤 **语音输入** | 双麦克风阵列 → ES8389 Codec → I2S 采集 |
| 🧠 **AI 对话** | WebSocket 连接后端 → ASR(语音识别) → LLM(大模型) → TTS(语音合成) |
| 🔊 **语音输出** | TTS 音频流 → ES8389 → 双扬声器播放 |
| 👤 **数字人形象** | 4.3" LCD 上渲染 M5Stack-Avatar 面部动画 |
| 😊 **表情联动** | LLM 输出情绪标签 → Avatar 表情切换 (happy/sad/thinking...) |
| 👄 **嘴型同步** | TTS 音频 RMS 能量 → 嘴巴张开幅度实时联动 |
| 📷 **视觉扩展** | OV3660 摄像头 (3MP，预留) |

### 1.2 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-S31-Korvo-1 固件                    │
│                                                             │
│  ┌─────────┐  ┌─────────────┐  ┌────────────────────────┐  │
│  │ 音频管线  │  │  协议层      │  │  显示/数字人            │  │
│  │ I2S→Opus │  │ WebSocket   │  │  LVGL + 聊天气泡       │  │
│  │ Opus→I2S │  │ Opus流      │  │  M5Stack-Avatar 面部   │  │
│  │ ES8389   │  │ WiFi 管理   │  │  情绪/嘴型同步          │  │
│  └────┬─────┘  └──────┬──────┘  └───────────┬────────────┘  │
│       │               │                      │               │
│  ┌────┴───────────────┴──────────────────────┴───────────┐  │
│  │                  Board 抽象层                          │  │
│  │  LCD RGB / GT1151触摸 / ES8389音频 / SD卡 / 按键 / LED │  │
│  └────────────────────────┬──────────────────────────────┘  │
│                           │                                  │
│  ┌────────────────────────┴──────────────────────────────┐  │
│  │              ESP32-S31 硬件层                          │  │
│  │  4.3"LCD 800×480 | GT1151触控 | ES8389 | OV3660 | SD  │  │
│  │  Wi-Fi 6 | BLE 5.4 | 16MB Flash | 16MB PSRAM         │  │
│  └───────────────────────────────────────────────────────┘  │
└──────────────────────────┬──────────────────────────────────┘
                           │ WebSocket / Opus 流
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              xiaozhi-esp32-server-golang 后端                 │
│                                                             │
│  VAD → ASR (SenseVoice/FunASR) → LLM (DeepSeek/Qwen)       │
│  → TTS (CosyVoice/EdgeTTS) → 音频流返回                    │
│                                                             │
│  管理后台: Vue.js + Gin                                     │
│  数据存储: MySQL / Redis / Qdrant                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、硬件规格

### 2.1 ESP32-S31 芯片

| 参数 | 规格 |
|------|------|
| CPU | 双核 32-bit **RISC-V** @ **320 MHz** (128bit 宽数据通路 + SIMD) |
| SRAM | 512 KB |
| PSRAM | 16 MB (Octal SPI DDR @ 250MHz) |
| Flash | 16 MB |
| Wi-Fi | **Wi-Fi 6** (802.11ax) |
| 蓝牙 | **Bluetooth 5.4** (LE Audio + Classic) |
| 802.15.4 | Thread + Zigbee 3.0 (Matter) |
| 以太网 | 千兆以太网 MAC |
| 图形加速 | PPA 2D 图形加速器 + JPEG 编解码器 |
| GPIO | 60 个 |

### 2.2 Korvo-1 开发板

| 组件 | 型号 | 接口 |
|------|------|------|
| 模块 | ESP32-S31-WROOM-3 (16MB Flash + 16MB PSRAM) | - |
| LCD | 4.3" 800×480 RGB (ST7262E43/GC9503) | **RGB 565 16-bit 并行** |
| 触控 | GT1151 电容触摸 | **I2C** |
| 音频 Codec | **ES8389** 立体声 | I2S + I2C |
| 麦克风 | 双路模拟麦克风 | 通过 ES8389 接入 |
| 功放 | 2× NS4150B (3W Class-D) | PA_CTRL 控制 |
| 扬声器 | 双声道输出 | - |
| 摄像头 | OV3660 3MP | **DVP 8-bit 并行** |
| 存储 | microSD 卡槽 | SDMMC 4-bit |
| 按键 | PLAY / SET / VOL- / VOL+ | ADC 分压 (GPIO42) |
| RGB LED | WS2812 | GPIO37 |
| USB | USB OTG + Type-C UART + Type-C 电源 | - |

### 2.3 完整 GPIO 引脚表

#### LCD RGB 接口 (16-bit RGB565, 800×480)

| LCD 信号 | GPIO | 说明 |
|---------|------|------|
| LCD_B3 | **GPIO8** | Blue LSB |
| LCD_B4 | **GPIO9** | |
| LCD_B5 | **GPIO10** | |
| LCD_B6 | **GPIO11** | |
| LCD_B7 | **GPIO12** | Blue MSB |
| LCD_G2 | **GPIO13** | Green LSB |
| LCD_G3 | **GPIO14** | |
| LCD_G4 | **GPIO15** | |
| LCD_G5 | **GPIO16** | |
| LCD_G6 | **GPIO17** | |
| LCD_G7 | **GPIO18** | Green MSB |
| LCD_R3 | **GPIO19** | Red LSB |
| LCD_R4 | **GPIO33** | |
| LCD_R5 | **GPIO34** | |
| LCD_R6 | **GPIO35** | |
| LCD_R7 | **GPIO36** | Red MSB |
| LCD_PCLK | **GPIO40** | Pixel Clock |
| LCD_H_EN | **GPIO43** | Data Enable (DE) |
| LCD_H_SYNC | **GPIO44** | Horizontal Sync |
| LCD_V_SYNC | **GPIO45** | Vertical Sync |
| LCD_CS | **GPIO38** | SPI CS |
| LCD_MOSI | **GPIO60** | SPI MOSI |
| LCD_SCK | **GPIO61** | SPI Clock |

#### I2C 总线 (共享，地址不同)

| GPIO | 功能 | 挂载设备 | I2C 地址 |
|------|------|---------|---------|
| **GPIO0** | SDA | GT1151 触摸 / OV3660 摄像头 / ES8389 音频 | 0x14 / 0x78 / 0x20 |
| **GPIO1** | SCL | (同上) | |

#### 音频 I2S (ES8389)

⚠️ **引脚命名注意**：原理图上标注的是 ES8389 芯片视角，而 ESP-IDF 的 I2S 驱动配置是 ESP32 视角，两者刚好相反：

```
信号流向:                               ES8389视角          ESP32 I2S驱动配置
ESP32 ──dout──→ Codec DIN  (GPIO5)  =  I2S_DSDIN(输入)    .dout(ESP32输出)
ESP32 ←──din──  Codec DOUT (GPIO6)  =  I2S_SDOUT(输出)    .din(ESP32输入)
```

| 信号 | GPIO | ES8389 视角 | ESP32 I2S 配置 | 说明 |
|------|------|------------|---------------|------|
| I2S_MCLK | **GPIO2** | MCLK (输入) | `.mclk` | 主时钟 |
| I2S_SCLK | **GPIO3** | BCLK (输入) | `.bclk` | 位时钟 |
| I2S_LRCK | **GPIO4** | WS (输入) | `.ws` | 字时钟 |
| I2S_DSDIN | **GPIO5** | **DIN** (数据输入) | **`.dout`** | 数据从 ESP32 发往 Codec |
| I2S_SDOUT | **GPIO6** | **DOUT** (数据输出) | **`.din`** | 数据从 Codec 发往 ESP32 |
| PA_CTRL | **GPIO7** | - | - | 功放使能 (控制 2 个 NS4150B) |

**快速记忆**：ESP32 的 `dout` 连接 Codec 的 `DIN`（输入脚），ESP32 的 `din` 连接 Codec 的 `DOUT`（输出脚）。

#### 摄像头 DVP 接口 (OV3660, 8-bit)

| 信号 | GPIO |
|------|------|
| DVP_Y2-Y9 | **GPIO46-53** |
| DVP_PCLK | **GPIO54** |
| XMCLK | **GPIO55** |
| DVP_VSYNC / PWDN | **GPIO56** |
| DVP_HREF | **GPIO57** |
| CAM_RESET | **GND** (常开) |
| CAM_I2C | GPIO0/GPIO1 (共享) |

#### 其他外设

| 外设 | GPIO | 说明 |
|------|------|------|
| RGB LED (WS2812) | **GPIO37** | 状态指示灯 |
| SD 卡控制 | **GPIO39** | SD 卡电源使能 |
| ADC 按键阵列 | **GPIO42** | PLAY/SET/VOL-/VOL+ 四个按键 |
| SD_D0-D3 | **GPIO27-30** | SD 卡数据 |
| SD_CLK | **GPIO31** | SD 卡时钟 |
| SD_CMD | **GPIO32** | SD 卡命令 |

---

## 三、开发环境

### 3.1 环境要求

| 组件 | 版本 | 说明 |
|------|------|------|
| ESP-IDF | **master 分支 (v6.2-dev)** | S31 为预览目标，仅 master 支持 |
| 工具链 | riscv32-esp-elf | RISC-V GCC 工具链 |
| 系统 | Windows 10+ | PowerShell 环境 |
| 后端服务器 | xiaozhi-esp32-server-golang | Docker Compose 部署 |

### 3.2 环境搭建

```powershell
# 1. 克隆 ESP-IDF master 分支
cd ~/esp
git clone -b master --depth 1 https://github.com/espressif/esp-idf.git esp-idf-master

# 2. 安装工具链 (指定 esp32s31 目标)
cd esp-idf-master
.\install.ps1 esp32s31

# 3. 克隆 xiaozhi-esp32 项目
cd d:\code\esps31
git clone --depth 1 -b main https://github.com/78/xiaozhi-esp32.git xiaozhi-esp32

# 4. 每次新终端激活环境
cd ~/esp/esp-idf-master
.\export.ps1
cd d:\code\esps31\xiaozhi-esp32
```

### 3.3 编译命令

```powershell
# 第一次需要 --preview (S31 为预览目标)
idf.py --preview set-target esp32s31

# 后续编译
idf.py build

# 烧录 (通过 USB Type-C UART 口)
idf.py -p COMx flash

# 监控日志
idf.py -p COMx monitor
```

### 3.4 后端部署

参照 [xiaozhi-esp32-server-golang](https://github.com/hackers365/xiaozhi-esp32-server-golang) 的 Docker Compose 部署说明。

```bash
git clone https://github.com/hackers365/xiaozhi-esp32-server-golang.git
cd xiaozhi-esp32-server-golang
docker compose up -d
```

---

## 四、开发状态与移植记录

### 4.1 已完成

- ✅ ESP-IDF master 环境搭建 (esp32s31 预览目标)
- ✅ S31-Korvo-1 板级定义 (Board 类实现)
- ✅ LCD 800×480 RGB 驱动配置 (GC9503)
- ✅ GT1151 触摸驱动配置
- ✅ ES8389 音频 Codec 配置
- ✅ 完整的 GPIO 引脚映射表
- ✅ Kconfig / CMakeLists 板型集成

### 4.2 依赖兼容性状态

由于 **ESP32-S31 是 2026 年 3 月发布的预览目标芯片**，部分第三方组件的预编译库和 API 尚未适配。以下是组件兼容性矩阵：

| 组件 | 状态 | 说明 |
|------|------|------|
| ✅ `esp_lcd_panel_io_additions` | 可用 | 3-wire SPI 面板 IO |
| ✅ `esp_lcd_touch_gt1151` | 可用 | 触控驱动 |
| ✅ `esp_codec_dev` | 可用 | 音频 Codec 框架 |
| ✅ `lvgl/lvgl` | 可用 | 纯软件渲染，架构无关 |
| ✅ `esp_lvgl_port` | 可用 | LVGL ESP-IDF 移植 |
| ⚠️ `esp-sr` | **跳过** | 语音识别，无 S31 预编译库 |
| ⚠️ `esp_audio_codec` | **跳过** | Opus 编解码，无 S31 预编译库 |
| ⚠️ `78/esp-wifi-connect` | **跳过** | 依赖 json 组件 (IDF 6.x 已移除) |
| ⚠️ `78/esp-ml307` | **跳过** | 4G 模块，不需要 |
| ⚠️ `78/uart-eth-modem` | **跳过** | 以太网模块，不需要 |
| ⚠️ 各 LCD 驱动 (gc9a01/axs15231b 等) | **跳过** | 不使用的屏幕驱动 |
| ⚠️ `esp_video` | **跳过** | 仅支持 esp32s3/esp32p4 |
| ⚠️ `esp32-camera` | **跳过** | 仅支持 esp32s3 (摄像头后续可加) |

### 4.3 兼容性适配方案

对于因预览目标而跳过的组件，采用了以下适配策略：

**方案：Stub 头文件 + 强制包含兼容头**
- `boards/esp32-s31-korvo-1/stubs/` — **空头文件**，用于满足 `#include` 解析
- `boards/esp32-s31-korvo-1/esp32s31_compat.h` — **强制包含的兼容头** (`-include`)，提供缺失的类型定义和函数桩

当某组件发布对 esp32s31 的支持后，只需：
1. 从 `idf_component.yml` 中移除该组件的 `rules: - if: target not in [esp32s31]`
2. 从 `esp32s31_compat.h` 中移除对应的 stub 定义
3. 重新编译即可

---

## 五、简化版固件开发计划

鉴于完整 xiaozhi-esp32 项目对 esp-sr/esp_audio_codec 等组件的深度依赖，计划分两个阶段实现：

### 阶段 1：精简版固件 (核心硬件验证)

创建一个独立的 ESP-IDF 项目，直接从零实现核心功能：

```
独立项目: d:\code\esps31\firmware          ← 新建
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # 主程序
│   ├── board.h                 # 板级引脚定义
│   ├── lcd_driver.c            # LCD RGB 驱动
│   ├── touch_driver.c          # GT1151 触控驱动
│   ├── audio_driver.c          # ES8389 音频驱动
│   ├── wifi_manager.c          # WiFi 连接管理
│   └── websocket_client.c      # WebSocket 客户端
├── components/
│   ├── m5avatar_port/           # M5Stack-Avatar 移植 (LVGL Canvas)
│   └── opus_codec/              # Opus 编解码
└── partitions/
    └── partitions.csv
```

### 阶段 2：功能迭代

| 优先级 | 功能 | 依赖 | 预估工时 |
|--------|------|------|---------|
| **P0** | LCD 点亮 + LVGL 基础显示 | esp_lcd, lvgl | 2 天 |
| **P0** | GT1151 触控 | esp_lcd_touch_gt1151 | 1 天 |
| **P0** | ES8389 音频 I2S 通路 | esp_codec_dev | 2 天 |
| **P0** | WiFi + WebSocket 通信 | esp_wifi, esp_websocket | 2 天 |
| **P1** | Opus 编解码 (引用 esp_audio_codec 源码) | Opus 软件库 | 2 天 |
| **P1** | M5Stack-Avatar 面部绘制 (LVGL Canvas) | lvgl | 3 天 |
| **P1** | 情绪标签 → 表情映射 | 自定义映射表 | 1 天 |
| **P2** | 嘴型同步 (音频 RMS → 张嘴幅度) | 音频能量计算 | 1 天 |
| **P2** | 服务端对接 (WebSocket 协议) | - | 2 天 |
| **P3** | OV3660 摄像头拍照 | esp_camera 组件 | 2 天 |
| **P3** | SD 卡存储 | sdmmc | 1 天 |

---

## 六、技术参考

### 6.1 关键链接

| 资源 | 链接 |
|------|------|
| xiaozhi-esp32 客户端 | https://github.com/78/xiaozhi-esp32 |
| xiaozhi-esp32-server-golang | https://github.com/hackers365/xiaozhi-esp32-server-golang |
| M5Stack-Avatar | https://github.com/genarks/m5stack-avatar |
| ESP-IDF | https://github.com/espressif/esp-idf |
| S31-Korvo-1 用户指南 | https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html |
| S31 状态页 | https://developer.espressif.com/hardware/esp32s31/ |

### 6.2 项目目录结构

```
d:\code\esps31\
├── doc/
│   └── ESP32-S31-Korvo-1-数字人AI聊天项目文档.md   ← 本文档
├── xiaozhi-esp32/               # xiaozhi-esp32 原始项目
│   ├── main/
│   │   ├── boards/
│   │   │   └── esp32-s31-korvo-1/   ← Korvo-1 板级定义
│   │   │       ├── config.h
│   │   │       ├── pin_config.h
│   │   │       ├── config.json
│   │   │       ├── esp32-s31-korvo-1.cc
│   │   │       ├── esp_lcd_gc9503.c / .h
│   │   │       ├── esp32s31_compat.h
│   │   │       ├── sdkconfig.defaults
│   │   │       └── stubs/
│   │   │           ├── http.h
│   │   │           ├── web_socket.h
│   │   │           ├── ...
│   │   │           └── cJSON.h
│   │   └── ...
│   ├── CMakeLists.txt
│   └── sdkconfig.defaults
├── korvo1-schematics.pdf        # 原理图
├── korvo1-schematics.txt        # 原理图文本提取
└── firmware/                    # 阶段 1: 精简版固件 (待创建)
```

### 6.3 M5Stack-Avatar 集成思路

将 M5Stack-Avatar 的面部绘制逻辑移植到 **LVGL Canvas Widget** 上：

```
LVGL 显示管线
  │
  ├── 底层: esp_lcd_panel_rgb (S31 LCD 控制器)
  ├── LVGL: lv_display 初始化
  │    ├── 状态栏 (WiFi/音量/电量)
  │    ├── 聊天气泡区域 (lv_label)
  │    └── Avatar 区域 (lv_canvas)
  │         └── 绘制函数移植自 M5Stack-Avatar
  │              ├── drawFace()     - 面部轮廓
  │              ├── drawEyes()     - 眼睛 (跟随情绪变化)
  │              └── drawMouth()    - 嘴巴 (根据音频能量开合)
  └── 情绪控制: SetEmotion("happy") → 切换眼睛/嘴巴的绘制参数
```

嘴型同步：从 TTS 音频 PCM 数据中计算 RMS 能量值 → 映射为 0.0~1.0 的开合度 → 传递给嘴巴绘制函数。
