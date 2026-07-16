# ELRS Netpack

[![Build Status](https://github.com/i-am-grub/elrs-netpack/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/i-am-grub/elrs-netpack/actions/workflows/build.yml)

> [!IMPORTANT]
> This project **is not** officially affiliated or supported by the ExpressLRS
> organization. They do not have an obligation to provide help or support to you
> if you plan to utilize this project.

The ELRS Netpack is firmware for Ethernet-equipped ESP32 development
boards to support interfacing with ExpressLRS backpack compatible
devices. This device is designed to act as the equivalent of the timer
backpack, but instead of interfacing with the host device over a serial
connection, a tcp socket connection is used instead.

Supported boards:

| Board | Chip | Ethernet | PoE |
| --- | --- | --- | --- |
| [Waveshare ESP32-S3-ETH](https://www.waveshare.com/esp32-s3-eth.htm) | ESP32-S3 | W5500 (SPI) | PoE variant available |
| [Olimex ESP32-POE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/) (incl. `-EA`, and the non-ISO ESP32-POE) | ESP32-WROOM-32 | LAN8710A/LAN8720 (RMII) | Yes (isolated on ISO) |

> [!TIP]
> If you have a **PoE (Power-over-Ethernet) switch**, use a PoE-capable board —
> a single Ethernet cable then both powers the netpack and connects it to the
> network, with no separate USB power supply needed at the field. Without PoE,
> power the board over its USB port.

Since the Waveshare board uses the W5500 ethernet chip, the newest versions of
ESP-IDF are used directly. The W5500 is not currently supported by the Arduino
ESP32 versions included in PlatformIO.

> [!NOTE]
> Support for the W5500 was added in v5.0 of ESP-IDF, PlatformIO is limited to
> version v2.0.17 of Arduino ESP32, which is based on v4.4.7 of ESP-IDF. While
> newer versions of Arduino ESP32 based on the latest version of ESP-IDF
> exist, PlatformIO does not officially support them.

## Firmware Installation

To install the ELRS Netpack firmware, use the [Netpack Installer](https://github.com/i-am-grub/netpack-installer) plugin for RotorHazard.

> [!NOTE]
> This firmware is the **network** option — a backpack the timing computer
> reaches over Ethernet. If your ELRS Timer Backpack connects to the timing
> computer over **USB** instead, you don't need this project; see
> [VRxC_ELRS](https://github.com/bob9/VRxC_ELRS) for the USB backpack and
> HDZero goggle setup.

### Updating over the network (OTA) — no USB needed

A Netpack that is already running this firmware can update itself over
Ethernet:

1. Browse to **`http://elrs-netpack.local/`** (or `http://<the-device-ip>/`).
2. Drag the `…-ota.bin` file from the [Releases](../../releases) page onto the
   page (or click to browse for it) and press **Install firmware**.
3. The device verifies the image, writes it to the standby OTA slot, and
   reboots into it. Network settings are kept.

> [!NOTE]
> OTA takes the **`…-ota.bin`** asset (the app image alone). The merged image
> is only for USB flashing — the page will reject it. Devices running firmware
> from before OTA support was added must be flashed **once over USB** (below)
> to receive the new partition layout; every update after that can be OTA.

### Flashing a prebuilt image

Grab the firmware from the [**Releases**](../../releases) page — each release
ships three assets **per supported board**; pick the ones whose name contains
your board's slug (`waveshare-esp32-s3-eth` or `olimex-esp32-poe-iso`):

- `elrs-netpack-<version>-<board>-merged.bin` — the bootloader, partition
  table and application **merged into one file**, flashed in one step at
  offset `0x0`. **This is the one you want for USB flashing.**
- `elrs-netpack-<version>-<board>-ota.bin` — the app image alone, for the
  drag-and-drop **network update page** (see above).
- `elrs-netpack-<version>-<board>-binaries.zip` — the individual binaries
  (bootloader / partition table / OTA data / app) with their flash offsets,
  for advanced use.

(Development builds of the same merged images are also produced by every CI
run on `main` — see the GitHub Actions build artifacts.)

How the board connects to your computer differs:

- **Waveshare ESP32-S3-ETH** uses the chip's **native USB**, so on Windows
  10/11, macOS and Linux it appears as a serial port with **no driver to
  install** — just connect the board's **USB-C** port. esptool chip name:
  `esp32s3`; typical port: `COMx` / `/dev/cu.usbmodem*` / `/dev/ttyACM0`.
- **Olimex ESP32-POE-ISO** connects over its **micro-USB** port through a
  CH340 USB-serial converter — Windows and macOS may need the
  [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_ZIP.html) (Linux
  has it built in). esptool chip name: `esp32`; typical port: `COMx` /
  `/dev/cu.wchusbserial*` / `/dev/ttyUSB0`.

> [!WARNING]
> On the **non-isolated** Olimex ESP32-POE, never have PoE Ethernet and USB
> connected at the same time — it can damage the board and your computer. The
> **ISO** variants are isolated and safe.

The command examples below use the Waveshare board — for the Olimex,
substitute `--chip esp32` and its port name.

#### Option A — Web flasher (easiest; Windows, macOS &amp; Linux, nothing to install)

1. Open [espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/)
   in **Chrome or Edge** (Web Serial is not available in Safari or Firefox).
2. Connect the board's USB-C port to your computer.
3. Click **Connect** and select the board's port (see the port names under
   Option B if you're not sure which one it is).
4. Set **Flash Address** to `0x0`, choose the merged `elrs-netpack-merged.bin`,
   and click **Program**. When it finishes, the board reboots into the firmware.

#### Option B — esptool (command line)

Install esptool once — it needs Python 3:

- **Windows:** `pip install esptool`
- **macOS:** `pip3 install esptool` (or `brew install esptool`)
- **Linux:** `pipx install esptool` (or `pip3 install --user esptool`; on
  Debian/Ubuntu, `sudo apt install esptool` also works)

**Windows**

1. Find the port: open **Device Manager → Ports (COM &amp; LPT)** and note the
   board's `COMx` (it shows as *USB Serial Device*), e.g. `COM4`.
2. Flash:

   ```
   esptool --chip esp32s3 --port COM4 --baud 921600 write_flash 0x0 elrs-netpack-merged.bin
   ```

**macOS**

1. Find the port — list it with:

   ```
   ls /dev/cu.usbmodem*
   ```

   (e.g. `/dev/cu.usbmodem101`).
2. Flash:

   ```
   esptool.py --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 write_flash 0x0 elrs-netpack-merged.bin
   ```

**Linux**

1. Give yourself permission to use serial ports (once, then log out and back
   in):

   ```
   sudo usermod -aG dialout $USER
   ```

   (On Arch-based distros the group is `uucp` instead of `dialout`.)
2. Find the port — the board shows up as a USB CDC device:

   ```
   ls /dev/ttyACM*
   ```

   (e.g. `/dev/ttyACM0`).
3. Flash:

   ```
   esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x0 elrs-netpack-merged.bin
   ```

> [!TIP]
> If the flasher can't connect, hold the board's **BOOT** button while plugging
> it in (or while clicking Connect), then release — this forces the ESP32 into
> download mode. After flashing, the board reboots into the firmware
> automatically.

#### Flashing the individual binaries (advanced)

The `…-binaries.zip` release asset contains the parts the merged image is
built from, plus a `README.txt` with the exact command for that board. The
bootloader offset differs by chip — `0x0` on the ESP32-S3 (Waveshare), `0x1000`
on the classic ESP32 (Olimex):

```
# Waveshare ESP32-S3-ETH
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 elrs-netpack.bin

# Olimex ESP32-POE-ISO
esptool.py --chip esp32 --port <PORT> --baud 921600 write_flash \
  0x1000 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 elrs-netpack.bin
```

(`ota_data_initial.bin` resets the OTA slot selector so the bootloader starts
the app you just flashed to `ota_0` at `0x20000`, rather than an old image
left in the other slot.)

### Releases (maintainers)

**Every commit pushed to `main` is released automatically** — CI builds the
firmware for every supported board and publishes a release versioned
`v1.0.<build-number>` with each board's merged image + individual binaries
attached. Boards are defined by the `sdkconfig.board.<slug>` files and the
matrix in `.github/workflows/build.yml`; to build one locally:

```
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.olimex-esp32-poe-iso" set-target esp32 build
```

To cut a milestone release under a specific version instead, push a tag:

```
git tag v2.0.0
git push bob9 v2.0.0
```

## Network Configuration

The netpack connects to your network over its Ethernet port and serves
the MSP socket on TCP port `8080` (configurable). A race timer / venue agent
connects to `<netpack-ip>:8080`. The device also serves its **firmware update
page** on port `80` — browsing to `http://<netpack-ip>/` (or
`http://elrs-netpack.local/`) shows the device's version and lets you install
updates over the network.

There are two ways the board can get its IP address:

- **DHCP** — the default in a clean build. The board requests an address from
  your router; find it in the router's client list or via mDNS at
  `elrs-netpack.local`.
- **Static IP** — pin the board to a fixed address. This is more reliable for a
  race timer that always connects to the same IP.

### Setting a static IP — no rebuild needed

Network settings are stored on the device and can be changed at any time over
the **USB port** (the same one used for flashing) with any serial terminal —
no toolchain or rebuild required.

1. Connect the board's USB port to your computer.
2. Open a serial terminal on the board's port (115200 baud on the Olimex; any
   baud rate works on the Waveshare's native USB),
   e.g. `idf.py monitor`, `screen /dev/ttyACM0`, PuTTY, or the serial console
   in a Chrome-based web tool such as
   [serial.huhn.me](https://serial.huhn.me/).
3. At the `netpack>` prompt, type:

   ```
   netconfig static 192.168.1.50
   reboot
   ```

Available commands:

| Command | Effect |
| --- | --- |
| `netconfig` | Show the saved settings and the current IP |
| `netconfig static <ip> [netmask] [gateway]` | Use a static IP address (netmask defaults to `255.255.255.0`, gateway to `.1` of the subnet) |
| `netconfig dhcp` | Go back to DHCP (the default) |
| `timeconfig` | Show the time settings and current local time |
| `timeconfig tz <posix-tz>` | Set the timezone, e.g. `timeconfig tz AEST-10` |
| `timeconfig server <host\|ip>` | Set the NTP server (default `pool.ntp.org`; reboot to apply) |
| `reboot` | Restart the board to apply saved settings |
| `help` | List all commands |

Settings persist across reboots and reflashes (they live in the NVS data
partition, which flashing the app does not erase). `esptool.py erase_flash`
resets them to defaults.

> [!NOTE]
> Choose a static IP that is on the **same subnet** as the computer running the
> timer, is **outside your router's DHCP pool**, and does not collide with
> another device. On boot the board logs the address it is using on the serial
> console.

For developers building from source, the defaults used when nothing has been
configured over the console can be baked in under **`TCP Socket Server
options`** in `idf.py menuconfig` (`USE_STATIC_IP`, `STATIC_IP_ADDR`,
`STATIC_NETMASK`, `STATIC_GATEWAY`, `TCP_SERVER_PORT`).

## Goggle Test Page

The netpack serves a **test page on port 80** — browse to `http://<netpack-ip>/`
(or `http://elrs-netpack.local/`). Type a pilot's **ELRS bind phrase** (the UID
is derived exactly as the ELRS Configurator does it) and fire test messages at
their goggles to verify the whole link before racing:

- **OSD message** — shows centred text on their OSD (plus a Clear button)
- **Channel change** — Raceband, F/E, and Low Band channels (Low Band needs
  goggle firmware with remote band switching)
- **Time sync** — sets their goggle clock from the netpack clock
- **DVR name** — labels their next DVR recording

Messages go through the normal ESPNOW send path, so a working test means OSD,
channel changes and the rest will work for that pilot on race day.

## Goggle Clock Synchronization

The netpack keeps the clock of bound VRX backpacks (e.g. HDZero goggles) in
sync so DVR recordings carry correct timestamps. It syncs its own clock over
NTP, then sends the time over ESP-NOW (`MSP_ELRS_BACKPACK_SET_RTC`) when a
goggle backpack boots and every 60 seconds after that. On HDZero goggles the
backpack sets both the Linux OS clock and the hardware RTC, so recording
names and file timestamps come out right.

Set your timezone once over the serial console (the goggles expect local wall
time):

```
timeconfig tz AEST-10
```

Timezones use the POSIX `TZ` format — e.g. `AEST-10` (Brisbane, no DST),
`AEST-10AEDT,M10.1.0,M4.1.0/3` (Sydney/Melbourne), `NZST-12NZDT,M9.5.0,M4.1.0/3`
(New Zealand). Note the sign is inverted relative to UTC offsets: UTC+10 is
written `-10`.

If the venue network has no internet access, point the netpack at a local NTP
server (e.g. the machine running the race timer) with
`timeconfig server <ip>`. Alternatively, anything connected to the TCP socket
can push an `MSP_ELRS_BACKPACK_SET_RTC` packet with 6 payload bytes (years
since 1900, month 0-11, day, hour, minute, second in local time) and it is
forwarded to the goggles as-is. A TCP client that sends the time (the
`dd-pits` venue agent does this automatically) is treated as **authoritative**:
the netpack seeds its own clock from it and pauses its NTP-based broadcasts
while the client keeps sending, so the two sources never fight — with dd-pits
connected, no `timeconfig` setup is needed at all.

> [!NOTE]
> The goggles-side handling requires the ExpressLRS Backpack firmware on the
> goggles to process `SET_RTC` received over ESP-NOW (recent firmware built
> from the Backpack repo with this support).

## 3D-Printable Case by [Hazard Creative](https://github.com/HazardCreative)

[![3D-Printable Case for RH+ELRS Netpack](resources/3d-case/case-photo.jpg)](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)

The resources for a 3D printable case for the development board can be found [here](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)