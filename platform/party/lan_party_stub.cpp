#include "lan_party_launch.h"

#include "libdatachannel_party_transport.h"

#include <memory>
#include <utility>

std::string mdkr_lan_party_select_advertised_host(
    const std::vector<std::string> &) {
    return {};
}

bool mdkr_lan_party_build_manifest(const std::string &,
                                   MdkrLanPartyManifest &out) {
    out = {};
    return false;
}

bool mdkr_lan_party_can_start(const std::string &,
                              const std::string &) {
    return false;
}

std::string mdkr_lan_party_advertised_host() { return {}; }

std::string mdkr_lan_party_web_root() { return {}; }

bool mdkr_lan_party_build_launch_config(MdkrLanPartyTransportConfig &out,
                                        std::string &reason) {
    out = {};
    reason = "Phone controllers are not included in this build.";
    return false;
}

std::unique_ptr<MdkrPartyTransport> mdkr_create_lan_party_transport(
    MdkrLanPartyTransportConfig) {
    // The cloud factory is itself the fail-closed unavailable transport in a
    // build without native Phone Party support. Reusing it keeps the launcher
    // transport invariant intact without linking any socket or WebRTC code.
    return mdkr_create_native_party_transport();
}
