#include "online/match_signal_client.h"

/* Red-first placeholder: the full JS-conformant client lands behind the
 * failing suite in tests/test_match_signal_client.cpp. */

struct MdkrMatchSignalClient::State {};

MdkrMatchSignalClient::MdkrMatchSignalClient(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

MdkrMatchSignalClient::~MdkrMatchSignalClient() = default;

std::unique_ptr<MdkrMatchSignalClient> MdkrMatchSignalClient::create(
    const MdkrMatchSignalClientOptions &, std::string *errorMessage) {
    if (errorMessage != nullptr) *errorMessage = "unimplemented";
    return nullptr;
}

bool MdkrMatchSignalClient::connect(std::string *errorCode) {
    if (errorCode != nullptr) *errorCode = "unimplemented";
    return false;
}

MdkrMatchSignalSendResult MdkrMatchSignalClient::send(
    const MdkrMatchSignalOutbound &) {
    return MdkrMatchSignalSendResult{};
}

void MdkrMatchSignalClient::close() {}

MdkrMatchSignalSnapshot MdkrMatchSignalClient::snapshot() const {
    return MdkrMatchSignalSnapshot{};
}

void MdkrMatchSignalClient::drainEvents(
    std::vector<MdkrMatchSignalEvent> &out) {
    out.clear();
}
