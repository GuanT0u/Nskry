#pragma once
#include <algorithm>
#include <cstdint>

namespace nskry::record {
// Times are monotonic 100 ns units, not wall clock. No state allocated at idle.
enum class State {
    Idle,
    Starting,
    Recording,
    Paused,
    Stopping,
    Discarding,
    Publishing,
    Done,
    Failed
};
class Timeline {
  public:
    bool Start() {
        if (state != State::Idle)
            return false;
        state = State::Starting;
        return true;
    }
    bool Ready(int64_t now) {
        if (state != State::Starting)
            return false;
        base = now;
        state = State::Recording;
        return true;
    }
    bool Pause(int64_t now) {
        if (state != State::Recording)
            return false;
        frozen = Elapsed(now);
        state = State::Paused;
        return true;
    }
    bool Resume(int64_t now) {
        if (state != State::Paused)
            return false;
        base = now - frozen;
        state = State::Recording;
        return true;
    }
    bool Stop(int64_t now, bool discard) {
        if (state != State::Starting && state != State::Recording && state != State::Paused &&
            !(discard && state == State::Stopping))
            return false;
        frozen = Elapsed(now);
        state = discard ? State::Discarding : State::Stopping;
        return true;
    }
    bool Publish() {
        if (state != State::Stopping)
            return false;
        state = State::Publishing;
        return true;
    }
    int64_t Elapsed(int64_t now) const {
        return state == State::Recording ? (std::max)(int64_t(0), now - base) : frozen;
    }
    void Complete(bool success) {
        state = success ? State::Done : State::Failed;
    }
    State state = State::Idle;

  private:
    int64_t base{}, frozen{};
};
// Single pending sample + latest frame: no catch-up burst after a slow encoder.
struct FrameSchedule {
    explicit FrameSchedule(unsigned fps = 20) : interval(10000000 / std::clamp(fps, 1u, 30u)) {
    }
    bool Due(int64_t now) const {
        return now >= next;
    }
    void Submitted(int64_t now) {
        next = now + interval;
    }
    int64_t next{}, interval;
};
} // namespace nskry::record
