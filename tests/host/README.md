# tests/host

Host-side harnesses for the board firmware. None of them need an ESP32: they compile the
pure-C core of `projects/cyber_fly/main/` with an ordinary host compiler and check the wire
protocol byte for byte.

| File | What it checks |
| --- | --- |
| `verify_hostcheck.c` | The MPRB/MPRC core: a silent (or `mic == NULL`) frame is still **valid**, `fly_link_build_mprb()` is deterministic, a real MPRC round-trips byte-identically, sequence gaps are counted once, and CRC / magic / short-datagram tampering is rejected without losing the last good state. **23/23 pass**, `gcc -Wall -Wextra`, zero warnings. |
| `verify_mprc.c` | Interop: decodes a recorded 200-frame MPRC stream (seq 0→199 continuous) with the same decoder the renderer uses, plus two negative tests that must be rejected. |
| `verify_fly_link.c` | Replays the orchestrator's recorded `downlink_10s.bin` and asserts the MPRB frames it emits carry distinct per-band levels, so the cloud's band→neuron mapping is actually testable. |

## Running them

From the workspace root:

```sh
gcc -std=c11 -O2 -Wall -Wextra -I projects/cyber_fly/main \
    -o hostcheck tests/host/verify_hostcheck.c \
    projects/cyber_fly/main/fly_link.c projects/cyber_fly/main/fly_game.c -lm
./hostcheck
```

`verify_mprc.c` and `verify_fly_link.c` build the same way. Both take the recorded downlink
stream as their first argument (they default to a server-side path). That stream is produced
on the cloud side by `stage1_loop.js --emit-dl` — see the sibling repository
[cyber-fly-brain](https://github.com/snownmouse/cyber-fly-brain) — and is not versioned here.

```sh
gcc -std=c11 -O2 -Wall -Wextra -I projects/cyber_fly/main \
    -o verify_mprc tests/host/verify_mprc.c \
    projects/cyber_fly/main/fly_link.c projects/cyber_fly/main/fly_game.c -lm
./verify_mprc downlink_10s.bin
```

These deliberately do **not** link `fly_mic.c` or `fly_wifi.c`: those pull in `esp_codec_dev`
/ `esp_wifi`. For the ESP half, use `projects/cyber_fly/quickcheck_esp.py`, which recompiles
those files using the exact command lines recorded in `build/compile_commands.json`.
