#include "check.hpp"
#include "support.hpp"

#include "starlifter/node.hpp"

using namespace support;

namespace {

NodeConfig configFor(const std::string& id) {
    NodeConfig config;
    config.id = id;
    config.type = NodeType::Reclaimer;
    config.missionId = "orbital-demo-1";
    return config;
}

std::string heartbeat(TimeMs now, std::uint64_t seq, const char* from = kCoordinator) {
    HeartbeatPacket packet;
    packet.timestamp = now;
    packet.source = {from, Role::Coordinator};
    packet.seq = seq;
    return encode(packet);
}

std::string offer(const Task& task, TimeMs now, std::uint64_t seq, TimeMs acceptBy,
                  const char* to = "A1", const char* from = kCoordinator) {
    AssignmentPacket packet;
    packet.timestamp = now;
    packet.source = {from, Role::Coordinator};
    packet.destination = to;
    packet.seq = seq;
    packet.task = task;
    packet.acceptBy = acceptBy;
    return encode(packet);
}

// Offer one task to a fresh node whose controller has been prepared by
// `prepare`, and return the reason it gave for refusing (or nullopt).
template <typename Prepare>
std::optional<RejectReason> refusalFor(Prepare prepare, const Task& task = makeTask("T-1"),
                                       TimeMs lateBy = 0) {
    FakeControl control;
    prepare(control);
    Node node(configFor("A1"), control);
    const TimeMs acceptBy = kT0 + 10 * kSecond;
    const TimeMs arrival = lateBy > 0 ? acceptBy + lateBy : kT0;
    node.receive(offer(task, kT0, 1, acceptBy), arrival);
    const auto acks = only<AckPacket>(node.tick(arrival));
    if (acks.size() != 1) return RejectReason::Refused;  // would fail the caller's check
    return acks[0].reason;
}

void test_a_node_alone_reports_itself_autonomous() {
    TEST("a node that has never heard its coordinator says so, and keeps reporting");

    FakeControl control;
    Node node(configFor("A1"), control);

    auto statuses = only<StatusPacket>(node.tick(kT0));
    CHECK_EQ(statuses.size(), std::size_t(1));
    if (statuses.empty()) return;
    CHECK(statuses[0].link == LinkView::Autonomous);
    CHECK(statuses[0].mode == Mode::Nominal);
    CHECK_EQ(statuses[0].source.id, std::string("A1"));
    CHECK(statuses[0].source.role == Role::Node);
    CHECK_EQ(statuses[0].capabilities.size(), std::size_t(2));
    CHECK(!statuses[0].task);
    const std::uint64_t firstSeq = statuses[0].seq;

    // Nothing has changed, so nothing more is said until the interval is up.
    CHECK(node.tick(kT0 + 1 * kSecond).empty());
    statuses = only<StatusPacket>(node.tick(kT0 + 5 * kSecond));
    CHECK_EQ(statuses.size(), std::size_t(1));
    if (!statuses.empty()) CHECK(statuses[0].seq > firstSeq);

    // Hearing the coordinator is a change, and is reported at once.
    node.receive(heartbeat(kT0 + 6 * kSecond, 1), kT0 + 6 * kSecond);
    statuses = only<StatusPacket>(node.tick(kT0 + 6 * kSecond));
    CHECK_EQ(statuses.size(), std::size_t(1));
    if (!statuses.empty()) CHECK(statuses[0].link == LinkView::Connected);
    CHECK(node.link() == LinkView::Connected);
}

void test_an_offer_is_taken_and_reported() {
    TEST("an offer is taken, acknowledged, and appears in the next status");

    FakeControl control;
    Node node(configFor("A1"), control);
    node.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);

    const auto out = node.tick(kT0);
    const auto acks = only<AckPacket>(out);
    const auto statuses = only<StatusPacket>(out);
    CHECK_EQ(acks.size(), std::size_t(1));
    CHECK_EQ(statuses.size(), std::size_t(1));
    if (acks.empty() || statuses.empty()) return;

    CHECK(acks[0].result == AckResult::Accepted);
    CHECK(!acks[0].reason);
    CHECK_EQ(acks[0].taskId, std::string("T-1"));
    CHECK_EQ(acks[0].destination, std::string(kCoordinator));
    CHECK(statuses[0].task && statuses[0].task->id == "T-1");
    CHECK(statuses[0].task && statuses[0].task->state == TaskState::Active);
    CHECK(statuses[0].link == LinkView::Connected);  // the offer itself is contact
    CHECK(acks[0].seq < statuses[0].seq);            // one counter across both kinds
    CHECK_EQ(control.beginCalls, 1);
    CHECK_EQ(control.lastBegun.id, std::string("T-1"));

    // Progress is read from local control and passed on.
    control.fraction = 0.25;
    const auto later = only<StatusPacket>(node.tick(kT0 + 5 * kSecond));
    CHECK(later.size() == 1 && later[0].task && later[0].task->progress &&
          *later[0].task->progress == 0.25);
}

void test_the_vehicle_has_the_final_say() {
    TEST("an offer is refused, with the reason, whenever the vehicle cannot or will not");

    CHECK(!refusalFor([](FakeControl&) {}));  // the baseline: accepted

    CHECK(refusalFor([](FakeControl& c) { c.refuse = true; }) == RejectReason::Refused);
    CHECK(refusalFor([](FakeControl& c) { c.currentMode = Mode::Safe; }) == RejectReason::SafeMode);
    CHECK(refusalFor([](FakeControl& c) { c.currentMode = Mode::Degraded; }) ==
          RejectReason::Degraded);
    CHECK(refusalFor([](FakeControl& c) { c.can = {TaskAction::Refine}; }) ==
          RejectReason::Unsupported);
    CHECK(refusalFor([](FakeControl& c) { c.where.zone = Zone::NEA; }) ==
          RejectReason::Unsupported);
    CHECK(refusalFor([](FakeControl&) {}, makeTask("T-1"), 1) == RejectReason::Expired);

    // A vehicle that is not nominal is never even asked.
    FakeControl safe;
    safe.currentMode = Mode::Safe;
    Node node(configFor("A1"), safe);
    node.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);
    node.tick(kT0);
    CHECK_EQ(safe.beginCalls, 0);
    CHECK(!node.task());

    // Busy: one task at a time.
    FakeControl control;
    Node busy(configFor("A1"), control);
    busy.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);
    busy.tick(kT0);
    busy.receive(offer(makeTask("T-2"), kT0 + kSecond, 2, kT0 + 11 * kSecond), kT0 + kSecond);
    const auto acks = only<AckPacket>(busy.tick(kT0 + kSecond));
    CHECK(acks.size() == 1 && acks[0].taskId == "T-2" && acks[0].reason == RejectReason::Busy);
    CHECK_EQ(control.beginCalls, 1);
    CHECK(busy.task() && busy.task()->id == "T-1");
}

void test_an_offer_is_taken_at_most_once() {
    TEST("a duplicated or repeated offer never starts the work twice");

    FakeControl control;
    Node node(configFor("A1"), control);
    const std::string packet = offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond);

    node.receive(packet, kT0);
    node.receive(packet, kT0);  // the link delivered it twice
    CHECK_EQ(only<AckPacket>(node.tick(kT0)).size(), std::size_t(1));
    CHECK_EQ(control.beginCalls, 1);
    CHECK_EQ(node.ignoredPackets(), std::uint64_t(1));

    // The coordinator offering the task it is already doing gets the same
    // answer again, and the work is not restarted.
    node.receive(offer(makeTask("T-1"), kT0 + kSecond, 2, kT0 + 11 * kSecond), kT0 + kSecond);
    auto acks = only<AckPacket>(node.tick(kT0 + kSecond));
    CHECK(acks.size() == 1 && acks[0].result == AckResult::Accepted);
    CHECK_EQ(control.beginCalls, 1);

    // Once finished, a further offer of the same task is answered with the
    // truth -- a status saying it is done -- not by doing it again and not
    // by a refusal that would send another node after it.
    control.currentWork = WorkState::Completed;
    node.tick(kT0 + 2 * kSecond);
    node.receive(offer(makeTask("T-1"), kT0 + 3 * kSecond, 3, kT0 + 13 * kSecond), kT0 + 3 * kSecond);
    const auto out = node.tick(kT0 + 3 * kSecond);
    CHECK(only<AckPacket>(out).empty());
    const auto statuses = only<StatusPacket>(out);
    CHECK(statuses.size() == 1 && statuses[0].task && statuses[0].task->id == "T-1" &&
          statuses[0].task->state == TaskState::Completed);
    CHECK_EQ(control.beginCalls, 1);
}

void test_only_its_own_coordinator_is_obeyed() {
    TEST("packets from anyone else, or for anyone else, are ignored");

    FakeControl control;
    Node node(configFor("A1"), control);
    const TimeMs acceptBy = kT0 + 10 * kSecond;

    node.receive(heartbeat(kT0, 1, "someone-else"), kT0);                       // impostor
    node.receive(offer(makeTask("T-1"), kT0, 2, acceptBy, "A1", "someone-else"), kT0);
    node.receive(offer(makeTask("T-2"), kT0, 3, acceptBy, "A2"), kT0);          // not for us
    node.receive("{ this is not a packet", kT0);

    StatusPacket status;  // a status is something a node sends, not obeys
    status.timestamp = kT0;
    status.source = {"A2", Role::Node};
    node.receive(encode(status), kT0);

    HeartbeatPacket fromNode;  // a heartbeat from a node is not the coordinator
    fromNode.timestamp = kT0;
    fromNode.source = {kCoordinator, Role::Node};
    node.receive(encode(fromNode), kT0);

    const auto out = node.tick(kT0);
    CHECK(only<AckPacket>(out).empty());
    CHECK_EQ(control.beginCalls, 0);
    CHECK(node.link() == LinkView::Autonomous);  // none of that was contact
    CHECK_EQ(node.ignoredPackets(), std::uint64_t(6));
}

void test_losing_contact_changes_nothing_about_the_work() {
    TEST("out of contact, a node finishes its task and holds the record until heard");

    FakeControl control;
    control.result.processedKg = 5.2;
    control.result.smessc = true;
    control.result.odciContributionKg = 5.2;
    Node node(configFor("A1"), control);

    node.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);
    node.tick(kT0);
    CHECK(node.link() == LinkView::Connected);

    // The coordinator goes quiet. Past the timeout the node knows it.
    auto statuses = only<StatusPacket>(node.tick(kT0 + 16 * kSecond));
    CHECK(node.link() == LinkView::Autonomous);
    CHECK(statuses.size() == 1 && statuses[0].link == LinkView::Autonomous);
    CHECK(statuses.size() == 1 && statuses[0].task &&
          statuses[0].task->state == TaskState::Active);  // still working

    // The work finishes with nobody listening.
    control.currentWork = WorkState::Completed;
    auto out = node.tick(kT0 + 60 * kSecond);
    CHECK(only<EventPacket>(out).empty());            // held, not thrown at a dead link
    CHECK_EQ(node.heldEvents(), std::size_t(1));
    statuses = only<StatusPacket>(out);
    CHECK(statuses.size() == 1 && statuses[0].task &&
          statuses[0].task->state == TaskState::Completed);

    // It keeps reporting, in case only the downlink failed.
    CHECK_EQ(only<StatusPacket>(node.tick(kT0 + 65 * kSecond)).size(), std::size_t(1));
    CHECK_EQ(node.heldEvents(), std::size_t(1));

    // Contact returns: the record goes out, once.
    node.receive(heartbeat(kT0 + 300 * kSecond, 9), kT0 + 300 * kSecond);
    out = node.tick(kT0 + 300 * kSecond);
    const auto events = only<EventPacket>(out);
    CHECK_EQ(events.size(), std::size_t(1));
    CHECK_EQ(node.heldEvents(), std::size_t(0));
    statuses = only<StatusPacket>(out);
    CHECK(statuses.size() == 1 && statuses[0].link == LinkView::Connected);
    CHECK(only<EventPacket>(node.tick(kT0 + 305 * kSecond)).empty());

    if (events.empty()) return;
    const EventPacket& event = events[0];
    CHECK(event.taskId && *event.taskId == "T-1");
    CHECK(event.action == EventAction::Captured);
    CHECK_EQ(event.podId, std::string("A1"));
    CHECK_EQ(event.missionId, std::string("orbital-demo-1"));
    CHECK(event.missionType == NodeType::Reclaimer);
    CHECK_EQ(event.timestamp, kT0 + 60 * kSecond);  // when it happened, not when it was sent
    CHECK(event.target && event.target->massKg && *event.target->massKg == 5.2);
    CHECK(event.processedKg && *event.processedKg == 5.2);
    CHECK(event.smessc);
    CHECK(event.odciContributionKg && *event.odciContributionKg == 5.2);
    CHECK(event.outcomeStatus == OutcomeStatus::Stored);
}

void test_a_local_fault_fails_the_task() {
    TEST("when the vehicle safes itself the task is failed, and no event is claimed");

    FakeControl control;
    Node node(configFor("A1"), control);
    node.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);
    node.tick(kT0);

    // The vehicle goes to safe mode. This fake does not even report the
    // work as failed; the agent must not rely on being told.
    control.currentMode = Mode::Safe;
    const auto out = node.tick(kT0 + kSecond);
    const auto statuses = only<StatusPacket>(out);
    CHECK(statuses.size() == 1 && statuses[0].mode == Mode::Safe);
    CHECK(statuses.size() == 1 && statuses[0].task &&
          statuses[0].task->state == TaskState::Failed);
    CHECK(only<EventPacket>(out).empty());
    CHECK_EQ(node.heldEvents(), std::size_t(0));

    // Local control silently dropping the work is a failure too.
    FakeControl forgetful;
    Node other(configFor("A1"), forgetful);
    other.receive(offer(makeTask("T-2"), kT0, 1, kT0 + 10 * kSecond), kT0);
    other.tick(kT0);
    forgetful.currentWork = WorkState::Idle;
    other.tick(kT0 + kSecond);
    CHECK(other.task() && other.task()->state == TaskState::Failed);
}

void test_a_failed_task_can_be_offered_again() {
    TEST("a task that failed here can be retried here");

    FakeControl control;
    Node node(configFor("A1"), control);
    node.receive(offer(makeTask("T-1"), kT0, 1, kT0 + 10 * kSecond), kT0);
    node.tick(kT0);
    control.currentWork = WorkState::Failed;
    node.tick(kT0 + kSecond);
    CHECK(node.task() && node.task()->state == TaskState::Failed);

    node.receive(offer(makeTask("T-1"), kT0 + 2 * kSecond, 2, kT0 + 12 * kSecond), kT0 + 2 * kSecond);
    const auto acks = only<AckPacket>(node.tick(kT0 + 2 * kSecond));
    CHECK(acks.size() == 1 && acks[0].result == AckResult::Accepted);
    CHECK_EQ(control.beginCalls, 2);
    CHECK(node.task() && node.task()->state == TaskState::Active);
}

}  // namespace

void runNodeTests() {
    std::printf("node\n");
    test_a_node_alone_reports_itself_autonomous();
    test_an_offer_is_taken_and_reported();
    test_the_vehicle_has_the_final_say();
    test_an_offer_is_taken_at_most_once();
    test_only_its_own_coordinator_is_obeyed();
    test_losing_contact_changes_nothing_about_the_work();
    test_a_local_fault_fails_the_task();
    test_a_failed_task_can_be_offered_again();
}
