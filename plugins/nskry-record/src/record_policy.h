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
    Recovering,
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
            !(discard && (state == State::Stopping || state == State::Recovering)))
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
    void BeginRecovery(int64_t now) {
        if (state == State::Discarding)
            return;
        frozen = Elapsed(now);
        state = State::Recovering;
    }
    bool SealRecovery() {
        if (state == State::Discarding)
            return false;
        if (state == State::Recovering)
            state = State::Publishing; // Same non-cancelable terminal ownership decision.
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
inline bool ValidSampleTime(int64_t time, int64_t duration, int64_t previousEnd) {
    return time >= 0 && time >= previousEnd && duration > 0 && duration <= INT64_MAX - time;
}
inline bool ValidFrameFormat(uint32_t width, uint32_t height, uint32_t fps) {
    return width >= 2 && height >= 2 && width <= 1920 && height <= 1920 && !(width & 1) && !(height & 1) &&
           (width <= 1080 || height <= 1080) && uint64_t(width) * height <= 1920 * 1080 && fps >= 1 && fps <= 60;
}
// Single pending sample + latest frame: no catch-up burst after a slow encoder.
struct FrameSchedule {
    explicit FrameSchedule(unsigned fps = 20) : interval(10000000 / std::clamp(fps, 1u, 60u)) {
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
