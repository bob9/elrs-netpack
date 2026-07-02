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

### Setting a custom static IP

The static IP is defined under **`TCP Socket Server options`** in the project
config. Set it either with `menuconfig` or by editing `sdkconfig` directly, then
rebuild and flash.

**Option A — `idf.py menuconfig` (recommended):**

```
idf.py menuconfig
```

Open **`TCP Socket Server options`** and set:

| Option | Example |
| --- | --- |
| `Use static IP address instead of DHCP` | enabled |
| `Static IP address` | `192.168.1.50` |
| `Static netmask` | `255.255.255.0` |
| `Static gateway` | `192.168.1.1` |
| `TCP server listening port` | `8080` |

Save, then build and flash:

```
idf.py build flash
```

**Option B — edit `sdkconfig` directly**, then `idf.py build flash`:

```
CONFIG_USE_STATIC_IP=y
CONFIG_STATIC_IP_ADDR="192.168.1.50"
CONFIG_STATIC_NETMASK="255.255.255.0"
CONFIG_STATIC_GATEWAY="192.168.1.1"
CONFIG_TCP_SERVER_PORT=8080
```

To switch back to DHCP, set `Use static IP address instead of DHCP` to disabled
(`CONFIG_USE_STATIC_IP` unset / `n`) and reflash.

> [!NOTE]
> Choose a static IP that is on the **same subnet** as the computer running the
> timer, is **outside your router's DHCP pool**, and does not collide with
> another device. The board defaults to `192.168.1.195`. On boot it logs the
> address it is using (`Static IP: …`) on the serial console.

## 3D-Printable Case by [Hazard Creative](https://github.com/HazardCreative)

[![3D-Printable Case for RH+ELRS Netpack](resources/3d-case/case-photo.jpg)](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)

The resources for a 3D printable case for the development board can be found [here](https://github.com/i-am-grub/elrs-netpack/tree/main/resources/3d-case)