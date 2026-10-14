# Wi-Fi 诊断报告 — 2026-10-06 15:19（真机扫描铁证）

## 一句话结论

板子的 2.4 GHz 电台**只听到一个热点**：`517`（−77 ~ −80 dBm，信道 10，WPA2）。
配置里要连的 **`your-ssid` 完全不存在**。硬件与固件均无问题。

## 原始证据（板端实时日志，`hw_probe` 标签）

```
W (73274)  hw_probe: no live radio path (attempt 2); scanning 2.4 GHz to see what is actually audible
W (75774)  hw_probe: scan: 1 AP(s) visible on 2.4 GHz
W (75774)  hw_probe:   AP[0] "517"  rssi=-80 dBm  ch=10  auth=4
W (136264) hw_probe: no live radio path (attempt 4); scanning 2.4 GHz ...
W (138764) hw_probe: scan: 0 AP(s) visible on 2.4 GHz
W (138764) hw_probe: scan: nothing heard at all -- radio up but no beacon in range
W (199254) hw_probe: no live radio path (attempt 6); scanning 2.4 GHz ...
W (201754) hw_probe: scan: 1 AP(s) visible on 2.4 GHz
W (201754) hw_probe:   AP[0] "517"  rssi=-77 dBm  ch=10  auth=4
```

完整原始日志：`D:\ESP\scan6.log`

## 当前配置（板端要连的目标）

| 项 | 值 |
|---|---|
| SSID | `your-ssid` |
| Passphrase | `your-wifi-password` |
| 频段 | 仅 2.4 GHz（ESP32-S31 无 5 GHz 电台） |
| MAX_RETRY / TIMEOUT | 10 / 60 s |

## 时间线

| 时刻 | 现象 |
|---|---|
| 今早 10:2x | **连上** `your-ssid`，IP `10.226.230.146`，RSSI **−33 dBm**（同屋、很近） |
| 13:5x 起 | 持续 `disconnected (reason 201)` = `NO_AP_FOUND` |
| 15:19 | 扫描：空中**只有** `517`（−77 ~ −80 dBm），无 `your-ssid` |

`reason 201` 的含义是"目标 SSID 没被扫到"，配合扫描结果可以完全排除"密码错""固件 bug""天线坏"。
`517` 信号很弱且时有时无，更像**邻居的路由器**，不像是贴在一起的自建热点。

## 本轮为查出真因所做的固件修复

文件：`tests/firmware/hw_probe/main/`

1. **`continuous_stream_loop()` 改为无限重试**（原来 12 次后即"驻留"，热点回来也不自愈）。
   每 60 s 顺带扫一次 AP。
2. **`wifi_sta.c` 新增 `hwprobe_wifi_scan()`**。关键坑：连接重试会让 STA 处于
   "connecting"，此时 `esp_wifi_scan_start()` 直接返回 `ESP_ERR_WIFI_STATE`
   （`sta_scan: STA is connecting, scan are not allowed!`）。用 `s_scan_mode`
   抑制事件处理器抢连接后，先 `set_mode/start` 再扫描才成功。
3. **`esp_log_level_set("wifi", ESP_LOG_ERROR)`**。驱动每次连接尝试会喷约 40 行
   rate 表，把 ESP-Iris 的日志保留环瞬间冲满，导致**开机扫描结果几秒内就被挤掉**——
   这是"扫描看不到结果"的真正原因。
4. 早前两处：Wi-Fi 重试容忍 `ESP_ERR_INVALID_STATE`（先 `disconnect`+`stop` 再重驱动）；
   `continuous_stream_loop()` 永不 `return`（返回 `app_main` 会被 ESP-Iris 判定不健康 → 掉 Recovery）。

## 下一步（用户侧，三选一）

- **最省事**：把热点恢复成 **名称 `your-ssid` / 密码 `your-wifi-password` / 2.4 GHz**。
  固件已改成无限重试，**无需再刷机**，板子约 30 s 内自动连上并开始发数据。
- **换了名字/密码**：告知新的名字 + 密码（并确认 2.4 GHz），改 `sdkconfig` 重刷（约 4 分钟）。
- **热点只开 5 GHz**：ESP32 永远看不到，需在热点设置里开 2.4 GHz（选"2.4 GHz"或"2.4 + 5 GHz"）。
