#include "starlifter/node.hpp"

#include <algorithm>

namespace starlifter {

Node::Node(NodeConfig config, LocalControl& local)
    : config_(std::move(config)), local_(local) {}

void Node::receive(const std::string& text, TimeMs now) {
    Packet packet;
    if (!decode(text, packet)) {
        ++ignored_;
        return;
    }

    if (const HeartbeatPacket* heartbeat = std::get_if<HeartbeatPacket>(&packet)) {
        if (heartbeat->source.role != Role::Coordinator ||
            heartbeat->source.id != config_.coordinatorId) {
            ++ignored_;
            return;
        }
        lastHeardAt_ = now;
        return;
    }

    if (const AssignmentPacket* offer = std::get_if<AssignmentPacket>(&packet)) {
        if (offer->source.id != config_.coordinatorId || offer->destination != config_.id) {
            ++ignored_;
            return;
        }
        lastHeardAt_ = now;
        onAssignment(*offer, now);
        return;
    }

    // Status, ack and event packets are things nodes send, not things they
    // act on. One arriving here is a routing mistake, not an instruction.
    ++ignored_;
}

bool Node::alreadyAnswered(const AssignmentPacket& offer, TimeMs now) {
    // An offer only needs remembering until it expires: after acceptBy a
    // repeat is refused as expired anyway, so the memory stays small.
    answered_.erase(std::remove_if(answered_.begin(), answered_.end(),
                                   [now](const AnsweredOffer& a) { return a.acceptBy < now; }),
                    answered_.end());
    for (const AnsweredOffer& a : answered_) {
        if (a.seq == offer.seq && a.taskId == offer.task.id && a.acceptBy == offer.acceptBy) {
            return true;
        }
    }
    answered_.push_back({offer.seq, offer.task.id, offer.acceptBy});
    return false;
}

void Node::onAssignment(const AssignmentPacket& offer, TimeMs now) {
    if (alreadyAnswered(offer, now)) {
        ++ignored_;  // the same packet, delivered twice
        return;
    }

    if (task_ && task_->id == offer.task.id) {
        if (task_->state == TaskState::Active) {
            // Already working on exactly this. Say so again; change nothing.
            answer(offer, std::nullopt, now);
            return;
        }
        if (task_->state == TaskState::Completed) {
            // Already done. Refusing would invite the coordinator to hand
            // the same work to another node; the next status says it is
            // finished, which is the answer that is actually true.
            statusDue_ = true;
            return;
        }
        // Failed here before: a fresh offer is a legitimate retry.
    }

    const std::optional<RejectReason> refusal = consider(offer, now);
    if (!refusal) {
        currentTask_ = offer.task;
        task_ = TaskStatus{offer.task.id, TaskState::Active, 0.0};
        statusDue_ = true;
    }
    answer(offer, refusal, now);
}

std::optional<RejectReason> Node::consider(const AssignmentPacket& offer, TimeMs now) {
    if (now > offer.acceptBy) return RejectReason::Expired;

    const Mode mode = local_.mode();
    if (mode == Mode::Safe) return RejectReason::SafeMode;
    if (mode == Mode::Degraded) return RejectReason::Degraded;

    if (task_ && task_->state == TaskState::Active) return RejectReason::Busy;

    const std::vector<TaskAction> capabilities = local_.capabilities();
    const bool capable = std::find(capabilities.begin(), capabilities.end(),
                                   offer.task.action) != capabilities.end();
    if (!capable || local_.location().zone != offer.task.location.zone) {
        return RejectReason::Unsupported;
    }

    // Everything the fleet layer can check has passed. The vehicle decides.
    if (!local_.begin(offer.task, now)) return RejectReason::Refused;
    return std::nullopt;
}

void Node::answer(const AssignmentPacket& offer, std::optional<RejectReason> refusal, TimeMs now) {
    AckPacket ack;
    ack.timestamp = now;
    ack.source = {config_.id, Role::Node};
    ack.destination = offer.source.id;
    ack.seq = seq_++;
    ack.taskId = offer.task.id;
    ack.result = refusal ? AckResult::Rejected : AckResult::Accepted;
    ack.reason = refusal;
    outbox_.push_back(encode(ack));
}

std::string Node::makeEvent(TimeMs now) const {
    const WorkOutcome outcome = local_.outcome();
    EventPacket event;
    event.timestamp = now;
    event.missionId = config_.missionId;
    event.podId = config_.id;
    event.missionType = config_.type;
    event.location = local_.location();
    event.target = currentTask_.target;
    event.action = eventActionFor(currentTask_.action);
    event.processedKg = outcome.processedKg;
    event.repurposedAs = outcome.repurposedAs;
    event.outcomeStatus = outcome.status;
    event.smessc = outcome.smessc;
    event.odciContributionKg = outcome.odciContributionKg;
    event.taskId = currentTask_.id;
    return encode(event);
}

std::string Node::makeStatus(TimeMs now) {
    StatusPacket status;
    status.timestamp = now;
    status.source = {config_.id, Role::Node};
    status.seq = seq_++;
    status.type = config_.type;
    status.mode = local_.mode();
    status.phase = local_.phase();
    status.link = link_;
    status.capabilities = local_.capabilities();
    status.location = local_.location();
    status.task = task_;
    return encode(status);
}

std::vector<std::string> Node::tick(TimeMs now) {
    local_.tick(now);

    // Contact. A node that has never heard its coordinator is autonomous.
    const LinkView link = lastHeardAt_ && now - *lastHeardAt_ <= config_.contactTimeoutMs
                              ? LinkView::Connected
                              : LinkView::Autonomous;
    if (link != link_) {
        link_ = link;
        statusDue_ = true;
    }

    // The task in hand. Note what is absent: nothing here looks at link_.
    // Whether the coordinator can be heard has no bearing on the work.
    const Mode mode = local_.mode();
    if (task_ && task_->state == TaskState::Active) {
        const WorkState work = local_.work();
        if (mode == Mode::Safe || work == WorkState::Failed || work == WorkState::Idle) {
            // Idle here means local control dropped the work without
            // reporting a result. Unfinished is unfinished.
            task_->state = TaskState::Failed;
            statusDue_ = true;
        } else if (work == WorkState::Completed) {
            task_->state = TaskState::Completed;
            task_->progress = 1.0;
            heldEvents_.push_back(makeEvent(now));
            statusDue_ = true;
        } else {
            task_->progress = std::clamp(local_.progress(), 0.0, 1.0);
        }
    }
    if (!lastReportedMode_ || *lastReportedMode_ != mode) {
        statusDue_ = true;
    }

    std::vector<std::string> out;
    out.swap(outbox_);

    // Store and forward: the public record of finished work stays on board
    // through an outage the node can see, and goes out when contact returns.
    if (link_ == LinkView::Connected) {
        for (std::string& event : heldEvents_) out.push_back(std::move(event));
        heldEvents_.clear();
    }

    // Status keeps going out regardless of link_: the node cannot know that
    // its own transmissions are the half of the link that failed.
    if (statusDue_ || !lastStatusAt_ || now - *lastStatusAt_ >= config_.statusIntervalMs) {
        out.push_back(makeStatus(now));
        lastStatusAt_ = now;
        lastReportedMode_ = mode;
        statusDue_ = false;
    }
    return out;
}

}  // namespace starlifter
