# cyber_fly

A thin fork of `projects/desktop_pet` that renders a **cyber fly** on the
ESP-Mosaico 480×480 CO5300 LCD, driven by the **32-byte MPRC brain downlink
frame** (see `tests/brain_b/STAGE1-SENSOR-TO-BRAIN-DESIGN.md` §3) instead of
virtual-pet stats.

**Status (2026-10-06): the real closed loop runs on hardware.** The board
associates to a phone hotspot, opens the ES8311 microphone, streams a 68-byte
MPRB feature frame to the cloud orchestrator every 50 ms, and renders the 32-byte
MPRC command frame that comes back. Measured: `tx 9565 / rx 9512`, zero bad
frames, RTT 102 ms, 27.7 fps. The built-in fake generator (`fly_fake_drive`) is
now only a fallback for when no cloud is reachable.

## What it is

The screen is a window into the fly brain's output, not a decorative pet. Every
animated element traces back to a real field of the decoded `fly_state_t`:

| Screen element | MPRC field | Notes |
|---|---|---|
| Body heading / orientation | `turn` (i8, −128..127 → −1..1) | smoothed into `heading` |
| Wing flutter / startle ring | `motor` + `flags.escape` | amplitude + flash |
| Front-leg grooming rub | `groom` (aDN1) | leg phase |
| Forward lean + speed lines | `thrust` | body offset + trails |
| Antenna quiver | `arousal` | amplitude |
| "AROUSAL" bar | `arousal` | top-left |
| "LINK" bar | `link_q` (RTT/loss) | top-right, red if low |
| "BRAIN" bar + FLAGS/seq/CRC | `brain_load`, `flags`, `seq`, CRC | corner readout |
| Escape burst | `flags.bit0` | driven by the brain's escape neuron |
| Turn left / right | `flags.bit1` / `bit2` | from the **board's** IMU, see below |
| Tap screen | injects `flags.escape` | interactive demo hook |

## Where steering comes from (and why it is not in the cloud)

In the v4 subcircuit the LEFT/RIGHT neurons (DNa) have **no named sensory
input** — they receive only the fast reflex. The brain genuinely cannot tell
left from right on its own, and a cloud round trip cannot supply it either: at
102 ms it is far too slow for a balance reflex. So the board sends the signed
sideways acceleration in `accel[0]` of the MPRB frame, and the orchestrator
turns it into `flags` with no neural step in between (design doc §0.5).

`main/fly_imu.{h,c}` reads the BMI270 and carries over both of Task A's
start-up traps: the data registers read back as **zero** until the first
conversion lands (averaging those is how a board lying flat on the desk reports
|a| = 0.59 g instead of 1.0 g), and the IMU is at address **0x69**, not 0x68.
A failed read sends `accel = 0` — "no lateral acceleration", never a fabricated
turn.

MPRC layout (offsets): `0` magic `MPRC` (0x4352504D), `4` version=1, `5` type=4,
`6` payload_len=32, `8` seq u32, `12` echo_ts_us u32, `16` turn i8, `17` thrust
u8, `18` motor u8, `19` arousal u8, `20` flags u8, `21` groom u8, `22` feed u8,
`23` link_q u8, `24` brain_load u8, `25` reserved u8, `26` reserved2 u16,
`28` crc32 (CRC-32/ISO-HDLC over [0,28)). Decode is CRC-verified and rejects
bad frames.

## Run the host simulator

Prerequisites (the sim compiles the shared game sources on the host, so it needs a
real C compiler — not just Python):

```sh
python -m pip install Pillow        # required by host/run_game.py
cc --version || gcc --version       # the host must have cc / gcc / clang
```

On Windows without a toolchain this fails with
`RuntimeError: no C compiler found (tried cc, gcc, and clang)` — install MinGW-w64 or
LLVM, or run it under WSL.

```sh
python mosaico.py game sim projects/cyber_fly             # interactive 480x480 window
python mosaico.py game sim projects/cyber_fly --headless --frames 300   # CI / no display
```

This renders the fly from the fake downlink at 30 fps. Tap/click injects an escape
burst. Pause/Resume/Reset are sent as host controls.

## Install to the board

```sh
python mosaico.py --workspace D:\ESP\esp-mosaico-vibe iris system-update --project projects/cyber_fly
```

The board mirrors its framebuffer to the host over Iris (same path as
`desktop_pet`'s `pet_screen_mirror`).

## Verified: the decoder really eats the server's frames

The MPRC decoder here is **not** only exercised by its own fake generator. It was compiled
(unchanged, with `gcc -O2 -Wall -Wextra`, zero warnings) and run against the **real
downlink stream produced by the cloud orchestrator**:

```sh
# on the server, after stage1_loop.js --emit-dl downlink_10s.bin
gcc -O2 -Wall -Wextra -o verify_mprc verify_mprc.c fly_game.c -lm
./verify_mprc /root/b_loop/downlink_10s.bin --dump 4
```

Result — **200/200 frames decoded, 0 failures, seq 0→199 continuous**
(66 escape, 8 left-turn, 14 groom, 110 feed; `turn` range [−115, 0]).
Two negative tests confirm the checks are real, not decorative:

| Tamper | Expected | Got |
|---|---|---|
| flip one bit of `turn` (offset 16) | rejected | rejected by CRC (`-7`) |
| flip one bit of `magic` (offset 0) | rejected | rejected by magic (`-3`) |

Full transcript: `tests/brain_b/b_loop/interop-mprc-raw.txt`.
Source: `tests/brain_b/b_loop/verify_mprc.c`.

Note the escape frames carry `turn = 0` and the bump frames carry `flags = 0x02` with no
escape — **escape and steering are separate in time**, because steering comes from the IMU
sideways acceleration, not from the brain (design doc §0.5).

## Later stages

### ✅ Real closed loop: Wi-Fi + microphone + UDP downlink (2026-10-06, second pass)

The first version of the link had three defects that only a real board would
have exposed. All three are fixed:

| gap | symptom it would have caused | fix |
|---|---|---|
| `cyber_fly` contained **no Wi-Fi code at all** — `mosaico_game_app` only calls `bsp_power_init()` | the socket opened before any lease existed, so every `sendto()` failed with `ENETUNREACH` while the log claimed the link was up | `main/fly_wifi.{h,c}`, ported from Task A's `wifi_sta.c` |
| the uplink frame was filled with a **hardcoded −60 dBFS**, i.e. a fabricated silence | the cloud would answer, the brain would read "nothing is happening", and the screen would sit still forever | `main/fly_mic.{h,c}` — real ES8311 capture, one 50 ms window per control frame |

`fly_link_udp_start()` now associates **before** it opens the socket, and the
microphone failure path deliberately sends a *valid frame carrying a silence
floor* rather than fabricating a stimulus: a dead codec must look like silence,
never like a sound the fly then reacts to.

`main/fly_wifi.c` keeps all three Task A findings:

- **`flywifi_` prefix** — a bare `wifi_sta_disconnect` collides with a symbol in
  the prebuilt `libnet80211.a` and fails the ELF link with `multiple definition`.
- **`ESP_ERR_INVALID_STATE` on `esp_wifi_init` is not fatal** — after a timed-out
  association the driver stays up, so every later attempt would fail. It is
  brought down and driven again from the same call.
- **unbounded retries** — the board may boot before the hotspot exists. It waits
  30 s between attempts and logs the disconnect reason (15 = wrong password,
  201 = SSID not visible, which on a 2.4 GHz-only radio also means "5 GHz only").

Bring-up runs on its **own FreeRTOS task**, not in `on_start()`: `on_start()` runs
before the app is declared healthy and ESP-Iris only waits ~45 s for that, so
blocking on a hotspot there made the installer report `outcome_unknown` even
though the app came up fine.

`on_stats` now names the failing stage instead of just saying "built-in":
`未联网` (no lease) / `启动中` (still associating) / `已发无回` (Wi-Fi up, cloud
silent) / `云端` (working). "No Wi-Fi" and "cloud not replying" need completely
different fixes.

Config (`main/Kconfig.projbuild`), all under the menu **"Cyber Fly brain downlink"**:

| option | default | what it does |
|---|---|---|
| `FLY_LINK_ENABLE` | `y` | off = the board animates from the built-in generator only |
| `FLY_LINK_HOST` | `192.168.1.100` | cloud orchestrator, dotted quad **on purpose** — lwIP DNS would add a blocking resolver call to the link task |
| `FLY_LINK_PORT` | `33333` | cloud UDP port |
| `FLY_WIFI_SSID` | `your-ssid` | must be a **2.4 GHz** AP (the radio has no 5 GHz) |
| `FLY_WIFI_PASSWORD` | `your-wifi-password` | **an empty password against a WPA2 AP retries forever and looks exactly like a dead hotspot** |
| `FLY_WIFI_TIMEOUT_SEC` | `20` | per-attempt association timeout |
| `FLY_WIFI_RETRY_SEC` | `10` | pause between attempts — lower to 5 while commissioning a hotspot |
| `FLY_MIC_GAIN_DB` | `30` | Task A measured a −44.7 dBFS noise floor at 30 dB, which puts the room floor and music on opposite sides of the cloud's −44 dBFS gate |

To change any of them:

```sh
idf.py -C projects/cyber_fly menuconfig     # or: idf.py ... reconfigure
```

The values land in `projects/cyber_fly/build/sdkconfig` — **check that file if
the board ignores what you set**, it is the only source of truth at build time.

### Wi-Fi: four bugs that kept the board off the network (2026-10-06, found by inspection)

Symptom: hotspot on, board never associates, retrying forever, reason unclear.

1. **`CONFIG_FLY_WIFI_PASSWORD` was empty.** Task A's password is written into
   `build/sdkconfig` at build time and was never carried into this project's
   Kconfig default. A protected AP plus an empty password means the driver
   attempts an open association and is dropped, forever.
2. **`esp_wifi_stop()` without `esp_wifi_deinit()`.** `stop()` leaves the driver
   INITIALIZED, so the next `esp_wifi_init()` returns `ESP_ERR_INVALID_STATE`
   and the retry loop can never recover. (Task A's `wifi_sta.c` has the same
   omission — harmless there because the probe rebooted after one attempt,
   fatal once a caller retries forever.)
3. **`threshold.authmode = WIFI_AUTH_OPEN`**, copied from the bench probe. It
   means "accept whatever the AP offers", which on an encrypted AP means the
   PSK is never sent. Now `WIFI_AUTH_WPA2_PSK`.
4. **Retries did not reset the netif**, so a stale non-zero address could report
   "connected" while traffic had nowhere to go — a much more confusing symptom
   than "never connected".

Bring-up now logs `连接目标 SSID=… 密码长度=N` and warns loudly on an empty
password, so the cause is in the log rather than something you have to infer.

### Host-side verification (no hardware needed)

`tests/brain_b/b_loop/verify_hostcheck.c` — **23/23 pass**, `gcc -Wall -Wextra`
zero warnings. It pins the behaviour the board depends on:

- a silent (or `mic == NULL`) MPRB is still a **valid** frame: magic, type,
  `payload_len` and CRC all check out, and `rms_dbfs` reads ≤ −59 dBFS;
- `fly_link_build_mprb()` is deterministic;
- a real MPRC is accepted, `take()` returns it byte-identically;
- sequence gaps are counted once, contiguous frames add none;
- a flipped `turn` bit is rejected by CRC, a flipped `magic` by magic, a 20-byte
  datagram as short — and **the last good state survives all three**;
- `link_q` is high on a fresh frame and 0 past `FLY_LINK_TIMEOUT_MS`.

Run it with any host compiler:

```sh
gcc -std=c11 -O2 -Wall -Wextra -I projects/cyber_fly/main \
    -o hostcheck tests/brain_b/b_loop/verify_hostcheck.c \
    projects/cyber_fly/main/fly_link.c projects/cyber_fly/main/fly_game.c -lm
./hostcheck
```

`verify_fly_link.c` additionally replays the orchestrator's real
`downlink_10s.bin` (200 frames) and now emits MPRB frames carrying **distinct**
per-band levels, asserting each lands at its own offset — the old placeholder
filled all three bands with one value, which made the cloud's band→neuron
mapping impossible to test.

### `quickcheck_esp.py` — 3-second check for the half the host test cannot see

`fly_link.c`'s ESP half, `fly_mic.c` and `fly_wifi.c` are all invisible to the
host harness (they sit behind `FLY_LINK_ESP_UDP` or include `esp_codec_dev.h` /
`esp_wifi.h`). A full IDF build is **2355 targets / ~20 minutes**, and on
2026-10-06 that round trip was spent twice on a single line:

1. `ip_addr_t` has no member `addr` (lwIP reaches the IPv4 word through the
   `u_addr.ip4` union),
2. `ipaddr_aton()` takes `ip_addr_t*`, not the `struct in_addr*` that
   `sin_addr` is — the fix is `inet_pton()`.

So this script recompiles just those three files using the **exact** command
lines recorded in `build/compile_commands.json`:

```sh
python quickcheck_esp.py       # add MOSAICO_QUICKCHECK_CC=<gcc> if the recorded path is stale
```

```
  ok   fly_link.c
  ok   fly_mic.c
  ok   fly_wifi.c
PASS
```

It tokenises the command line itself rather than using `shlex`, because neither
`shlex` mode survives a real IDF command: `posix=False` leaves the quote
characters in `-DFOO="a b"` (gcc then reports `missing terminating " character`
and the broken `-D` swallows every later flag, producing hundreds of bogus
`<stdio.h>` errors), while `posix=True` eats the backslashes in Windows paths
(`D:\ESP\...` → `D:ESP...`).

Run this before every full build of the ESP half.

### Host simulator

`mosaico.py game sim` needs a native host compiler. On Windows the Python child
process often cannot see a PATH set by the calling shell, so point it at the
compiler explicitly:

```sh
set MOSAICO_HOST_CC=D:\ESP\tools\mingw64\bin\gcc.exe
python mosaico.py game sim projects/cyber_fly --headless --frames 300
```

Result: 300 frames, `crc_ok: 1`, `seq` continuous, host render 0.60 ms/frame.

This also fixed a real bug inherited from `desktop_pet`: `fly_view.c` called
`snprintf()` without including `<stdio.h>`, and carried an unused `C_DARK`
colour. Neither showed up under the ESP toolchain; both are hard errors under
the simulator's `-Werror`.

### Still needs hardware

1. With the hotspot up, `on_stats` should log `源=云端 tx=… rx=… RTT=…ms`.
   Still `已发无回` ⇒ the NAT refused the reply; fall back to the board polling
   the cloud for the newest MPRC. The ~400 ms end-to-end budget can absorb a
   50 ms poll, and that path needs no cloud change.
2. Play the song and watch the fly startle on screen.
