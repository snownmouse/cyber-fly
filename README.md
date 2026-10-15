# Cyber-Fly

[简体中文](README_CN.md)

**A fly brain in the cloud, a fly body on the bench.** An ESP32 board carries the real
sensors (microphone + BMI270 IMU) and the real actuators; a fruit-fly connectome
subcircuit running on a cloud server does the "thinking". The two are wired into a live
**sense → think → act** loop — a real closed loop, not a simulation and not a desktop pet.

![Cyber-Fly closed-loop architecture](https://raw.githubusercontent.com/snownmouse/cyber-fly-brain/main/brain_b/b_loop/cyber_fly_flow.svg)

> The cloud brain lives in its own repository:
> **[snownmouse/cyber-fly-brain](https://github.com/snownmouse/cyber-fly-brain)**.
> This repository is the board side — firmware, the bench probe and the hardware docs.

## The idea: a biological mapping

| Real structure | In this project | Role |
| --- | --- | --- |
| Sensory organs (ear / vestibular) | Microphone + BMI270 IMU | hear sound, sense tilt and sideways acceleration |
| Brainstem / spinal cord (I/O gateway) | **ESP32 board** | read sensors, drive actuators, run local reflexes |
| Brain / connectome | **Cloud connectome simulation** | spiking-rate computation, behaviour output |
| Motor neurons / muscles | Motors, wing flutter, steering | turn the brain's command into motion |

The board is **not** the brain. Fast spinal-level reflexes (for example left/right balance)
stay on the ESP32; high-level decisions go to the cloud — the same split biology makes
between a spinal reflex and a cortical decision.

## How the loop runs

```
perceive    (board: microphone + IMU)
   → phone hotspot        (2.4 GHz, UDP)
   → relay.js             (UDP hub, port 33333)
   → cloud brain          (stage1_loop.js · 6,296-neuron v4 subcircuit)
   → behaviour command    (relay, downlink)
   → board executes       (motor / wings / steering)
   → the world changes
   → perceive again
```

Every 50 ms the board sends a **68-byte MPRB** feature frame up; the cloud answers with a
**32-byte MPRC** command frame that drives the fly on the 480×480 CO5300 LCD. Every
animated element on that screen traces back to a real decoded field — body heading from
`turn`, wing flutter from `motor`, grooming from `groom`, and so on.

## Status

**The real closed loop runs on hardware.** The board associates to a phone hotspot, opens
the ES8311 microphone, streams MPRB to the cloud orchestrator every 50 ms, and renders the
MPRC command that comes back.

- uplink / downlink `tx 9565 / rx 9512`, zero bad frames, RTT **102 ms**, **27.7 fps**
- subcircuit **v4: 6,296 neurons / 475,562 edges / 2.82× real-time margin**
- MPRC decoder verified against the cloud's **real** downlink — **200/200 frames**, seq
  0→199 continuous, two tamper tests rejected by CRC and by magic
- host harness **23/23 pass**; ESP-half syntax check green on all 8 `main/` sources

## Repository layout

| Path | What it is |
| --- | --- |
| `projects/cyber_fly/` | Onboard firmware — the fly body (`main/` = 22 source files) |
| `tests/host/` | Host harnesses that compile the board sources and check the wire protocol |
| `tests/firmware/hw_probe/` | Bench probe — the board facts everything else is built on |
| `docs/hardware-interface-checklist.md` | Verified board interface (SoC, I2C addresses, codec, power-up order) |

The cloud side — the stage-1 orchestrator, the relay, the dashboards and their design docs —
lives in **[snownmouse/cyber-fly-brain](https://github.com/snownmouse/cyber-fly-brain)**.

## Quick start

Requires **Python 3.10+** and a host C compiler.

```sh
# host simulator: renders the fly from the built-in fake downlink
python mosaico.py game sim projects/cyber_fly

# flash to the board
python mosaico.py iris system-update --project projects/cyber_fly
```

Configuration, the Kconfig options, the Wi-Fi and IMU traps, and the 3-second ESP syntax
check are all documented in **[projects/cyber_fly/README.md](projects/cyber_fly/README.md)**.

## Documentation

- [projects/cyber_fly/README.md](projects/cyber_fly/README.md) — firmware deep dive
- [docs/hardware-interface-checklist.md](docs/hardware-interface-checklist.md) — verified board interface
- [tests/firmware/hw_probe/README.md](tests/firmware/hw_probe/README.md) — bench probe

Cloud side, in [cyber-fly-brain](https://github.com/snownmouse/cyber-fly-brain):

- [STAGE1-SENSOR-TO-BRAIN-DESIGN.md](https://github.com/snownmouse/cyber-fly-brain/blob/main/brain_b/STAGE1-SENSOR-TO-BRAIN-DESIGN.md) — sensor → brain → command design
- [cyber_fly_brief.md](https://github.com/snownmouse/cyber-fly-brain/blob/main/brain_b/b_loop/cyber_fly_brief.md) — project walkthrough (中文)
- [cyber_fly_intro.html](https://github.com/snownmouse/cyber-fly-brain/blob/main/brain_b/b_loop/cyber_fly_intro.html) — visual introduction

## Background

The cloud side is inspired by the FlyWire consortium's *Nature* 2024 full adult fruit-fly
connectome and by Shiu, Sterne et al., *Nature* 634:210–219 (2024) — a leaky
integrate-and-fire model that reproduces the complete sensory-to-motor transform from
synaptic connectivity alone. This project is a **functional** realisation of the same
principle (sensory input → connectome computation → motor output), not a per-neuron
simulation of all ~140,000 cells.

## Built on

The board firmware and host simulator run inside the **ESP-Mosaico Vibe** workspace for the
Espressif ESP32-S3 toolchain (CLI entry `mosaico.py`, pinned ESP-IDF, BSP and the Raylib
Lite Engine). That framework's own documentation is unchanged and lives in
[`docs/`](docs/README.md) and [`AGENTS.md`](AGENTS.md). The framework code is included here
as-is so the firmware can be built; it remains the property of its authors.

## Licence

Our own code — the firmware under `projects/cyber_fly/`, the bench probe under
`tests/firmware/hw_probe/` and the host harnesses under `tests/host/` — is licensed under the
**Apache License 2.0**; see [LICENSE](LICENSE). The `docs/` framework documentation, the
submodules and the `mosaico.py` toolchain keep their upstream terms.
