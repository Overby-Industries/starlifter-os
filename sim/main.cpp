// starlifter_fleet_sim -- one coordinator and five nodes through twenty
// simulated minutes that go wrong in the ways the design has to survive:
//
//   * two pods at a near-Earth asteroid, 40 light-seconds away
//   * a refinery that drops out of contact for five minutes mid-task
//   * a reclaimer that faults and safes itself mid-capture
//   * a miner that goes silent for good just as it is offered work
//
// Usage:
//   starlifter_fleet_sim [--log FILE] [--feed FILE] [--quiet]
//
//   --log FILE    write every packet put on the wire, one per line (JSON
//                 Lines), for validation against the OTF-1 schema
//   --feed FILE   write the completed-work events the coordinator received:
//                 the public record a transparency dashboard would show
//   --quiet       print only the final summary, not the timeline
//
// Exits non-zero if the run broke an invariant: a task recorded as done
// more than once, a node working on something it was not given, or a
// packet on the wire that is not valid OTF-1.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "starlifter/sim.hpp"

using namespace starlifter;
using starlifter::sim::FleetSim;
using starlifter::sim::NodeSpec;

namespace {

constexpr TimeMs kSecond = 1000;
constexpr TimeMs kMinute = 60 * kSecond;
constexpr TimeMs kTick = 500;
constexpr TimeMs kRunFor = 20 * kMinute;

NodeSpec pod(const char* id, NodeType type, const char* mission, Zone zone,
             std::vector<TaskAction> capabilities, TimeMs taskDurationMs, TimeMs oneWayDelayMs) {
    NodeSpec spec;
    spec.node.id = id;
    spec.node.type = type;
    spec.node.missionId = mission;
    spec.pod.location.zone = zone;
    spec.pod.capabilities = std::move(capabilities);
    spec.pod.taskDurationMs = taskDurationMs;
    spec.link.oneWayDelayMs = oneWayDelayMs;
    return spec;
}

Task task(const char* id, TaskAction action, Zone zone) {
    Task t;
    t.id = id;
    t.action = action;
    t.location.zone = zone;
    return t;
}

Task task(const char* id, TaskAction action, Zone zone, ObjectType objectType, double massKg,
          const char* catalogId = nullptr) {
    Task t = task(id, action, zone);
    Target target;
    target.objectType = objectType;
    target.massKg = massKg;
    if (catalogId != nullptr) target.catalogId = catalogId;
    t.target = target;
    return t;
}

std::string clock(TimeMs sinceStart) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "+%02lld:%02lld.%lld",
                  static_cast<long long>(sinceStart / kMinute),
                  static_cast<long long>((sinceStart / kSecond) % 60),
                  static_cast<long long>((sinceStart % kSecond) / 100));
    return buf;
}

bool writeLines(const char* path, const std::vector<std::string>& lines) {
    std::ofstream out(path, std::ios::binary);
    for (const std::string& line : lines) out << line << '\n';
    return static_cast<bool>(out);
}

const char* kindOf(const Packet& packet) {
    if (std::holds_alternative<EventPacket>(packet)) return "event";
    if (std::holds_alternative<StatusPacket>(packet)) return "status";
    if (std::holds_alternative<HeartbeatPacket>(packet)) return "heartbeat";
    if (std::holds_alternative<AssignmentPacket>(packet)) return "assignment";
    return "ack";
}

}  // namespace

int main(int argc, char** argv) {
    const char* logPath = nullptr;
    const char* feedPath = nullptr;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else if (std::strcmp(argv[i], "--feed") == 0 && i + 1 < argc) {
            feedPath = argv[++i];
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else {
            std::fprintf(stderr, "usage: %s [--log FILE] [--feed FILE] [--quiet]\n", argv[0]);
            return 2;
        }
    }

    TimeMs start = 0;
    parseTimestamp("2031-03-14T09:00:00Z", start);

    FleetSim fleet({}, start, kTick);

    // Low Earth orbit: half a second each way through a relay.
    fleet.addNode(pod("A1", NodeType::Reclaimer, "orbital-demo-1", Zone::LEO,
                      {TaskAction::Capture, TaskAction::Deorbit}, 90 * kSecond, 500));
    fleet.addNode(pod("A2", NodeType::Reclaimer, "orbital-demo-1", Zone::LEO,
                      {TaskAction::Capture, TaskAction::Deorbit}, 90 * kSecond, 500));
    fleet.addNode(pod("S1", NodeType::Shuttle, "starlifter-demo-1", Zone::LEO,
                      {TaskAction::Deploy}, 2 * kMinute, 500));

    // A near-Earth asteroid: 40 light-seconds each way.
    NodeSpec refinery = pod("R7", NodeType::Refinery, "nea-2031-alpha", Zone::NEA,
                            {TaskAction::Process, TaskAction::Refine}, 3 * kMinute, 40 * kSecond);
    refinery.link.outages = {{start + 4 * kMinute, start + 9 * kMinute}};
    fleet.addNode(refinery);

    NodeSpec miner = pod("M3", NodeType::Miner, "nea-2031-alpha", Zone::NEA,
                         {TaskAction::Process, TaskAction::Store}, 3 * kMinute, 40 * kSecond);
    miner.link.outages = {{start + 6 * kMinute, start + 100 * kRunFor}};  // never returns
    fleet.addNode(miner);

    fleet.submitAt(start, task("T-0001", TaskAction::Capture, Zone::LEO, ObjectType::Fragment, 5.2,
                               "NORAD-33953"));
    fleet.submitAt(start, task("T-0002", TaskAction::Capture, Zone::LEO, ObjectType::SpentStage,
                               1400.0, "SIM-0002"));
    fleet.submitAt(start, task("T-0003", TaskAction::Capture, Zone::LEO, ObjectType::Fragment, 12.5,
                               "SIM-0003"));
    fleet.submitAt(start, task("T-0004", TaskAction::Deorbit, Zone::LEO,
                               ObjectType::DerelictSatellite, 310.0, "SIM-0004"));
    fleet.submitAt(start, task("T-0005", TaskAction::Deploy, Zone::LEO));
    fleet.submitAt(start, task("T-0006", TaskAction::Process, Zone::NEA, ObjectType::Regolith, 1200.0));
    fleet.submitAt(start, task("T-0007", TaskAction::Refine, Zone::NEA, ObjectType::Ore, 800.0));
    fleet.submitAt(start, task("T-0008", TaskAction::Process, Zone::NEA, ObjectType::Volatile, 450.0));
    fleet.submitAt(start + 5 * kMinute, task("T-0009", TaskAction::Capture, Zone::LEO,
                                             ObjectType::Fragment, 3.1, "SIM-0009"));

    fleet.safeModeAt(start + 2 * kMinute, "A2");  // faults mid-task

    std::printf("Starlifter OS fleet simulation\n");
    std::printf("start %s, %lld simulated minutes, %lld ms tick\n\n",
                formatTimestamp(start).c_str(), static_cast<long long>(kRunFor / kMinute),
                static_cast<long long>(kTick));

    // Run, narrating each change in the coordinator's picture of the fleet.
    std::map<std::string, std::string> taskLine;
    std::map<std::string, std::string> nodeLine;
    if (!quiet) std::printf("Timeline (as the coordinator sees it)\n");
    while (fleet.now() < start + kRunFor) {
        fleet.step();
        if (quiet) continue;
        const std::string at = clock(fleet.now() - kTick - start);
        for (const NodeView& node : fleet.coordinator().nodes()) {
            const std::string line = std::string(toString(node.contact)) + ", " + toString(node.mode);
            std::string& previous = nodeLine[node.id];
            if (previous != line) {
                std::printf("  %s  node %-3s %s%s\n", at.c_str(), node.id.c_str(), line.c_str(),
                            previous.empty() ? "  (first heard from)" : "");
                previous = line;
            }
        }
        for (const TaskView& view : fleet.coordinator().tasks()) {
            std::string line = toString(view.phase);
            if (!view.nodeId.empty()) line += " (" + view.nodeId + ")";
            std::string& previous = taskLine[view.task.id];
            if (previous != line) {
                std::printf("  %s  %s  %-8s %s\n", at.c_str(), view.task.id.c_str(),
                            toString(view.task.action), line.c_str());
                previous = line;
            }
        }
    }

    const Coordinator& coordinator = fleet.coordinator();
    const FleetSummary summary = coordinator.summary();

    std::printf("\nNodes\n");
    for (const NodeView& node : coordinator.nodes()) {
        std::printf("  %-3s %-9s %-4s %-10s %-8s link delay %5.1f s\n", node.id.c_str(),
                    toString(node.type), toString(node.location.zone), toString(node.contact),
                    toString(node.mode), static_cast<double>(node.linkDelayMs) / 1000.0);
    }

    std::printf("\nTasks\n");
    for (const TaskView& view : coordinator.tasks()) {
        std::printf("  %s  %-8s %-4s %-9s %-3s failures %d\n", view.task.id.c_str(),
                    toString(view.task.action), toString(view.task.location.zone),
                    toString(view.phase), view.nodeId.empty() ? "-" : view.nodeId.c_str(),
                    view.attempts);
    }

    // What went over the wire, and whether all of it is valid OTF-1.
    std::map<std::string, int> sentByKind;
    int undecodable = 0;
    for (const std::string& text : fleet.wireLog()) {
        Packet packet;
        if (decode(text, packet)) ++sentByKind[kindOf(packet)];
        else ++undecodable;
    }
    std::printf("\nWire\n  %zu packets sent, %llu lost to outages\n ", fleet.wireLog().size(),
                static_cast<unsigned long long>(fleet.packetsLost()));
    for (const auto& entry : sentByKind) std::printf(" %s %d ", entry.first.c_str(), entry.second);
    std::printf("\n");

    std::printf("\nSummary\n");
    std::printf("  tasks      %d completed, %d in doubt, %d active, %d offered, %d queued, %d failed\n",
                summary.completed, summary.inDoubt, summary.active, summary.offered,
                summary.queued, summary.failed);
    std::printf("  nodes      %d in contact, %d lost\n", summary.nodesInContact, summary.nodesLost);
    std::printf("  record     %d events, %.1f kg processed, %.1f kg toward ODCI\n", summary.events,
                summary.processedKg, summary.odciKg);
    std::printf("  conflicts  %zu\n", summary.conflicts);
    for (const std::string& conflict : coordinator.conflicts()) {
        std::printf("             %s\n", conflict.c_str());
    }
    if (summary.inDoubt > 0) {
        std::printf("\n  In doubt means offered to, or held by, a node that has gone silent.\n"
                    "  Those tasks were deliberately not handed to another node: silence is\n"
                    "  not evidence that the work stopped.\n");
    }

    // Invariants.
    bool broken = undecodable > 0 || summary.conflicts > 0 || summary.malformedPackets > 0;
    std::map<std::string, int> recordsPerTask;
    for (const std::string& text : coordinator.feed()) {
        Packet packet;
        if (!decode(text, packet)) continue;
        if (const EventPacket* event = std::get_if<EventPacket>(&packet)) {
            if (event->taskId && ++recordsPerTask[*event->taskId] > 1) broken = true;
        }
    }

    if (logPath != nullptr) {
        if (!writeLines(logPath, fleet.wireLog())) {
            std::fprintf(stderr, "could not write %s\n", logPath);
            return 2;
        }
        std::printf("\nwire log   %s (%zu packets)\n", logPath, fleet.wireLog().size());
    }
    if (feedPath != nullptr) {
        if (!writeLines(feedPath, coordinator.feed())) {
            std::fprintf(stderr, "could not write %s\n", feedPath);
            return 2;
        }
        std::printf("event feed %s (%zu events)\n", feedPath, coordinator.feed().size());
    }

    if (broken) {
        std::printf("\nINVARIANT BROKEN: see conflicts, duplicate records or invalid packets above\n");
        return 1;
    }
    return 0;
}
