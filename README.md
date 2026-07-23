# ESP32-C3 MicroLink Wake-on-LAN

ESP-IDF firmware that joins a local Wi-Fi network, connects an ESP32-C3 to a
Tailscale tailnet through MicroLink, and exposes a Wake-on-LAN HTTP endpoint
bound only to the device's Tailscale IP.

## Flow

```text
iPhone / PC with Tailscale
          |
          | POST /wol over the private tailnet
          v
ESP32-C3 + MicroLink
          |
          | UDP Magic Packet to local subnet broadcast
          v
Wake-on-LAN computer
```

## Important limitation

MicroLink lists ESP32-C3 as compatible but untested. The C3 has no PSRAM, so
this project uses the minimum 64 KiB coordination/JSON buffers and four active
peers. This is intended for a small tailnet (roughly 30 peers or fewer). Runtime
validation on the actual board is still required.

## Prerequisites

- ESP-IDF 5.0 or newer.
- ESP32-C3 with at least 4 MiB flash.
- A reusable Tailscale auth key. Prefer a tagged, preauthorized key restricted
  with tailnet ACL/grants.
- Wake-on-LAN enabled in the target computer BIOS/UEFI and operating system.
- ESP32-C3 and target computer on the same IPv4 subnet.

## Configuration

Edit the tracked `sdkconfig.credentials` file and set:

- Wi-Fi SSID and password.
- Tailscale auth key and device name.
- Target computer MAC address.
- A random bearer token with at least 16 characters.

`sdkconfig.credentials` is intentionally tracked with all sensitive values
empty. Before every commit or push, restore the SSID, Wi-Fi password, Tailscale
auth key, target MAC and bearer token to empty strings. The generated
`sdkconfig` remains ignored by Git because it contains the merged values used
by the build.

## Build and flash

```bash
source /home/eduardo/tools/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

After registration, note the `100.x.y.z` address printed in the serial log.

## Use from a PC

Health check:

```bash
curl http://100.x.y.z:8080/health
```

Send the Wake-on-LAN command:

```bash
curl -X POST \
  -H 'Authorization: Bearer YOUR_WOL_SHARED_SECRET' \
  http://100.x.y.z:8080/wol
```

Expected response:

```json
{"status":"magic_packet_sent"}
```

## Use from iPhone

With Tailscale connected, create an Apple Shortcut using `Get Contents of URL`:

- URL: `http://100.x.y.z:8080/wol`
- Method: `POST`
- Header: `Authorization`
- Value: `Bearer YOUR_WOL_SHARED_SECRET`

Keep the shortcut private because it contains the bearer token.

## Security

- The HTTP socket binds specifically to the MicroLink VPN address, not to the
  Wi-Fi/LAN address.
- `POST /wol` requires a bearer token in addition to tailnet membership.
- Restrict access further with Tailscale ACLs/grants so only your iPhone and
  selected PCs can reach this device and port.
- Do not commit a populated `sdkconfig.credentials`, `sdkconfig`, auth keys,
  Wi-Fi passwords, MAC addresses, or bearer tokens.

## Endpoints

- `GET /health` — confirms that the private service is running.
- `POST /wol` — validates the bearer token and sends the Magic Packet three
  times to UDP port 9 on the Wi-Fi subnet's directed broadcast address.

## Included third-party code

MicroLink and its `wireguard_lwip` dependency are vendored under `components/`
from upstream commit `216da3300f0493b0860247d43f7af5ce29df63a5`.
MicroLink is licensed under MIT; upstream license files are retained.
