#include "platform/windows/windows_ui_automation.hpp"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <UIAutomation.h>
#include <atomic>
#include <bit>
#include <new>
#include <optional>

namespace blackbox::platform::windows {
using core::AccessibilityTree;
using core::AccessibleRole;
using core::AccessibleAction;
using core::AccessibleNode;
using core::AccessibleSnapshot;
namespace {
struct Connection {
    HWND window{};
    std::shared_ptr<AccessibilityTree> tree;
    std::atomic<bool> alive{true};
};
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
std::string utf8(LPCWSTR text) {
    if (!text) return {};
    const auto count = wcsnlen_s(text, 4097U);
    if (count > 4096U) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(count), result.data(), length, nullptr, nullptr);
    return result;
}
CONTROLTYPEID control_type(AccessibleRole role) {
    switch (role) {
    case AccessibleRole::button: return UIA_ButtonControlTypeId;
    case AccessibleRole::checkbox: return UIA_CheckBoxControlTypeId;
    case AccessibleRole::edit: return UIA_EditControlTypeId;
    case AccessibleRole::choice: return UIA_ComboBoxControlTypeId;
    case AccessibleRole::item: return UIA_ListItemControlTypeId;
    case AccessibleRole::disclosure: return UIA_GroupControlTypeId;
    default: return UIA_TextControlTypeId;
    }
}
class Provider final : public IRawElementProviderSimple, public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider, public IValueProvider, public IToggleProvider,
                       public IExpandCollapseProvider, public ISelectionItemProvider {
public:
    Provider(std::shared_ptr<Connection> connection, std::uint32_t id) : connection_{std::move(connection)}, id_{id} {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *result = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *result = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot) && id_ == 0U) *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IInvokeProvider)) *result = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(IValueProvider)) *result = static_cast<IValueProvider*>(this);
        else if (iid == __uuidof(IToggleProvider)) *result = static_cast<IToggleProvider*>(this);
        else if (iid == __uuidof(IExpandCollapseProvider)) *result = static_cast<IExpandCollapseProvider*>(this);
        else if (iid == __uuidof(ISelectionItemProvider)) *result = static_cast<ISelectionItemProvider*>(this);
        if (!*result) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
        if (!options) return E_POINTER; *options = ProviderOptions_ServerSideProvider; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!state.node) return S_OK;
        const auto role = state.node->role;
        if (pattern == UIA_InvokePatternId && (role == AccessibleRole::button || role == AccessibleRole::item))
            return QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void**>(result));
        if (pattern == UIA_ValuePatternId && (role == AccessibleRole::edit || role == AccessibleRole::choice))
            return QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void**>(result));
        if (pattern == UIA_TogglePatternId && role == AccessibleRole::checkbox)
            return QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(result));
        if (pattern == UIA_ExpandCollapsePatternId && role == AccessibleRole::disclosure)
            return QueryInterface(__uuidof(IExpandCollapseProvider), reinterpret_cast<void**>(result));
        if (pattern == UIA_SelectionItemPatternId && role == AccessibleRole::item)
            return QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(result));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override try {
        if (!result) return E_POINTER; VariantInit(result);
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto* node = state.node;
        if (property == UIA_NamePropertyId || property == UIA_AutomationIdPropertyId || property == UIA_FrameworkIdPropertyId) {
            const auto text = property == UIA_FrameworkIdPropertyId ? L"BlackBox" :
                property == UIA_AutomationIdPropertyId ? wide(std::to_string(id_)) :
                node ? wide(node->name) : L"BlackBox";
            result->vt = VT_BSTR; result->bstrVal = SysAllocString(text.c_str());
            return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (property == UIA_LiveSettingPropertyId) {
            result->vt = VT_I4; result->lVal = node && node->live ? Polite : Off;
        } else if (property == UIA_ControlTypePropertyId) {
            result->vt = VT_I4; result->lVal = node ? control_type(node->role) : UIA_PaneControlTypeId;
        } else if (property == UIA_IsEnabledPropertyId || property == UIA_HasKeyboardFocusPropertyId ||
                   property == UIA_IsKeyboardFocusablePropertyId || property == UIA_IsOffscreenPropertyId ||
                   property == UIA_IsControlElementPropertyId || property == UIA_IsContentElementPropertyId) {
            bool value = true;
            if (property == UIA_IsEnabledPropertyId) value = !node || node->enabled;
            if (property == UIA_HasKeyboardFocusPropertyId) value = node && node->focused;
            if (property == UIA_IsKeyboardFocusablePropertyId) value = node && node->role != AccessibleRole::text;
            if (property == UIA_IsOffscreenPropertyId) value = !IsWindowVisible(connection_->window) || (node && node->offscreen);
            result->vt = VT_BOOL; result->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        return id_ == 0U && connection_->alive ? UiaHostProviderFromHwnd(connection_->window, result) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        if (id_ != 0U && direction == NavigateDirection_Parent) return fragment(0U, result);
        const auto& nodes = state.snapshot->nodes;
        if (nodes.empty()) return S_OK;
        if (id_ == 0U && direction == NavigateDirection_FirstChild) return fragment(nodes.front().id, result);
        if (id_ == 0U && direction == NavigateDirection_LastChild) return fragment(nodes.back().id, result);
        if (state.node && (direction == NavigateDirection_NextSibling || direction == NavigateDirection_PreviousSibling)) {
            const auto index = static_cast<std::size_t>(state.node - nodes.data());
            if (direction == NavigateDirection_NextSibling && index + 1U < nodes.size()) return fragment(nodes[index + 1U].id, result);
            if (direction == NavigateDirection_PreviousSibling && index != 0U) return fragment(nodes[index - 1U].id, result);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if (!result) return E_POINTER;
        *result = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*result) return E_OUTOFMEMORY;
        LONG index = 0, value = UiaAppendRuntimeId;
        SafeArrayPutElement(*result, &index, &value);
        index = 1; value = std::bit_cast<LONG>(id_); SafeArrayPutElement(*result, &index, &value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if (!result) return E_POINTER; *result = {};
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        POINT origin{}; ClientToScreen(connection_->window, &origin);
        if (state.node) *result = {origin.x + state.node->x, origin.y + state.node->y, state.node->width, state.node->height};
        else { RECT rect{}; GetWindowRect(connection_->window, &rect); *result = {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right - rect.left), static_cast<double>(rect.bottom - rect.top)}; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override { if (!result) return E_POINTER; *result = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return send(AccessibleAction::focus); }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if (!result) return E_POINTER;
        *result = new (std::nothrow) Provider{connection_, 0U}; return *result ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        POINT origin{}; ClientToScreen(connection_->window, &origin);
        for (const auto& node : state.snapshot->nodes)
            if (!node.offscreen && x >= origin.x + node.x && y >= origin.y + node.y &&
                x < origin.x + node.x + node.width && y < origin.y + node.y + node.height) return fragment(node.id, result);
        return fragment(0U, result);
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        for (const auto& node : state.snapshot->nodes) if (node.focused) return fragment(node.id, result);
        return fragment(0U, result);
    }
    HRESULT STDMETHODCALLTYPE Invoke() override { return send(AccessibleAction::invoke); }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override try {
        if (!value || wcsnlen_s(value, 4097U) > 4096U) return E_INVALIDARG;
        return send(AccessibleAction::set_value, utf8(value));
    } catch (...) { return E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override try {
        if (!result) return E_POINTER; *result = nullptr;
        const auto state = lookup(); if (!state.valid || !state.node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = SysAllocString(wide(state.node->value).c_str()); return *result ? S_OK : E_OUTOFMEMORY;
    } catch (...) { return E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override {
        if (!result) return E_POINTER;
        const auto state = lookup(); if (!state.valid || !state.node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = state.node->read_only || !state.node->enabled; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Toggle() override { return send(AccessibleAction::toggle); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* result) override {
        if (!result) return E_POINTER;
        const auto state = lookup(); if (!state.valid || !state.node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = state.node->selected ? ToggleState_On : ToggleState_Off; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Expand() override { return send(AccessibleAction::expand); }
    HRESULT STDMETHODCALLTYPE Collapse() override { return send(AccessibleAction::collapse); }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* result) override {
        if (!result) return E_POINTER;
        const auto state = lookup(); if (!state.valid || !state.node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = state.node->expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Select() override { return send(AccessibleAction::select); }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override {
        if (!result) return E_POINTER;
        const auto state = lookup(); if (!state.valid || !state.node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = state.node->selected; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER; *result = nullptr; return S_OK;
    }
private:
    struct Lookup { std::shared_ptr<const AccessibleSnapshot> snapshot; const AccessibleNode* node{}; bool valid{}; };
    Lookup lookup() const {
        Lookup result{connection_->tree->snapshot()};
        if (!connection_->alive || !result.snapshot->available) return result;
        if (id_ == 0U) { result.valid = true; return result; }
        for (const auto& node : result.snapshot->nodes) if (node.id == id_) { result.node = &node; result.valid = true; break; }
        return result;
    }
    HRESULT fragment(std::uint32_t id, IRawElementProviderFragment** result) {
        *result = new (std::nothrow) Provider{connection_, id}; return *result ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT send(AccessibleAction action, std::string value = {}) try {
        const auto state = lookup(); if (!state.valid) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!state.node || !state.node->enabled) return UIA_E_ELEMENTNOTENABLED;
        return connection_->tree->request({id_, action, std::move(value), {}}) ? S_OK : UIA_E_INVALIDOPERATION;
    } catch (...) { return E_OUTOFMEMORY; }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Connection> connection_;
    std::uint32_t id_{};
};
}

struct WindowsUiAutomation::Native {
    std::shared_ptr<Connection> connection;
    Provider* root{};
    std::uint32_t focus{};
    std::vector<std::uint32_t> ids;
    std::shared_ptr<const AccessibleSnapshot> previous;
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR, DWORD_PTR context) {
        auto* self = reinterpret_cast<Native*>(context);
        if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId && self->root) {
            self->connection->tree->enable();
            return UiaReturnRawElementProvider(window, wparam, lparam, self->root);
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }
};
WindowsUiAutomation::WindowsUiAutomation() : native_{std::make_unique<Native>()} {}
WindowsUiAutomation::~WindowsUiAutomation() { detach(); }
bool WindowsUiAutomation::attach(void* window, std::shared_ptr<AccessibilityTree> tree) {
    detach();
    native_->connection = std::make_shared<Connection>();
    native_->connection->window = static_cast<HWND>(window);
    native_->connection->tree = std::move(tree);
    native_->root = new Provider{native_->connection, 0U};
    if (!SetWindowSubclass(native_->connection->window, Native::window_proc, 1U,
                            reinterpret_cast<DWORD_PTR>(native_.get()))) { detach(); return false; }
    return true;
}
void WindowsUiAutomation::notify_changes() {
    if (!native_->root || !native_->connection->tree->enabled()) return;
    const auto state = native_->connection->tree->snapshot();
    std::vector<std::uint32_t> ids;
    for (const auto& node : state->nodes) {
        ids.push_back(node.id);
        if (native_->previous) {
            const auto& previous = native_->previous->nodes;
            const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& item) { return item.id == node.id; });
            if (old != previous.end()) {
                auto changed = [&](PROPERTYID property, VARIANT before, VARIANT after) {
                    auto* provider = new Provider{native_->connection, node.id};
                    UiaRaiseAutomationPropertyChangedEvent(provider, property, before, after);
                    provider->Release(); VariantClear(&before); VariantClear(&after);
                };
                auto string_value = [](const std::string& text) { VARIANT v{}; v.vt = VT_BSTR; v.bstrVal = SysAllocString(wide(text).c_str()); return v; };
                auto integer_value = [](LONG value) { VARIANT v{}; v.vt = VT_I4; v.lVal = value; return v; };
                if (old->value != node.value && (node.role == AccessibleRole::edit || node.role == AccessibleRole::choice))
                    changed(UIA_ValueValuePropertyId, string_value(old->value), string_value(node.value));
                if (old->selected != node.selected && node.role == AccessibleRole::checkbox)
                    changed(UIA_ToggleToggleStatePropertyId, integer_value(old->selected ? ToggleState_On : ToggleState_Off), integer_value(node.selected ? ToggleState_On : ToggleState_Off));
                if (old->name != node.name) {
                    changed(UIA_NamePropertyId, string_value(old->name), string_value(node.name));
                    if (node.live) {
                        auto* provider = new Provider{native_->connection, node.id};
                        UiaRaiseAutomationEvent(provider, UIA_LiveRegionChangedEventId);
                        provider->Release();
                    }
                }
            }
        }
        if (node.focused && node.id != native_->focus) {
            native_->focus = node.id;
            auto* focused = new Provider{native_->connection, node.id};
            UiaRaiseAutomationEvent(focused, UIA_AutomationFocusChangedEventId);
            focused->Release();
        }
    }
    native_->previous = state;
    if (ids != native_->ids) {
        native_->ids = std::move(ids);
        UiaRaiseStructureChangedEvent(native_->root, StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }
}
void WindowsUiAutomation::detach() noexcept {
    if (!native_->connection) return;
    native_->connection->alive = false;
    RemoveWindowSubclass(native_->connection->window, Native::window_proc, 1U);
    if (native_->root) { native_->root->Release(); native_->root = nullptr; }
    native_->connection.reset();
}
} // namespace blackbox::platform::windows
