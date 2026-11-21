#include "starlifter/coordinator.hpp"

#include <algorithm>

namespace starlifter {

const char* toString(Contact value) {
    return value == Contact::InContact ? "in-contact" : "lost";
}

const char* toString(TaskPhase value) {
    switch (value) {
        case TaskPhase::Queued: return "queued";
        case TaskPhase::Offered: return "offered";
        case TaskPhase::Active: return "active";
        case TaskPhase::InDoubt: return "in-doubt";
        case TaskPhase::Completed: return "completed";
        case TaskPhase::Failed: return "failed";
    }
    return "";
}

Coordinator::Coordinator(CoordinatorConfig config) : config_(std::move(config)) {}

bool Coordinator::submit(const Task& task) {
    if (task.id.empty() || taskIndex_.count(task.id) != 0) return false;
    taskIndex_[task.id] = tasks_.size();
    TaskRecord record;
    record.task = task;
    tasks_.push_back(std::move(record));
    return true;
}

Coordinator::TaskRecord* Coordinator::findTask(const std::string& id) {
    const auto found = taskIndex_.find(id);
    return found == taskIndex_.end() ? nullptr : &tasks_[found->second];
}

bool Coordinator::holds(const TaskRecord& task, const NodeRecord& node) const {
    return task.nodeId == node.view.id &&
           (task.phase == TaskPhase::Offered || task.phase == TaskPhase::Active);
}

// --------------------------------------------------------------------------
// Incoming packets

void Coordinator::receive(const std::string& text, TimeMs now) {
    Packet packet;
    if (!decode(text, packet)) {
        ++malformed_;
        return;
    }
    if (const StatusPacket* status = std::get_if<StatusPacket>(&packet)) {
        onStatus(*status, now);
    } else if (const HeartbeatPacket* heartbeat = std::get_if<HeartbeatPacket>(&packet)) {
        onHeartbeat(*heartbeat, now);
    } else if (const AckPacket* ack = std::get_if<AckPacket>(&packet)) {
        onAck(*ack, now);
    } else if (const EventPacket* event = std::get_if<EventPacket>(&packet)) {
        onEvent(*event, text, now);
    } else {
        ++ignored_;  // an assignment is something a coordinator sends
    }
}

void Coordinator::heard(NodeRecord& node, TimeMs now) {
    node.view.lastHeardAt = now;
    node.view.contact = Contact::InContact;
}

void Coordinator::onStatus(const StatusPacket& status, TimeMs now) {
    const Stamp stamp{status.timestamp, status.seq};
    const auto known = nodes_.find(status.source.id);
    if (known != nodes_.end() && !(known->second.latest < stamp)) {
        // Older than something already acted on. A status is a snapshot;
        // applying an old one would roll the fleet picture backwards.
        ++stale_;
        return;
    }

    NodeRecord& node = nodes_[status.source.id];
    node.latest = stamp;
    node.view.id = status.source.id;
    node.view.type = status.type;
    node.view.mode = status.mode;
    node.view.link = status.link;
    node.view.phase = status.phase.value_or("");
    node.view.location = status.location;
    node.view.capabilities = status.capabilities;
    node.view.linkDelayMs = std::max<TimeMs>(0, now - status.timestamp);
    node.reportsActiveTask = status.task && status.task->state == TaskState::Active;
    node.awaitingStatus = false;
    heard(node, now);

    reconcile(node, status);
}

void Coordinator::onHeartbeat(const HeartbeatPacket& heartbeat, TimeMs now) {
    const auto known = nodes_.find(heartbeat.source.id);
    if (heartbeat.source.role != Role::Node || known == nodes_.end()) {
        // Not a node, or a node that has not yet said what it is.
        ++ignored_;
        return;
    }
    const Stamp stamp{heartbeat.timestamp, heartbeat.seq};
    if (!(known->second.latest < stamp)) {
        ++stale_;
        return;
    }
    known->second.latest = stamp;
    heard(known->second, now);
}

void Coordinator::onAck(const AckPacket& ack, TimeMs now) {
    const auto known = nodes_.find(ack.source.id);
    TaskRecord* task = findTask(ack.taskId);
    if (ack.destination != config_.id || known == nodes_.end() || task == nullptr) {
        ++ignored_;
        return;
    }
    NodeRecord& node = known->second;
    heard(node, now);
    // An ack is not a snapshot and is never dropped for being old, but it
    // does advance the node's stamp: any status the node sent *before* this
    // answer must not be read as its state *after* it.
    const Stamp stamp{ack.timestamp, ack.seq};
    if (node.latest < stamp) node.latest = stamp;

    if (ack.result == AckResult::Accepted) {
        if (holds(*task, node)) {
            task->phase = TaskPhase::Active;
        } else {
            conflicts_.insert(node.view.id + " accepted " + task->task.id +
                              ", which is not on offer to it");
        }
        return;
    }

    if (!holds(*task, node) || task->phase != TaskPhase::Offered) {
        return;  // late word about an offer that has already been settled
    }
    if (*ack.reason == RejectReason::Unsupported || *ack.reason == RejectReason::Refused) {
        task->excluded.push_back(node.view.id);
    } else {
        // Busy, expired, degraded, safe: true for now. Let the node's next
        // snapshot say when that has changed, rather than asking again at
        // once and flooding the link with offers it will keep refusing.
        node.awaitingStatus = true;
    }
    requeue(*task, node);
}

void Coordinator::onEvent(const EventPacket& event, const std::string& text, TimeMs now) {
    if (event.taskId) {
        // One record per piece of completed work, however often it arrives.
        if (!recordedEvents_.insert(event.podId + '\n' + *event.taskId).second) {
            ++stale_;
            return;
        }
    }
    feed_.push_back(text);
    if (event.outcomeStatus != OutcomeStatus::Failed) {
        processedKg_ += event.processedKg.value_or(0.0);
        odciKg_ += event.odciContributionKg.value_or(0.0);
    }

    const auto known = nodes_.find(event.podId);
    if (known == nodes_.end()) return;
    heard(known->second, now);

    if (!event.taskId) return;
    TaskRecord* task = findTask(*event.taskId);
    if (task == nullptr || !holds(*task, known->second)) return;
    if (event.outcomeStatus == OutcomeStatus::Failed) {
        fail(*task, known->second);
    } else {
        complete(*task, known->second);
    }
}

// The node has sent a snapshot of itself. Bring the task picture into line
// with it. Every transition here is driven by something the node said.
void Coordinator::reconcile(NodeRecord& node, const StatusPacket& status) {
    if (status.task) {
        TaskRecord* task = findTask(status.task->id);
        if (task != nullptr && holds(*task, node)) {
            switch (status.task->state) {
                case TaskState::Active: task->phase = TaskPhase::Active; break;
                case TaskState::Completed: complete(*task, node); break;
                case TaskState::Failed: fail(*task, node); break;
            }
        } else if (status.task->state == TaskState::Active) {
            conflicts_.insert(node.view.id + " is working on " + status.task->id +
                              (task == nullptr ? ", which this coordinator never issued"
                                               : ", which is not assigned to it"));
        }
        // A finished task that is already settled here is just the node
        // repeating its last outcome, as it is meant to.
    }

    if (node.view.assignedTaskId.empty()) return;
    TaskRecord* outstanding = findTask(node.view.assignedTaskId);
    if (outstanding == nullptr) return;
    if (status.task && status.task->id == outstanding->task.id) return;

    // The node's snapshot does not mention the task this coordinator has
    // outstanding with it.
    if (outstanding->phase == TaskPhase::Active) {
        // It took the task, and this snapshot is newer than its acceptance
        // (an older one would have been discarded as stale). It no longer
        // has the work -- a restart, most likely.
        fail(*outstanding, node);
    } else if (outstanding->phase == TaskPhase::Offered &&
               status.timestamp > outstanding->acceptBy + config_.clockSkewMarginMs) {
        // Sent after the offer expired, and the task is not there. A node
        // may only take an offer before acceptBy, so it did not take this.
        requeue(*outstanding, node);
    }
    // Otherwise the offer may still be in flight. Wait.
}

// --------------------------------------------------------------------------
// Task transitions

void Coordinator::complete(TaskRecord& task, NodeRecord& node) {
    task.phase = TaskPhase::Completed;
    node.view.assignedTaskId.clear();
}

void Coordinator::fail(TaskRecord& task, NodeRecord& node) {
    node.view.assignedTaskId.clear();
    ++task.attempts;
    if (task.attempts >= config_.maxAttempts) {
        task.phase = TaskPhase::Failed;  // nodeId stays: who failed it last
    } else {
        task.phase = TaskPhase::Queued;
        task.nodeId.clear();
    }
}

void Coordinator::requeue(TaskRecord& task, NodeRecord& node) {
    node.view.assignedTaskId.clear();
    task.phase = TaskPhase::Queued;
    task.nodeId.clear();
}

bool Coordinator::eligible(const NodeRecord& node, const TaskRecord& task) const {
    const NodeView& view = node.view;
    const auto has = [](const auto& list, const auto& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    return view.contact == Contact::InContact &&
           view.mode == Mode::Nominal &&
           view.link == LinkView::Connected &&   // it can hear offers
           view.assignedTaskId.empty() &&        // one outstanding offer per node
           !node.reportsActiveTask &&
           !node.awaitingStatus &&
           view.location.zone == task.task.location.zone &&
           has(view.capabilities, task.task.action) &&
           !has(task.excluded, view.id);
}

// --------------------------------------------------------------------------
// The scheduler

std::vector<Outgoing> Coordinator::tick(TimeMs now) {
    std::vector<Outgoing> out;

    // Contact supervision. Marking a node lost moves no work: see the note
    // at the top of coordinator.hpp.
    for (auto& entry : nodes_) {
        NodeView& view = entry.second.view;
        if (view.contact == Contact::InContact && now - view.lastHeardAt > config_.nodeTimeoutMs) {
            view.contact = Contact::Lost;
        }
    }

    if (!lastHeartbeatAt_ || now - *lastHeartbeatAt_ >= config_.heartbeatIntervalMs) {
        HeartbeatPacket heartbeat;
        heartbeat.timestamp = now;
        heartbeat.source = {config_.id, Role::Coordinator};
        heartbeat.seq = seq_++;
        out.push_back({"", encode(heartbeat)});
        lastHeartbeatAt_ = now;
    }

    // Offer each queued task, oldest first, to the eligible node that has
    // gone longest without an offer. nodes_ is ordered by id, so ties break
    // the same way every run.
    for (TaskRecord& task : tasks_) {
        if (task.phase != TaskPhase::Queued) continue;

        NodeRecord* chosen = nullptr;
        for (auto& entry : nodes_) {
            NodeRecord& node = entry.second;
            if (!eligible(node, task)) continue;
            if (chosen == nullptr || node.lastOfferedAt < chosen->lastOfferedAt) chosen = &node;
        }
        if (chosen == nullptr) continue;

        task.phase = TaskPhase::Offered;
        task.nodeId = chosen->view.id;
        // The offer has to survive the trip: allow for the link delay last
        // observed from this node before the accept window starts counting.
        task.acceptBy = now + chosen->view.linkDelayMs + config_.acceptWindowMs;
        chosen->view.assignedTaskId = task.task.id;
        chosen->lastOfferedAt = now;

        AssignmentPacket offer;
        offer.timestamp = now;
        offer.source = {config_.id, Role::Coordinator};
        offer.destination = chosen->view.id;
        offer.seq = seq_++;
        offer.task = task.task;
        offer.acceptBy = task.acceptBy;
        out.push_back({chosen->view.id, encode(offer)});
    }
    return out;
}

// --------------------------------------------------------------------------
// Views

TaskPhase Coordinator::phaseOf(const TaskRecord& task) const {
    if (task.phase == TaskPhase::Offered || task.phase == TaskPhase::Active) {
        const auto node = nodes_.find(task.nodeId);
        if (node != nodes_.end() && node->second.view.contact == Contact::Lost) {
            return TaskPhase::InDoubt;
        }
    }
    return task.phase;
}

std::vector<NodeView> Coordinator::nodes() const {
    std::vector<NodeView> views;
    views.reserve(nodes_.size());
    for (const auto& entry : nodes_) views.push_back(entry.second.view);
    return views;
}

std::vector<TaskView> Coordinator::tasks() const {
    std::vector<TaskView> views;
    views.reserve(tasks_.size());
    for (const TaskRecord& task : tasks_) {
        views.push_back({task.task, phaseOf(task), task.nodeId, task.attempts});
    }
    return views;
}

std::optional<TaskView> Coordinator::task(const std::string& id) const {
    const auto found = taskIndex_.find(id);
    if (found == taskIndex_.end()) return std::nullopt;
    const TaskRecord& task = tasks_[found->second];
    return TaskView{task.task, phaseOf(task), task.nodeId, task.attempts};
}

FleetSummary Coordinator::summary() const {
    FleetSummary s;
    for (const TaskRecord& task : tasks_) {
        switch (phaseOf(task)) {
            case TaskPhase::Queued: ++s.queued; break;
            case TaskPhase::Offered: ++s.offered; break;
            case TaskPhase::Active: ++s.active; break;
            case TaskPhase::InDoubt: ++s.inDoubt; break;
            case TaskPhase::Completed: ++s.completed; break;
            case TaskPhase::Failed: ++s.failed; break;
        }
    }
    for (const auto& entry : nodes_) {
        if (entry.second.view.contact == Contact::InContact) ++s.nodesInContact;
        else ++s.nodesLost;
    }
    s.events = static_cast<int>(feed_.size());
    s.processedKg = processedKg_;
    s.odciKg = odciKg_;
    s.malformedPackets = malformed_;
    s.stalePackets = stale_;
    s.ignoredPackets = ignored_;
    s.conflicts = conflicts_.size();
    return s;
}

}  // namespace starlifter
