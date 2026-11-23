#include "ui/toolbar_accessibility.h"
#include <oleacc.h>
#include <atomic>
#include <new>

namespace nskry {
namespace {
class AccessibleToolbar final : public IAccessible {
public:
    explicit AccessibleToolbar(std::shared_ptr<ToolbarAccessibility::State> state) : m_state(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDispatch && id != IID_IAccessible) return E_NOINTERFACE;
        *out = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --m_references;
        if (!count) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return DISP_E_BADINDEX; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override {
        if (!parent) return E_POINTER;
        *parent = nullptr;
        HWND window{};
        {
            const std::lock_guard lock(m_state->mutex);
            window = m_state->window;
        }
        return window ? ::AccessibleObjectFromWindow(window, OBJID_WINDOW, IID_IDispatch,
            reinterpret_cast<void**>(parent)) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override {
        if (!count) return E_POINTER;
        const std::lock_guard lock(m_state->mutex);
        *count = static_cast<LONG>(m_state->items.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const std::lock_guard lock(m_state->mutex);
        return GetItem(child) ? S_FALSE : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const std::lock_guard lock(m_state->mutex);
        if (Self(child)) return CopyString(m_state->name.c_str(), out);
        const auto* item = GetItem(child);
        return item ? CopyString(item->name.c_str(), out) : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        ::VariantInit(role);
        const std::lock_guard lock(m_state->mutex);
        if (!Self(child) && !GetItem(child)) return E_INVALIDARG;
        *role = Id(Self(child) ? ROLE_SYSTEM_TOOLBAR : ROLE_SYSTEM_PUSHBUTTON);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* value) override {
        if (!value) return E_POINTER;
        ::VariantInit(value);
        const std::lock_guard lock(m_state->mutex);
        if (Self(child)) {
            *value = Id(m_state->items.empty() ? STATE_SYSTEM_INVISIBLE : 0);
            return S_OK;
        }
        const auto* item = GetItem(child);
        if (!item) return E_INVALIDARG;
        *value = Id(STATE_SYSTEM_FOCUSABLE | (item->selected ? STATE_SYSTEM_PRESSED : 0) |
                    (child.lVal == m_state->focused ? STATE_SYSTEM_FOCUSED : 0));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* value) override {
        if (!value) return E_POINTER;
        ::VariantInit(value);
        const std::lock_guard lock(m_state->mutex);
        if (!m_state->focused) return S_FALSE;
        *value = Id(m_state->focused);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        const std::lock_guard lock(m_state->mutex);
        return GetItem(child) ? CopyString(m_state->action.c_str(), out) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        return Post(ToolbarAccessibility::InvokeMessage, child);
    }
    HRESULT STDMETHODCALLTYPE accSelect(LONG flags, VARIANT child) override {
        return flags == SELFLAG_TAKEFOCUS ? Post(ToolbarAccessibility::FocusMessage, child) : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE accLocation(LONG* left, LONG* top, LONG* width, LONG* height, VARIANT child) override {
        if (!left || !top || !width || !height) return E_POINTER;
        const std::lock_guard lock(m_state->mutex);
        RECT bounds{};
        if (Self(child)) {
            for (const auto &item : m_state->items) ::UnionRect(&bounds, &bounds, &item.screenBounds);
        } else if (const auto* item = GetItem(child)) {
            bounds = item->screenBounds;
        } else {
            return E_INVALIDARG;
        }
        *left = bounds.left; *top = bounds.top;
        *width = bounds.right - bounds.left; *height = bounds.bottom - bounds.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(LONG x, LONG y, VARIANT* hit) override {
        if (!hit) return E_POINTER;
        ::VariantInit(hit);
        const std::lock_guard lock(m_state->mutex);
        for (size_t i = 0; i < m_state->items.size(); ++i)
            if (::PtInRect(&m_state->items[i].screenBounds, {x, y})) {
                *hit = Id(static_cast<LONG>(i + 1));
                return S_OK;
            }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(LONG direction, VARIANT start, VARIANT* result) override {
        if (!result) return E_POINTER;
        ::VariantInit(result);
        const std::lock_guard lock(m_state->mutex);
        const auto count = static_cast<LONG>(m_state->items.size());
        LONG child = 0;
        if (Self(start) && direction == NAVDIR_FIRSTCHILD) child = 1;
        else if (Self(start) && direction == NAVDIR_LASTCHILD) child = count;
        else if (GetItem(start) && direction == NAVDIR_NEXT) child = start.lVal + 1;
        else if (GetItem(start) && direction == NAVDIR_PREVIOUS) child = start.lVal - 1;
        if (child < 1 || child > count) return S_FALSE;
        *result = Id(child);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR* out) override { return Empty(out); }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* out) override { return Empty(out); }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* out) override { return Empty(out); }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* out) override { return Empty(out); }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT, LONG* topic) override {
        if (!topic) return E_POINTER;
        *topic = -1;
        return Empty(file);
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override {
        if (!selection) return E_POINTER;
        ::VariantInit(selection);
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    static bool Self(VARIANT child) { return child.vt == VT_I4 && child.lVal == CHILDID_SELF; }
    static VARIANT Id(LONG value) { VARIANT v{}; v.vt = VT_I4; v.lVal = value; return v; }
    static HRESULT Empty(BSTR* out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_FALSE;
    }
    static HRESULT CopyString(const wchar_t* text, BSTR* out) {
        *out = ::SysAllocString(text);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    const ToolbarAccessibility::Item* GetItem(VARIANT child) const {
        if (child.vt != VT_I4 || child.lVal < 1 || static_cast<size_t>(child.lVal) > m_state->items.size())
            return nullptr;
        return &m_state->items[child.lVal - 1];
    }
    HRESULT Post(UINT message, VARIANT child) const {
        const std::lock_guard lock(m_state->mutex);
        if (!m_state->window || !GetItem(child)) return E_INVALIDARG;
        return ::PostMessageW(m_state->window, message, m_state->generation, child.lVal) ? S_OK : E_FAIL;
    }

    std::atomic<ULONG> m_references{1};
    std::shared_ptr<ToolbarAccessibility::State> m_state;
};
} // namespace

void ToolbarAccessibility::Update(HWND window, std::vector<Item> items, const wchar_t* name, const wchar_t* action) {
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->window = window;
        ++m_state->generation;
        m_state->focused = 0;
        m_state->items = std::move(items);
        m_state->name = name;
        m_state->action = action;
    }
    ::NotifyWinEvent(EVENT_OBJECT_REORDER, window, OBJID_CLIENT, CHILDID_SELF);
}

void ToolbarAccessibility::Focus(HWND window, LONG child) {
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->focused = child;
    }
    ::NotifyWinEvent(EVENT_OBJECT_FOCUS, window, OBJID_CLIENT, child);
}

void ToolbarAccessibility::Detach() {
    const std::lock_guard lock(m_state->mutex);
    m_state->window = nullptr;
    m_state->items.clear();
    ++m_state->generation;
    m_state->focused = 0;
}

bool ToolbarAccessibility::Current(UINT generation) const {
    const std::lock_guard lock(m_state->mutex);
    return m_state->window && m_state->generation == generation;
}

LRESULT ToolbarAccessibility::GetObject(HWND, WPARAM wp, LPARAM lp) const {
    auto* provider = new (std::nothrow) AccessibleToolbar(m_state);
    if (!provider) return 0;
    const LRESULT result = ::LresultFromObject(IID_IAccessible, wp, provider);
    provider->Release();
    return result;
}
} // namespace nskry
