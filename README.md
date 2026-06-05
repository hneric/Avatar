# Korvo-1 AI Avatar Chat

基于 **ESP32-S31-Korvo-1** 开发板的 AI 对话交互系统，支持语音唤醒、WebSocket 云端对话、实时 TTS 语音合成、LVGL 鳄鱼表情动画，以及蓝牙耳机 HFP 音频路由。

## 硬件平台

| 组件 | 型号/规格 |
|---|---|
| SoC | ESP32-S31（双核 320MHz，16MB PSRAM，16MB Flash） |
| LCD | RGB 接口 800×480，16 位色 |
| 触摸 | GT1151（I2C） |
| 音频 Codec | ES8389（I2S 接口） |
| Wi-Fi | 2.4GHz 802.11 b/g/n |
| Bluetooth | Classic BT HFP（耳机模式） + BLE |
| 摄像头 | DVP 接口 OV3660（预留） |
| 存储 | 16MB SPI Flash + 16MB Octal PSRAM |
| 其他 | RGB LED、SD 卡控制、ADC 按键 |

## 功能特性

### 🗣 AI 语音对话
- 板载 ES8389 麦克风拾音 → Opus 编码 → WebSocket 上传云端
- 云端 STT 识别 → LLM 对话 → TTS 合成 → Opus 流式下发
- 本地 Opus 解码（libopus） → ES8389 扬声器播放
- VAD（语音活动检测）：自动判断说话起止，12 秒超时保护

### 🐊 鳄鱼表情动画（Avatar）
- 270×270 像素 LVGL 动画头像
- **5 级嘴型动画**：依据 TTS 音频 RMS 动态驱动张嘴幅度
- **眨眼动画**：每 ~3.6 秒自动眨眼一次
- **思考表情**：AI 处理时显示思考状态
- 支持回退模式（无图片资源时使用纯 LVGL 控件绘制的简易头像）

### 📱 触摸 UI（LVGL）
- WiFi 列表扫描与选择连接
- 蓝牙耳机发现与配对
- 对话文本显示（用户 + AI）
- 连接状态实时指示
- 掉线后"Chat Again"重连按钮

### 💾 OTA 升级
- 3 个 OTA 分区（factory + 2 个升级槽）
- Spiffs 数据分区（字库等资源）

## 项目结构

```
firmware/
├── CMakeLists.txt                 # ESP-IDF 项目入口
├── sdkconfig                      # Kconfig 编译配置
├── sdkconfig.defaults             # 默认配置覆盖
├── partitions.csv                 # 分区表
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                     # 主程序：初始化、LVGL UI、VAD 逻辑
│   ├── board.h                    # 板级引脚定义
│   ├── Kconfig.projbuild          # 项目 Kconfig 选项
│   ├── wifi_manager.c/h           # WiFi 连接管理
│   ├── websocket_client.c/h       # WebSocket 客户端（xiaozhi 协议）
│   ├── opus_encoder.c/h           # Opus 音频编码
│   ├── tts_player.c/h             # TTS Opus 解码播放（带 PCM 输出回调）
│   ├── bt_headset.c/h             # 蓝牙耳机 HFP AG（音频路由）
│   ├── audio_driver.c/h           # ES8389 音频驱动封装
│   ├── lcd_driver.c/h             # LCD RGB 驱动
│   ├── touch_driver.c/h           # GT1151 触摸驱动
│   ├── cbin_font.c/h              # 自定义字体加载
│   ├── font_emoji.h               # Emoji 字体表
│   ├── font_emoji_32.c            # Emoji 位图数据
│   ├── emoji/                     # 各 Emoji 的 LVGL 图片源文件
│   ├── avatar_assets/
│   │   ├── croc_avatar_assets.h   # 鳄鱼头像层定义
│   │   └── croc_avatar_assets.c   # 头像图片位图数据
│   └── ...
└── managed_components/
    └── esp-opus/                  # libopus 组件
```

## 鳄鱼表情制作说明

### 分层结构

头像由多个独立的 LVGL 图片层叠加渲染：

| 层级 | 图片 | 位置 | 尺寸 | 说明 |
|---|---|---|---|---|
| 底层 | `croc_avatar_base` | (0, 0) | 270×270 | 鳄鱼头部底座（固定） |
| 嘴部 | `croc_avatar_mouth_0~4` | ~(75, 202) | 72~125 × 19~50 | 5 级张嘴动画 |
| 眨眼 | `croc_avatar_blink` | (47, 79) | 177×81 | 覆盖眼部的半闭图层 |
| 思考 | `croc_avatar_thinking` | (0, 0) | 1×1 | 问号/思考图标 |

### 素材制作流程

1. **绘制原始 PNG**：每个图层导出为单独的 PNG 文件，背景透明
2. **转换为 LVGL 二进制**：使用 [LVGL 图片转换工具](https://lvgl.io/tools/imageconverter) 将 PNG 转为 C 数组（RGB565 格式）
3. **定义图层结构**：
   ```c
   typedef struct {
       const lv_image_dsc_t *img;  // LVGL 图片描述符
       int16_t x, y;               // 相对于画布的偏移
       int16_t w, h;               // 裁剪区域
   } croc_avatar_layer_t;
   ```
4. **注册到动画系统**：在 `avatar_anim_timer_cb`（`main.c`）中根据状态切换图层

### 嘴型动画驱动

嘴型由 TTS 播放音量实时驱动：

```c
int open = MIN(MAX((rms - 220) / 80, 0), 22);
// 根据 open 值选择 mouth_0 ~ mouth_4 五个级别
```

### 添加新表情

1. 准备透明 PNG（建议尺寸 ≤ 270×270）
2. 在线转换工具生成 C 数组
3. 在 `croc_avatar_assets.h` 声明 `extern const lv_image_dsc_t` 和 `croc_avatar_layer_t`
4. 在 `croc_avatar_assets.c` 添加位图数据和图层定义
5. 在 `avatar_anim_timer_cb` 中添加状态判断逻辑

## 编译与烧录

### 环境要求

- **ESP-IDF**：版本 7812a5df（当前使用的 IDF 提交）
- **工具链**：ESP32-S3/S31 交叉编译器
- **Python 3** + 依赖包

### 构建步骤

```bash
cd firmware
idf.py set-target esp32s31
idf.py menuconfig        # 配置 WiFi SSID/密码、WebSocket 地址等
idf.py build
idf.py flash monitor
```

### 关键配置项

```
Korvo-1 Network →
    WiFi SSID           # 路由器 SSID
    WiFi password       # 路由器密码
    WebSocket host      # 后端服务地址（默认 120.25.213.109:8989）

Korvo-1 Bluetooth Headset →
    Enable HFP AG       # 启用蓝牙耳机模式
    Device name         # 广播名称
    Auto connect        # 目标耳机名称或 MAC
```

## WebSocket 协议

与 `xiaozhi` 后端服务的通信协议：

1. **Hello**：设备注册
2. **Listen Start/Stop**：音频上传控制
3. **Audio Frame**：Opus 编码音频帧（实时上传）
4. **STT Result**：语音识别结果
5. **TTS Start/Sentence/Stop**：合成语音下发

## 许可

本项目基于 ESP-IDF 和 LVGL 框架开发，组件许可见各子目录。
