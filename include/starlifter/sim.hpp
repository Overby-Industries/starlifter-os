#pragma once

// A deterministic fleet in a box: one coordinator, any number of nodes, and
// the links between them, all driven by a simulated clock.
//
// Nothing here is flight software. It exists so the coordinator and the
// fleet agent can be exercised against the conditions that actually shape
// the design -- light-delay, outages, a vehicle faulting mid-task -- and so
// that a scenario replays identically every time. There is no randomness
// and no wall clock: the same scenario always produces the same packets in
// the same order.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "starlifter/coordinator.hpp"
#include "starlifter/local_control.hpp"
#include "starlifter/node.hpp"

namespace starlifter::sim {

// A stand-in for a pod's own control software: it holds station, works on a
// task for a fixed time, and can be told to fault.
struct PodConfig {
    Location location;
    std::vector<TaskAction> capabilities;
    TimeMs taskDurationMs = 60000;
};

class SimulatedPod : public LocalControl {
public:
    explicit SimulatedPod(PodConfig config);

    // Fault injection. A real vehicle decides these for itself; a test has
    // to be able to make them happen.
    void enterSafeMode();  // sticky, like ShuttleFSM's AbortSafeMode
    void degrade();        // finish the task in hand, take nothing new

    void tick(TimeMs now) override;
    Mode mode() const override { return mode_; }
    std::string phase() const override;
    Location location() const override { return config_.location; }
    std::vector<TaskAction> capabilities() const override { return config_.capabilities; }
    bool begin(const Task& task, TimeMs now) override;
    WorkState work() const override { return work_; }
    double progress() const override;
    WorkOutcome outcome() const override;

private:
    PodConfig config_;
    Mode mode_ = Mode::Nominal;
    WorkState work_ = WorkState::Idle;
    Task task_;
    TimeMs startedAt_ = 0;
    TimeMs now_ = 0;
};

// A window during which nothing sent on a link arrives, in either
// direction. Packets already in flight when it opens still land.
struct Outage {
    TimeMs from = 0;
    TimeMs until = 0;  // exclusive
};

struct LinkConfig {
    TimeMs oneWayDelayMs = 0;
    std::vector<Outage> outages;
};

struct NodeSpec {
    NodeConfig node;
    PodConfig pod;
    LinkConfig link;
};

class FleetSim {
public:
    FleetSim(CoordinatorConfig config, TimeMs start, TimeMs tickMs);

    void addNode(const NodeSpec& spec);

    // Things that happen at a given simulated time.
    void submitAt(TimeMs at, const Task& task);
    void safeModeAt(TimeMs at, const std::string& nodeId);
    void degradeAt(TimeMs at, const std::string& nodeId);

    void step();
    void runUntil(TimeMs end);

    TimeMs now() const { return now_; }
    Coordinator& coordinator() { return coordinator_; }
    const Coordinator& coordinator() const { return coordinator_; }
    const Node& node(const std::string& id) const;
    SimulatedPod& pod(const std::string& id);

    // Every packet put on the wire, in transmission order, including those
    // an outage then swallowed. One OTF-1 packet per entry.
    const std::vector<std::string>& wireLog() const { return wireLog_; }
    std::uint64_t packetsLost() const { return lost_; }

private:
    struct Member {
        NodeSpec spec;
        std::unique_ptr<SimulatedPod> pod;
        std::unique_ptr<Node> node;
    };

    enum class Injection { Submit, SafeMode, Degrade };
    struct Scheduled {
        TimeMs at;
        Injection kind;
        std::string nodeId;
        Task task;
    };

    struct InFlight {
        bool toCoordinator;
        std::string nodeId;
        std::string packet;
    };

    void carry(Member& member, bool toCoordinator, const std::string& packet);

    Coordinator coordinator_;
    TimeMs now_;
    TimeMs tickMs_;
    std::map<std::string, Member> members_;
    std::vector<Scheduled> scheduled_;
    std::map<std::pair<TimeMs, std::uint64_t>, InFlight> inFlight_;  // by arrival, then send order
    std::uint64_t sendOrder_ = 0;
    std::vector<std::string> wireLog_;
    std::uint64_t lost_ = 0;
};

}  // namespace starlifter::sim
