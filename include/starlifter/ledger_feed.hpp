#pragma once

// Recording the coordinator's event feed in the Overby Ledger.
//
// Coordinator::feed() is every completed-work event the fleet has reported,
// verbatim and in arrival order: the public record, as opposed to the
// status/heartbeat/assignment/ack bookkeeping that never leaves the fleet.
// This turns that feed into entries in a tamper-evident ledger and hands back
// the same events stamped with a coordinate anyone can check.
//
// All of the real work is the ledger bridge's (otp::Bridge, in the
// overby-transparency-protocol submodule). What is here is the fleet-side
// glue: run a feed through it, and check a stamped feed against a ledger.
//
// What the ledger gets, per event: a named entry for the pod (a machine is not
// a person), citing OTF-1, whose detail commits to a SHA-256 digest of the
// event with its token field removed. What comes back is that event with the
// coordinate written into ProofOfCleanupToken. Publishing the same event twice
// records it once, however it is spelled, so a feed that is replayed, or a
// coordinator that restarts and re-sends, does not double-count a capture.
//
// An event's time in the ledger is when it HAPPENED, not when word got
// through. A node that finished a task during an outage and reported it
// minutes later is recorded at the instant of completion.

#include <cstddef>
#include <string>
#include <vector>

#include <otp/otp_bridge.h>

namespace starlifter {

struct FeedPublication {
    bool ok = true;
    std::string error;                 // when !ok: what stopped it
    std::size_t failedEvent = 0;       // when !ok: which event, counting from 1

    std::size_t published = 0;         // new entries appended
    std::size_t alreadyPublished = 0;  // events the ledger already held
    std::vector<std::string> stamped;  // each handled event, with its coordinate
};

// Publish `events` (one OTF-1 event packet each, as Coordinator::feed() holds
// them) in order. Stops at the first event the bridge refuses -- malformed, not
// an event, already carrying a token it will not overwrite -- and says which.
// Events before that one stay in the ledger: an append-only ledger cannot take
// them back. A caller that wants all-or-nothing keeps its own copy of the
// ledger and discards it on failure, as starlifter_publish does.
FeedPublication publishFeed(otp::Bridge& bridge, const std::vector<std::string>& events,
                            const otp::PublishOptions& options = {});

struct FeedVerification {
    bool ok = true;
    std::size_t verified = 0;
    std::vector<std::string> problems;  // "event 3: entry_mismatch: ..."
};

// Check each stamped event against the ledger: its coordinate checks out AND
// the entry it names commits to this event. Every problem is reported, not
// just the first.
FeedVerification verifyFeed(const otp::Bridge& bridge, const std::vector<std::string>& stamped);

}  // namespace starlifter
