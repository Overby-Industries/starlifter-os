#include "check.hpp"
#include "support.hpp"

#include <memory>

#include "starlifter/sim.hpp"

using namespace support;
using starlifter::sim::FleetSim;
using starlifter::sim::NodeSpec;

namespace {

constexpr TimeMs kTick = 500;
constexpr TimeMs kForever = kT0 + 365LL * 24 * 60 * kMinute;

NodeSpec reclaimer(const std::string& id, TimeMs oneWayDelayMs = 500) {
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

bool everythingOnTheWireIsOtf1(const FleetSim& fleet) {
    for (const std::string& text : fleet.wireLog()) {
        Packet packet;
        if (!decode(text, packet)) return false;
    }
    return !fleet.wireLog().empty();
}

std::vector<AssignmentPacket> offersOf(const FleetSim& fleet, const std::string& taskId) {
    std::vector<AssignmentPacket> found;
    for (const AssignmentPacket& offer : only<AssignmentPacket>(fleet.wireLog())) {
        if (offer.task.id == taskId) found.push_back(offer);
    }
    return found;
}

std::size_t offersTo(const FleetSim& fleet, const std::string& nodeId) {
    std::size_t count = 0;
    for (const AssignmentPacket& offer : only<AssignmentPacket>(fleet.wireLog())) {
        if (offer.destination == nodeId) ++count;
    }
    return count;
}

std::vector<EventPacket> recordFor(const FleetSim& fleet, const std::string& taskId) {
    std::vector<EventPacket> found;
    for (const EventPacket& event : only<EventPacket>(fleet.coordinator().feed())) {
        if (event.taskId && *event.taskId == taskId) found.push_back(event);
    }
    return found;
}

TaskPhase phase(const FleetSim& fleet, const std::string& taskId) {
    const auto task = fleet.coordinator().task(taskId);
    return task ? task->phase : TaskPhase::Failed;
}

std::string holder(const FleetSim& fleet, const std::string& taskId) {
    const auto task = fleet.coordinator().task(taskId);
    return task ? task->nodeId : "";
}

NodeView view(const FleetSim& fleet, const std::string& nodeId) {
    for (const NodeView& node : fleet.coordinator().nodes()) {
        if (node.id == nodeId) return node;
    }
    return {};
}

void test_a_quiet_fleet_does_everything_once() {
    TEST("with every link up, every task is done, and done exactly once");

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(reclaimer("A1"));
    fleet.addNode(reclaimer("A2"));
    fleet.addNode(reclaimer("A3"));
    for (int i = 1; i <= 6; ++i) fleet.submitAt(kT0, makeTask("T-" + std::to_string(i)));
    fleet.runUntil(kT0 + 10 * kMinute);

    const auto summary = fleet.coordinator().summary();
    CHECK_EQ(summary.completed, 6);
    CHECK_EQ(summary.events, 6);
    CHECK_EQ(summary.nodesInContact, 3);
    CHECK_EQ(summary.malformedPackets, std::uint64_t(0));
    CHECK(fleet.coordinator().conflicts().empty());
    CHECK_EQ(fleet.packetsLost(), std::uint64_t(0));
    CHECK(everythingOnTheWireIsOtf1(fleet));
    for (int i = 1; i <= 6; ++i) {
        const std::string id = "T-" + std::to_string(i);
        CHECK_EQ(recordFor(fleet, id).size(), std::size_t(1));
        CHECK_EQ(offersOf(fleet, id).size(), std::size_t(1));
    }
    CHECK(summary.odciKg > 31.19 && summary.odciKg < 31.21);  // 6 x 5.2 kg of debris
    // Three nodes, six tasks: the work was shared, two each.
    CHECK_EQ(offersTo(fleet, "A1"), std::size_t(2));
    CHECK_EQ(offersTo(fleet, "A2"), std::size_t(2));
    CHECK_EQ(offersTo(fleet, "A3"), std::size_t(2));
}

void test_light_delay_does_not_break_assignment() {
    TEST("a node 40 light-seconds away is offered work it has time to accept");

    NodeSpec refinery;
    refinery.node.id = "R7";
    refinery.node.type = NodeType::Refinery;
    refinery.node.missionId = "nea-2025-alpha";
    refinery.pod.location.zone = Zone::NEA;
    refinery.pod.capabilities = {TaskAction::Process, TaskAction::Refine};
    refinery.pod.taskDurationMs = kMinute;
    refinery.link.oneWayDelayMs = 40 * kSecond;

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(refinery);
    fleet.submitAt(kT0, makeTask("T-1", TaskAction::Process, Zone::NEA, 1200.0));
    fleet.submitAt(kT0, makeTask("T-2", TaskAction::Refine, Zone::NEA, 800.0));
    fleet.runUntil(kT0 + 10 * kMinute);

    CHECK(phase(fleet, "T-1") == TaskPhase::Completed);
    CHECK(phase(fleet, "T-2") == TaskPhase::Completed);
    CHECK_EQ(view(fleet, "R7").linkDelayMs, 40 * kSecond);
    for (const AckPacket& ack : only<AckPacket>(fleet.wireLog())) {
        CHECK(ack.result == AckResult::Accepted);  // nothing arrived already expired
    }
    for (const AssignmentPacket& offer : only<AssignmentPacket>(fleet.wireLog())) {
        CHECK(offer.acceptBy >= offer.timestamp + 40 * kSecond);
    }
    const auto record = recordFor(fleet, "T-1");
    CHECK(record.size() == 1 && record[0].action == EventAction::Processed);
    CHECK(record.size() == 1 && record[0].processedKg && *record[0].processedKg == 1200.0);
    CHECK(record.size() == 1 && !record[0].odciContributionKg);  // not debris removal
    CHECK(everythingOnTheWireIsOtf1(fleet));
}

void test_an_outage_mid_task_loses_nothing() {
    TEST("a node cut off mid-task finishes alone; the task is not duplicated or lost");

    NodeSpec cutOff = reclaimer("A1");
    cutOff.link.outages = {{kT0 + 30 * kSecond, kT0 + 150 * kSecond}};

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(cutOff);
    fleet.addNode(reclaimer("A2"));  // idle and capable throughout
    fleet.submitAt(kT0, makeTask("T-1"));

    // Mid-outage. The node has finished; the coordinator cannot know.
    fleet.runUntil(kT0 + 100 * kSecond);
    CHECK_EQ(holder(fleet, "T-1"), std::string("A1"));
    CHECK(phase(fleet, "T-1") == TaskPhase::InDoubt);
    CHECK(view(fleet, "A1").contact == Contact::Lost);
    CHECK(view(fleet, "A2").contact == Contact::InContact);
    CHECK(fleet.node("A1").link() == LinkView::Autonomous);
    CHECK(fleet.node("A1").task() && fleet.node("A1").task()->state == TaskState::Completed);
    CHECK_EQ(fleet.node("A1").heldEvents(), std::size_t(1));
    CHECK(fleet.coordinator().feed().empty());
    CHECK_EQ(offersTo(fleet, "A2"), std::size_t(0));  // the idle node was NOT sent after it
    CHECK(fleet.packetsLost() > 0);

    // The link returns. The node's own report settles it.
    fleet.runUntil(kT0 + 200 * kSecond);
    CHECK(phase(fleet, "T-1") == TaskPhase::Completed);
    CHECK(view(fleet, "A1").contact == Contact::InContact);
    CHECK_EQ(fleet.node("A1").heldEvents(), std::size_t(0));
    const auto record = recordFor(fleet, "T-1");
    CHECK_EQ(record.size(), std::size_t(1));
    // The record says when the capture happened, not when word got through.
    CHECK(record.size() == 1 && record[0].timestamp < kT0 + 70 * kSecond);
    CHECK_EQ(offersOf(fleet, "T-1").size(), std::size_t(1));
    CHECK(fleet.coordinator().conflicts().empty());
    CHECK(everythingOnTheWireIsOtf1(fleet));
}

void test_a_vehicle_fault_moves_the_work() {
    TEST("when a vehicle safes itself mid-task, it says so and another node takes over");

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(reclaimer("A1"));
    fleet.addNode(reclaimer("A2"));
    fleet.submitAt(kT0, makeTask("T-1"));
    fleet.safeModeAt(kT0 + 20 * kSecond, "A1");
    fleet.runUntil(kT0 + 3 * kMinute);

    CHECK(phase(fleet, "T-1") == TaskPhase::Completed);
    CHECK_EQ(holder(fleet, "T-1"), std::string("A2"));
    const auto task = fleet.coordinator().task("T-1");
    CHECK(task && task->attempts == 1);
    CHECK(view(fleet, "A1").mode == Mode::Safe);
    CHECK_EQ(offersTo(fleet, "A1"), std::size_t(1));  // nothing more once it had safed
    const auto record = recordFor(fleet, "T-1");
    CHECK(record.size() == 1 && record[0].podId == "A2");  // only real work is recorded
    CHECK(fleet.coordinator().conflicts().empty());
    CHECK(everythingOnTheWireIsOtf1(fleet));
}

void test_a_degraded_node_finishes_and_takes_no_more() {
    TEST("a degraded node completes the task in hand and is offered nothing new");

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(reclaimer("A1"));
    fleet.submitAt(kT0, makeTask("T-1"));
    fleet.submitAt(kT0, makeTask("T-2"));
    fleet.degradeAt(kT0 + 20 * kSecond, "A1");
    fleet.runUntil(kT0 + 5 * kMinute);

    CHECK(phase(fleet, "T-1") == TaskPhase::Completed);
    CHECK(phase(fleet, "T-2") == TaskPhase::Queued);
    CHECK(view(fleet, "A1").mode == Mode::Degraded);
    CHECK_EQ(offersTo(fleet, "A1"), std::size_t(1));
}

void test_a_node_lost_for_good_leaves_its_task_in_doubt() {
    TEST("a node that never comes back leaves one task in doubt and the fleet still working");

    NodeSpec gone = reclaimer("A1");
    gone.link.outages = {{kT0 + 30 * kSecond, kForever}};

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(gone);
    fleet.addNode(reclaimer("A2"));
    fleet.submitAt(kT0, makeTask("T-1"));               // goes to A1, which then vanishes
    fleet.submitAt(kT0 + kMinute, makeTask("T-2"));     // arrives after A1 is gone
    fleet.runUntil(kT0 + 30 * kMinute);

    CHECK(phase(fleet, "T-1") == TaskPhase::InDoubt);
    CHECK_EQ(holder(fleet, "T-1"), std::string("A1"));
    for (const AssignmentPacket& offer : offersOf(fleet, "T-1")) {
        CHECK_EQ(offer.destination, std::string("A1"));  // never offered to anyone else
    }
    CHECK_EQ(offersOf(fleet, "T-1").size(), std::size_t(1));

    // Losing a node does not stop the rest of the fleet.
    CHECK(phase(fleet, "T-2") == TaskPhase::Completed);
    CHECK_EQ(holder(fleet, "T-2"), std::string("A2"));
    const auto summary = fleet.coordinator().summary();
    CHECK_EQ(summary.inDoubt, 1);
    CHECK_EQ(summary.completed, 1);
    CHECK_EQ(summary.nodesLost, 1);
}

void test_losing_the_coordinator_does_not_halt_the_fleet() {
    TEST("with every link down at once, nodes keep working and the picture catches up");

    // From the nodes' side, every link failing together is what a
    // coordinator outage looks like.
    const starlifter::sim::Outage blackout{kT0 + 10 * kSecond, kT0 + 2 * kMinute};
    NodeSpec a1 = reclaimer("A1");
    NodeSpec a2 = reclaimer("A2");
    a1.link.outages = {blackout};
    a2.link.outages = {blackout};

    FleetSim fleet({}, kT0, kTick);
    fleet.addNode(a1);
    fleet.addNode(a2);
    fleet.submitAt(kT0, makeTask("T-1"));
    fleet.submitAt(kT0, makeTask("T-2"));
    fleet.submitAt(kT0 + kMinute, makeTask("T-3"));  // queued during the blackout

    fleet.runUntil(kT0 + 110 * kSecond);
    // Both nodes finished on their own; neither stopped to wait.
    CHECK(fleet.node("A1").task() && fleet.node("A1").task()->state == TaskState::Completed);
    CHECK(fleet.node("A2").task() && fleet.node("A2").task()->state == TaskState::Completed);
    CHECK(fleet.node("A1").link() == LinkView::Autonomous);
    CHECK_EQ(fleet.coordinator().summary().inDoubt, 2);
    CHECK(phase(fleet, "T-3") == TaskPhase::Queued);  // nobody to offer it to

    fleet.runUntil(kT0 + 5 * kMinute);
    const auto summary = fleet.coordinator().summary();
    CHECK_EQ(summary.completed, 3);
    CHECK_EQ(summary.events, 3);
    CHECK_EQ(summary.inDoubt, 0);
    for (const char* id : {"T-1", "T-2", "T-3"}) {
        CHECK_EQ(recordFor(fleet, id).size(), std::size_t(1));
        CHECK_EQ(offersOf(fleet, id).size(), std::size_t(1));
    }
    CHECK(fleet.coordinator().conflicts().empty());
    CHECK(everythingOnTheWireIsOtf1(fleet));
}

std::unique_ptr<FleetSim> eventfulScenario() {
    NodeSpec cutOff = reclaimer("A1");
    cutOff.link.outages = {{kT0 + 30 * kSecond, kT0 + 150 * kSecond}};
    auto fleet = std::make_unique<FleetSim>(CoordinatorConfig{}, kT0, kTick);
    fleet->addNode(cutOff);
    fleet->addNode(reclaimer("A2"));
    fleet->addNode(reclaimer("A3", 40 * kSecond));
    for (int i = 1; i <= 5; ++i) fleet->submitAt(kT0, makeTask("T-" + std::to_string(i)));
    fleet->safeModeAt(kT0 + 20 * kSecond, "A2");
    fleet->runUntil(kT0 + 15 * kMinute);
    return fleet;
}

void test_a_scenario_replays_identically() {
    TEST("the same scenario always produces the same packets in the same order");

    const auto first = eventfulScenario();
    const auto second = eventfulScenario();
    CHECK(first->wireLog().size() > 100);
    CHECK(first->wireLog() == second->wireLog());
    CHECK(first->coordinator().feed() == second->coordinator().feed());

    // And through an outage, a fault and a 40 s link, still no task done twice.
    for (int i = 1; i <= 5; ++i) {
        CHECK(recordFor(*first, "T-" + std::to_string(i)).size() <= 1);
    }
    CHECK(first->coordinator().conflicts().empty());
    CHECK(everythingOnTheWireIsOtf1(*first));
}

}  // namespace

void runFleetTests() {
    std::printf("fleet\n");
    test_a_quiet_fleet_does_everything_once();
    test_light_delay_does_not_break_assignment();
    test_an_outage_mid_task_loses_nothing();
    test_a_vehicle_fault_moves_the_work();
    test_a_degraded_node_finishes_and_takes_no_more();
    test_a_node_lost_for_good_leaves_its_task_in_doubt();
    test_losing_the_coordinator_does_not_halt_the_fleet();
    test_a_scenario_replays_identically();
}
