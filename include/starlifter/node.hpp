#pragma once

// The fleet agent: the part of Starlifter OS that runs on a vehicle or pod.
//
// It speaks OTF-1 to the coordinator on one side and talks to the vehicle's
// own control software through LocalControl on the other. It holds no
// authority over the vehicle. Its whole job is bookkeeping: report honestly,
// answer offers, and keep doing something sensible when nobody is listening.
//
// What it guarantees:
//
//  * An offer is taken at most once. A duplicated or replayed assignment
//    packet is recognised and ignored; an offer that arrives after its
//    acceptBy instant is refused.
//  * Local control is asked, never told. A vehicle that is not nominal, is
//    busy, lacks the capability, or simply declines, refuses the offer and
//    says why.
//  * Loss of contact changes nothing about the work in hand. The node keeps
//    its current task to completion and then holds its local default; it
//    never stops because the coordinator went quiet (architecture.md,
//    principle 3).
//  * Nothing is lost to an outage it can see. The record of completed work
//    is held on board until the coordinator is heard again, and every status
//    repeats the latest task's outcome until a newer task replaces it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "starlifter/local_control.hpp"
#include "starlifter/wire.hpp"

namespace starlifter {

struct NodeConfig {
    std::string id;
    NodeType type = NodeType::Reclaimer;
    std::string missionId;

    // The only source this node takes offers and heartbeats from.
    std::string coordinatorId = "fleet-coordinator";

    // A status is sent at least this often, and at once on any change.
    TimeMs statusIntervalMs = 5000;

    // With no coordinator packet for this long, the node considers itself
    // out of contact. Light-delay does not enter into it: a constant delay
    // shifts when heartbeats arrive, not how far apart they are.
    TimeMs contactTimeoutMs = 15000;
};

class Node {
public:
    // `local` must outlive the node.
    Node(NodeConfig config, LocalControl& local);

    // Hand the node one packet that arrived over the link.
    void receive(const std::string& packet, TimeMs now);

    // Advance one step. Returns the packets to transmit, in order.
    std::vector<std::string> tick(TimeMs now);

    const std::string& id() const { return config_.id; }
    LinkView link() const { return link_; }

    // The task in hand, or the last one finished. Empty until the first.
    const std::optional<TaskStatus>& task() const { return task_; }

    // Completed-work events held on board, waiting for contact.
    std::size_t heldEvents() const { return heldEvents_.size(); }

    // Packets discarded: malformed, not from this node's coordinator, not
    // addressed to it, or a repeat of an offer already answered.
    std::uint64_t ignoredPackets() const { return ignored_; }

private:
    struct AnsweredOffer {
        std::uint64_t seq;
        std::string taskId;
        TimeMs acceptBy;
    };

    void onAssignment(const AssignmentPacket& offer, TimeMs now);
    std::optional<RejectReason> consider(const AssignmentPacket& offer, TimeMs now);
    void answer(const AssignmentPacket& offer, std::optional<RejectReason> refusal, TimeMs now);
    bool alreadyAnswered(const AssignmentPacket& offer, TimeMs now);
    std::string makeEvent(TimeMs now) const;
    std::string makeStatus(TimeMs now);

    NodeConfig config_;
    LocalControl& local_;

    std::uint64_t seq_ = 0;
    std::optional<TimeMs> lastHeardAt_;
    LinkView link_ = LinkView::Autonomous;

    Task currentTask_;                 // details of task_, when there is one
    std::optional<TaskStatus> task_;   // current or most recent
    std::vector<AnsweredOffer> answered_;

    std::vector<std::string> outbox_;      // acks awaiting the next tick
    std::vector<std::string> heldEvents_;  // events awaiting contact
    std::optional<TimeMs> lastStatusAt_;
    std::optional<Mode> lastReportedMode_;
    bool statusDue_ = true;
    std::uint64_t ignored_ = 0;
};

}  // namespace starlifter
