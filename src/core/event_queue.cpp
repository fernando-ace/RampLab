#include "airside/core/event_queue.hpp"

#include <stdexcept>

namespace airside {

bool EventQueue::LaterEvent::operator()(const Event& left, const Event& right) const noexcept {
    if (left.timestamp != right.timestamp) {
        return left.timestamp > right.timestamp;
    }
    return left.sequence > right.sequence;
}

std::uint64_t EventQueue::schedule(
    SimTime timestamp,
    EventType type,
    EntityRef entity,
    std::int64_t data) {
    if (timestamp < SimTime::zero()) {
        throw std::invalid_argument("event timestamp cannot be negative");
    }

    const auto sequence = next_sequence_++;
    events_.push(Event{timestamp, type, entity, sequence, data});
    return sequence;
}

std::optional<Event> EventQueue::pop() {
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = events_.top();
    events_.pop();
    return event;
}

const Event* EventQueue::peek() const noexcept {
    return events_.empty() ? nullptr : &events_.top();
}

bool EventQueue::empty() const noexcept { return events_.empty(); }

std::size_t EventQueue::size() const noexcept { return events_.size(); }

void EventQueue::clear() noexcept {
    events_ = {};
    next_sequence_ = 0;
}

}  // namespace airside
