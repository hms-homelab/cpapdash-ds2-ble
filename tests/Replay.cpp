#include "Replay.h"

#include "cpapdash/ds2ble/Protocol.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace cpapdash::ds2ble;

namespace ds2test {

std::vector<std::pair<bool, Bytes>> readCapture(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::vector<std::pair<bool, Bytes>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string t, who, hex, extra;
        if (!(ss >> t >> who >> hex) || (ss >> extra)) continue;
        if (who != "APP>" && who != "<DEV") continue;
        out.emplace_back(who == "APP>", fromHex(hex));
    }
    return out;
}

ReplayMachine::ReplayMachine(const std::vector<std::pair<bool, Bytes>>& capture) {
    Bytes cur;
    for (size_t i = 0; i + 1 < capture.size();) {
        if (!capture[i].first || capture[i + 1].first) {
            ++i;
            continue;
        }
        const Frame f = decode(capture[i].second);
        const Bytes& resp = capture[i + 1].second;
        if (f.cmd == kCmdCursor && f.sub == kCurOpen) {
            cur = f.data;
            opens_[f.data] = resp;
            if (!f.data.empty()) oldest_.emplace(f.data[0], f.data);
        } else if (f.cmd == kCmdCursor && f.sub == kCurChunk) {
            chunks_[{cur, f.data}] = resp;
        } else if (f.cmd == kCmdCursor && f.sub == kCurBookmark) {
            bookmarks_[cur] = resp;
        } else {
            static_[{f.cmd, f.sub, f.cmd == kCmdSetClock ? Bytes{} : f.data}] = resp;
        }
        i += 2;
    }
}

Bytes ReplayMachine::answer(const Bytes& request) {
    const Frame f = decode(request);
    if (f.cmd == kCmdCursor && f.sub == kCurOpen) {
        Bytes key = f.data;
        if (key.size() == 1 && !opens_.count(key) && oldest_.count(key[0])) key = oldest_[key[0]];
        if (key.size() == 1) ++oldestOpens;
        auto it = opens_.find(key);
        if (it == opens_.end()) throw UnexpectedRequest("open " + toHex(f.data) + " is not in the capture");
        open_ = key;
        return it->second;
    }
    if (f.cmd == kCmdCursor && f.sub == kCurChunk) {
        auto it = chunks_.find({open_, f.data});
        if (it == chunks_.end()) throw UnexpectedRequest("chunk " + toHex(f.data) + " of open " + toHex(open_));
        return it->second;
    }
    if (f.cmd == kCmdCursor && f.sub == kCurBookmark) {
        auto it = bookmarks_.find(open_);
        if (it == bookmarks_.end()) throw UnexpectedRequest("bookmark after open " + toHex(open_));
        return it->second;
    }
    auto it = static_.find({f.cmd, f.sub, f.cmd == kCmdSetClock ? Bytes{} : f.data});
    if (it == static_.end()) throw UnexpectedRequest("request " + toHex(request) + " is not in the capture");
    return it->second;
}

void drive(SyncSession& s, Machine& m, size_t split, size_t limit) {
    Bytes req = s.begin();
    while (!req.empty() && !s.done() && !s.failed() && s.requests() <= limit) {
        const Bytes resp = m.answer(req);
        const size_t step = split ? split : resp.size();
        req.clear();
        for (size_t k = 0; k < resp.size(); k += step) {
            const size_t n = std::min(step, resp.size() - k);
            Bytes next = s.feed(resp.data() + k, n);
            if (!next.empty()) req = std::move(next);
        }
    }
}

}  // namespace ds2test
