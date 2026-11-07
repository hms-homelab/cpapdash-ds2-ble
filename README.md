# cpapdash-ds2-ble

Read a Philips Respironics DreamStation 2 over Bluetooth LE, after the night.
C++17, no dependencies, MIT.

The library holds the protocol and nothing else: the frames, the commands and
the record walk. It has **no radio code**. A caller connects with whatever
Bluetooth stack its platform has, writes the bytes this library hands out and
feeds back every notification. What comes out is the machine's records in
plaintext, with a bookmark per stream to resume from next time.

What it does not do: there is no live data over Bluetooth (the session being
recorded is never served; it arrives once the mask is off), and Bluetooth never
carries the flow waveform. Nothing here writes to the machine except the clock,
and only when a caller asks.

## Use

```cmake
include(FetchContent)
FetchContent_Declare(cpapdash_ds2_ble
    GIT_REPOSITORY https://github.com/hms-homelab/cpapdash-ds2-ble.git
    GIT_TAG        <tag>)
FetchContent_MakeAvailable(cpapdash_ds2_ble)
target_link_libraries(my_app cpapdash::ds2_ble)
```

```cpp
#include "cpapdash/ds2ble/Protocol.h"     // service and characteristic UUIDs, commands
#include "cpapdash/ds2ble/SyncSession.h"

using namespace cpapdash::ds2ble;

SyncOptions opts;
opts.bookmarks = loadBookmarks();          // empty only on the very first read
opts.on_record = [](const Record& r) {     // store the record, then its bookmark
    save(cardFileName(r.stream, r.session), r.body);
};
SyncSession s(opts);
link.write(s.begin());                     // Write Command to kDataCharUuid
// for every notification on kDataCharUuid:
auto next = s.feed(bytes, len);
if (!next.empty()) link.write(next);
if (s.done())   storeBookmarks(s.bookmarks());
if (s.failed()) log(s.error());
```

A caller times out an answer itself (a few seconds is plenty) and calls
`abort()`.

## The protocol, as observed

All of it was learned by observing the Bluetooth traffic between a
DreamStation 2 and its owner's phone app, and by comparing what crossed the
link with the files the same machine wrote. No vendor software was
disassembled or consulted.

### Link

| | |
|---|---|
| Service | `4553867f-f809-49f4-aefc-e190a1f459f3` |
| Data characteristic | `22a4e311-a097-4517-9b81-cf32af60b982`: requests are Write Commands, answers are notifications |
| Advertised name | starts with `PR BT` |
| MTU | the machine offers 247; every answer seen fits one notification, but the deframer does not rely on it |
| Pairing | LE Secure Connections, **numeric comparison**: the machine shows a six-digit code, the reader shows the same code to its user, both confirm within about 30 s. The blower must be off. The machine drops the link while it shows the code; reconnect inside the window |
| Bonds | **one at a time**. Pairing a new reader replaces the old one, whose operating system must then forget the machine |

The first subscribe to the data characteristic is refused with "insufficient
authentication" until the reader is bonded.

### Frame

```
10 01 | cmd u16 BE | sub u8 | 10 02 | data | 10 03 | c0 c1
```

A `0x10` inside cmd, sub or data is sent as `10 10`; the two check bytes are
never doubled. The check is a Fletcher-16 modulo 255 over the undoubled cmd,
sub and data, both sums seeded `0x55`, in the 1..255 form (`c0 = 255 - ((s1 +
s2) % 255)`, `c1 = 255 - ((s1 + c0) % 255)`). Every answer's data starts with a
status byte, `00` on success.

### Commands

| cmd/sub | Request | Answer |
|---|---|---|
| `002b/01` | | serial number, ASCII |
| `0000/00` | | supported command ids, u16 BE each |
| `0020/00` | u32 BE Unix seconds, UTC | sets the machine's clock (a write: off unless asked) |
| `000d/03` | | firmware version, ASCII |
| `002c/01` | | model number, ASCII |
| `009a/09` | | newest session number, u32 BE |
| `0098/00` | `00 a0 00 00 00 00 00 01` | seven (id, u32, flag); meaning unknown, kept raw |
| `009a/01` | stream u8 [+ 10-byte bookmark] | open the next record after the bookmark: u32 BE chunks, length, session; all zero, or status `06`, means none |
| `009a/02` | u32 BE chunk index | the next part of the record's body |
| `009a/03` | | the 10-byte bookmark of the record just opened |

### Streams and records

| Stream | Content | File it equals |
|---|---|---|
| 0 | always empty | |
| 1 | session summary | `<session as 8 hex digits>.B01` |
| 2 | session events | `<session>.B02` |
| 3 | machine log | an entry of a `D/*.B03` file |
| 4 | machine log | an entry of an `E/*.B04` file |

A record body is the content of that file, in plaintext: blocks of the
DataFormatVersion 3 container (a 15-byte header, a record-size table, an 8-bit
header sum, the data, and a CRC-32 of the data). `verifyBody()` checks both
sums of every block.

**Bookmarks** are opaque: stored per stream and echoed back. A reader resumes
each stream after its stored bookmark and walks from the oldest record only on
its very first read; the machine has been seen to answer a bare open of the
events stream with "none" after another reader had read it.

### The read file (`.ds2r`)

When one device walks the streams and another keeps the bookmarks (a reader
next to the machine, a service far from it), the reader hands back one file
per request. `ReadFile.h` reads and writes it.

```
"DS2R" | version u8 = 1
then items:  kind u8 | len u32 LE | payload[len]
  0x01 ANSWER      cmd u16 BE | sub u8 | status u8 | data       one per identity request
  0x02 RECORD      stream u8 | session u32 BE | bookmark[10] | body
  0x03 STREAM_END  stream u8 | how u8 | records u16 LE | bookmark[10]
  0x04 END         result u8 | more u8 | status u8
```

Each RECORD carries the bookmark that resumes after it and is written only
once its body is whole, so a requestor can store a record and advance that
stream's bookmark together, and a read cut short still advances exactly as
far as what arrived. STREAM_END `how` is 0 (the open answered all zero), 6
(nothing after the bookmark) or 0xFF (stopped by the reader's cap or a
failure). `parseReadFile()` refuses a file whose items do not add up, so
nothing of it is stored; `machineSerial()`, `model()`, `firmware()` and
`newestSession()` read the identity answers.

```cpp
#include "cpapdash/ds2ble/ReadFile.h"

const ReadFile f = parseReadFile(bytes);         // throws ReadFileError
for (const ReadRecord& r : f.records)
    store(f.machineSerial(), r.stream, r.session, r.body, r.bookmark);
```

## Tests

```bash
cmake -S . -B build && cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

The tests that run everywhere use a synthetic machine. A capture of a real
sync holds a person's therapy records, so none is kept here; point
`DS2BLE_CAPTURE` at one (one frame per line, `<seconds> APP> <hex>` or
`<seconds> <DEV <hex>`) and, optionally, `DS2BLE_COUNTS=1:N,2:N,3:N,4:N`, to
replay it. The replay answers only the requests the captured app sent, in the
state it sent them, so a pass proves the reader speaks byte for byte what the
app spoke. `ds2ble-replay <capture> [out-dir]` prints and writes the records of
a capture.

## License

MIT. See `LICENSE`.
