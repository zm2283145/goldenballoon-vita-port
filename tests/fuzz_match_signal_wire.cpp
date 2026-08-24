/*
 * W3 N7 libFuzzer target: the native match-signal client's two hand-rolled
 * parsers -- the RFC 6455 server-frame extractor and the server-message
 * validation state machine -- driven through the shipped seam
 * mdkr_match_signal_fuzz_wire (platform/online/match_signal_client.cpp), so
 * the fuzzer exercises the exact production code with no socket and no
 * thread. Build with -DMDKR_ENABLE_FUZZERS=ON (Clang/libFuzzer only); seed
 * corpus in tests/fuzz_corpus/match_signal_wire/.
 */
#include "online/match_signal_client.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mdkr_match_signal_fuzz_wire(data, size);
    return 0;
}
