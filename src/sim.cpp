#include "starlifter/sim.hpp"

#include <algorithm>
#include <stdexcept>

namespace starlifter::sim {

// --------------------------------------------------------------------------
// SimulatedPod

SimulatedPod::SimulatedPod(PodConfig config) : config_(std::move(config)) {}

void SimulatedPod::enterSafeMode() {
    mode_ = Mode::Safe;
    if (work_ == WorkState::Active) work_ = WorkState::Failed;
}

void SimulatedPod::degrade() {
    if (mode_ == Mode::Nominal) mode_ = Mode::Degraded;
}

void SimulatedPod::tick(TimeMs now) {
    now_ = now;
    if (work_ == WorkState::Active && now - startedAt_ >= config_.taskDurationMs) {
        work_ = WorkState::Completed;
    }
}

std::string SimulatedPod::phase() const {
    if (mode_ == Mode::Safe) return "SafeMode";
    if (work_ != WorkState::Active) return "StationKeeping";
    switch (task_.action) {
        case TaskAction::Capture: return "Capturing";
        case TaskAction::Process: return "Processing";
        case TaskAction::Refine: return "Refining";
        case TaskAction::Store: return "Storing";
        case TaskAction::Deorbit: return "Deorbiting";
        case TaskAction::Deploy: return "Deploying";
    }
    return "StationKeeping";
}

bool SimulatedPod::begin(const Task& task, TimeMs now) {
    if (mode_ != Mode::Nominal) return false;
    task_ = task;
    startedAt_ = now;
    now_ = now;
    work_ = WorkState::Active;
    return true;
}

double SimulatedPod::progress() const {
    if (work_ == WorkState::Completed) return 1.0;
    if (work_ != WorkState::Active || config_.taskDurationMs <= 0) return 0.0;
    const double fraction =
        static_cast<double>(now_ - startedAt_) / static_cast<double>(config_.taskDurationMs);
    return std::clamp(fraction, 0.0, 1.0);
}

WorkOutcome SimulatedPod::outcome() const {
    WorkOutcome outcome;
    const double massKg = task_.target ? task_.target->massKg.value_or(0.0) : 0.0;
    outcome.processedKg = massKg;
    outcome.status =
        task_.action == TaskAction::Deploy ? OutcomeStatus::Delivered : OutcomeStatus::Stored;
    // A simulated pod always completes its work cleanly, so it always claims
    // compliance. A real vehicle would have to earn this flag.
    outcome.smessc = true;
    if (task_.action == TaskAction::Capture || task_.action == TaskAction::Deorbit) {
        outcome.odciContributionKg = massKg;  // debris mass removed from orbit
    }
    return outcome;
}

// --------------------------------------------------------------------------
// FleetSim

FleetSim::FleetSim(CoordinatorConfig config, TimeMs start, TimeMs tickMs)
    : coordinator_(std::move(config)), now_(start), tickMs_(tickMs) {
    if (tickMs_ <= 0) throw std::invalid_argument("FleetSim: tick must be positive");
}

void FleetSim::addNode(const NodeSpec& spec) {
    if (members_.count(spec.node.id) != 0) {
        throw std::invalid_argument("FleetSim: duplicate node id " + spec.node.id);
    }
    Member member;
    member.spec = spec;
    member.pod = std::make_unique<SimulatedPod>(spec.pod);
    member.node = std::make_unique<Node>(spec.node, *member.pod);
    members_.emplace(spec.node.id, std::move(member));
}

void FleetSim::submitAt(TimeMs at, const Task& task) {
    scheduled_.push_back({at, Injection::Submit, "", task});
}

void FleetSim::safeModeAt(TimeMs at, const std::string& nodeId) {
    scheduled_.push_back({at, Injection::SafeMode, nodeId, {}});
}

void FleetSim::degradeAt(TimeMs at, const std::string& nodeId) {
    scheduled_.push_back({at, Injection::Degrade, nodeId, {}});
}

const Node& FleetSim::node(const std::string& id) const { return *members_.at(id).node; }

SimulatedPod& FleetSim::pod(const std::string& id) { return *members_.at(id).pod; }

void FleetSim::carry(Member& member, bool toCoordinator, const std::string& packet) {
    for (const Outage& outage : member.spec.link.outages) {
        if (now_ >= outage.from && now_ < outage.until) {
            ++lost_;
            return;
        }
    }
    const TimeMs arrival = now_ + std::max<TimeMs>(0, member.spec.link.oneWayDelayMs);
    inFlight_[{arrival, sendOrder_++}] = {toCoordinator, member.spec.node.id, packet};
}

void FleetSim::step() {
    // 1. The world acts: scheduled work arrives, scheduled faults strike.
    for (std::size_t i = 0; i < scheduled_.size();) {
        if (scheduled_[i].at > now_) {
            ++i;
            continue;
        }
        const Scheduled item = scheduled_[i];
        scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i));
        switch (item.kind) {
            case Injection::Submit: coordinator_.submit(item.task); break;
            case Injection::SafeMode: members_.at(item.nodeId).pod->enterSafeMode(); break;
            case Injection::Degrade: members_.at(item.nodeId).pod->degrade(); break;
        }
    }

    // 2. Packets whose travel time is up are delivered, in the order sent.
    while (!inFlight_.empty() && inFlight_.begin()->first.first <= now_) {
        const InFlight item = inFlight_.begin()->second;
        inFlight_.erase(inFlight_.begin());
        if (item.toCoordinator) {
            coordinator_.receive(item.packet, now_);
        } else {
            members_.at(item.nodeId).node->receive(item.packet, now_);
        }
    }

    // 3. Every node acts, then the coordinator.
    for (auto& entry : members_) {
        for (const std::string& packet : entry.second.node->tick(now_)) {
            wireLog_.push_back(packet);
            carry(entry.second, true, packet);
        }
    }
    for (const Outgoing& outgoing : coordinator_.tick(now_)) {
        wireLog_.push_back(outgoing.packet);
        if (outgoing.destination.empty()) {
            for (auto& entry : members_) carry(entry.second, false, outgoing.packet);
        } else {
            const auto member = members_.find(outgoing.destination);
            if (member != members_.end()) carry(member->second, false, outgoing.packet);
        }
    }

    now_ += tickMs_;
}

void FleetSim::runUntil(TimeMs end) {
    while (now_ < end) step();
}

}  // namespace starlifter::sim
