# hw_probe — ESP-Mosaico 硬件验收固件（任务 A）

一次性诊断固件，回答硬件接口确认清单里**无法从文档回答的 3 个问题**：

| # | 问题 | 本固件给出的答案 |
|---|---|---|
| 1 | 你手上这块板子是 v1.0 还是 v1.2？ | 从 eFuse 读出实际版本并打印 BSP 选用的引脚映射 |
| 2 | 麦克风的底噪多大？增益该设多少？ | 静音采集 DC 偏置 / AC RMS / 峰值 / 削顶率 + 0-50 dB 增益扫描并给出推荐值 |
| 3 | 2.4 GHz 下真实丢包率和抖动是多少？ | 突发测试（限速 1000 包）与稳态测试（20 Hz × 20 s），靠服务器 ACK 回显时间戳算真实 RTT |

附带一项 **BMI270 自检**，用于回归验证清单第 8 节第 3 处修正：**本板 BMI270 的 I2C 地址是 0x69，不是手册默认的 0x68**。

---

## 遵守的 5 处修正

| 清单修正 | 本固件的体现 |
|---|---|
| 板型由 eFuse 自动识别，不用看丝印 | `bsp_board_variant_get()` 打印实际版本；`bsp_power_init()` 内部同样走这条路径 |
| ES8311 不手写寄存器 | 只用 `bsp_audio_codec_microphone_init()`，全部寄存器配置由 BSP 完成 |
| **BMI270 地址是 0x69** | 自检项明确标注 0x69，并用"静止时 \|a\| ≈ 1 g"双重确认数据通路真的通了 |
| 包内必须带序列号 + 本地时间戳，实测抖动 | 68 字节包内含 `seq` 与 `send_ts_us`；服务器在 ACK 里回显时间戳，RTT 由板子算 |
| 发特征不发原始音频 | 每帧只发 3 个频段能量 + RMS + IMU 六轴，共 68 字节；16 kHz 原始 PCM 从不发送 |

额外遵守清单第 6 节的硬性顺序：**`bsp_power_init()` 是镜像里第一个被调用的 BSP 函数**。

---

## 目录结构

```
tests/firmware/hw_probe/
├── CMakeLists.txt              # 含 ../../../cmake/mosaico_application.cmake 契约
├── partitions.csv              # 与 hello_world 完全一致的保留分区表，请勿改动
├── sdkconfig.defaults
├── sdkconfig.application.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── Kconfig.projbuild       # Wi-Fi / 服务器 / 麦克风 / UDP 全部在这里配
│   ├── main.c                  # 步骤编排与最终汇总
│   ├── probe_common.{h,c}      # 68 字节包协议、CRC32、统计、报告
│   ├── board_probe.{h,c}       # 板型 + 芯片信息 + BMI270 自检
│   ├── mic_probe.{h,c}         # 底噪、Goertzel 频段能量、增益扫描
│   ├── wifi_sta.{h,c}          # STA 连接（已关闭省电模式）
│   └── udp_probe.{h,c}         # 突发 + 稳态流，RTT/抖动统计
└── tools/
    └── udp_probe_server.py     # 服务器端接收 + 统计 + 回 ACK
```

---

## 步骤 1：配置

```sh
idf.py menuconfig
# → HW Probe (task A: board / microphone / UDP link)
```

至少要填 3 项：

| 配置项 | 说明 |
|---|---|
| `Wi-Fi station → Wi-Fi SSID` | **必须是 2.4 GHz 网络**。ESP32-S31 是 2.4 GHz 单频段，5 GHz SSID 根本扫不到 |
| `Wi-Fi station → Wi-Fi password` | 开放网络留空 |
| `UDP probe target → Server IPv4 address` | 跑 `udp_probe_server.py` 那台机器的**局域网地址**（不是 127.0.0.1） |

其余默认值即可，第一次跑建议不要改：

- 麦克风采集 5 秒、增益 30 dB、开启增益扫描
- 突发 1000 包、间隔 1000 µs
- 稳态 20 Hz × 20 秒

---

## 步骤 2：构建与安装

> ⚠️ **`idf.py flash` 在本仓库被禁用。** `partitions.csv` 里的 `factory` 分区装的是**保留的 Recovery 镜像**，直接烧写会把它覆盖掉，仓库的 CMake 故意让 `flash` / `app-flash` 目标失败。

```sh
# 首次安装，或分区布局/外部资源有变化
python mosaico.py iris system-update --project tests/firmware/hw_probe

# 仅改代码后的日常更新
python mosaico.py iris app-update --project tests/firmware/hw_probe
```

看日志：

```sh
python mosaico.py iris logs
```

首次在裸板或状态不明的板子上跑，先执行：

```sh
python mosaico.py recover
```

---

## 步骤 3：启动服务器端

在云服务器或同一局域网的机器上：

```sh
python3 tools/udp_probe_server.py --port 33333
```

脚本会打印本机对外的局域网地址，把它填进 `Server IPv4 address`。加 `--quiet` 可以只看会话汇总。

---

## 步骤 4：上板运行

测试**麦克风底噪**时有两点必须做到，否则数字没有意义：

1. **环境安静** —— 关掉空调、风扇、音乐，人不要说话
2. **板子静止平放** —— 不要手持，手持会产生振动和摩擦噪声

串口日志会依次输出：

```
0/5  BOARD POWER              ← 确认 bsp_power_init() 成功
1/5  BOARD IDENTITY           ← 板型版本、芯片、flash/PSRAM、MAC
2/5  BMI270 SELF-CHECK        ← 验证 0x69
3/5  MICROPHONE NOISE FLOOR   ← 底噪三频段 + 判定
3b/5 MICROPHONE GAIN SWEEP    ← 0/10/20/30/40/50 dB 对比 + 推荐值
4/5  WI-FI LINK               ← IP、信道、RSSI
5/5  UDP LINK QUALITY
     5a/5 BURST
     5b/5 STEADY STREAM
SUMMARY                       ← PASS/FAIL 汇总
PARAMETERS TO CARRY INTO TASK B/C  ← 这就是要带回下一步的常量
```

---

## 结果怎么判读

### 关键判定阈值

| 项目 | 通过条件 | 说明 |
|---|---|---|
| 板型 | eFuse 可读 | 失败说明 eFuse 未烧录，先跑 `mosaico.py recover` |
| BMI270 | \|a\| 落在 0.5 – 1.6 g | 静止时重力约 1 g。**这条同时证明 0x69 地址正确** |
| 麦克风底噪 | AC RMS ≤ −45 dBFS | ≤ −55 dBFS 为"干净"；> −45 dBFS 说明有电气或声学干扰 |
| DC 偏置 | \|DC\| < 300 LSB | 过大指 codec 配置异常，不是噪声 |
| Wi-Fi RSSI | ≥ −80 dBm | ≥ −70 dBm 为良好；更弱是丢包/抖动的头号原因 |
| UDP 丢包 | ≤ 2% | 参照 flybrain-robot-bridge 在 2.4 GHz 下的报告值 |
| RTT 抖动 | ≤ 30 ms | 抖动比平均延迟重要得多 |

### 三个容易看错的地方

**1. "丢包 0% 且没有 ACK" 是失败，不是完美。**
如果服务器没跑，板子收不到任何 ACK。固件会单独报 `server reachable: FAIL`，不会把它算成零丢包。

**2. `send cadence` 那一行比丢包率更容易被忽略。**
稳态测试每帧要先采 50 ms 音频再发。如果"worst"明显超过 50 ms，说明**单帧成本已经超出一个帧周期**，实际帧率低于 20 Hz。这不是 bug，是必须知道的容量事实——要么接受更慢的节奏，要么把音频窗口缩短到 50 ms 以下。

**3. 增益不是越高越好。**
扫描会挑"底噪仍 ≤ −50 dBFS 且没有削顶"的最高增益。如果所有增益的底噪都偏高，问题在电气或声学（外壳共振、麦克风被遮挡），不在增益设置——这点脚本会明确提示。

---

## 输出示例

```
==================== 3/5  MICROPHONE NOISE FLOOR ====================
  captured 80000 samples over 5 s
  noise floor (AC RMS) : -68.4 dBFS
  peak                 : -51.2 dBFS (crest 17.2 dB)
  DC offset            : 12.0 LSB
  band <200 Hz         : -74.1 dBFS  (JO-B low band)
  band 200-500 Hz      : -78.6 dBFS  (JO-A mid band)
  band 500-2000 Hz     : -80.2 dBFS  (onset cue)
  [PASS] noise floor                    clean (<= -55 dBFS)

==================== 5b/5  STEADY STREAM ====================
  packets sent         : 400
  ACKs received        : 398 (unique 398)
  loss                 : 0.50% (2 packets)
  RTT min/avg/max      : 6.1 / 11.4 / 38.7 ms
  RTT jitter           : 4.2 ms stddev
  send cadence         : avg 50.1 ms, worst 63.8 ms
  [PASS] frame budget holds             target 50 ms, worst 63.8 ms
```

服务器端会给出对应的另一端统计（丢包位置、乱序、到达间隔）：

```
================ SESSION: burst ================
  received         : 998 datagrams
  sequence range   : 0..999  (expected 1000)
  loss             : 2 packets (0.20 %)
  out of order     : 0
  inter-arrival    : avg 1.02 ms  p95 1.41 ms  max 12.30 ms
```

---

## 收尾

测完把 `SUMMARY` 和服务器端两个 `SESSION` 段落的输出贴回来。下面这些数字会直接变成任务 B/C 的常量：

- 麦克风增益 dB 值
- 三个频段的底噪基线（用于定 JO 刺激阈值）
- 实测丢包率（决定下行是否需要插值）
- 实测 RTT 与抖动（决定行为回传的节流策略）

诊断完成后这个固件可以直接被真实应用覆盖：

```sh
python mosaico.py iris system-update --project projects/<你的应用>
```

---

## 已知限制

- **只测 2.4 GHz。** 芯片仅支持 2.4 GHz 单频段，Wi-Fi 6 的 802.11ax 是 20 MHz 非 AP 模式，因此测出来的是 2.4 GHz 拥挤程度，不是芯片上限。
- **突发测试的 `interval_us` 用忙等实现。** 默认 tick 是 10 ms，`vTaskDelay` 无法保证毫秒级间隔。突发每次约 1 秒，代价可忽略。
- **单机测试不反映真实部署。** 板子与服务器在同一房间时数字最好看。真正要验证的是**部署位置**——隔着墙、隔着人、路由器在隔壁房间时的数字才算数。
- **RTT 含服务器处理时间。** 脚本收到即回 ACK，但如果服务器负载高，这部分会计入 RTT。用云服务器时注意这一点。
