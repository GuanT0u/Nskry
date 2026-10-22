#include "record_policy.h"
#include "record_options.h"
#include "record_trim.h"
#include <iostream>
int main() {
    using namespace nskry::record;
    if (DefaultVideoBitrate(320, 180, 30) != 500000 || DefaultVideoBitrate(1920, 1080, 60) != 8000000)
        return 1;
    Options decoded;
    if (!DecodeDefaults(EncodeDefaults({60, Quality::Hd}), decoded) || decoded.fps != 60 || decoded.quality != Quality::Hd ||
        DecodeDefaults(0, decoded) || DecodeDefaults((2u << 16) | (30u << 8), decoded))
        return 19;
    if (!ValidOptions({30, Quality::High}) || !ValidOptions({60, Quality::Hd}) ||
        ValidOptions({20, Quality::Medium}) || ValidOptions({30, Quality(3)}) ||
        !(Bitrate(1920, 1080, 60, Quality::Hd) > Bitrate(1920, 1080, 60, Quality::High)) ||
        !(Bitrate(1920, 1080, 60, Quality::High) > Bitrate(1920, 1080, 60, Quality::Medium)) ||
        !ValidTrim(1, 10, 10) || ValidTrim(-1, 2, 3) || ValidTrim(3, 3, 3) || ValidTrim(1, 4, 3) ||
        ClipSample(5, 10, 8, 12).time != 0 || ClipSample(5, 10, 8, 12).duration != 4 ||
        ClipSample(INT64_MAX, 1, 0, 10).duration || !ValidFrameFormat(1920, 1080, 60))
        return 18;
    Timeline t;
    if (!t.Start() || t.Start() || !t.Ready(100) || t.Elapsed(150) != 50)
        return 1;
    if (!t.Pause(160) || t.Pause(170) || t.Elapsed(900) != 60)
        return 2;
    if (!t.Resume(1000) || t.Elapsed(1040) != 100)
        return 3;
    for (int i = 0; i < 1000; ++i) {
        if (!t.Pause(1040 + i * 2) || !t.Resume(1041 + i * 2))
            return 4;
    }
    if (!t.Stop(4000, false) || t.Resume(4001) || t.Pause(4001) || t.Stop(4001, false))
        return 5;
    auto final = t.Elapsed(5000);
    if (!t.Stop(5001, true) || t.Elapsed(6000) != final)
        return 6;
    t.Complete(true);
    if (t.state != State::Done || t.Start())
        return 7;
    Timeline early;
    early.Start();
    early.Stop(100, true);
    if (early.Ready(200) || early.Elapsed(300))
        return 8;
    FrameSchedule s;
    if (!s.Due(0))
        return 9;
    s.Submitted(0);
    if (s.Due(1) || !s.Due(500000))
        return 10;
    s.Submitted(10000000);
    if (s.Due(10000001) || s.next != 10500000)
        return 11;
    Timeline publish;
    if (FrameSchedule(0).interval != 10000000 || FrameSchedule(999).interval != 166666)
        return 13;
    publish.Start();
    publish.Ready(0);
    publish.Stop(10, false);
    if (!publish.Publish() || publish.Stop(11, true) || publish.Elapsed(12) != 10)
        return 12;
    Timeline recovery;
    recovery.Start();
    recovery.Ready(100);
    recovery.BeginRecovery(250);
    if (recovery.Elapsed(999) != 150 || recovery.Pause(300) || recovery.Resume(301) || recovery.Stop(400, false))
        return 14;
    if (!recovery.Stop(500, true) || recovery.SealRecovery() || recovery.Elapsed(700) != 150)
        return 15;
    Timeline retain;
    retain.Start();
    retain.Ready(0);
    retain.BeginRecovery(10);
    if (!retain.SealRecovery() || retain.Stop(11, true))
        return 16;
    if (!ValidSampleTime(0, 1, 0) || ValidSampleTime(-1, 1, 0) || ValidSampleTime(5, 1, 6) ||
        ValidSampleTime(INT64_MAX, 1, 0) || ValidSampleTime(0, 0, 0))
        return 17;
    std::cout << "Record state, rapid pause/resume, timestamps, stop/cancel and bounded scheduling passed.\n";
}
