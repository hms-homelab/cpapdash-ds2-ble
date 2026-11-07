#pragma once

#include "cpapdash/ds2ble/Frame.h"
#include "cpapdash/ds2ble/SyncSession.h"

#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace ds2test {

using Bytes = std::vector<uint8_t>;

/// Something that answers one request frame with the bytes of its answer.
class Machine {
public:
    virtual ~Machine() = default;
    virtual Bytes answer(const Bytes& request) = 0;
};

class UnexpectedRequest : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A capture as text, one frame per line: "<seconds> APP> <hex>" for what the
/// reader wrote, "<seconds> <DEV <hex>" for what the machine notified.
std::vector<std::pair<bool, Bytes>> readCapture(const std::string& path);

/**
 * A machine with no radio that answers from a capture. It learns every
 * request/answer pair with the cursor state it was sent in, and answers ONLY
 * requests the capture holds in that state; anything else throws
 * UnexpectedRequest, so a passing replay also proves the reader sent exactly
 * what the capture's reader sent.
 *
 * Two allowances: the set-clock request carries the time of sending, so its
 * data is not compared; and an open with no bookmark is answered with the
 * first open of that stream in the capture (the oldest record it holds).
 */
class ReplayMachine : public Machine {
public:
    explicit ReplayMachine(const std::vector<std::pair<bool, Bytes>>& capture);
    Bytes answer(const Bytes& request) override;
    int oldestOpens = 0;   ///< opens with no bookmark answered

private:
    std::map<std::tuple<uint16_t, uint8_t, Bytes>, Bytes> static_;
    std::map<Bytes, Bytes> opens_;
    std::map<uint8_t, Bytes> oldest_;
    std::map<std::pair<Bytes, Bytes>, Bytes> chunks_;
    std::map<Bytes, Bytes> bookmarks_;
    Bytes open_;
};

/// Runs one read against a machine. Each answer is fed back in pieces of
/// `split` bytes (0 = whole). Stops at done, failed, or after `limit` requests.
void drive(cpapdash::ds2ble::SyncSession& s, Machine& m, size_t split = 0, size_t limit = 100000);

}  // namespace ds2test
