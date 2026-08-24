/*
 * W3 N7 libFuzzer target: the MatchRoom transport's hand-rolled wire
 * parsers -- the HTTP/1.1 response parse (status line + chunked decode),
 * the /connect RFC 6455 frame decoder, and the lobby/ice-server JSON
 * mapping -- driven through the shipped seam mdkr_online_room_fuzz_wire
 * (platform/online/match_live_transport.cpp), so the fuzzer exercises the
 * exact production code with no socket and no thread. Build with
 * -DMDKR_ENABLE_FUZZERS=ON (Clang/libFuzzer only); seed corpus in
 * tests/fuzz_corpus/online_live_wire/.
 */
#include "online/match_live_transport.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mdkr_online_room_fuzz_wire(data, size);
    return 0;
}
