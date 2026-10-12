#pragma once
#include "core/settings_manager.h"
#include <functional>
namespace nskry {
class InputPage {
  public:
    InputPage(HWND parent, std::vector<ShortcutDefinition> definitions, bool jumps, std::function<void(bool)> recording,
              std::function<void(const std::wstring &)> navigate);
    ~InputPage();
    HWND Hwnd() const {
        return m_hwnd;
    }
    void Load(const UserSettings &settings);
    void Store(UserSettings &settings);
    void Jump(const std::wstring &id);
    void StopRecording(bool accept = false);

  private:
    friend struct InputPageTestAccess;
    struct Row {
        ShortcutDefinition definition;
        HWND label{}, display{}, edit{}, record{}, type{}, remove{};
        int y{};
    };
    struct Heading {
        HWND hwnd{};
        int y{};
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Layout();
    void Scroll(int);
    void TypeDone(size_t);
    void BeginRecord(size_t);
    void CreateRow(size_t);
    void ReleaseRow(size_t);
    HWND m_hwnd{};
    std::vector<Row> m_rows;
    std::vector<Heading> m_headings;
    bool m_jumps{};
    int m_scroll{}, m_height{}, m_recording = -1, m_flashRow = -1, m_flashPhase{};
    input::Recorder m_recorder;
    std::unordered_map<std::wstring, std::wstring> m_values;
    std::function<void(bool)> m_recordState;
    std::function<void(const std::wstring &)> m_navigate;
};
} // namespace nskry
