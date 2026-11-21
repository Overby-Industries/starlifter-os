#include "starlifter/ledger_feed.hpp"

namespace starlifter {

FeedPublication publishFeed(otp::Bridge& bridge, const std::vector<std::string>& events,
                            const otp::PublishOptions& options) {
    FeedPublication out;
    out.stamped.reserve(events.size());

    for (std::size_t i = 0; i < events.size(); ++i) {
        const otp::Published published = bridge.publish_otf1(events[i], options);
        if (!published.ok) {
            out.ok = false;
            out.error = published.error;
            out.failedEvent = i + 1;
            return out;
        }
        if (published.already_published) {
            ++out.alreadyPublished;
        } else {
            ++out.published;
        }
        out.stamped.push_back(published.stamped);
    }
    return out;
}

FeedVerification verifyFeed(const otp::Bridge& bridge, const std::vector<std::string>& stamped) {
    FeedVerification out;
    for (std::size_t i = 0; i < stamped.size(); ++i) {
        const otp::Verification v = bridge.verify_otf1(stamped[i]);
        if (v.verified()) {
            ++out.verified;
            continue;
        }
        out.ok = false;
        out.problems.push_back("event " + std::to_string(i + 1) + ": " + otp::to_string(v.verdict) +
                               ": " + v.reason);
    }
    return out;
}

}  // namespace starlifter
