#include "check.hpp"
#include "support.hpp"

using namespace support;

namespace {

// A healthy LEO reclaimer's snapshot. Tests change what they need to.
StatusPacket snapshot(const std::string& id, TimeMs at, std::uint64_t seq) {
    StatusPacket status;
    status.timestamp = at;
    status.source = {id, Role::Node};
    status.seq = seq;
    status.type = NodeType::Reclaimer;
    status.mode = Mode::Nominal;
    status.link = LinkView::Connected;
    status.capabilities = {TaskAction::Capture, TaskAction::Deorbit};
    status.location.zone = Zone::LEO;
    return status;
}

StatusPacket holding(const std::string& id, TimeMs at, std::uint64_t seq,
                     const std::string& taskId, TaskState state) {
    StatusPacket status = snapshot(id, at, seq);
    status.task = TaskStatus{taskId, state, std::nullopt};
    return status;
}

// Deliver a packet the instant it was sent (no link delay).
void tell(Coordinator& coordinator, const StatusPacket& status) {
    coordinator.receive(encode(status), status.timestamp);
}

void answer(Coordinator& coordinator, const std::string& nodeId, const std::string& taskId,
            TimeMs at, std::uint64_t seq, std::optional<RejectReason> refusal = std::nullopt,
            const char* to = kCoordinator) {
    AckPacket ack;
    ack.timestamp = at;
    ack.source = {nodeId, Role::Node};
    ack.destination = to;
    ack.seq = seq;
    ack.taskId = taskId;
    ack.result = refusal ? AckResult::Rejected : AckResult::Accepted;
    ack.reason = refusal;
    coordinator.receive(encode(ack), at);
}

std::string completionEvent(const std::string& nodeId, const std::string& taskId, TimeMs at) {
    EventPacket event;
    event.timestamp = at;
    event.missionId = "orbital-demo-1";
    event.podId = nodeId;
    event.missionType = NodeType::Reclaimer;
    event.location.zone = Zone::LEO;
    event.action = EventAction::Captured;
    event.processedKg = 5.2;
    event.outcomeStatus = OutcomeStatus::Stored;
    event.smessc = true;
    event.odciContributionKg = 5.2;
    event.taskId = taskId;
    return encode(event);
}

std::vector<AssignmentPacket> offers(Coordinator& coordinator, TimeMs now) {
    return only<AssignmentPacket>(packetsOf(coordinator.tick(now)));
}

TaskPhase phase(const Coordinator& coordinator, const std::string& taskId) {
    const auto task = coordinator.task(taskId);
    return task ? task->phase : TaskPhase::Failed;
}

int attempts(const Coordinator& coordinator, const std::string& taskId) {
    const auto task = coordinator.task(taskId);
    return task ? task->attempts : -1;
}

// A coordinator with one node known and T-1 offered to it at kT0.
struct Offered {
    Coordinator coordinator;
    TimeMs acceptBy = 0;

    Offered() {
        tell(coordinator, snapshot("A1", kT0, 1));
        coordinator.submit(makeTask("T-1"));
        const auto sent = offers(coordinator, kT0);
        if (sent.size() == 1) acceptBy = sent[0].acceptBy;
    }
};

void test_heartbeats_and_contact() {
    TEST("a heartbeat goes to everyone on schedule, and a node's keeps it in contact");

    Coordinator coordinator;
    auto out = coordinator.tick(kT0);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK(out.size() == 1 && out[0].destination.empty());  // broadcast
    auto beats = only<HeartbeatPacket>(packetsOf(out));
    CHECK(beats.size() == 1 && beats[0].source.role == Role::Coordinator);
    CHECK(beats.size() == 1 && beats[0].source.id == kCoordinator);

    CHECK(coordinator.tick(kT0 + 1 * kSecond).empty());
    const auto next = only<HeartbeatPacket>(packetsOf(coordinator.tick(kT0 + 5 * kSecond)));
    CHECK(next.size() == 1 && beats.size() == 1 && next[0].seq > beats[0].seq);

    // Two nodes report once. One then sends only heartbeats, one nothing.
    tell(coordinator, snapshot("A1", kT0, 1));
    tell(coordinator, snapshot("A2", kT0, 1));
    for (TimeMs t = kT0 + 5 * kSecond; t <= kT0 + kMinute; t += 5 * kSecond) {
        HeartbeatPacket beat;
        beat.timestamp = t;
        beat.source = {"A1", Role::Node};
        beat.seq = static_cast<std::uint64_t>((t - kT0) / kSecond);
        coordinator.receive(encode(beat), t);
        coordinator.tick(t);
    }
    const auto nodes = coordinator.nodes();
    CHECK_EQ(nodes.size(), std::size_t(2));
    CHECK(nodes.size() == 2 && nodes[0].id == "A1" && nodes[0].contact == Contact::InContact);
    CHECK(nodes.size() == 2 && nodes[1].id == "A2" && nodes[1].contact == Contact::Lost);
    CHECK_EQ(coordinator.summary().nodesInContact, 1);
    CHECK_EQ(coordinator.summary().nodesLost, 1);
}

void test_work_goes_only_to_a_node_that_can_take_it() {
    TEST("a task is offered only to a node that is healthy, listening, capable and in place");

    Coordinator coordinator;
    CHECK(coordinator.submit(makeTask("T-1")));
    CHECK(!coordinator.submit(makeTask("T-1")));  // ids are unique
    CHECK(offers(coordinator, kT0).empty());      // nobody to offer it to

    StatusPacket deaf = snapshot("N-autonomous", kT0, 1);
    deaf.link = LinkView::Autonomous;  // it would not hear the offer
    tell(coordinator, deaf);

    StatusPacket degraded = snapshot("N-degraded", kT0, 1);
    degraded.mode = Mode::Degraded;
    tell(coordinator, degraded);

    StatusPacket safe = snapshot("N-safe", kT0, 1);
    safe.mode = Mode::Safe;
    tell(coordinator, safe);

    StatusPacket far = snapshot("N-far", kT0, 1);
    far.location.zone = Zone::NEA;
    tell(coordinator, far);

    StatusPacket unable = snapshot("N-unable", kT0, 1);
    unable.capabilities = {TaskAction::Refine};
    tell(coordinator, unable);

    // Says it is busy with something this coordinator never gave it.
    tell(coordinator, holding("N-busy", kT0, 1, "X-9", TaskState::Active));

    CHECK(offers(coordinator, kT0).empty());
    CHECK(phase(coordinator, "T-1") == TaskPhase::Queued);

    tell(coordinator, snapshot("N-good", kT0 + kSecond, 1));
    const auto sent = offers(coordinator, kT0 + kSecond);
    CHECK_EQ(sent.size(), std::size_t(1));
    if (sent.empty()) return;
    CHECK_EQ(sent[0].destination, std::string("N-good"));
    CHECK_EQ(sent[0].task.id, std::string("T-1"));
    CHECK(sent[0].source.role == Role::Coordinator);
    CHECK_EQ(sent[0].acceptBy, kT0 + kSecond + 10 * kSecond);
    CHECK(phase(coordinator, "T-1") == TaskPhase::Offered);
    CHECK(offers(coordinator, kT0 + 2 * kSecond).empty());  // not offered twice
}

void test_one_thing_at_a_time_and_work_is_spread() {
    TEST("a node holds one offer at a time, and idle nodes take turns");

    Coordinator coordinator;
    tell(coordinator, snapshot("A1", kT0, 1));
    tell(coordinator, snapshot("A2", kT0, 1));
    coordinator.submit(makeTask("T-1"));
    coordinator.submit(makeTask("T-2"));
    coordinator.submit(makeTask("T-3"));

    auto sent = offers(coordinator, kT0);
    CHECK_EQ(sent.size(), std::size_t(2));
    CHECK(sent.size() == 2 && sent[0].task.id == "T-1" && sent[0].destination == "A1");
    CHECK(sent.size() == 2 && sent[1].task.id == "T-2" && sent[1].destination == "A2");
    CHECK(phase(coordinator, "T-3") == TaskPhase::Queued);

    // A1 finishes; A2 is still working. The waiting task goes to A1.
    tell(coordinator, holding("A1", kT0 + 10 * kSecond, 2, "T-1", TaskState::Completed));
    tell(coordinator, holding("A2", kT0 + 10 * kSecond, 2, "T-2", TaskState::Active));
    sent = offers(coordinator, kT0 + 10 * kSecond);
    CHECK(sent.size() == 1 && sent[0].task.id == "T-3" && sent[0].destination == "A1");

    // Both idle. A1 sorts first, but A2 has waited longer for an offer.
    tell(coordinator, holding("A2", kT0 + 15 * kSecond, 3, "T-2", TaskState::Completed));
    tell(coordinator, holding("A1", kT0 + 20 * kSecond, 3, "T-3", TaskState::Completed));
    tell(coordinator, snapshot("A2", kT0 + 20 * kSecond, 4));
    coordinator.submit(makeTask("T-4"));
    sent = offers(coordinator, kT0 + 20 * kSecond);
    CHECK(sent.size() == 1 && sent[0].task.id == "T-4" && sent[0].destination == "A2");

    const auto summary = coordinator.summary();
    CHECK_EQ(summary.completed, 3);
    CHECK_EQ(summary.offered, 1);
    CHECK_EQ(summary.queued, 0);
}

void test_the_accept_deadline_allows_for_the_link() {
    TEST("an offer to a distant node is given time to get there");

    Coordinator coordinator;
    // Sent 40 s before it arrived: a one-way light-time of 40 s.
    coordinator.receive(encode(snapshot("M3", kT0 - 40 * kSecond, 1)), kT0);
    coordinator.submit(makeTask("T-1"));
    const auto sent = offers(coordinator, kT0);
    CHECK_EQ(sent.size(), std::size_t(1));
    if (sent.empty()) return;
    CHECK_EQ(sent[0].acceptBy, kT0 + 40 * kSecond + 10 * kSecond);
    CHECK_EQ(coordinator.nodes()[0].linkDelayMs, 40 * kSecond);
}

void test_accept_then_complete() {
    TEST("an accepted offer becomes active, and its completion is recorded exactly once");

    Offered f;
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Offered);

    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Active);

    const std::string event = completionEvent("A1", "T-1", kT0 + kMinute);
    f.coordinator.receive(event, kT0 + kMinute);
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Completed);
    CHECK_EQ(f.coordinator.feed().size(), std::size_t(1));
    CHECK(f.coordinator.feed().size() == 1 && f.coordinator.feed()[0] == event);  // verbatim

    f.coordinator.receive(event, kT0 + kMinute + kSecond);  // the link repeats it
    const auto summary = f.coordinator.summary();
    CHECK_EQ(f.coordinator.feed().size(), std::size_t(1));
    CHECK_EQ(summary.events, 1);
    CHECK_EQ(summary.completed, 1);
    CHECK(summary.odciKg == 5.2);
    CHECK(summary.processedKg == 5.2);
    const auto done = f.coordinator.task("T-1");
    CHECK(done && done->nodeId == "A1");  // who did it stays on record
}

void test_completion_survives_a_lost_event() {
    TEST("if the event never arrives, the node's status still settles the task");

    Offered f;
    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
    tell(f.coordinator, holding("A1", kT0 + kMinute, 3, "T-1", TaskState::Completed));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Completed);
    CHECK(f.coordinator.feed().empty());

    // And if the acceptance was lost as well: the offer was still pending
    // here when the node reported the work done.
    Offered g;
    tell(g.coordinator, holding("A1", kT0 + kMinute, 2, "T-1", TaskState::Completed));
    CHECK(phase(g.coordinator, "T-1") == TaskPhase::Completed);
}

void test_a_refused_offer_goes_back_in_the_queue() {
    TEST("a refusal returns the task to the queue without hammering the node");

    Offered f;
    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2, RejectReason::Busy);
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Queued);
    CHECK_EQ(attempts(f.coordinator, "T-1"), 0);  // a refusal is not a failure

    // "Busy" is true for now. No new offer until the node reports again.
    CHECK(offers(f.coordinator, kT0 + kSecond).empty());
    CHECK(offers(f.coordinator, kT0 + 2 * kSecond).empty());
    tell(f.coordinator, snapshot("A1", kT0 + 3 * kSecond, 3));
    auto sent = offers(f.coordinator, kT0 + 3 * kSecond);
    CHECK(sent.size() == 1 && sent[0].destination == "A1");

    // "Unsupported" is permanent: this node is not asked again for this
    // task, however healthy it later looks. Another node gets it.
    answer(f.coordinator, "A1", "T-1", kT0 + 4 * kSecond, 4, RejectReason::Unsupported);
    tell(f.coordinator, snapshot("A1", kT0 + 5 * kSecond, 5));
    CHECK(offers(f.coordinator, kT0 + 5 * kSecond).empty());
    tell(f.coordinator, snapshot("A2", kT0 + 6 * kSecond, 1));
    sent = offers(f.coordinator, kT0 + 6 * kSecond);
    CHECK(sent.size() == 1 && sent[0].destination == "A2" && sent[0].task.id == "T-1");
}

void test_an_offer_never_taken_is_noticed() {
    TEST("an offer that was lost is noticed from the node's own status, not from a timer");

    Offered f;
    CHECK_EQ(f.acceptBy, kT0 + 10 * kSecond);

    // Sent before the offer expired: it may simply not have arrived yet.
    tell(f.coordinator, snapshot("A1", kT0 + 5 * kSecond, 2));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Offered);

    // Sent after it expired, but within the allowance for clock skew.
    tell(f.coordinator, snapshot("A1", f.acceptBy + 1000, 3));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Offered);

    // Sent clearly after it expired, and the task is not there. The node
    // may only take an offer before acceptBy, so it did not take this one.
    tell(f.coordinator, snapshot("A1", f.acceptBy + 1001, 4));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Queued);
    CHECK_EQ(attempts(f.coordinator, "T-1"), 0);

    const auto sent = offers(f.coordinator, f.acceptBy + 1001);
    CHECK(sent.size() == 1 && sent[0].destination == "A1");
    CHECK(sent.size() == 1 && sent[0].acceptBy > f.acceptBy);  // a fresh deadline
}

// Keep A2 reporting every 5 s from `from` to `until` while A1 says nothing,
// and count every offer made in that time.
std::size_t offersWhileA1IsSilent(Coordinator& coordinator, TimeMs from, TimeMs until,
                                  std::uint64_t& a2Seq) {
    std::size_t made = 0;
    for (TimeMs t = from; t <= until; t += 5 * kSecond) {
        tell(coordinator, snapshot("A2", t, a2Seq++));
        made += offers(coordinator, t).size();
    }
    return made;
}

void test_silence_is_never_evidence() {
    TEST("a task held by a silent node is in doubt -- and is NOT handed to another");

    // The node took the task, then went quiet. An idle, healthy, capable
    // node sits right there for the next hour. The task must not move.
    Offered f;
    std::uint64_t a2Seq = 1;
    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
    CHECK_EQ(offersWhileA1IsSilent(f.coordinator, kT0 + 5 * kSecond, kT0 + 60 * kMinute, a2Seq),
             std::size_t(0));

    CHECK(phase(f.coordinator, "T-1") == TaskPhase::InDoubt);
    const auto held = f.coordinator.task("T-1");
    CHECK(held && held->nodeId == "A1");
    auto summary = f.coordinator.summary();
    CHECK_EQ(summary.inDoubt, 1);
    CHECK_EQ(summary.nodesLost, 1);
    CHECK_EQ(summary.nodesInContact, 1);

    // The same holds for an offer that was never even answered: the node
    // may have taken it and be unable to say so.
    Offered g;
    std::uint64_t seq = 1;
    CHECK_EQ(offersWhileA1IsSilent(g.coordinator, kT0 + 5 * kSecond, kT0 + 15 * kSecond, seq),
             std::size_t(0));
    CHECK(phase(g.coordinator, "T-1") == TaskPhase::Offered);  // not yet overdue
    CHECK_EQ(offersWhileA1IsSilent(g.coordinator, kT0 + 20 * kSecond, kT0 + 60 * kMinute, seq),
             std::size_t(0));
    CHECK(phase(g.coordinator, "T-1") == TaskPhase::InDoubt);
}

void test_a_returning_node_settles_its_own_task() {
    TEST("when a silent node is heard again, what it says decides the task");

    const TimeMs back = kT0 + 61 * kMinute;

    {   // It finished while out of contact.
        Offered f;
        std::uint64_t seq = 1;
        answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
        offersWhileA1IsSilent(f.coordinator, kT0 + 5 * kSecond, kT0 + 60 * kMinute, seq);
        tell(f.coordinator, holding("A1", back, 3, "T-1", TaskState::Completed));
        CHECK(phase(f.coordinator, "T-1") == TaskPhase::Completed);
        CHECK_EQ(f.coordinator.summary().nodesLost, 0);
    }
    {   // It is still working.
        Offered f;
        std::uint64_t seq = 1;
        answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
        offersWhileA1IsSilent(f.coordinator, kT0 + 5 * kSecond, kT0 + 60 * kMinute, seq);
        tell(f.coordinator, holding("A1", back, 3, "T-1", TaskState::Active));
        CHECK(phase(f.coordinator, "T-1") == TaskPhase::Active);
        CHECK(offers(f.coordinator, back).empty());
    }
    {   // It restarted and no longer has the work. Now it may move.
        Offered f;
        std::uint64_t seq = 1;
        answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
        offersWhileA1IsSilent(f.coordinator, kT0 + 5 * kSecond, kT0 + 60 * kMinute, seq);
        tell(f.coordinator, snapshot("A1", back, 0));  // counter reset by the restart
        CHECK(phase(f.coordinator, "T-1") == TaskPhase::Queued);
        CHECK_EQ(attempts(f.coordinator, "T-1"), 1);
        tell(f.coordinator, snapshot("A2", back, seq++));
        const auto sent = offers(f.coordinator, back);
        CHECK(sent.size() == 1 && sent[0].task.id == "T-1" && sent[0].destination == "A2");
    }
    {   // It never got the offer in the first place.
        Offered f;
        std::uint64_t seq = 1;
        offersWhileA1IsSilent(f.coordinator, kT0 + 5 * kSecond, kT0 + 60 * kMinute, seq);
        CHECK(phase(f.coordinator, "T-1") == TaskPhase::InDoubt);
        tell(f.coordinator, snapshot("A1", back, 2));
        CHECK(phase(f.coordinator, "T-1") == TaskPhase::Queued);
        CHECK_EQ(attempts(f.coordinator, "T-1"), 0);
    }
}

void test_a_failed_task_is_retried_then_given_up() {
    TEST("a task that fails is retried, and abandoned after the configured attempts");

    Coordinator coordinator;
    tell(coordinator, snapshot("A1", kT0, 1));
    coordinator.submit(makeTask("T-1"));

    std::uint64_t seq = 2;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const TimeMs t = kT0 + attempt * 10 * kSecond;
        CHECK_EQ(offers(coordinator, t).size(), std::size_t(1));
        tell(coordinator, holding("A1", t + kSecond, seq++, "T-1", TaskState::Failed));
        CHECK_EQ(attempts(coordinator, "T-1"), attempt);
        CHECK(phase(coordinator, "T-1") ==
              (attempt < 3 ? TaskPhase::Queued : TaskPhase::Failed));
    }
    CHECK(offers(coordinator, kT0 + kMinute).empty());
    CHECK_EQ(coordinator.summary().failed, 1);
}

void test_an_old_status_cannot_roll_the_picture_back() {
    TEST("packets that arrive out of order are not applied out of order");

    Coordinator coordinator;
    tell(coordinator, snapshot("A1", kT0 + 10 * kSecond, 10));

    StatusPacket older = snapshot("A1", kT0 + 9 * kSecond, 9);
    older.mode = Mode::Safe;
    coordinator.receive(encode(older), kT0 + 11 * kSecond);  // arrives late
    CHECK(coordinator.nodes()[0].mode == Mode::Nominal);
    CHECK_EQ(coordinator.summary().stalePackets, std::uint64_t(1));

    coordinator.receive(encode(snapshot("A1", kT0 + 10 * kSecond, 10)), kT0 + 12 * kSecond);
    CHECK_EQ(coordinator.summary().stalePackets, std::uint64_t(2));  // an exact repeat

    // A node that restarts counts from zero again. Its clock has moved on,
    // so it is still understood.
    StatusPacket restarted = snapshot("A1", kT0 + 20 * kSecond, 0);
    restarted.mode = Mode::Degraded;
    tell(coordinator, restarted);
    CHECK(coordinator.nodes()[0].mode == Mode::Degraded);

    // The case that matters most: a status sent just BEFORE the node took a
    // task arrives just AFTER its acceptance. Read naively it says "I do
    // not have that task" and would send the work to someone else.
    Offered f;
    answer(f.coordinator, "A1", "T-1", kT0 + 1000, 5);
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Active);
    f.coordinator.receive(encode(snapshot("A1", kT0 + 500, 4)), kT0 + 2000);
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Active);
    CHECK_EQ(attempts(f.coordinator, "T-1"), 0);
}

void test_a_node_that_drops_its_task_has_failed_it() {
    TEST("a status newer than the acceptance that no longer lists the task fails it");

    Offered f;
    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 2);
    tell(f.coordinator, snapshot("A1", kT0 + 2 * kSecond, 3));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Queued);
    CHECK_EQ(attempts(f.coordinator, "T-1"), 1);
}

void test_bad_packets_are_counted_and_dropped() {
    TEST("malformed and misdirected packets change nothing");

    Offered f;
    f.coordinator.receive("{ nope", kT0);
    f.coordinator.receive("", kT0);
    CHECK_EQ(f.coordinator.summary().malformedPackets, std::uint64_t(2));

    AssignmentPacket stray;  // a coordinator does not take assignments
    stray.timestamp = kT0;
    stray.source = {"another-coordinator", Role::Coordinator};
    stray.destination = kCoordinator;
    stray.task = makeTask("T-9");
    stray.acceptBy = kT0 + kMinute;
    f.coordinator.receive(encode(stray), kT0);

    answer(f.coordinator, "A1", "T-unknown", kT0 + kSecond, 2);             // no such task
    answer(f.coordinator, "A1", "T-1", kT0 + kSecond, 3, std::nullopt, "another-coordinator");
    answer(f.coordinator, "stranger", "T-1", kT0 + kSecond, 1);             // never reported in

    HeartbeatPacket unknown;
    unknown.timestamp = kT0;
    unknown.source = {"stranger", Role::Node};
    f.coordinator.receive(encode(unknown), kT0);

    CHECK_EQ(f.coordinator.summary().ignoredPackets, std::uint64_t(5));
    CHECK(phase(f.coordinator, "T-1") == TaskPhase::Offered);  // untouched by any of it
    CHECK_EQ(f.coordinator.nodes().size(), std::size_t(1));
    CHECK(f.coordinator.tasks().size() == 1);
}

void test_conflicts_are_reported_not_acted_on() {
    TEST("a node doing work it was not given is reported, and nothing is reassigned");

    Offered f;
    tell(f.coordinator, snapshot("A2", kT0, 1));

    // A2 claims to have accepted the task that is on offer to A1.
    answer(f.coordinator, "A2", "T-1", kT0 + kSecond, 2);
    CHECK_EQ(f.coordinator.conflicts().size(), std::size_t(1));
    const auto task = f.coordinator.task("T-1");
    CHECK(task && task->nodeId == "A1" && task->phase == TaskPhase::Offered);

    // A2 reports working on it, repeatedly. One conflict, not one per status.
    tell(f.coordinator, holding("A2", kT0 + 2 * kSecond, 3, "T-1", TaskState::Active));
    tell(f.coordinator, holding("A2", kT0 + 3 * kSecond, 4, "T-1", TaskState::Active));
    CHECK_EQ(f.coordinator.conflicts().size(), std::size_t(2));
    CHECK_EQ(f.coordinator.summary().conflicts, std::size_t(2));
    CHECK(f.coordinator.task("T-1") && f.coordinator.task("T-1")->nodeId == "A1");
}

}  // namespace

void runCoordinatorTests() {
    std::printf("coordinator\n");
    test_heartbeats_and_contact();
    test_work_goes_only_to_a_node_that_can_take_it();
    test_one_thing_at_a_time_and_work_is_spread();
    test_the_accept_deadline_allows_for_the_link();
    test_accept_then_complete();
    test_completion_survives_a_lost_event();
    test_a_refused_offer_goes_back_in_the_queue();
    test_an_offer_never_taken_is_noticed();
    test_silence_is_never_evidence();
    test_a_returning_node_settles_its_own_task();
    test_a_failed_task_is_retried_then_given_up();
    test_an_old_status_cannot_roll_the_picture_back();
    test_a_node_that_drops_its_task_has_failed_it();
    test_bad_packets_are_counted_and_dropped();
    test_conflicts_are_reported_not_acted_on();
}
