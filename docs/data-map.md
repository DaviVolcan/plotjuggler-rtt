# Data map reference

The data map is a JSON file that describes one telemetry record byte by byte. The plugin loads it when streaming starts and reports the first problem it finds (missing key, field past the end of the record, unknown type) in a dialog.

## Example

```json
{
  "version": 1,
  "record_size": 23,
  "byte_order": "little",
  "magic": "0xC3A55A01",
  "seq_field": "seq",
  "nominal_rate_hz": 1000,
  "fields": [
    {"name": "seq",       "offset": 4,  "type": "uint32"},
    {"name": "current_a", "offset": 8,  "type": "float32", "unit": "A"},
    {"name": "current_b", "offset": 12, "type": "float32", "unit": "A"},
    {"name": "speed_rpm", "offset": 16, "type": "float32", "unit": "rpm"},
    {"name": "temp_cdeg", "offset": 20, "type": "int16",   "unit": "0.01 degC"},
    {"name": "state",     "offset": 22, "type": "uint8"}
  ]
}
```

This is [`examples/telemetry_map.json`](../examples/telemetry_map.json), matching the struct in [`examples/firmware/telemetry.c`](../examples/firmware/telemetry.c).

## Top-level keys

| Key | Required | Default | Meaning |
|---|---|---|---|
| `record_size` | yes | | Size of one record in bytes, magic included. Must equal `sizeof` of your packed struct. |
| `magic` | yes | | 32-bit word at offset 0 of every record. String (`"0xC3A55A01"`) or number. |
| `seq_field` | yes | | Name of the field that counts records. Drives the time axis and loss detection. |
| `fields` | yes | | Array of field definitions, see below. |
| `nominal_rate_hz` | no | `1000` | Rate at which `seq` advances. Time in seconds is `seq / nominal_rate_hz`. |
| `seq_step` | no | `1` | How much `seq` advances between two records. See [Decimated counters](#decimated-counters). |
| `reset_below_seq` | no | `-1` | `seq` going backwards counts as a target reset only if the new value is below this. `-1`: any step back is a reset. |
| `dump_magic` | no | | Magic of the second record type, see [Burst captures](#burst-captures). |
| `dump_fields` | no | `[]` | Fields published from dump records. |
| `byte_order` | no | `"little"` | Only `"little"` is accepted. |
| `version` | no | `0` | Free-form, for your own bookkeeping. Not checked. |

## Fields

| Key | Required | Meaning |
|---|---|---|
| `name` | yes | Series name in PlotJuggler, shown as `rtt/<name>`. Must be unique. |
| `offset` | yes | Byte offset inside the record. Must be 4 or more, since bytes 0 to 3 hold the magic. |
| `type` | yes | `uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `float32` or `float64`. |
| `unit` | no | For documentation; the plugin does not use it. |

Fields can be listed in any order and do not have to cover every byte; padding or fields you do not want to plot can be left out.

A record whose `float32`/`float64` fields contain NaN or infinity is dropped and counted in `rtt/_diag/malformed`.

## Choosing a magic

Pick a value that is unlikely to appear inside your data at the same distance as one record. Values with non-printable bytes, such as `0xC3A55Axx`, work well: they never match the J-Link text banner, and a false match also requires the next record to match.

Use the low byte as a layout version and change it whenever the record changes. A map that does not match the firmware then never syncs, instead of decoding the wrong bytes.

## Decimated counters

Often the counter in the record is a fast tick (say an 8 kHz control loop) while records are sent at 1 kHz. Set `nominal_rate_hz` to the tick rate and `seq_step` to the ratio:

```json
"nominal_rate_hz": 8000,
"seq_step": 8
```

Jitter in when the sending task runs makes the delta vary around 8. Only a jump larger than `2 * seq_step` is counted as lost records.

## Burst captures

Some data is too fast to stream live. A common pattern is to record a burst into a RAM buffer on the target (around a step response, say) and replay it over RTT afterwards. The plugin supports this with a second record type:

- Dump records have the same size and layout but start with `dump_magic` instead of `magic`.
- Their `seq` is the tick at which the sample was taken, so they appear at the right place in time, in the past.
- Only the fields in `dump_fields` are published, so fields the burst does not fill do not show up as zeros.
- A dump record never counts as a reset, and the jump back to live data does not count as lost records.

```json
"nominal_rate_hz": 8000,
"seq_step": 8,
"dump_magic": "0xC3A55A82",
"dump_fields": ["current_a", "current_b"]
```

For the replayed samples to land at the right instant, `seq` must count the fast loop's ticks, not records: here the loop runs at 8 kHz and live records go out every 8th tick, hence `nominal_rate_hz` 8000 and `seq_step` 8. See [`examples/firmware/burst_capture.c`](../examples/firmware/burst_capture.c) and [`examples/burst_map.json`](../examples/burst_map.json) for the complete pattern.

Sizing a burst:

- **RAM**: samples × fields × field size. 128 samples of two `float` is 1 KiB.
- **Replay time**: samples ÷ records per second the link carries. Replacing the live record, 128 samples take 128 ms at 1 kHz. Sending them alongside live data is fine too, as long as the link has room for both.
- **Pre-trigger**: if the event is not known in advance, keep writing into a ring buffer and freeze it a fixed number of samples after the trigger, as the example does. If it is known (a scripted setpoint profile), start recording a few ticks before it and a plain array is enough.

The target must not overwrite the buffer while it is being sent. The example freezes it until the replay ends and ignores triggers in the meantime.

## Diagnostic series

| Series | Meaning |
|---|---|
| `rtt/_diag/lost_records` | Total records missing according to `seq` since the connection started. |
| `rtt/_diag/malformed` | Total records dropped after sync: magic lost mid-stream, or NaN/Inf values. |

If no data shows up at all, the map and the firmware most likely disagree on `magic` or `record_size`; the parser keeps discarding bytes and never syncs.

### Connecting to a target that is already running

While no client is connected, J-Link keeps reading the RTT buffer and stores the data on the PC, up to about 1 MiB. On connect it sends that backlog first, which is why old data appears for a moment before the live stream. Once the backlog is full, J-Link drops the newest data, so there is a jump in `seq` between the backlog and the live stream. That jump is counted in `lost_records`: those records were lost, but before you connected. Watch whether the counter *grows* during the session, not its value right after connecting.

J-Link also cuts the stream mid-record at that point. The plugin only accepts a record once the magic of the next record has arrived, so the cut record is dropped (and counted in `malformed`) instead of being decoded with shifted fields. The newest record is therefore always held back until the next one starts, which adds one record period of delay.
