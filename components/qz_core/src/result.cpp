// Error tokens of console protocol v1 (ARCHITECTURE.md section 16): Errc <-> string, 1:1.
#include "qz/core/result.hpp"

#include <string_view>

namespace qz {

std::string_view to_token(Errc code) noexcept {
    // No default label on purpose: -Wswitch (an error here) forces a new Errc value to get its
    // token before the build passes.
    switch (code) {
        case Errc::kBadArgs:
            return "bad_args";
        case Errc::kUnknownCommand:
            return "unknown_cmd";
        case Errc::kUnsupported:
            return "unsupported";
        case Errc::kInvalidState:
            return "invalid_state";
        case Errc::kBusy:
            return "busy";
        case Errc::kIo:
            return "io";
        case Errc::kTimeout:
            return "timeout";
        case Errc::kNotFound:
            return "not_found";
        case Errc::kNoTime:
            return "no_time";
        case Errc::kNoCredentials:
            return "no_creds";
        case Errc::kBatteryLow:
            return "battery_low";
        case Errc::kCorrupt:
            return "corrupt";
        case Errc::kNoSpace:
            return "no_space";
        case Errc::kInternal:
            return "internal";
    }
    // A value outside the enumeration (corrupted memory) still gets a non-empty token.
    return "internal";
}

} // namespace qz
