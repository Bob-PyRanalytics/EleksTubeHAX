# MQTT display protocol

Status: specification. Not implemented in the firmware yet.

This fork turns the clock into a display driven entirely over MQTT
(`MQTT_PLAIN_ENABLED`). The clock is a pure subscriber: a script on a server decides
what to show, formats it and publishes it, and the clock draws what it's told. The
clock never fetches data itself; the only HTTP it does is downloading image files it
has been told to fetch.

```
data source ──▶ publisher script ──MQTT──▶ broker ──MQTT──▶ clock
                       │                                      │
                       └──────── HTTP (image files) ◀─────────┘
```

## Topics

All topics are under `elekstubehax/<device>/`, where `<device>` is the clock's MQTT
client ID: the device name followed by the last three bytes of its MAC address,
lowercased (e.g. `ipstube-a1b2c3`). The clock prints it on the serial console at boot
(`Set device name: ...`).

| Topic | Direction | Retained | Purpose |
|---|---|---|---|
| `elekstubehax/<device>/display` | server → clock | yes | What to show now |
| `elekstubehax/<device>/fetch` | server → clock | no | Download one image file to the clock's filesystem |
| `elekstubehax/<device>/fetched` | clock → server | no | Result of a fetch |

Upstream's plain-mode topics are unchanged and work alongside these:
`elekstubehax/<device>/report/online` (`true`/`false`, retained, also the last will),
`<device>/report/#` and `<device>/directive/#`.

All payloads are JSON objects. The clock ignores fields it doesn't know, and ignores
a message with an unknown `mode` or that it can't parse (it keeps showing what it
was showing).

## Times

`expires` and `until` are absolute Unix times in seconds (UTC), not durations. The
clock gets its time over NTP. Absolute times mean a retained message stays correct
after the clock reboots or reconnects: one that has already expired is ignored, and
a countdown carries on rather than restarting. Until the clock has synchronised its
time, it doesn't act on `display` messages that carry a time.

## `display`

Each message replaces the previous one. The server decides which mode to show and
for how long (rotation is the publisher's job, not the clock's).

Fields common to every mode:

| Field | Required | Meaning |
|---|---|---|
| `mode` | yes | `clock`, `tiles` or `countdown` |
| `expires` | no | Unix time after which the clock goes back to `clock`. Absent: show until replaced |

`expires` also protects against a publisher that stops: a value left retained on the
broker doesn't stay on the displays for ever. A publisher showing a reading for 10
seconds sends `expires` = now + 10 and needs no second message to go back to the clock.

### `clock`

```json
{"mode": "clock"}
```

The firmware's normal clock, with whichever clock face is selected.

### `tiles`

```json
{"mode": "tiles", "tiles": ["23", "dot", "24", "22", "k", "w"], "expires": 1792245610}
```

| Field | Required | Meaning |
|---|---|---|
| `tiles` | yes | Exactly 6 entries, displays left to right. Each is a file name on the clock's filesystem without `.bmp`, or `null` to switch that display off |

The server does all the formatting: number layout, signs, clamping, which icon,
which positions show a blank image. The firmware only draws the named files. A file
that doesn't exist is drawn as a display switched off. The names are whatever the
uploaded filesystem image and the fetched files contain, so the publisher and the
filesystem image have to agree on them.

### `countdown`

```json
{"mode": "countdown", "until": 1792245600, "face": 2, "blank": "blank", "expires": 1792245660}
```

| Field | Required | Meaning |
|---|---|---|
| `until` | yes | Unix time the countdown reaches zero |
| `face` | yes | Clock-face set whose digits are used (1 = files `10`–`19`, 2 = `20`–`29`, ...) |
| `blank` | no | File drawn instead of leading zeros in the hours. Absent: the hours show `00` |

The clock counts down locally and shows `H H M M S S` on all six displays, updating
at most once a second. The minutes and seconds always show two digits. With `blank`
set, the hours' tens digit is blank when it's 0, and both hour digits are blank when
the hours are 0. So with 5 minutes 30 seconds left, the displays show
`blank blank 0 5 3 0`.

At zero it holds `00 00 00` (with the same blanking) until `expires` or the next
message. To hold the zeros for a minute, the publisher sets `expires` = `until` + 60.

A tenths-of-a-second display isn't supported: reading an image from flash takes
longer than 100 ms.

## `fetch` and `fetched`

The server sends one `fetch` per file. The clock downloads it with an HTTP GET,
stores it, and reports the result on `fetched`. Fetching and storing are separate
from displaying: a `tiles` message shows the files once they're stored.

```json
{"url": "http://192.0.2.10:8080/img/sunset/0/1.bmp", "id": "sunset", "frame": 0, "total_frames": 1, "tile": 1}
```

| Field | Required | Meaning |
|---|---|---|
| `url` | yes | Plain HTTP URL of a BMP file |
| `id` | yes | Name of the image or sequence: `[a-z0-9-]`, at most 16 characters |
| `frame` | yes | Frame number, from 0 |
| `total_frames` | yes | Frames in the sequence. Always 1 for now; animation is not implemented |
| `tile` | yes | Display position the file is meant for, 1–6 |

The file is stored as `/<id>_<frame>_<tile>.bmp` and can then be shown as the tile
`<id>_<frame>_<tile>`, e.g. `sunset_0_1`. A fetch with the same `id`, `frame` and
`tile` overwrites the file.

```json
{"id": "sunset", "frame": 0, "tile": 1, "ok": true}
{"id": "sunset", "frame": 0, "tile": 1, "ok": false, "error": "HTTP 404"}
```

`fetch` isn't retained, and MQTT delivery to the clock is QoS 0, so a server that
gets no `fetched` reply within a reasonable time should send the `fetch` again.

The filesystem's flash is slow to write and limited in space, so fetches should be
occasional (minutes apart), not continuous.

## Image files

- BMP, at most 135 × 240 pixels, uncompressed: 24-bit, or 8-bit, 4-bit or 1-bit
  palettised. Palettised images take about a third of the space of 24-bit ones.
- File names (without `.bmp`): `[a-z0-9_-]`, at most 24 characters.
- Clock-face sets are the files `10`–`19`, `20`–`29` and so on, numbered without gaps
  from `10`. `tiles` can use them too (e.g. `23` is digit 3 of face set 2).

## Broker permissions

A suggested ACL, with one login for clocks and one for publishers:

| Login | Read | Write |
|---|---|---|
| clock | `elekstubehax/<device>/display`, `elekstubehax/<device>/fetch`, `<device>/directive/#` | `elekstubehax/<device>/fetched`, `elekstubehax/<device>/report/#`, `<device>/report/#` |
| publisher | `elekstubehax/+/fetched`, `elekstubehax/+/report/#`, `+/report/#` | `elekstubehax/+/display`, `elekstubehax/+/fetch` |

Publishers should publish `display` retained, so a clock that reboots or reconnects
shows the right thing at once. They should publish `fetch` not retained, so files
aren't downloaded again on every reconnect.

## Example layouts

How a publisher might lay out the modes it shows, with `blank` as a blank-tube image.
These are choices for the publisher, not rules in the firmware.

| Showing | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| 3.42 kW | 3 | . | 4 | 2 | k | W |
| 18 °C, rain | rain icon | blank | 1 | 8 | ° | C |
| 5 °C | icon | blank | blank | 5 | ° | C |
| −3 °C | icon | blank | − | 3 | ° | C |
| −12 °C | icon | − | 1 | 2 | ° | C |
| Picture | part 1 | part 2 | part 3 | part 4 | part 5 | part 6 |
