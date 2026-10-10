# libcari

C library for [**CARI 2.1**](https://github.com/M17-Project/CARI) (Common Amateur Radio Interface).

It is split in two layers so the protocol logic can be reused anywhere:

| Layer | Header | Needs | Contents |
|---|---|---|---|
| Codec | `cari/cari_proto.h` | nothing | Frame encode/decode with the reply rules, replies, TLV capability records, parameter/action/quantity/format/command tables, parameter rules and stream-combination check, sample scaling (Table 18/19), supervision packets, FLAC framing helpers (CRC-8/16, frame-header parser, frame split and resync) |
| FLAC | `cari/cari_flac.h` | libFLAC | Compressed baseband encoder/decoder producing/consuming bare frames with explicit rate and bit depth, unsigned offset |
| Slave | `cari/cari_slave.h` | libzmq | CTRL REP dispatch to device callbacks, DL/UL/SPVN socket bind/rebind/close, TX state machine with UL underrun trip, supervision publishing, transparent DL compression / UL decompression |
| Master | `cari/cari_master.h` | libzmq | REQ transactions with timeout and socket reopen, optional retries, typed helpers for every command, data-plane connectors |

The codec layer does no I/O and no allocation - it can be used on a microcontroller or inside a GNU Radio block.

## Build

```sh
cmake -S . -B build          # -DCARI_WITH_FLAC=OFF / -DCARI_WITH_ZMQ=OFF to drop layers
cmake --build build
ctest --test-dir build
sudo cmake --install build   # installs headers, libcari.so, cariConfig.cmake, cari.pc
```

Dependencies: libzmq (`libzmq3-dev`), libFLAC (`libflac-dev`), CMake >= 3.16. `-DCARI_SANITIZE=ON` builds with ASan/UBSan.

## Writing a slave

The library owns everything the specification defines, while the device provides a description and hardware callbacks.

```c
cari_tlv_writer_t w;
uint8_t caps[128];
cari_tlv_init(&w, caps, sizeof(caps));
cari_tlv_flag(&w, CARI_SCAP_IQ);
cari_tlv_flag(&w, CARI_SCAP_RX);
cari_tlv_flag(&w, CARI_SCAP_FMT_CS16);
cari_tlv_range_u64(&w, CARI_SCAP_RX_FREQ, 400000000, 510000000);
cari_tlv_value_f32(&w, CARI_SCAP_SAMPLE_RATE, 250000);   /* discrete value = equal bounds */

cari_subdev_desc_t sd = { caps, w.len };
cari_slave_desc_t d = { .ident = "My RRU", .nsubdevs = 1, .subdevs = &sd };
cari_slave_ops_t ops = { .set_param = my_set_param, .stream_start = my_start, .stream_stop = my_stop };

cari_slave_t *s = cari_slave_new(&d, &ops, my_ctx);
cari_slave_param_init(s, 0, CARI_P_RX_FREQ, cari_value_u64(433475000));
cari_slave_param_init(s, 0, CARI_P_SAMPLE_RATE, cari_value_f32(250000));
cari_slave_param_init(s, 0, CARI_P_FORMAT, cari_value_u8(CARI_FMT_CS16));
cari_slave_start(s);                 /* binds tcp://*:17002 */
cari_slave_run(s, &stop);
```

A parameter is supported when it was declared with `cari_slave_param_init()`. Before `set_param` is called the library has already checked the address, capability prerequisites (TX frequency needs a transmitter, AGC needs capability 0x05, compression needs device capability 0x00, etc.), read-only, booleans, formats, modes, the declared ranges (including discrete equal-bound pairs) and the stream locks. The callback may quantize the value, the quantized value is what CID 0x83 returns.

Streams are pumped by device threads:

- DL: call `cari_slave_dl_write()` with samples in the signal format. It never blocks (PUB drops for slow subscribers) and compresses when parameter 0x0D is on.
- UL: call `cari_slave_ul_read()` only as fast as samples are transmitted - that is the backpressure. When the device runs dry it ramps down, unkeys and calls `cari_slave_ul_underrun()` (any thread), the library moves the subdevice to *tripped* from the CTRL thread, calls `stream_stop`, and sets error flag bit 4 until action 0x05 or a reset.

`stream_stop` must not return while the pump thread still uses these functions. Subdevice's data socket is never touched by the CTRL thread while its stream runs (rebind is refused with status 8), which keeps ZeroMQ's one-thread-per-socket rule.

See `tests/test_slave.c` for a complete mock device and `cari-dummy-device` for a real one.

## Writing a master

```c
cari_master_t *m = cari_master_new(NULL, "192.168.1.200", 1000);   /* port 17002 by default */
uint32_t flags;
cari_m_ping(m, &flags);
cari_value_t f = cari_value_u64(433475000);
cari_m_set_param(m, 0, CARI_P_RX_FREQ, &f);
cari_m_bind_dl(m, 0, 17005);
void *sub = cari_master_connect_dl(m, 17005, 64);   /* SUB, empty filter */
cari_m_exec(m, 0, CARI_ACT_RX_START);
```

Every helper returns the reply status (`>= 0`) or a negative `CARI_E_*`. On a timeout, the REQ socket is closed and reopened, as the specification recommends, so the next command works. `cari_master_set_retries()` repeats timed-out commands (only for commands that are safe to repeat). Replies are validated against the command (CID echo, 4-byte error replies, data length per CID).

## CARI 2.1 notes

CARI 2.1 made the earlier open points normative (status precedence, binds, supervision edge cases, TX stop, trips) - libcari follows them. Implementation details worth knowing:

- **End of transmission.** `cari_slave_ul_read()` returns `CARI_UL_END` when it reads the zero-byte marker (also with compression) and does not read the socket again until the next TX start. The device plays out what it has buffered, ramps down, unkeys and calls `cari_slave_ul_finished()`, the library then returns the subdevice to idle. During the play-out the TX state stays 1, TX start and UL rebind return 8, and TX stop aborts. Masters use `cari_master_ul_send_end()` and `cari_m_wait_tx_idle()`, after checking `cari_m_supports(m, 2, 1, ...)` - a 2.0 slave would treat the marker as an empty message and trip.
- **Deviation** (0x0C) outside FM/PM returns 8, after the value checks (a NaN still returns 5).
- **Device reset**: the reply is sent first, then all streams are stopped, all sockets closed and `dev_reset` called.
- **FLAC**: libFLAC emits a block once it holds blocksize + 1 samples, so a compressed DL stream lags one block behind (10 ms with the default block size of rate/100). Frames come one per message by default. The decoder accepts frames only with explicit rate/bit depth, the expected channel count and bit depth and (if configured) rate. Data before the first verified frame is skipped. libFLAC >= 1.4 "conceals" a missing frame with silence - libcari drops that, CARI keeps time at the slave.

## Tests

`ctest` runs:

- `test_codec` - every CID's encoding and decoding, all reply rules, TLV parsing (unknown IDs skipped, unpaired/mistyped ranges rejected), parameter rules, combination check, supervision packets, tables and value parsing.
- `test_samples` - scaling of all eight formats (end points, clipping, full 8-bit round trips), FLAC CRC-8/CRC-16 check values, frame-header parsing (rate codes, explicitness), split with garbage, false sync codes inside frames, joining mid-frame and damaged frames.
- `test_flac` - libFLAC round trips for all integer formats and rates up to 655,350 Hz, joining a stream mid-frame, rejected rate/format mismatches, recovery after a corrupt frame.
- `test_slave` - a two-subdevice mock slave and a master in one process over loopback TCP: reply rules on the wire, parameters, stream locks, DL raw and compressed, UL raw and compressed with underrun trip and recovery, end of transmission (play-out state, refused start/rebind, abort, compressed), supervision, device reset, master timeout/reopen and retries.
