#pragma once
namespace nskry {
enum class MonitorHideOverride { FollowGlobal, Enabled, Disabled };
struct MonitorVisibility {
    bool sourceHidden{}, processingPaused{};
};
inline bool MonitorHideEnabled(bool global, MonitorHideOverride choice) {
    return choice == MonitorHideOverride::Enabled || (choice == MonitorHideOverride::FollowGlobal && global);
}
inline MonitorVisibility DecideMonitorVisibility(bool enabled, bool sourceFocused, bool minimized, bool protectedInteraction) {
    const bool hide = enabled && sourceFocused && !protectedInteraction;
    return {hide, hide || minimized};
}
}
