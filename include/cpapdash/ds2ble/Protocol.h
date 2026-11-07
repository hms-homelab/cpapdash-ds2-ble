#pragma once

#include <cstdint>

namespace cpapdash::ds2ble {

/**
 * The machine is the GATT server. All data rides one characteristic of one
 * vendor service: requests are ATT Write Commands (no response) to it, the
 * answers are notifications on it. One answer per request, with the request's
 * cmd and sub.
 *
 * Bonding is required before the characteristic's notifications can be
 * enabled: the machine answers the first subscribe "insufficient
 * authentication", which makes the central pair. Pairing is LE Secure
 * Connections with NUMERIC COMPARISON: the machine shows a six-digit code,
 * the central must show the same code to its user, and both sides confirm
 * within about 30 seconds. The machine drops the link while it shows its
 * code, so a central reconnects inside that window. The machine keeps ONE
 * bond at a time: pairing a new reader replaces the old one (whose own
 * operating system must then forget the machine).
 */
constexpr const char* kServiceUuid = "4553867f-f809-49f4-aefc-e190a1f459f3";
constexpr const char* kDataCharUuid = "22a4e311-a097-4517-9b81-cf32af60b982";
/// The machine advertises a name that starts with this.
constexpr const char* kNamePrefix = "PR BT";

/// The commands a reader sends. Every answer's data starts with a status
/// byte; 0x00 is success.
enum Cmd : uint16_t {
    kCmdCommands = 0x0000,   ///< sub 00: the command ids the machine supports, u16 BE each
    kCmdFirmware = 0x000D,   ///< sub 03: firmware version, ASCII, NUL
    kCmdSetClock = 0x0020,   ///< sub 00, data u32 BE Unix seconds UTC: sets the machine's clock (a WRITE)
    kCmdSerial = 0x002B,     ///< sub 01: serial number, ASCII, NUL
    kCmdModel = 0x002C,      ///< sub 01: model number, ASCII, NUL
    kCmdCounters = 0x0098,   ///< sub 00 with kCountersRequest: seven (id, u32, flag); meaning unknown
    kCmdCursor = 0x009A,     ///< the record cursor, subs below
};

enum CursorSub : uint8_t {
    kCurOpen = 0x01,      ///< data: stream u8 [+ bookmark]; answer u32 BE chunks, length, session; all zero = none
    kCurChunk = 0x02,     ///< data: u32 BE chunk index; answer: the next part of the body
    kCurBookmark = 0x03,  ///< answer: the 10-byte bookmark of the record just opened
    kCurNewest = 0x09,    ///< answer: u32 BE newest session number
};

constexpr uint8_t kStatusOk = 0x00;
/// Answer to a cursor open: nothing after the bookmark. Seen on a live
/// machine where another reader had the all-zero header; handled the same.
constexpr uint8_t kStatusNoNext = 0x06;

/// The data of the counters request, as the machine's own app sends it.
constexpr uint8_t kCountersRequest[8] = {0x00, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

}  // namespace cpapdash::ds2ble
