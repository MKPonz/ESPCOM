# ESPCom — an ESP32 A-Com for Digimon virtual pets

A ESP32 port of the [DMComm](https://dmcomm.github.io/) A-Com,
built for the **Digital Monster Ver. 20th (DM20)**. It lets an ESP32 act as
another Digivice: run battles, transfer Copymon, and connect to the internet over WiFi.

The official builds target the Arduino Nano (A-Com) and Raspberry Pi Pico W
(WiFiCom). This one targets a plain ESP32 DevKitV1.

Verified working: OTHER battles, native 20th battles, Copymon send, and
app access over WiFi (w0rld.io and battle.nacatech.es tested).

## Hardware

Two resistors and two wires.

`GPIO33` (D33) is used because it is ADC1-capable (ADC2 conflicts with WiFi)
and can drive output.

## Build

1. Arduino IDE with ESP32 board support (Boards Manager → "esp32" by
   Espressif). Board: **ESP32 Dev Module**.
2. Library Manager → install **PubSubClient** (Nick O'Leary) and
   **ArduinoJson** v7 (Benoit Blanchon).
3. Open `secrets.h` and fill in the wificom.dev
   values. 
4. Upload. Baud Rate: **115200**.

### Generating DigiROMs

Set the toy to **Battle → 20th** for battles, or **Copymon → Get** for
`--copy`.

**First boot** — a freshly flashed device has no WiFi credentials, so it
comes up as its own access point, `ESPCom-Setup` (password `digimon2026`).
Join it from your phone's WiFi settings; browse to `http://192.168.4.1`. Enter your network
name and password, save, and it restarts onto your WiFi.

**If it can't join** — it falls back to `ESPCom-Setup` after about 15
seconds. While parked there with credentials on file it retries the real
network every two minutes, but only when nobody is connected to the setup
AP, so it won't drop out from under you mid-configuration.

**Moving networks** — use the Forget button, which erases the stored
credentials and restarts into setup mode. Useful when the old network is no
longer in range.

**Build note:** the web server, DNS, and preferences libraries push the
build past the default partition. Set **Tools → Partition Scheme → Huge APP
(3MB No OTA/1MB SPIFFS)** or the upload will fail on size.

### wificom.dev specifics

- The identity payload must report `"name": "wificom"`. Anything else and the
  site won't recognise the device.
- Presence expires. Publish to the output topic periodically (this build uses
  20s) or apps querying for devices will find none, even while the device
  page still shows online.

## Serial commands

| Command | Action |
|---|---|
| `t` | Voltage test — check the divider and idle level |
| `c` | Send a test packet and log line transitions |
| `I` | Print the version block (used by host tools to identify the device) |
| `P` | Pause — clear the active DigiROM |

## Credits

- **BladeSabre** — the [DMComm project](https://github.com/dmcomm/dmcomm-python),
  which reverse-engineered these protocols. All timings, packet structures,
  and DM20 field layouts here are ported from that work.
- **Ben | Katsu**, Alpha Project — the original A-Com circuit.
- **mechawrench** — [wificom-lib](https://github.com/mechawrench/wificom-lib),
  the reference for wificom.dev integration.
- **humulos** — [Digitama Hatchery](https://humulos.com/digimon/dm20/), used
  to verify the monster index tables.
- **Seki** — creator of Digimon w0rld.

MIT licensed. See [LICENSE](LICENSE).

Digimon and Digital Monster are trademarks of Bandai Co., Ltd. This is an
independent fan project, not affiliated with or endorsed by Bandai.
