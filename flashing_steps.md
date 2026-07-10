# Flashing guide — 10 camera slaves (USB first flash)

Step-by-step workflow to provision **10 ESP32-P4 slave boards**, each with a **unique node ID** and **static IP**. The **first flash on every board is always via USB**; later updates use **Ethernet OTA**.

---

## Network plan

| Role | IP | Notes |
|------|-----|--------|
| PC (receiver) | `192.168.1.10` | Runs `tcp_receiver.py` |
| Slave node *N* | `192.168.1.(19 + N)` | One IP per board |

| Node ID | IP address |
|---------|------------|
| 1 | `192.168.1.20` |
| 2 | `192.168.1.21` |
| 3 | `192.168.1.22` |
| 4 | `192.168.1.23` |
| 5 | `192.168.1.24` |
| 6 | `192.168.1.25` |
| 7 | `192.168.1.26` |
| 8 | `192.168.1.27` |
| 9 | `192.168.1.28` |
| 10 | `192.168.1.29` |

All slaves send telemetry to **`192.168.1.10:9000`**.  
All listen for control on **`:9001`** and OTA on **`:3232`** on their own IP.

**Use an Ethernet switch** for normal operation (PC + all ESPs). A direct cable to the PC only works for one board at a time.

---

## One-time PC setup

1. Set the PC Ethernet adapter to **`192.168.1.10`**, subnet mask **`255.255.255.0`**.
2. Open a terminal in the project folder:
   ```powershell
   cd C:\Users\orreg\Documents\ESP\04_freertos_tasks
   ```
3. Use the same PSK on the PC as in firmware (`RPI_TCP_PSK` in `main/rpi_camera_config.h` or menuconfig).

### Build profiles (sdkconfig overlays)

| Node | Overlay file | IP |
|------|--------------|-----|
| 1 | *(default `sdkconfig.defaults` only)* | `.20` |
| 2 | `sdkconfig.defaults.slave2` | `.21` |
| 3 | `sdkconfig.defaults.slave3` | `.22` |
| 4 | `sdkconfig.defaults.slave4` | `.23` |
| 5 | `sdkconfig.defaults.slave5` | `.24` |
| 6 | `sdkconfig.defaults.slave6` | `.25` |
| 7 | `sdkconfig.defaults.slave7` | `.26` |
| 8 | `sdkconfig.defaults.slave8` | `.27` |
| 9 | `sdkconfig.defaults.slave9` | `.28` |
| 10 | `sdkconfig.defaults.slave10` | `.29` |

Each overlay file (except node 1) contains:

```ini
CONFIG_RPI_NODE_ID=<N>
CONFIG_RPI_ETH_STATIC_IP="192.168.1.<19+N>"
CONFIG_RPI_ETH_STATIC_GW="192.168.1.10"
CONFIG_RPI_ETH_STATIC_NETMASK="255.255.255.0"
```

Base defaults string (always include):

```
sdkconfig.defaults;sdkconfig.defaults.pre_v3
```

For node *N* > 1, append `;sdkconfig.defaults.slave<N>`.

---

## Per-device USB flash loop (repeat 10 times)

Do this for device 1, then 2, … through 10.

### Step A — Connect one board only

1. Connect **one** ESP to the PC via **USB** (note the COM port, e.g. `COM5`).
2. Power off or disconnect other boards so you do not flash the wrong device.

### Step B — Select the profile for this node

See the table above. Example for **node 5**:

```
SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slave5
```

### Step C — Clean config and build

**Important:** delete `sdkconfig` when switching node ID or IP so the new overlay is applied.

```powershell
cd C:\Users\orreg\Documents\ESP\04_freertos_tasks
Remove-Item sdkconfig -ErrorAction SilentlyContinue
```

**Node 1 — build:**

```powershell
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" build
```

**Node 5 — build (example):**

```powershell
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slave5" build
```

### Step D — First flash on this board (USB)

On each **brand-new board** (or first time with the OTA partition table), run **erase-flash once**, then flash.

**Node 1:**

```powershell
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" -p COM5 flash monitor
```

**Node 5 (example):**

```powershell
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slave5" erase-flash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slave5" -p COM5 flash monitor
```

Replace `COM5` with the actual port.

In the serial monitor, confirm lines like:

```text
I rpi_eth: static IP 192.168.1.24 gw 192.168.1.10 mask 255.255.255.0
I rpi_camera: slave IDLE on 192.168.1.24 — send START from master (tcp_master.py)
```

Press **Ctrl+]** to exit monitor.

### Step E — Quick check (recommended)

1. Connect the board to the **switch** with the PC (Ethernet, not only USB).
2. Ping: `ping 192.168.1.24` (use this node’s IP).
3. Control:
   ```powershell
   python tcp_master.py --host 192.168.1.24 status
   ```
   Expect `state=IDLE`.

### Step F — Label and store

Mark the board with **node ID** and **IP** (e.g. `Node 5 — 192.168.1.24`). Set it aside and repeat from **Step A** for the next device.

---

## Progress checklist

| # | Node ID | IP | COM port | Overlay file | Done |
|---|---------|-----|----------|--------------|------|
| 1 | 1 | 192.168.1.20 | | default | ☐ |
| 2 | 2 | 192.168.1.21 | | slave2 | ☐ |
| 3 | 3 | 192.168.1.22 | | slave3 | ☐ |
| 4 | 4 | 192.168.1.23 | | slave4 | ☐ |
| 5 | 5 | 192.168.1.24 | | slave5 | ☐ |
| 6 | 6 | 192.168.1.25 | | slave6 | ☐ |
| 7 | 7 | 192.168.1.26 | | slave7 | ☐ |
| 8 | 8 | 192.168.1.27 | | slave8 | ☐ |
| 9 | 9 | 192.168.1.28 | | slave9 | ☐ |
| 10 | 10 | 192.168.1.29 | | slave10 | ☐ |

---

## After all 10 are flashed

### Start the receiver (one process for all nodes)

```powershell
python tcp_receiver.py --psk "YOUR_PSK" --client-timeout 30
```

Images are stored under `received_packets/node_1/` … `received_packets/node_10/`.

### Start capture on each slave

```powershell
python tcp_master.py --host 192.168.1.20 start
python tcp_master.py --host 192.168.1.21 start
python tcp_master.py --host 192.168.1.22 start
# ... through 192.168.1.29
```

### Other master commands

```powershell
python tcp_master.py --host 192.168.1.20 status
python tcp_master.py --host 192.168.1.20 stop
python tcp_master.py --host 192.168.1.20 restart
python tcp_master.py --host 192.168.1.20 monitor
```

---

## Later firmware updates (Ethernet OTA, no USB)

Node ID and IP stay the same on each board. OTA does **not** change identity.

1. Bump `VERSION` in the root `CMakeLists.txt` for each release (optional but recommended).
2. Build once:
   ```powershell
   idf.py build
   ```
3. OTA to each device:
   ```powershell
   python tcp_master.py --host 192.168.1.20 ota
   python tcp_master.py --host 192.168.1.21 ota
   # ... one command per device
   ```

The `ota` command sends the OTA control command on port **9001**, then uploads `build/freertos_tasks.bin` to port **3232**.  
Use `--image <path>` only if the binary is not the default build output.

Standalone uploader (without control prep):

```powershell
python ota_tcp_upload.py 192.168.1.20
```

---

## Common mistakes

| Mistake | Result |
|---------|--------|
| Forgot `Remove-Item sdkconfig` when switching nodes | Every board gets the same node ID/IP |
| Two boards on USB at once | Wrong device flashed |
| Skipped `erase-flash` on new boards | OTA partition table missing; boot/spool issues |
| Two nodes with the same IP | IP conflict; only one works |
| All 10 on direct cable to PC without switch | Only one link at a time |

---

## Quick reference — copy/paste templates

**Node 1 flash:**

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" erase-flash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3" -p COM5 flash monitor
```

**Node N flash (N = 2..10):**

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slaveN" erase-flash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pre_v3;sdkconfig.defaults.slaveN" -p COM5 flash monitor
```

Replace `N` and `COM5` accordingly.
