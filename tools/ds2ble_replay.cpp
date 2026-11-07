// ds2ble-replay <capture.txt> [out-dir]
//
// Reads a DreamStation 2 from a captured exchange instead of a radio (see
// tests/Replay.h for the capture's text form) and prints one line per record:
// stream, session, length, card file name, bookmark. With an out-dir, each
// body is also written there as <NN>-<card file name>, NN its order of
// arrival, so it can be compared with another reader's output.

#include "Replay.h"
#include "cpapdash/ds2ble/SyncSession.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace cpapdash::ds2ble;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ds2ble-replay <capture.txt> [out-dir]\n";
        return 2;
    }
    try {
        ds2test::ReplayMachine m(ds2test::readCapture(argv[1]));
        SyncSession s;
        ds2test::drive(s, m);
        if (!s.done()) {
            std::cerr << "read failed: " << s.error() << "\n";
            return 1;
        }
        const auto& id = s.identity();
        std::cout << "model " << id.model << " firmware " << id.firmware << " newest " << id.newest_session
                  << " requests " << s.requests() << "\n";
        size_t n = 0;
        for (const auto& r : s.records()) {
            std::cout << int(r.stream) << " " << r.session << " " << r.body.size() << " "
                      << cardFileName(r.stream, r.session) << " " << toHex(r.bookmark_out) << "\n";
            if (argc > 2) {
                char name[64];
                std::snprintf(name, sizeof name, "%04zu-%s", n, cardFileName(r.stream, r.session).c_str());
                std::filesystem::create_directories(argv[2]);
                std::ofstream out(std::filesystem::path(argv[2]) / name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(r.body.data()), static_cast<std::streamsize>(r.body.size()));
            }
            ++n;
        }
        std::cout << n << " records\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
