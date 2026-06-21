# Raspberry Pi Camera Telemetry Application

Industrial-style telemetry node built on **ESP32-P4** (Waveshare Module DEV-KIT) with an **OV5647** Raspberry Pi camera on **MIPI-CSI**. The device captures a JPEG snapshot at a fixed interval, attaches metadata (node ID, sequence, timestamp, temperature), and sends it over **Ethernet TCP** to a PC receiver. Failed uploads are **spooled on flash** and retried when the network returns.

---

## Table of contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Software architecture](#software-architecture)
4. [Runtime flow](#runtime-flow)
5. [Camera pipeline](#camera-pipeline)
6. [Ethernet and networking](#ethernet-and-networking)
7. [TCP protocol](#tcp-protocol)
8. [Local spool (store-and-forward)](#local-spool-store-and-forward)
9. [Health, watchdog, and auto-recovery](#health-watchdog-and-auto-recovery)
10. [NVS persistence](#nvs-persistence)
11. [Flash partition layout](#flash-partition-layout)
12. [Configuration](#configuration)
13. [Building and flashing](#building-and-flashing)
14. [PC receiver setup](#pc-receiver-setup)
15. [Master / slave control (Phase 1)](#master--slave-control-phase-1)
16. [Source files](#source-files)
17. [Troubleshooting](#troubleshooting)

---

## Overview

| Item | Value (default) |
|------|-----------------|
| Target | ESP32-P4 |
| Camera | OV5647 @ 800×640 |
| Capture interval | 10 s (`RPI_SNAPSHOT_INTERVAL_SEC`) |
| Transport | TCP over Ethernet (persistent connection) |
| Receiver | `tcp_receiver.py` on Windows/Linux PC |
| Security | HMAC-SHA256 (PSK) + CRC32 on JPEG |
| Delivery | ACK-based; spool on failure |
| Role | **Slave** — master (PC) sends START/STOP/RESTART |
| Control port | **9001** (ESP listens, PC connects) |
| Telemetry port | **9000** (ESP connects to PC receiver) |

Each **node** has a numeric **node ID**. Packets carry a monotonic **sequence number** per device so the receiver can detect gaps and sort data. Sequence and failure counters survive reboots via **NVS**.

---

## Hardware

### Board and camera

- **Board:** Waveshare ESP32-P4-Module-DEV-KIT (or compatible) with RMII Ethernet (IP101 PHY)
- **Camera:** Raspberry Pi OV5647 on the board MIPI-CSI connector
- **Link to PC:** Direct Ethernet cable (or switch); PC and ESP must be on the same subnet

### Pin summary

| Function | GPIO / setting |
|----------|----------------|
| SCCB SCL | 8 |
| SCCB SDA | 7 |
| MIPI LDO channel | 3 @ 2500 mV |
| ETH MDC / MDIO | 31 / 52 |
| ETH PHY reset | 51 |
| RMII CLK | 50 (external in) |
| RMII TX_EN / TXD0 / TXD1 | 49 / 34 / 35 |
| RMII CRS_DV | 28 |
| RMII RXD0 / RXD1 | 29 / 30 |

> **Note:** On ESP32-P4, RMII **RXD0 = GPIO 29** and **RXD1 = GPIO 30** (not the reverse). Wrong pins cause `invalid RXD0 GPIO number` at boot.

### Default IP layout (direct cable)

| Device | IPv4 | Role |
|--------|------|------|
| PC (Ethernet adapter) | `192.168.1.10` | Runs `tcp_receiver.py` |
| ESP32-P4 | `192.168.1.20` | Camera node |
| Netmask | `255.255.255.0` | Both sides |

---

## Software architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      app_main (FreeRTOS)                     │
│  Camera loop: CSI → ISP → snapshot timer → JPEG → TCP/spool │
└──────────────┬──────────────────────────────┬───────────────┘
               │                              │
    ┌──────────▼──────────┐        ┌──────────▼──────────┐
    │  rpi_camera_eth.c   │        │  rpi_camera_tcp.c   │
    │  Static IP, link    │        │  Connect, send,     │
    │  up/down handling   │        │  ACK, spool flush   │
    └──────────┬──────────┘        └──────────┬──────────┘
               │                              │
               │                   ┌──────────▼──────────┐
               │                   │ rpi_camera_spool.c  │
               │                   │ FAT /spool on flash │
               │                   └─────────────────────┘
    ┌──────────▼──────────┐
    │ rpi_camera_health.c │
    │ WDT, boot count,    │
    │ auto-restart        │
    └─────────────────────┘
```

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| Main loop | `rpi_camera_main.c` | Camera init, frame capture, snapshot scheduling |
| Ethernet | `rpi_camera_eth.c` | RMII driver, static IP, link events |
| TCP uplink | `rpi_camera_tcp.c` | JPEG encode, packet build, HMAC, send/ACK, spool integration |
| Spool | `rpi_camera_spool.c` | Store failed packets on FAT; FIFO flush |
| Health | `rpi_camera_health.c` | Task watchdog, boot counter, recovery restart |
| Protocol | `rpi_camera_packet.h` | Wire format definitions |
| Config | `rpi_camera_config.h`, `Kconfig.projbuild` | Constants and menuconfig |
| PC receiver | `tcp_receiver.py` | Validate, ACK, save images |

---

## Runtime flow

1. **Boot** — Initialize Ethernet, wait for IP (static or DHCP).
2. **Health** — Log reset reason; increment boot counter in NVS; arm task watchdog (60 s).
3. **Camera** — Enable LDO, init OV5647, CSI (RAW8), ISP (RGB565), start streaming.
4. **TCP / spool** — Init NVS counters, JPEG engine, mount `/spool`, try TCP connect to receiver.
5. **Warmup** — Skip uploads for `RPI_CAM_WARMUP_SEC` (8 s) while AE/ISP settle.
6. **Main loop** (every frame):
   - Feed watchdog
   - Receive CSI frame → ISP → RGB565 in PSRAM
   - Maintain TCP connection; flush **one** spooled packet if pending
   - Every `RPI_SNAPSHOT_INTERVAL_SEC`: encode JPEG, build packet, send or spool
7. **On link down** — Close TCP socket; mark network not ready; reapply static IP safely on link up.

Send outcomes:

| Result | Meaning |
|--------|---------|
| `ESP_OK` | Packet delivered; receiver ACK OK; sequence saved to NVS |
| `ESP_ERR_NOT_FINISHED` | TCP failed; packet **saved to spool** (not lost) |
| `ESP_FAIL` | TCP and spool both failed; packet **lost** |

The camera loop **never blocks** waiting for the network: on failure it waits until the next interval.

---

## Camera pipeline

1. **Sensor:** OV5647 configured for `MIPI_2lane_24Minput_RAW8_800x640_50fps`.
2. **CSI:** RAW8 in/out (required on ESP32-P4 rev &lt; 3.0 for this path).
3. **ISP:** Converts RAW8 → RGB565; brightness boost via `RPI_ISP_BRIGHTNESS` (default 80).
4. **Buffer:** Frame in PSRAM, cache-aligned for DMA.
5. **Synchronization:** Frame completion signaled by `on_trans_finished` + semaphore (not `on_get_new_trans`, which would block after the first frame).

JPEG encoding uses the ESP JPEG driver (quality 85, YUV420 subsampling). Typical JPEG size ~160 KB.

**Temperature** in the packet is currently a **placeholder** (random 15–40 °C). Replace with a real sensor reading for production.

**Timestamp** is milliseconds since boot (`esp_timer_get_time()`), not UTC. Add SNTP for wall-clock time if needed.

---

## Ethernet and networking

- **PHY:** Generic driver, address 1, reset GPIO 51.
- **Clock:** RMII external clock on GPIO 50.
- **Static IP (default):** Applied on `ETHERNET_EVENT_CONNECTED`. Safe to call again on reconnect (`ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED` is ignored).
- **Link down:** Clears ready flag and closes the TCP socket so the next send reconnects cleanly.

For a **managed network with DHCP**, disable static IP in menuconfig: **Raspberry Pi Camera → Use static IPv4 on Ethernet**.

---

## TCP protocol

### Packet layout (network byte order)

Magic `"RPCM"` (`0x5250434D`), version 1.

| Field | Type | Description |
|-------|------|-------------|
| magic | uint32 | `0x5250434D` |
| version | uint16 | `1` |
| header_size | uint16 | Size of header struct |
| node_id | uint32 | Node identifier |
| sequence | uint32 | Monotonic per device |
| timestamp_ms | uint64 | ms since ESP boot |
| temperature_centi_c | int16 | °C × 100 |
| reserved | uint16 | 0 |
| jpeg_size | uint32 | Payload length |
| jpeg_crc32 | uint32 | CRC32 of JPEG bytes |
| hmac_sha256 | 32 bytes | HMAC-SHA256(PSK, header with hmac zeroed ∥ jpeg) |

Wire format: **header immediately followed by JPEG bytes** on the same TCP stream.

### ACK (12 bytes)

| Field | Type |
|-------|------|
| magic | uint32 (`RPCM`) |
| sequence | uint32 (must match packet) |
| status | uint32 (`0` = OK) |

Non-zero status codes on the receiver: bad magic, version, header, size, CRC, HMAC, internal error.

### Connection behaviour

- One **persistent TCP socket** to `RPI_TCP_SERVER_IP:RPI_TCP_SERVER_PORT`.
- Reconnect every 5 s if down (`RPI_TCP_CONNECT_RETRY_SEC`).
- Connect timeout 3 s; send timeout 8 s; ACK recv timeout 4 s; up to 3 retries per packet.
- PSK in firmware must **exactly match** `tcp_receiver.py --psk`.

---

## Local spool (store-and-forward)

When TCP upload fails after all retries:

1. The full wire packet (header + JPEG) is written to flash: `/spool/s00000022.pkt`
2. The **sequence number is kept** (not rolled back).
3. Lifetime failure counter in NVS is incremented.

When the network is available again:

- Each frame loop calls `rpi_camera_tcp_process_spool()` which tries to send **one** oldest spooled file (FIFO by sequence).
- On ACK success, the file is deleted and NVS sequence is updated if higher than before.

**Limits:**

| Setting | Default | Meaning |
|---------|---------|---------|
| Partition | 8 MB FAT | `spool` in `partitions.csv` |
| `RPI_SPOOL_MAX_FILES` | 24 | ~24 snapshots (~4 min at 10 s if full) |
| When full | Drop oldest | Then store new failure |

---

## Health, watchdog, and auto-recovery

| Feature | Default | Action |
|---------|---------|--------|
| Task WDT timeout | 60 s | Main loop must call `rpi_camera_health_feed()` each frame |
| Max frame timeouts | 120 | ~6 min of bad frames → `esp_restart()` |
| Max camera receive errors | 30 | → `esp_restart()` |
| Boot counter | NVS `rpi_health` | Logged every boot |
| Reset reason | Logged | power-on, panic, task_wdt, brownout, etc. |
| Init failure (no IP, camera) | — | Log, delay 2 s, `esp_restart()` |

This prevents permanent hang after PC sleep, cable glitches, or CSI stalls.

---

## NVS persistence

| Namespace | Key | Content |
|-----------|-----|---------|
| `rpi_tcp` | `last_seq` | Last ACK’d sequence number |
| `rpi_tcp` | `send_fail` | Lifetime TCP/spool failure count |
| `rpi_health` | `boot_count` | Total boot counter |

Sequence is saved only after a successful ACK (live send or spool flush).

---

## Flash partition layout

Custom table in `partitions.csv`:

| Partition | Type | Size |
|-----------|------|------|
| nvs | data | 24 KB |
| phy_init | data | 4 KB |
| factory | app | 4 MB |
| spool | FAT | 8 MB |

Changing the partition table requires **`idf.py erase-flash`** before the first flash with the new layout.

---

## Configuration

### Header: `main/rpi_camera_config.h`

Key defines:

```c
RPI_SNAPSHOT_INTERVAL_SEC    // seconds between uploads (default 10)
RPI_CAM_WARMUP_SEC           // skip uploads after boot (default 8)
RPI_NODE_ID                  // default 1
RPI_TCP_SERVER_IP            // default "192.168.1.10"
RPI_TCP_SERVER_PORT          // default 9000
RPI_TCP_PSK                  // must match Python receiver
RPI_SPOOL_MAX_FILES          // default 24
RPI_HEALTH_*                 // watchdog thresholds
```

### Menuconfig

`idf.py menuconfig` → **Raspberry Pi Camera**:

- TCP receiver IP / port / node ID
- Static Ethernet IP, gateway, netmask

### SDK defaults

`sdkconfig.defaults` — target ESP32-P4, PSRAM, Ethernet, custom partition table, task WDT.

For boards with chip rev **&lt; 3.0**, build with:

```text
sdkconfig.defaults;sdkconfig.defaults.pre_v3
```

---

## Building and flashing

```bash
cd C:\Users\orreg\Documents\ESP\04_freertos_tasks

# First time with spool partition (or after partition change):
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash

idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" build flash monitor
```

Expected boot log (abbreviated):

```text
I rpi_eth: Ethernet link up
I rpi_eth: static IP 192.168.1.20 gw 192.168.1.10 mask 255.255.255.0
I rpi_spool: mounted /spool — 0 packet(s) pending
I rpi_health: reset reason: power-on
I rpi_health: boot count: 1
I rpi_tcp: restored sequence from NVS: ...
I rpi_tcp: connected to receiver 192.168.1.10:9000
```

---

## PC receiver setup

### Network

On the **Ethernet adapter** used for the direct cable:

- IP: `192.168.1.10`
- Mask: `255.255.255.0`
- Gateway: leave empty (or `192.168.1.20`)

Disable sleep on that adapter (Power Management → do not turn off device).

Allow **TCP port 9000** through the firewall for Python.

### Run receiver

```bash
python tcp_receiver.py --bind 0.0.0.0 --port 9000 ^
  --psk "CHANGE_ME_TO_RANDOM_32PLUS_BYTES" ^
  --client-timeout 30 ^
  --verbose-failures
```

Use `--client-timeout` **greater than** the ESP send interval (e.g. **30 s** for a 10 s interval) so idle connections are not closed between snapshots.

### Output

Files under `received_packets/node_<id>/`:

- `latest.jpg` / `latest.json`
- `seq_<n>.jpg` / `seq_<n>.json` (archive)

Diagnostics print every 15 s: packet count, connect/disconnect counts, errors.

---

## Master / slave control (Phase 1)

The ESP is a **slave**: it boots to **IDLE** and does **not** capture until the PC master sends **START**.

| Port | Direction | Tool |
|------|-----------|------|
| **9001** | PC → ESP (control) | `tcp_master.py` |
| **9000** | ESP → PC (telemetry) | `tcp_receiver.py` |

### Slave states

`BOOT` → `NET_INIT` → **`IDLE`** → (master START) → `CAM_INIT` → `WARMUP` → **`RUNNING`**

- **STOP** → `PAUSED` (camera runs, no uploads)
- **START** from `PAUSED` → `RUNNING`
- **RESTART** → reboot

While connected, the slave sends a **STATE heartbeat** every 3 s on the control socket.

### Master commands

```bash
python tcp_master.py --host 192.168.1.20 status
python tcp_master.py --host 192.168.1.20 start
python tcp_master.py --host 192.168.1.20 stop
python tcp_master.py --host 192.168.1.20 restart
python tcp_master.py --host 192.168.1.20 monitor
```

### Typical session

1. Flash ESP; PC Ethernet `192.168.1.10`, ESP `192.168.1.20`.
2. `python tcp_receiver.py --psk "..." --client-timeout 30`
3. `python tcp_master.py status` → `IDLE`
4. `python tcp_master.py start` → `WARMUP` then `RUNNING`
5. Photos on port 9000 every 10 s.

Protocol magic: **`RPCC`** (`rpi_camera_control_packet.h`). OTA is a later phase.

---

## Source files

```
04_freertos_tasks/
├── APPLICATION.md          ← this document
├── partitions.csv          ← flash layout (spool partition)
├── sdkconfig.defaults
├── sdkconfig.defaults.pre_v3
├── tcp_receiver.py         ← PC telemetry receiver (:9000)
├── tcp_master.py           ← PC master control CLI (:9001)
├── CMakeLists.txt
└── main/
    ├── rpi_camera_control.c      ← state machine
    ├── rpi_camera_ctrl_server.c  ← TCP listen :9001
    ├── rpi_camera_control_packet.h
    ├── rpi_camera_main.c       ← camera task, slave boot
    ├── rpi_camera_eth.c    ← Ethernet
    ├── rpi_camera_tcp.c    ← TCP, JPEG, NVS seq/fail
    ├── rpi_camera_spool.c  ← flash spool
    ├── rpi_camera_health.c ← WDT, recovery
    ├── rpi_camera_config.h
    ├── rpi_camera_packet.h
    ├── rpi_camera_net.h
    ├── rpi_camera_tcp.h
    ├── rpi_camera_spool.h
    ├── rpi_camera_health.h
    ├── Kconfig.projbuild
    └── CMakeLists.txt
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `invalid RXD0 GPIO number` | Swapped RMII RXD pins | RXD0=29, RXD1=30 |
| Stuck after `Ethernet link up`, no IP | No DHCP on direct cable | Enable static IP; PC at .10, ESP at .20 |
| `connect 192.168.1.10:9000 failed` | Receiver not running or wrong PC IP | Start `tcp_receiver.py`; set adapter to .10 |
| `[DIAG] packets=0 connects=0` | ESP never reached PC | Check cable, IP, firewall, ping 192.168.1.20 |
| `send failed` then reconnect every ~10 s | PC sleep or receiver down | Disable sleep; run receiver as service |
| ESP reboot on link up after sleep | Fixed: DHCP already stopped | Flash latest `rpi_camera_eth.c` |
| `BAD_HMAC` on receiver | PSK mismatch | Same string in firmware and `--psk` |
| Duplicate seq after ESP reset | Expected if old seq reused | Receiver can ignore seq ≤ last seen |
| Spool full warnings | Long outage | Increase `RPI_SPOOL_MAX_FILES` or fix network |
| `task_wdt` reset | Main loop blocked | Check camera hang; thresholds in config |
| Slave stays IDLE, no photos | Master never sent START | `python tcp_master.py start` |
| `INVALID_STATE` on start | Wrong state | `status` first; use `stop` then `start` |

---

## Future improvements (not implemented)

- SNTP for real UTC timestamps
- Real temperature sensor
- TLS or encrypted payload
- OTA firmware updates
- Receiver duplicate-sequence handling and disk rotation policy
- Reduce NVS write frequency for flash wear

---

*ESP-IDF 6.x, ESP32-P4, Waveshare DEV-KIT, OV5647 MIPI camera.*
