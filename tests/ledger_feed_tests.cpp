#include "check.hpp"
#include "support.hpp"

#include <memory>

#include <otp/otp_bridge.h>

#include "starlifter/ledger_feed.hpp"
#include "starlifter/sim.hpp"

using namespace support;
using starlifter::sim::FleetSim;
using starlifter::sim::NodeSpec;

namespace {

NodeSpec reclaimerSpec(const std::string& id, TimeMs oneWayDelayMs = 500) {
    NodeSpec spec;
    spec.node.id = id;
    spec.node.type = NodeType::Reclaimer;
    spec.node.missionId = "orbital-demo-1";
    spec.pod.location.zone = Zone::LEO;
    spec.pod.capabilities = {TaskAction::Capture, TaskAction::Deorbit};
    spec.pod.taskDurationMs = kMinute;
    spec.link.oneWayDelayMs = oneWayDelayMs;
    return spec;
}

// A fleet that has a bad day: one pod is cut off mid-task for two minutes,
// another faults, a third is 40 light-seconds away. Run long enough that all
// the work finishes, and what comes out is the feed a real coordinator would
// hold.
std::unique_ptr<FleetSim> eventfulFleet() {
    NodeSpec cutOff = reclaimerSpec("A1");
    cutOff.link.outages = {{kT0 + 30 * 1000, kT0 + 150 * 1000}};

    auto fleet = std::make_unique<FleetSim>(CoordinatorConfig{}, kT0, 500);
    fleet->addNode(cutOff);
    fleet->addNode(reclaimerSpec("A2"));
    fleet->addNode(reclaimerSpec("A3", 40 * 1000));
    for (int i = 1; i <= 5; ++i) {
        fleet->submitAt(kT0, makeTask("T-" + std::to_string(i), TaskAction::Capture, Zone::LEO,
                                      static_cast<double>(i) + 0.2));
    }
    fleet->safeModeAt(kT0 + 20 * 1000, "A2");
    fleet->runUntil(kT0 + 15 * kMinute);
    return fleet;
}

otp::Ledger makeLedger(const char* instance = "starlifter-sim") {
    otp::Ledger ledger;
    ledger.set_instance(instance, "OTF-1/1.0");
    return ledger;
}

EventPacket decodeEvent(const std::string& text) {
    Packet packet;
    CHECK(decode(text, packet));
    const EventPacket* event = std::get_if<EventPacket>(&packet);
    CHECK(event != nullptr);
    return event != nullptr ? *event : EventPacket{};
}

std::string replacedOnce(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

void test_a_real_feed_is_recorded_and_stamped() {
    TEST("the feed from a fleet with an outage and a fault is recorded, stamped and verifies");

    const auto fleet = eventfulFleet();
    const std::vector<std::string>& feed = fleet->coordinator().feed();
    CHECK_EQ(feed.size(), std::size_t(5));  // one completed-work event per task

    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication published = starlifter::publishFeed(bridge, feed);
    CHECK(published.ok);
    if (!published.ok) std::printf("        %s\n", published.error.c_str());
    CHECK_EQ(published.published, feed.size());
    CHECK_EQ(published.alreadyPublished, std::size_t(0));
    CHECK_EQ(published.stamped.size(), feed.size());
    CHECK_EQ(ledger.size(), feed.size());
    CHECK(ledger.verify().intact);

    // Every stamped event is a valid OTF-1 event carrying a coordinate, and says
    // exactly what the original said: stamping adds the token and changes nothing.
    for (std::size_t i = 0; i < feed.size(); ++i) {
        const EventPacket original = decodeEvent(feed[i]);
        const EventPacket stamped = decodeEvent(published.stamped[i]);
        CHECK(!original.proofOfCleanupToken);
        CHECK(stamped.proofOfCleanupToken && stamped.proofOfCleanupToken->compare(0, 6, "otp:1:") == 0);
        CHECK_EQ(otp::canonical_otf1(stamped), otp::canonical_otf1(original));

        // The entry is named for the pod, cites OTF-1, and is dated when the work
        // happened rather than when the report got through.
        const otp::Entry* entry = ledger.get(i + 1);
        CHECK(entry != nullptr);
        if (entry == nullptr) continue;
        CHECK_EQ(entry->citation, std::string("OTF-1"));
        CHECK_EQ(entry->kind, std::string("otf1.captured"));
        CHECK_EQ(entry->subject_id, original.podId);
        CHECK(entry->disclosure == otp::DISC_NAMED);
        CHECK_EQ(entry->tick, static_cast<uint64_t>(original.timestamp));
    }

    const starlifter::FeedVerification verified = starlifter::verifyFeed(bridge, published.stamped);
    CHECK(verified.ok);
    CHECK_EQ(verified.verified, feed.size());
    CHECK(verified.problems.empty());
}

void test_events_are_dated_when_they_happened() {
    TEST("an event reported late is recorded at the instant it happened");

    // A1 finishes during its outage and the coordinator hears about it minutes
    // later. The record must say when the capture was done.
    const auto fleet = eventfulFleet();
    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    CHECK(starlifter::publishFeed(bridge, fleet->coordinator().feed()).ok);

    bool sawLateReport = false;
    for (const otp::Entry& entry : ledger.entries()) {
        // 150 s is when A1's link came back; anything dated before that and
        // by A1 was held on board through the outage.
        if (entry.subject_id == "A1" && entry.tick < static_cast<uint64_t>(kT0 + 150 * 1000)) {
            sawLateReport = true;
        }
    }
    CHECK(sawLateReport);
}

void test_publishing_twice_records_once() {
    TEST("replaying the feed records nothing twice");

    const auto fleet = eventfulFleet();
    const std::vector<std::string>& feed = fleet->coordinator().feed();
    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication first = starlifter::publishFeed(bridge, feed);
    CHECK(first.ok);

    const starlifter::FeedPublication again = starlifter::publishFeed(bridge, feed);
    CHECK(again.ok);
    CHECK_EQ(again.published, std::size_t(0));
    CHECK_EQ(again.alreadyPublished, feed.size());
    CHECK_EQ(ledger.size(), feed.size());
    CHECK(again.stamped == first.stamped);

    // A coordinator that restarted and re-sent, a new bridge over the same
    // ledger, and the already-stamped events themselves are all the same record.
    otp::Bridge later(ledger);
    CHECK_EQ(starlifter::publishFeed(later, feed).alreadyPublished, feed.size());
    CHECK_EQ(starlifter::publishFeed(later, first.stamped).alreadyPublished, feed.size());
    CHECK_EQ(ledger.size(), feed.size());
}

void test_an_altered_event_does_not_verify() {
    TEST("a stamped event that was altered, or a token lifted from another, does not verify");

    const auto fleet = eventfulFleet();
    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    starlifter::FeedPublication published = starlifter::publishFeed(bridge, fleet->coordinator().feed());
    CHECK(published.ok);
    CHECK(published.stamped.size() >= 2);
    if (published.stamped.size() < 2) return;

    // Inflate the mass credited toward the cleanup index in the first event.
    const EventPacket first = decodeEvent(published.stamped[0]);
    EventPacket inflated = first;
    inflated.processedKg = first.processedKg.value_or(0.0) + 100.0;
    std::vector<std::string> tampered = published.stamped;
    tampered[0] = encode(inflated);

    const starlifter::FeedVerification v = starlifter::verifyFeed(bridge, tampered);
    CHECK(!v.ok);
    CHECK_EQ(v.verified, published.stamped.size() - 1);
    CHECK_EQ(v.problems.size(), std::size_t(1));
    CHECK(!v.problems.empty() && v.problems[0].compare(0, 8, "event 1:") == 0);
    CHECK(!v.problems.empty() && v.problems[0].find("entry_mismatch") != std::string::npos);

    // The second event wearing the first event's token.
    EventPacket second = decodeEvent(published.stamped[1]);
    second.proofOfCleanupToken = first.proofOfCleanupToken;
    std::vector<std::string> lifted = published.stamped;
    lifted[1] = encode(second);
    const starlifter::FeedVerification w = starlifter::verifyFeed(bridge, lifted);
    CHECK(!w.ok);
    CHECK_EQ(w.problems.size(), std::size_t(1));
}

void test_a_third_party_with_a_copy_can_check_the_feed() {
    TEST("anyone holding a copy of the ledger can verify the published feed");

    const auto fleet = eventfulFleet();
    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication published = starlifter::publishFeed(bridge, fleet->coordinator().feed());
    CHECK(published.ok);

    // The auditor has only the ledger's canonical text and the public feed.
    otp::Ledger copy;
    CHECK(otp::Ledger::from_canonical(ledger.to_canonical(), &copy));
    CHECK(copy.verify().intact);
    const otp::Bridge auditor(copy);
    CHECK(starlifter::verifyFeed(auditor, published.stamped).ok);

    // A copy whose entries were quietly rewritten no longer vouches for the feed.
    otp::Ledger rewritten;
    CHECK(otp::Ledger::from_canonical(replacedOnce(ledger.to_canonical(), "captured by pod", "captured by POD"),
                                      &rewritten));
    const otp::Bridge distrust(rewritten);
    const starlifter::FeedVerification v = starlifter::verifyFeed(distrust, published.stamped);
    CHECK(!v.ok);
    CHECK(!v.problems.empty() && v.problems[0].find("token_invalid") != std::string::npos);

    // And one that is behind -- taken before the last events were published --
    // reports the events as missing rather than forged.
    otp::Ledger behind = makeLedger();
    otp::Bridge catchingUp(behind);
    std::vector<std::string> firstTwo(fleet->coordinator().feed().begin(),
                                      fleet->coordinator().feed().begin() + 2);
    CHECK(starlifter::publishFeed(catchingUp, firstTwo).ok);
    const starlifter::FeedVerification partial = starlifter::verifyFeed(catchingUp, published.stamped);
    CHECK_EQ(partial.verified, std::size_t(2));
    CHECK_EQ(partial.problems.size(), published.stamped.size() - 2);
}

void test_a_feed_with_a_bad_event_stops_and_says_which() {
    TEST("a feed with an event the bridge refuses stops there and names it");

    const auto fleet = eventfulFleet();
    std::vector<std::string> feed = fleet->coordinator().feed();
    CHECK(feed.size() >= 3);
    if (feed.size() < 3) return;

    // Garbage in the middle.
    std::vector<std::string> garbled = feed;
    garbled[2] = "{ not a packet";
    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication result = starlifter::publishFeed(bridge, garbled);
    CHECK(!result.ok);
    CHECK_EQ(result.failedEvent, std::size_t(3));
    CHECK(result.error.find("not a valid OTF-1 packet") != std::string::npos);
    // The events before it are in the ledger: an append-only ledger cannot take
    // them back, which is why the command-line tool works on a copy.
    CHECK_EQ(result.published, std::size_t(2));
    CHECK_EQ(ledger.size(), std::size_t(2));

    // A status packet that strayed into the feed is bookkeeping, not record.
    StatusPacket status;
    status.timestamp = kT0;
    status.source = {"A1", Role::Node};
    status.capabilities = {TaskAction::Capture};
    otp::Ledger other = makeLedger();
    otp::Bridge strict(other);
    const starlifter::FeedPublication refused = starlifter::publishFeed(strict, {encode(status)});
    CHECK(!refused.ok);
    CHECK(refused.error.find("only OTF-1 event packets are published") != std::string::npos);
    CHECK_EQ(other.size(), std::size_t(0));
}

void test_a_legacy_token_is_not_overwritten_by_default() {
    TEST("an event that already carries a token is refused unless the caller says to replace it");

    const auto fleet = eventfulFleet();
    EventPacket event = decodeEvent(fleet->coordinator().feed()[0]);
    event.proofOfCleanupToken = "0xabc123debriscredit";
    const std::string legacy = encode(event);

    otp::Ledger ledger = makeLedger();
    otp::Bridge bridge(ledger);
    const starlifter::FeedPublication refused = starlifter::publishFeed(bridge, {legacy});
    CHECK(!refused.ok);
    CHECK(refused.error.find("0xabc123debriscredit") != std::string::npos);
    CHECK_EQ(ledger.size(), std::size_t(0));

    otp::PublishOptions replace;
    replace.replace_existing_token = true;
    const starlifter::FeedPublication allowed = starlifter::publishFeed(bridge, {legacy}, replace);
    CHECK(allowed.ok);
    CHECK_EQ(ledger.size(), std::size_t(1));
}

void test_an_unidentified_ledger_is_refused() {
    TEST("a ledger with no instance name cannot be published into");

    const auto fleet = eventfulFleet();
    otp::Ledger anonymous;  // set_instance never called
    otp::Bridge bridge(anonymous);
    const starlifter::FeedPublication result = starlifter::publishFeed(bridge, fleet->coordinator().feed());
    CHECK(!result.ok);
    CHECK(result.error.find("set_instance") != std::string::npos);
    CHECK_EQ(anonymous.size(), std::size_t(0));
}

}  // namespace

void runLedgerFeedTests() {
    std::printf("ledger feed\n");
    test_a_real_feed_is_recorded_and_stamped();
    test_events_are_dated_when_they_happened();
    test_publishing_twice_records_once();
    test_an_altered_event_does_not_verify();
    test_a_third_party_with_a_copy_can_check_the_feed();
    test_a_feed_with_a_bad_event_stops_and_says_which();
    test_a_legacy_token_is_not_overwritten_by_default();
    test_an_unidentified_ledger_is_refused();
}
