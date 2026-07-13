# ELRS Netpack

[![Build Status](https://github.com/i-am-grub/elrs-netpack/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/i-am-grub/elrs-netpack/actions/workflows/build.yml)

> [!IMPORTANT]
> This project **is not** officially affiliated or supported by the ExpressLRS
> organization. They do not have an obligation to provide help or support to you
> if you plan to utilize this project.

The ELRS Netpack is firmware for the 
[Waveshare ESP32-S3 Ethernet](https://www.waveshare.com/esp32-s3-eth.htm)
development board to support interfacing with ExpressLRS backpack
compatible devices. This device is designed to act as the equivalent
of the timer backpack, but instead of interfacing with the host
device over a serial connection, a tcp socket connection is used
instead.

> [!TIP]
> If you have a **PoE (Power-over-Ethernet) switch**, buy the **PoE version** of
> the [Waveshare ESP32-S3-ETH](https://www.waveshare.com/esp32-s3-eth.htm) board.
> A single Ethernet cable then both powers the netpack and connects it to the
> network — no separate USB power supply needed at the field. Without PoE, power
> it over its USB-C port.

Since this board uses W5500 ethernet chip, the newest versions of ESP-IDF 
are used directly. The W5500 is not currently supported by the Arduino ESP32 
versions included in PlatformIO.

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

### Flashing a prebuilt image

Grab the firmware from the [**Releases**](../../releases) page — each release is
named after the board it supports (e.g. *ELRS Netpack v1.0.0 — Waveshare
ESP32-S3-ETH*) and ships two assets:

- `elrs-netpack-<version>-waveshare-esp32-s3-eth-merged.bin` — the bootloader,
  partition table and application **merged into one file**, flashed in one step
  at offset `0x0`. **This is the one you want.**
- `elrs-netpack-<version>-waveshare-esp32-s3-eth-binaries.zip` — the individual
  binaries (bootloader / partition table / app) with their flash offsets, for
  advanced use.

(Development builds of the same merged image are also produced by every CI run
on `main` — see the GitHub Actions build artifacts.)

The Waveshare ESP32-S3 uses the chip's **native USB**, so on Windows 10/11,
macOS and Linux it appears as a serial port with **no driver to install** —
just connect the board's **USB-C** port to your computer.

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

The `…-binaries.zip` release asset contains the three parts the merged image is
built from. Flash them at these offsets (same command on every OS, adjust the
port):

```
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 elrs-netpack.bin
```

### Releases (maintainers)

**Every commit pushed to `main` is released automatically** — CI builds the
firmware and publishes a release versioned `v1.0.<build-number>`, named after
the supported hardware, with the merged image + individual binaries attached.

To cut a milestone release under a specific version instead, push a tag:

```
git tag v2.0.0
git push bob9 v2.0.0
```

## Network Configuration

The netpack connects to your network over its Ethernet (W5500) port and serves
the MSP socket on TCP port `8080` (configurable). A race timer / venue agent
connects to `<netpack-ip>:8080`.

There are two ways the board can get its IP address:

- **DHCP** — the default in a clean build. The board requests an address from
  your router; find it in the router's client list or via mDNS at
  `elrs-netpack.local`.
- **Static IP** — pin the board to a fixed address. This is more reliable for a
  race timer that always connects to the same IP.

### Setting a static IP — no rebuild needed

Network settings are stored on the device and can be changed at any time over
the **USB-C port** (the same one used for flashing) with any serial terminal —
no toolchain or rebuild required.

1. Connect the board's USB-C port to your computer.
2. Open a serial terminal on the board's port (any baud rate works over USB),
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
forwarded to the goggles as-is.

> [!NOTE]
> The goggles-side handling requires the ExpressLRS Backpack firmware on the
> goggles to process `SET_RTC` received over ESP-NOW (recent firmware built
> from the Backpack repo with this support).

## 3D-Printable Case by [Hazard Creative](https://github.com/HazardCreative)

[![3D-Printable Case for RH+ELRS Netpack](resources/3d-case/case-photo.jpg)](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)

The resources for a 3D printable case for the development board can be found [here](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)