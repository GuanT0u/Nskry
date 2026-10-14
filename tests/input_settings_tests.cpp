#include "core/hotkey_manager.h"
#include "core/input_settings.h"
#include <iostream>
#include <windows.h>
namespace {
bool Check(bool value, const char *name) {
    if (!value)
        std::cerr << name << '\n';
    return value;
}
} // namespace
int wmain() {
    using namespace nskry;
    using namespace input;
    bool ok = true;
    wchar_t errorText[16]{};NskryInputResultV2 ack{sizeof(ack),2,errorText,16};
    NskryInputBindingV1 local[]{ {L"copy",L"Ctrl+C"} };
    NskryInputConfigV1 config{sizeof(config),1,1,local};
    ok &= Check(ConfigureConfirmed(&config,&ack,{{L"copy",Kind::Key}})==NSKRY_INPUT_APPLIED,"v2 accepts supported local configuration");
    local[0].shortcut=L"A+B";
    ok &= Check(ConfigureConfirmed(&config,&ack,{{L"copy",Kind::Key}})==NSKRY_INPUT_FAILED && pluginBindings[L"copy"]==L"Ctrl+C" && errorText[15]==0,
        "v2 rejects invalid syntax without replacing prior configuration, bounded error");
    local[0].id=L"unknown";
    ok &= Check(ConfigureConfirmed(&config,&ack,{{L"copy",Kind::Key}})==NSKRY_INPUT_UNSUPPORTED,"v2 distinguishes unsupported command");
    ack.version=99;
    ok &= Check(ConfigureConfirmed(&config,&ack,{{L"copy",Kind::Key}})==NSKRY_INPUT_UNSUPPORTED,"v2 descriptor version negotiation");
    std::vector<ShortcutDefinition> installed{
        {L"plugin.nskry-ocr.launch", L"Imported OCR", L"", L"nskry-ocr", L"Imported OCR", Kind::Global, false, true}};
    AddBuiltinCapturePlaceholders(installed);
    ok &= Check(installed.size() == 2 && !installed[0].official && installed[1].official,
                "installed source is never promoted by a product ID");
    nskry::input::Chord c;
    ok &= Check(Parse(L"Ctrl+Alt+A", Kind::Global, c) && c.modifiers == (MOD_CONTROL | MOD_ALT) && c.key == 'A',
                "global parse");
    ok &= Check(Parse(L"Control+Windows+F24", Kind::Global, c) && c.key == VK_F24, "aliases and F24");
    for (auto bad : {L"Ctrl+", L"Ctrl++A", L"A+B", L"Ctrl+Ctrl+A", L"F1junk", L"Alt", L"Wheel"})
        ok &= Check(!Parse(bad, Kind::Global, c), "invalid global rejected");
    ok &= Check(Parse(L"Alt", Kind::Hold, c) && !c.key, "modifier hold");
    ok &= Check(Parse(L"Shift+Wheel", Kind::Wheel, c) && c.wheel && c.modifiers == MOD_SHIFT, "wheel parse");
    Recorder recorder;
    recorder.Down(VK_LCONTROL);
    recorder.Down('A');
    recorder.Down('A');
    recorder.Down(VK_RCONTROL);
    ok &= Check(recorder.Text() == L"Ctrl+A" && !recorder.Up('A') && !recorder.Up(VK_LCONTROL) &&
                    recorder.Up(VK_RCONTROL),
                "union recording, repeats and independent modifier release");
    recorder.Reset();
    recorder.Down(VK_SHIFT);
    recorder.Down(VK_MENU);
    ok &= Check(recorder.Text(true) == L"Alt+Shift+Wheel", "stable live formatting");
    auto style = NormalizeInteractionStyle(InteractionStyle{});
    style.borderWidth = 100;
    style.cutOpacity = -9;
    style = NormalizeInteractionStyle(style);
    ok &= Check(style.borderWidth == 6 && style.cutOpacity == 0, "visual limits");
    std::wstring validation;
    ok &= Check(ValidateShortcuts(CoreShortcuts(), {}, validation),
                "defaults permit same cancel key across distinct contexts");
    ok &= Check(!ValidateShortcuts(CoreShortcuts(), {{L"core.redo", L"Ctrl+Z"}}, validation),
                "ambiguous local key rejected");
    ok &= Check(!ValidateShortcuts(CoreShortcuts(), {{L"core.monitor", L"Ctrl+C"}}, validation),
                "global cannot shadow a local key");
    HWND owner =
        CreateWindowExW(0, L"STATIC", L"input test", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND blocker =
        CreateWindowExW(0, L"STATIC", L"input blocker", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!owner || !blocker) {
        std::cerr << "Test windows failed: " << GetLastError() << '\n';
        return 1;
    }
    auto &manager = HotkeyManager::Instance();
    manager.Initialize(owner);
    std::wstring error;
    ok &= Check(!manager.ApplyBindings({{L"a", L"Ctrl+F24"}, {L"b", L"Control+F24"}}, error),
                "duplicate global validation");
    ok &= Check(!manager.ApplyBindings({{L"a", L"Ctrl+F24"}, {L"a", L""}}, error),
                "duplicate command ID rejected before mutation");
    // Reserve only test-owned rare chords; never send synthetic input to the desktop.
    std::vector<std::wstring> available;
    for (UINT key = VK_F24; key >= VK_F1 && available.size() < 3; --key) {
        if (RegisterHotKey(blocker, 100 + int(available.size()), MOD_CONTROL | MOD_ALT | MOD_SHIFT, key))
            available.push_back(L"Ctrl+Alt+Shift+" + KeyName(key));
    }
    if (available.size() == 3) {
        UnregisterHotKey(blocker, 100);
        UnregisterHotKey(blocker, 101);
        ok &= Check(manager.ApplyBindings({{L"a", available[0]}, {L"b", available[1]}}, error), "register test keys");
        ok &= Check(manager.Rebind(L"a", available[0]), "rebind to the same chord remains usable");
        ok &= Check(manager.ApplyBindings({{L"a", available[1]}, {L"b", available[0]}}, error), "swap transaction");
        ok &= Check(!manager.ApplyBindings({{L"a", available[2]}}, error), "external collision rolls back");
        ok &= Check(manager.Suspend(true) && manager.Suspend(false), "record suspension and restoration");
        ok &= Check(manager.ApplyBindings({}, error), "all bindings can be deleted");
    } else {
        std::cerr << "Could not reserve test chords: " << GetLastError() << '\n';
        ok = false;
    }
    manager.Shutdown();
    for (int i = 0; i < 3; ++i)
        UnregisterHotKey(blocker, 100 + i);
    DestroyWindow(owner);
    DestroyWindow(blocker);
    return ok ? 0 : 1;
}
