#include "core/image_edit_model.h"
#include "core/monitor_visibility_policy.h"
#include "core/window_visual_settings.h"
#include "capture/callback_gate.h"
#include <future>
#include <thread>
#include <iostream>
#include <vector>
#include <algorithm>

namespace {
bool Check(bool ok, const char* name) { if (!ok) std::cerr << "FAILED: " << name << '\n'; return ok; }
std::vector<int> Render(const nskry::ImageEditModel& model, int originalWidth) {
    std::vector<int> output(model.Width() * model.Height(), -1);
    for (const auto& p : model.Pieces({0, 0, model.Width(), model.Height()}))
        for (int y = p.output.top; y < p.output.bottom; ++y)
            for (int x = p.output.left; x < p.output.right; ++x)
                output[y * model.Width() + x] = (p.source.top + y - p.output.top) * originalWidth + p.source.left + x - p.output.left;
    return output;
}
}
int main() {
    using namespace nskry;
    bool ok = true;
    int owner = 0;
    auto gate = std::make_shared<CallbackGate<int>>(&owner);
    std::promise<void> entered, resume;
    auto ready = entered.get_future(); auto resumeFuture = resume.get_future();
    std::thread callback([&] { gate->Run([&](int& value) { entered.set_value(); resumeFuture.wait(); ++value; }); });
    ready.wait();
    auto stopping = std::async(std::launch::async, [&] { gate->Close(); });
    ok &= Check(stopping.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout, "stop drains in-flight lease");
    resume.set_value(); callback.join(); stopping.get();
    gate->Run([](int& value) { ++value; });
    ok &= Check(owner == 1, "queued callback after stop never touches owner");
    auto exceptional = std::make_shared<CallbackGate<int>>(&owner);
    try { exceptional->Run([](int&) { throw 1; }); } catch (int) {}
    exceptional->Close();
    WindowVisualSettings visuals;
    ok &= Check(!visuals.shadowEnabled && !visuals.cornersEnabled, "effects disabled by default");
    SetShadowPreset(visuals, VisualPreset::Win11); SetShadowPreset(visuals, VisualPreset::Custom);
    ok &= Check(visuals.shadowOpacity == 18 && visuals.shadowBlur == 4, "custom retains previous preset values");
    SetCornerPreset(visuals, VisualPreset::MacOS); SetCornerPreset(visuals, VisualPreset::Custom);
    visuals.cornersEnabled = true;
    ok &= Check(WindowCornerRadius(visuals, 200, 100, 192) == 20, "fixed DIP radius across DPI");
    visuals.cornerMode = CornerMode::Percent; visuals.cornerPercent = 10;
    ok &= Check(WindowCornerRadius(visuals, 200, 100, 192) == 10, "percentage uses physical short edge");
    visuals.shadowRange = 999; visuals.cornerRadius = 999;
    visuals = NormalizeVisualSettings(visuals);
    ok &= Check(visuals.shadowRange == 24 && visuals.cornerRadius == 24, "bounded settings");
    ok &= Check(!MonitorHideEnabled(false, MonitorHideOverride::FollowGlobal) &&
        MonitorHideEnabled(false, MonitorHideOverride::Enabled) &&
        !MonitorHideEnabled(true, MonitorHideOverride::Disabled), "per-window override");
    ok &= Check(DecideMonitorVisibility(true, true, false, false).sourceHidden, "source focus hides");
    ok &= Check(!DecideMonitorVisibility(true, true, false, true).sourceHidden, "menu/edit protection");
    const auto minimized = DecideMonitorVisibility(true, false, true, false);
    ok &= Check(!minimized.sourceHidden && minimized.processingPaused, "minimized remains paused after source blur");
    ok &= Check(!DecideMonitorVisibility(false, true, false, false).processingPaused, "disabled restores processing");
    ImageEditModel model(8, 6);
    ok &= Check(model.Cut(false, 4, 2), "reverse rows");
    ok &= Check(model.Cut(true, 2, 5), "columns after rows");
    std::vector<int> expected;
    for (int y : {0, 1, 4, 5}) for (int x : {0, 1, 5, 6, 7}) expected.push_back(y * 8 + x);
    ok &= Check(model.Width() == 5 && model.Height() == 4 && Render(model, 8) == expected, "all output pixels and seams");
    ok &= Check(!model.Cut(true, 0, 5) && !model.Cut(false, 1, 1) && !model.Cut(false, -1, 2), "invalid cuts leave pixels unchanged");
    ok &= Check(Render(model, 8) == expected, "transactional rejection");
    ok &= Check(model.Undo() && model.Width() == 8 && model.Redo() && Render(model, 8) == expected, "metadata undo redo");
    model.SetCrop({1, 1, 5, 4});
    ok &= Check(!model.Cut(true, 0, 1) && model.Cut(true, 2, 3) && model.Crop().right == 4, "crop remapping");
    ok &= Check(model.Undo() && model.Crop().left == 1 && model.Crop().right == 5, "undo restores crop");
    ok &= Check(!model.Pieces({-1, 0, 1, 1}).size(), "invalid ROI");
    ImageEditModel pixel(1, 2);
    ok &= Check(pixel.Cut(false, 0, 1) && pixel.Height() == 1 && Render(pixel, 1)[0] == 1, "single pixel remainder");
    ImageEditModel many(2000, 20000);
    for (int i = 0; i < 20; ++i) ok &= Check(many.Cut(false, 100, 101), "repeated cuts without pixels");
    for (int i = 0; i < 20; ++i) ok &= Check(many.Undo(), "large image metadata undo");
    ok &= Check(many.Height() == 20000 && many.Pieces({0, 0, 2000, 20000}).size() == 1, "original restored");
    ImageEditModel invalid(-1, 12);
    ok &= Check(invalid.Width() == 0 && invalid.Height() == 0 && !invalid.Cut(false, 0, 1), "invalid dimensions");
    ImageEditModel fragments(1000, 1000);
    for (int i = 0; i < 127; ++i) ok &= Check(fragments.Cut(false, 2 * i + 1, 2 * i + 2), "bounded fragments");
    const auto fragmentHeight = fragments.Height();
    ok &= Check(!fragments.Cut(true, 1, 2) && fragments.Height() == fragmentHeight && fragments.Width() == 1000,
        "piece cap rejects transactionally");
    ImageEditModel history(1, 1000);
    for (size_t i = 0; i < ImageEditModel::MaxHistory; ++i) ok &= Check(history.Cut(false, 0, 1), "bounded history");
    ok &= Check(!history.Cut(false, 0, 1) && history.Height() == 744, "history cap leaves image unchanged");
    ok &= Check(history.Undo() && history.Cut(false, 0, 2) && !history.Redo(), "new edit discards redo branch");
    return ok ? 0 : 1;
}
