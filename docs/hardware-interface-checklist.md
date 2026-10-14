# ESP-Mosaico 硬件接口确认清单

> 任务 1 交付物。所有结论均来自本地仓库一手源码 + 乐鑫官方规格书，附源文件路径可复核。
> 核实日期：2026-10-06 · BSP 版本：0.2.0 · ESP-IDF：6.2.0（本地 `D:\ESP\esp-idf`）

---

## 0. 一句话结论

**你不需要手写任何寄存器驱动。** 板载麦克风、IMU、屏幕、马达全部已被官方 BSP 封装成现成 API，
`bsp_audio_codec_microphone_init()` 和 `bsp_imu_init()` 各一行即可完成初始化。
原方案里"先通过 I2C 配置 ES8311 寄存器"这一步，已经被乐鑫做好了。

但有三处必须修正（见第 8 节），其中 **BMI270 的 I2C 地址不是 0x68 而是 0x69**，写错直接读不到数据。

---

## 1. 芯片与板级

| 项目 | 实测值 | 来源 |
|---|---|---|
| SoC | ESP32-S31（双核 RISC-V @ 320 MHz） | `esp-mosaico-bsp/README.md`、`sdkconfig:2423` |
| CPU 频率 | 320 MHz，双核均启用（非 UNICORE） | `sdkconfig` `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=320` |
| Flash | 16 MB（Quad SPI） | `sdkconfig:1245` |
| PSRAM | Octal SPI，250 MHz | `sdkconfig:2322-2328` |
| 片上 SRAM | 512 KB | 官方规格书 |
| GPIO 数 | 60 | 官方规格书 |
| SIMD | **其中一个核带 128-bit 宽数据通路 + SIMD 指令** | 官方规格书 |
| ESP-IDF 要求 | **≥ 6.2**（本地已是 6.2.0 ✅） | `esp-mosaico-bsp/idf_component.yml:12` |
| 目标设置命令 | `idf.py --preview set-target esp32s31`（需 preview 参数） | BSP README |

> 💡 **对项目的意义**：那个带 128-bit SIMD 的核是给你做本地 FFT/MFCC 特征提取的。
> 把音频特征提取任务**固定绑在这个核上**（`xTaskCreatePinnedToCore`），比随便丢给 FreeRTOS 调度快得多。

### 板型版本

| 版本 | 说明 |
|---|---|
| v1.0 | 独立变体，引脚映射 A |
| v1.1 | **使用 v1.2 的映射** |
| v1.2 | 独立变体，引脚映射 B |

**不需要看丝印。** BSP 从芯片 eFuse 的 `USER_DATA` 字段读取硬件版本并自动切换引脚：

```c
bsp_board_variant_t variant;
ESP_ERROR_CHECK(bsp_board_variant_get(&variant));  // 返回 V1_0 或 V1_2
```

- 源码：`onboard/esp_mosaico.c:33-68`
- eFuse 未烧录或版本不支持 → 返回 `ESP_ERR_NOT_SUPPORTED`，BSP 拒绝初始化

> ⚠️ 但两个版本的引脚差异**非常大**（I2C SDA 从 GPIO0 变 GPIO56，LCD 的 RST/SCL 两根还互换了）。
> 只要你**全程使用 BSP API**，就完全不用关心这件事；一旦你手写 GPIO 操作，就必须先判版本。

---

## 2. 音频链路（麦克风 → ES8311 → I2S）✅ 原方案正确

信号链：`MEMS 麦克风 → ES8311 codec（I2C 配置 + I2S 传数据）→ ESP32-S31 I2S0`

### ES8311

| 项目 | 值 | 来源 |
|---|---|---|
| I2C 地址（7-bit） | **0x19** ✅ 与你的判断一致 | `include/bsp/esp_mosaico.h:116` |
| 代码中的写法 | `BSP_AUDIO_CODEC_I2C_ADDR = (0x19U << 1)` = 0x32 | 同上 |
| 注意 | esp_codec_dev 要求传入**左移后的 8-bit 形式**，不要自己再移一次 | 头文件注释 |
| I2C 速率 | 400 kHz（挂共享总线） | `onboard/audio.c:80-84` |
| 工作模式 | ADC 模式（录音）、从模式（ESP32 提供 MCLK） | `onboard/audio.c:93-95` |
| 是否数字麦 | `digital_mic = false`（模拟麦） | `onboard/audio.c:97` |

### I2S 引脚（I2S_NUM_0，ESP32 主模式）

| 信号 | GPIO | 方向 |
|---|---|---|
| MCLK | **GPIO54** | 输出（256× 采样率） |
| BCLK / SCLK | **GPIO37** | 输出 |
| WS / LRCLK | **GPIO49** | 输出 |
| DIN（麦克风数据入） | **GPIO40** | 输入 |
| DOUT（扬声器数据出） | **GPIO52** | 输出 |
| PA_CTRL（功放使能） | **GPIO45** | 输出 |

来源：`include/bsp/esp_mosaico.h:109-114`，`onboard/audio.c:20-35`

### 采样率与声道

| 项目 | BSP 默认值 | 可改成 16 kHz 单声道？ |
|---|---|---|
| 采样率 | 48000 Hz | ✅ **可以** |
| 位深 | 16 bit | — |
| 声道 | 2（立体声） | ✅ **可以** |

**关键**：I2S 握手用默认 48 kHz 立体声初始化，**但 `esp_codec_dev_open()` 时传入 16 kHz / 1 声道，
驱动会自动重配 ES8311 的时钟分频**。这一点已被官方示例验证：

```c
// examples/voice_wake_cmd/main/main.c:117-127 —— 现成的 16kHz 单声道麦克风采集
esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
esp_codec_dev_sample_info_t in = {
    .sample_rate = 16000,
    .bits_per_sample = 16,
    .channel = 1,
    .channel_mask = 0,
};
esp_codec_dev_open(mic, &in);
esp_codec_dev_set_in_gain(mic, 30.0f);   // 增益 30 dB
```

**推荐配置：16 kHz / 16 bit / 单声道** — 每秒 32 KB 原始 PCM，与你方案里的估算一致，
而且 16 kHz 足够覆盖果蝇听觉相关频段和你说的 200-500 Hz 分频点。

> ⚠️ 坑：I2S 的 slot 模式初始化为 `STEREO`（`onboard/audio.c:24`）。
> 如果你 `esp_codec_dev_open()` 时用 `channel = 2`，读到的 buffer 是 L/R 交替，
> 但板上**只有一个物理麦克风**，右声道数据无意义。用 `channel = 1` 避开。

---

## 3. IMU（BMI270）⚠️ 地址与常见值不同

| 项目 | 值 | 来源 |
|---|---|---|
| 型号 | BMI270（六轴：加速度 + 陀螺仪） | BSP README |
| **I2C 地址（7-bit）** | **0x69** ⚠️ **不是常见的 0x68** | `include/bsp/esp_mosaico.h:152` |
| I2C 速率 | 400 kHz | `onboard/sensors.c:65-69` |
| 中断引脚 | **GPIO2**（与两个磁力计共享，二极管 OR） | `esp_mosaico.h:78,153` |
| 驱动组件 | `espressif2022/bmi270` `^1.1.0~2`，**public 依赖（应用可直接调用 BMI2 API）** | `idf_component.yml:36-38` |

### 默认配置

```c
// include/bsp/imu.h:23-28
.accel_odr   = BMI2_ACC_ODR_100HZ      // 加速度 100 Hz
.accel_range = BMI2_ACC_RANGE_4G       // ±4 g
.gyro_odr    = BMI2_GYR_ODR_100HZ      // 陀螺仪 100 Hz
.gyro_range  = BMI2_GYR_RANGE_2000     // ±2000 °/s
```

> ✅ **100 Hz 输出正好匹配你"每 50 ms 采一帧"的节奏**（50 ms → 每帧取 5 个 IMU 采样）。

### 现成 API

```c
bsp_imu_init();                                  // 内部已含上电 + I2C 初始化
bsp_imu_start(NULL);                             // NULL = 用上面的默认配置
bsp_imu_get_accel(&x_g, &y_g, &z_g);             // 单位 g
bsp_imu_get_gyro(&x_dps, &y_dps, &z_dps);        // 单位 °/s
struct bmi2_dev *dev = bsp_imu_get_handle();     // 需要裸寄存器访问时再拿句柄
```

> 📌 用**轮询**即可（100 Hz 下开销可忽略），不必接 GPIO2 中断 ——
> 何况那条中断线还跟两个磁力计共用， SOFTWARE 上要区分是谁触发的，麻烦。

---

## 4. I2C 总线拓扑（v1.0）

一条 `I2C_NUM_0` 总线上挂着 5 类设备：

| 设备 | 7-bit 地址 | 用途 |
|---|---|---|
| ES8311 音频 codec | 0x19 | 麦克风 / 扬声器 |
| **BMI270 IMU** | **0x69** | 六轴 |
| BMM150 磁力计 #0 | 0x11 | 磁吸检测 |
| BMM150 磁力计 #1 | 0x12 | 磁吸检测 |
| CST9217 触摸控制器 | （见 display 驱动） | 触摸屏 |
| BQ27220 电量计 | 0x55 | 电池 |

- v1.0：SDA = **GPIO0**，SCL = **GPIO1**
- v1.2：SDA = **GPIO56**，SCL = **GPIO3**
- 内部上拉已启用：**是**（`onboard/esp_mosaico.c:131`）

> ⚠️ 总线争用：ES8311 只在初始化和改增益时占用总线，BMI270 以 100 Hz 轮询（每次约 12 字节），
> 400 kHz 下总占用率 < 2%，**不构成瓶颈**。但不要在音频采集任务里做阻塞式 I2C 大批量读写。

---

## 5. 输出设备

| 设备 | 规格 / 引脚 | API |
|---|---|---|
| 屏幕 | 480×480 CO5300 QSPI LCD | `bsp/display.h` |
| 触摸 | CST9217 电容触控，INT = GPIO6 | `bsp/display.h` |
| 振动马达 | **GPIO8**，支持 PWM 调强度 | `bsp_motor_set(bool)` / `bsp_motor_set_strength(uint8_t percent)` |
| 状态 LED | GPIO3（**仅 v1.0 有**），低电平点亮 | `bsp_led_set(bool)` |
| AI 按键 | GPIO7，低电平有效 | `bsp/button.h` |
| BOOT 按键 | GPIO61 | `bsp/button.h` |

> 🎯 **马达支持 0-100 级强度调节**（`bsp_motor_set_strength`），这比简单的开关更适合表达
> "果蝇振翅强度"这类连续量 —— 你方案里的"驱动振动马达模拟身体反应"可以直接用强度映射。

---

## 6. 电源与上电顺序 ⚠️ 重要坑

板上外设不是上电即可用的，BSP 会分步开电源轨：

| 电源轨 | GPIO | 有效电平 | 说明 |
|---|---|---|---|
| VCC_3V3 | GPIO60 | **低电平**开启 | 带 PWM 软启动斜坡 |
| CODEC_3V3 | GPIO56（**仅 v1.0**） | **高电平**开启 | v1.2 上此函数为空操作 |
| 关机信号 | GPIO57 | 低电平请求关机 | 开漏输出 |

**必须最先调用的初始化序列：**

```c
bsp_board_variant_get(&variant);   // 1. 识别板型（读 eFuse）
bsp_power_init();                  // 2. 初始化电源 GPIO
// 之后才能 bsp_audio_* / bsp_imu_* / bsp_display_*（它们内部会自己开对应电源轨）
```

> ⚠️ 如果跳过 `bsp_power_init()`，codec 和传感器的 3.3V 轨不会上电，
> 表现是 **I2C 扫描不到 ES8311 / BMI270**，看起来像"芯片坏了"。

---

## 7. Wi-Fi 6 实际能力 ⚠️ 需要修正预期

ESP32-S31 确实支持 Wi-Fi 6，但**是有限的 ax**：

| 项目 | 实际值 |
|---|---|
| 频段 | 仅 2.4 GHz（2412-2484 MHz） |
| 空间流 | 1T1R（单发单收） |
| 802.11ax 模式 | **仅 20 MHz 非 AP 模式**（20 MHz-only non-AP） |
| 物理速率上限 | 150 Mbps |
| ax 特性 | OFDMA（上下行）、下行 MU-MIMO、Beamformee、空间复用、TWT |
| 完全兼容 | 802.11b/g/n |

来源：ESP32-S31 技术规格书（Preliminary v0.5）

> 🔍 **对你的方案的修正**：
> 你说"Wi-Fi 6 理论带宽足够" —— 结论对，但**理由要换**。
> 你的上行数据是 10-50 字节/帧 × 20 帧/秒 = **不到 1 KB/s**，连 150 Mbps 的万分之一都不到，
> **带宽从来不是问题**。真正的风险是 **2.4 GHz 频段的干扰导致的抖动和突发丢包**
> （邻居 WiFi、蓝牙、微波炉都在这一段）。Wi-Fi 6 在这里的价值是 **OFDMA 抗拥塞**，不是提速。
>
> 建议：UDP 包里带**递增序列号 + 本地时间戳**，云端据此算真实丢包率和抖动，别拍脑袋估。

---

## 8. 与你原方案的 5 处偏差

| # | 你的方案 | 核实结果 | 影响 |
|---|---|---|---|
| 1 | 需看丝印确认 CoreBoard 版本 | 版本有 v1.0/v1.2（v1.1 走 v1.2 映射），**BSP 从 eFuse 自动识别** | ✅ 简化了，不用手动查 |
| 2 | ES8311 地址 0x19，需手写 I2C 寄存器配置 | 地址 ✅ 正确；**但寄存器配置已被 BSP 封装**，一行 API 搞定 | ✅ 大幅简化 |
| 3 | BMI270 地址待确认 | **0x69**，不是常见的 0x68 | ❌ 写错读不到数据 |
| 4 | Wi-Fi 6 带宽充足 | 仅 20 MHz non-AP，2.4 GHz 1T1R；带宽确实够，但瓶颈是抖动不是带宽 | ⚠️ 预期修正 |
| 5 | ESP32-S31 跑不动果蝇脑 | ✅ 判断正确；且芯片有一核带 128-bit SIMD，本地特征提取很充裕 | ✅ 确认 |

---

## 9. 最小验证固件骨架（Day 1-2 可直接用）

目标：麦克风采集 → 串口打印音频特征；IMU 数据同时打印。

```c
#include "bsp/esp_mosaico.h"

void app_main(void)
{
    bsp_board_variant_t variant;
    ESP_ERROR_CHECK(bsp_board_variant_get(&variant));
    ESP_ERROR_CHECK(bsp_power_init());

    // --- 麦克风：16 kHz / 16 bit / 单声道 ---
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t in = {
        .sample_rate = 16000, .bits_per_sample = 16, .channel = 1, .channel_mask = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(mic, &in));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(mic, 30.0f));

    // --- IMU：100 Hz 默认配置 ---
    ESP_ERROR_CHECK(bsp_imu_init());
    ESP_ERROR_CHECK(bsp_imu_start(NULL));

    int16_t *pcm = heap_caps_malloc(1024 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float ax, ay, az, gx, gy, gz;

    while (1) {
        esp_codec_dev_read(mic, (uint8_t *)pcm, 1024 * sizeof(int16_t));  // 64 ms @16kHz
        bsp_imu_get_accel(&ax, &ay, &az);
        bsp_imu_get_gyro(&gx, &gy, &gz);
        // TODO: 在此插入 FFT / 频段能量计算，printf 输出结果
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

**可直接参考的官方示例（按贴近程度排序）：**

| 示例 | 路径 | 参考价值 |
|---|---|---|
| `voice_wake_cmd` | `submodule/esp-mosaico-bsp/examples/voice_wake_cmd` | ⭐ 16 kHz 单声道麦克风采集的完整流程 |
| `audio_loopback` | `.../examples/audio_loopback` | 麦克风→扬声器环回，验证 codec 硬件是否正常 |
| `imu_gesture` | `.../examples/imu_gesture` | ⭐ BMI270 摇一摇/倾斜/翻面识别 |

---

## 10. 仍需上板实测的 3 项（文档无法回答）

文档和源码只能确认到"应该能用"，这三项必须插上板子跑一遍：

| # | 待测项 | 怎么测 | 为什么必须测 |
|---|---|---|---|
| 1 | **你的板子实际是 v1.0 还是 v1.2** | 跑 `bsp_board_variant_get()` 打印结果 | 决定 I2C 引脚；虽然 BSP 自动适配，但要确认 eFuse 已正确烧录（否则返回 NOT_SUPPORTED） |
| 2 | **麦克风实际灵敏度 / 底噪** | 采集静音 5 秒，打印 PCM 的 RMS | 决定增益设多少（示例用 30 dB）、特征提取的阈值怎么定 |
| 3 | **UDP 实际丢包率与 RTT** | 板子连发 1000 个带序列号的小包，云端统计 | 决定要不要做重传/插值；2.4 GHz 干扰是现场问题，查不到 |

---

## 附：可复核的源文件

| 文件 | 提供了什么 |
|---|---|
| `submodule/esp-mosaico-bsp/README.md` | 功能总览、版本兼容性、示例清单 |
| `components/esp-mosaico-bsp/include/bsp/esp_mosaico.h` | **全部引脚与 I2C 地址定义**（第 62-161 行是核心） |
| `components/esp-mosaico-bsp/onboard/audio.c` | ES8311 初始化全流程 |
| `components/esp-mosaico-bsp/onboard/sensors.c` | BMI270 初始化与读取 |
| `components/esp-mosaico-bsp/onboard/esp_mosaico.c` | eFuse 版本识别、I2C 总线初始化 |
| `components/esp-mosaico-bsp/idf_component.yml` | 依赖组件与版本（BMI270 driver 出处） |
| `examples/voice_wake_cmd/main/main.c` | 16 kHz 单声道采集参考实现 |
