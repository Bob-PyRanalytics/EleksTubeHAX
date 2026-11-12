# MQTT display protocol

Status: `display` (`clock`, `tiles`) and `countdown` are implemented; `fetch` isn't yet.

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
| `elekstubehax/<device>/countdown` | server → clock | yes | A countdown, shown instead of `display` while it runs |
| `elekstubehax/<device>/fetch` | server → clock | no | Download one image file to the clock's filesystem |
| `elekstubehax/<device>/fetched` | clock → server | no | Result of a fetch |

Upstream's plain-mode topics are unchanged and work alongside these:
`elekstubehax/<device>/report/online` (`true`/`false`, retained, also the last will),
`<device>/report/#` and `<device>/directive/#`.

All payloads are JSON objects (except the empty `countdown` message that cancels
one). The clock ignores fields it doesn't know, and ignores a message with an
unknown `mode` or that it can't parse (it keeps showing what it was showing).

## Times

`expires` and `until` are absolute Unix times in seconds (UTC), not durations. The
clock gets its time over NTP. Absolute times mean a retained message stays correct
after the clock reboots or reconnects: one that has already expired is ignored, and
a countdown carries on rather than restarting. Until the clock has synchronised its
time, a message that carries a time waits, and is applied once the clock has the time,
unless another message on the same topic has replaced it by then.

## Backlight colour

`tiles` and `countdown` messages can carry an optional `backlight`, a colour as
`"#RRGGBB"`, shown on the backlight LEDs while that message is on the displays (e.g.
orange behind a power reading). The LEDs show the nearest colour they can. When the
displays go back to the clock, so does the clock's own colour. Without `backlight`,
the clock's colour stays. It applies to the steady, pulse and breath backlight
patterns; the rainbow pattern ignores it, and a backlight that is switched off stays
off. It isn't saved, so it never replaces the colour chosen for the clock.

## `display`

Each message replaces the previous one. The server decides which mode to show and
for how long (rotation is the publisher's job, not the clock's).

Fields common to every mode:

| Field | Required | Meaning |
|---|---|---|
| `mode` | yes | `clock` or `tiles` |
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
| `tiles` | yes | Exactly 6 entries, displays left to right. Each is a file name on the clock's filesystem without its extension, or `null` for a black display |

The server does all the formatting: number layout, signs, clamping, which icon,
which positions show a filler image. The firmware only draws the named files. A file
that doesn't exist is drawn black. (The displays share one power switch, so a single
display can't be turned off; black looks almost the same.) The names are whatever the
uploaded filesystem image and the fetched files contain, so the publisher and the
filesystem image have to agree on them.

| `backlight` | no | Backlight colour while shown, `"#RRGGBB"` (see Backlight colour) |

## `countdown`

A countdown has its own topic, so the publisher can keep rotating `display` while it
runs. A running countdown is shown instead of whatever `display` says; the clock
still takes in `display` messages meanwhile, and when the countdown ends it goes back
to the latest one (usually the clock, as rotation messages expire quickly). Each
`countdown` message replaces the previous one.

```json
{"until": 1792245600, "face": 2, "backlight": "#FF4800", "sweep": true, "speedup": 30, "pulse_after": 20, "expires": 1792245630}
```

| Field | Required | Meaning |
|---|---|---|
| `until` | yes | Unix time the countdown reaches zero |
| `face` | yes | Clock-face set whose digits are used, 1–99 (1 = files `10`–`19`, 2 = `20`–`29`, ...). Like a tile, a digit file that doesn't exist is drawn black |
| `blank` | no | File drawn instead of leading zeros in the hours (e.g. a logo or an unlit tube). Absent: the hours show `00` |
| `backlight` | no | Backlight colour while it runs, `"#RRGGBB"` (see Backlight colour) |
| `sweep` | no | `true`: a bright spot sweeps back and forth along the backlight LEDs while counting down, one pass about every 1.5 s. Absent or `false`: no sweep |
| `speedup` | no | Seconds before zero over which the sweep speeds up steadily, to about 5 passes a second at zero, 0–3600. Absent or 0: a steady speed |
| `pulse_after` | no | Seconds after zero that all the backlight LEDs pulse together (about every 0.6 s), 0–3600, then stay steady. Absent or 0: no pulse |
| `expires` | no | Unix time the countdown ends and the displays go back to `display`. Absent: it holds at zero until cancelled or replaced |

The clock counts down locally and shows `H H M M S S` on all six displays, updating
at most once a second. The minutes and seconds always show two digits. With `blank`
set, the hours' tens digit shows that file when it's 0, and both hour digits do when
the hours are 0. So with 5 minutes 30 seconds left and `"blank": "logo"`, the
displays show `logo logo 0 5 3 0`.

More than 99 hours away, it shows `99 59 59` until there are fewer left. At zero it
holds `00 00 00` (with the same blanking) until `expires` or the next message. To
hold the zeros for a minute, the publisher sets `expires` = `until` + 60. With
`"pulse_after": 20` and `expires` = `until` + 30, the backlight pulses for 20 seconds
after zero, stays steady for 10, and then the clock goes back to `display`.

The sweep and pulse never turn the LEDs off: they sit at the normal brightness for
the backlight's intensity setting and light up to about three times brighter. They
use the `backlight` colour (or the clock's own), whatever the backlight pattern; a
backlight that's switched off stays off. They're timed on the clock, so they need no
more messages. Drawing a digit holds the clock up for a moment, so the sweep can
stutter slightly when a digit changes.

To cancel a countdown, publish an empty retained message to the topic
(`mosquitto_pub -r -n -t elekstubehax/<device>/countdown`). That also deletes it from
the broker, so it doesn't come back when the clock reconnects.

A tenths-of-a-second display isn't supported: reading an image from flash takes
longer than 100 ms.

## `fetch` and `fetched`

The server sends one `fetch` per file. The clock downloads it with an HTTP GET,
stores it, and reports the result on `fetched`. Fetching and storing are separate
from displaying: a `tiles` message shows the files once they're stored.

```json
{"url": "http://192.0.2.10:8080/img/sunset/0/1.clk", "id": "sunset", "frame": 0, "total_frames": 1, "tile": 1}
```

| Field | Required | Meaning |
|---|---|---|
| `url` | yes | Plain HTTP URL of an image file, in the format the firmware is built for (see Image files) |
| `id` | yes | Name of the image or sequence: `[a-z0-9-]`, at most 16 characters |
| `frame` | yes | Frame number, from 0 |
| `total_frames` | yes | Frames in the sequence. Always 1 for now; animation is not implemented |
| `tile` | yes | Display position the file is meant for, 1–6 |

The file is stored as `/<id>_<frame>_<tile>.clk` (`.bmp` in a BMP build) and can
then be shown as the tile `<id>_<frame>_<tile>`, e.g. `sunset_0_1`. A fetch with the same `id`, `frame` and
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

At most 135 × 240 pixels; smaller images are centred. The format is chosen at build
time, for every image on the clock (clock faces, tiles and fetched files alike):

- **`.clk`** (`USE_CLK_FILES` defined): upstream's raw format. A 6-byte header (`CK`,
  then width and height as little-endian 16-bit values), then the pixels top to
  bottom as little-endian RGB565, with no row padding. This is exactly what the
  displays show, so nothing visible is lost compared with 24-bit, and a 135 × 240
  image is 64,806 bytes instead of 97,974. Make them from BMPs with
  `tools/conv-bmp-to-clk.py`, which needs Pillow.
- **`.bmp`** (the default): uncompressed 24-bit, or 8-bit, 4-bit or 1-bit
  palettised.

File names (without the extension): `[a-z0-9_-]`, at most 24 characters. Clock-face
sets are the files `10`–`19`, `20`–`29` and so on, numbered without gaps from `10`.
`tiles` can use them too (e.g. `23` is digit 3 of face set 2).

## Broker permissions

A suggested ACL, with one login for clocks and one for publishers:

| Login | Read | Write |
|---|---|---|
| clock | `elekstubehax/<device>/display`, `elekstubehax/<device>/countdown`, `elekstubehax/<device>/fetch`, `<device>/directive/#` | `elekstubehax/<device>/fetched`, `elekstubehax/<device>/report/#`, `<device>/report/#` |
| publisher | `elekstubehax/+/fetched`, `elekstubehax/+/report/#`, `+/report/#` | `elekstubehax/+/display`, `elekstubehax/+/countdown`, `elekstubehax/+/fetch` |

Publishers should publish `display` and `countdown` retained, so a clock that reboots
or reconnects shows the right thing at once. They should publish `fetch` not retained, so files
aren't downloaded again on every reconnect.

## Example layouts

How a publisher might lay out the modes it shows, with `logo` as the image for empty
positions.
These are choices for the publisher, not rules in the firmware.

| Showing | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| 3.42 kW | 3 | . | 4 | 2 | k | W |
| 18 °C, rain | rain icon | logo | 1 | 8 | ° | C |
| 5 °C | icon | logo | logo | 5 | ° | C |
| −3 °C | icon | logo | − | 3 | ° | C |
| −12 °C | icon | − | 1 | 2 | ° | C |
| Picture | part 1 | part 2 | part 3 | part 4 | part 5 | part 6 |
