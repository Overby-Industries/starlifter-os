#pragma once

// The fleet coordinator: fleet-wide state and a task scheduler.
//
// It learns everything it knows from OTF-1 packets and affects the fleet
// only by sending them. It never commands a vehicle; it offers a task and
// the vehicle answers.
//
// The rule the whole design turns on:
//
//     A task leaves a node only on evidence. Silence is never evidence.
//
// When a node that holds a task stops being heard from, the coordinator
// cannot tell "the node is dead" from "the node is fine and working, and the
// link is down". Across light-minutes and through outages the second is the
// ordinary case. Handing the task to another node on a timeout would
// therefore routinely send two vehicles after the same piece of debris. So
// there is deliberately no timeout that reassigns work. A task is returned
// to the queue only when the node itself says so:
//
//   * it rejects the offer,
//   * it reports the task failed,
//   * a status it sent *after the offer expired* does not list the task
//     (the offer was never taken), or
//   * a status newer than its acceptance no longer lists the task
//     (the node lost it).
//
// A task held by a silent node is "in doubt": visible, counted, and left
// exactly where it is. Deciding to release one anyway is a judgement about
// risk, which is what the advisory layer and a human are for -- not a
// scheduler.

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "starlifter/wire.hpp"

namespace starlifter {

struct CoordinatorConfig {
    std::string id = "fleet-coordinator";

    TimeMs heartbeatIntervalMs = 5000;

    // A node not heard from for this long is marked lost. That changes how
    // it is reported and stops new offers to it. It moves no work.
    TimeMs nodeTimeoutMs = 20000;

    // How long a node has to take an offer once it arrives. The acceptBy
    // instant in each offer is now + that node's observed link delay + this.
    TimeMs acceptWindowMs = 10000;

    // Slack allowed for disagreement between a node's clock and this one
    // when deciding that a status was sent after an offer expired.
    TimeMs clockSkewMarginMs = 1000;

    // A task that has failed this many times is not offered again.
    int maxAttempts = 3;
};

enum class Contact { InContact, Lost };

enum class TaskPhase {
    Queued,     // waiting for an eligible node
    Offered,    // offered to a node, no answer yet
    Active,     // a node has taken it
    InDoubt,    // offered to or held by a node that has gone silent
    Completed,
    Failed      // failed maxAttempts times
};

const char* toString(Contact value);
const char* toString(TaskPhase value);

struct NodeView {
    std::string id;
    NodeType type = NodeType::Reclaimer;
    Mode mode = Mode::Nominal;
    LinkView link = LinkView::Autonomous;
    std::string phase;
    Location location;
    std::vector<TaskAction> capabilities;
    Contact contact = Contact::InContact;
    TimeMs lastHeardAt = 0;
    TimeMs linkDelayMs = 0;       // one-way, as last observed
    std::string assignedTaskId;   // the offer or task outstanding with it, if any
};

struct TaskView {
    Task task;
    TaskPhase phase = TaskPhase::Queued;
    std::string nodeId;  // who holds it, completed it, or last failed it
    int attempts = 0;    // failures so far
};

struct FleetSummary {
    int queued = 0;
    int offered = 0;
    int active = 0;
    int inDoubt = 0;
    int completed = 0;
    int failed = 0;
    int nodesInContact = 0;
    int nodesLost = 0;
    int events = 0;              // completed-work events on the public feed
    double processedKg = 0.0;
    double odciKg = 0.0;         // Orbital Debris Cleanup Index contribution
    std::uint64_t malformedPackets = 0;
    std::uint64_t stalePackets = 0;
    std::uint64_t ignoredPackets = 0;
    std::size_t conflicts = 0;
};

// One packet for the transport to carry. An empty destination is a
// broadcast to every node.
struct Outgoing {
    std::string destination;
    std::string packet;
};

class Coordinator {
public:
    explicit Coordinator(CoordinatorConfig config = {});

    // Add work to the queue. Returns false if a task with this id exists.
    bool submit(const Task& task);

    // Hand the coordinator one packet that arrived from the fleet.
    void receive(const std::string& packet, TimeMs now);

    // Advance one step: supervise contact, send a heartbeat when due, and
    // offer queued tasks to eligible nodes. Returns the packets to send.
    std::vector<Outgoing> tick(TimeMs now);

    std::vector<NodeView> nodes() const;       // ordered by id
    std::vector<TaskView> tasks() const;       // in submission order
    std::optional<TaskView> task(const std::string& id) const;
    FleetSummary summary() const;

    // Every completed-work event received, verbatim and in arrival order:
    // the feed a transparency dashboard or ledger would consume.
    const std::vector<std::string>& feed() const { return feed_; }

    // Situations the protocol's rules should make impossible -- a node
    // working on a task that is not assigned to it. Reported, never acted
    // on: there is no cancel message, and guessing which side is right is
    // how a second vehicle gets sent.
    const std::set<std::string>& conflicts() const { return conflicts_; }

private:
    // Where a source's packets stand in its own order: by the time it says
    // it sent them, then by its sequence number. Timestamp first, so a node
    // that restarts and counts from zero again is still understood.
    struct Stamp {
        TimeMs timestamp = std::numeric_limits<TimeMs>::min();
        std::uint64_t seq = 0;
        bool operator<(const Stamp& other) const {
            return timestamp != other.timestamp ? timestamp < other.timestamp : seq < other.seq;
        }
    };

    struct NodeRecord {
        NodeView view;
        Stamp latest;
        bool reportsActiveTask = false;  // per its own last status
        bool awaitingStatus = false;     // refused an offer; wait for its next snapshot
        TimeMs lastOfferedAt = std::numeric_limits<TimeMs>::min();
    };

    struct TaskRecord {
        Task task;
        TaskPhase phase = TaskPhase::Queued;  // never InDoubt: that is derived
        std::string nodeId;
        TimeMs acceptBy = 0;
        int attempts = 0;
        std::vector<std::string> excluded;  // nodes that said never to offer this again
    };

    void onStatus(const StatusPacket& status, TimeMs now);
    void onHeartbeat(const HeartbeatPacket& heartbeat, TimeMs now);
    void onAck(const AckPacket& ack, TimeMs now);
    void onEvent(const EventPacket& event, const std::string& text, TimeMs now);
    void reconcile(NodeRecord& node, const StatusPacket& status);

    void heard(NodeRecord& node, TimeMs now);
    void complete(TaskRecord& task, NodeRecord& node);
    void fail(TaskRecord& task, NodeRecord& node);
    void requeue(TaskRecord& task, NodeRecord& node);
    bool eligible(const NodeRecord& node, const TaskRecord& task) const;
    TaskRecord* findTask(const std::string& id);
    TaskPhase phaseOf(const TaskRecord& task) const;
    bool holds(const TaskRecord& task, const NodeRecord& node) const;

    CoordinatorConfig config_;
    std::uint64_t seq_ = 0;
    std::optional<TimeMs> lastHeartbeatAt_;

    std::map<std::string, NodeRecord> nodes_;        // ordered: scheduling is deterministic
    std::vector<TaskRecord> tasks_;
    std::map<std::string, std::size_t> taskIndex_;

    std::vector<std::string> feed_;
    std::set<std::string> recordedEvents_;
    std::set<std::string> conflicts_;
    double processedKg_ = 0.0;
    double odciKg_ = 0.0;
    std::uint64_t malformed_ = 0;
    std::uint64_t stale_ = 0;
    std::uint64_t ignored_ = 0;
};

}  // namespace starlifter
