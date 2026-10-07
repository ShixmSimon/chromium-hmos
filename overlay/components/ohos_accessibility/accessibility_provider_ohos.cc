#include "components/ohos_accessibility/accessibility_provider_ohos.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <arkui/native_interface_accessibility.h>

#include "base/logging.h"
#include "base/no_destructor.h"

namespace ohos_accessibility {
namespace {

constexpr int32_t kSuccess = ARKUI_ACCESSIBILITY_NATIVE_RESULT_SUCCESSFUL;
constexpr int32_t kFailure = ARKUI_ACCESSIBILITY_NATIVE_RESULT_FAILED;

struct Provider {
  ArkUI_AccessibilityProvider* native = nullptr;
  void* window = nullptr;
  uint64_t generation = 0;
  std::string instance;
  bool requested = false;
  int64_t focused = -1;
  // Diagnostics: how many queries this provider has answered.
  int queries = 0;
  Snapshot snapshot;
  ActionCallback action;
};

std::mutex& Mutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

std::map<std::string, Provider>& Providers() {
  static base::NoDestructor<std::map<std::string, Provider>> providers;
  return *providers;
}

Provider* Find(const char* instance) {
  if (!instance) {
    return nullptr;
  }
  for (auto& [component, provider] : Providers()) {
    if (provider.instance == instance) {
      provider.requested = true;
      return &provider;
    }
  }
  return nullptr;
}

const Node* GetNode(const Provider& provider, int64_t id) {
  if (id == -1) {
    id = provider.snapshot.root;
  }
  auto found = provider.snapshot.nodes.find(id);
  return found == provider.snapshot.nodes.end() ? nullptr : &found->second;
}

bool Fill(const Provider& provider, const Node& node,
          ArkUI_AccessibilityElementInfo* info) {
  if (!info) {
    return false;
  }
  OH_ArkUI_AccessibilityElementInfoSetElementId(info, node.id);
  OH_ArkUI_AccessibilityElementInfoSetParentId(info, node.parent);
  OH_ArkUI_AccessibilityElementInfoSetComponentType(info, node.role.c_str());
  OH_ArkUI_AccessibilityElementInfoSetAccessibilityText(info, node.name.c_str());
  OH_ArkUI_AccessibilityElementInfoSetContents(
      info, node.password ? "" : node.value.c_str());
  OH_ArkUI_AccessibilityElementInfoSetAccessibilityDescription(
      info, node.description.c_str());
  std::vector<int64_t> children = node.children;
  if (!children.empty()) {
    OH_ArkUI_AccessibilityElementInfoSetChildNodeIds(
        info, static_cast<int32_t>(children.size()), children.data());
  }
  ArkUI_AccessibleRect rect = {node.bounds.x(), node.bounds.y(),
                              node.bounds.right(), node.bounds.bottom()};
  OH_ArkUI_AccessibilityElementInfoSetScreenRect(info, &rect);
  OH_ArkUI_AccessibilityElementInfoSetEnabled(info, node.enabled);
  OH_ArkUI_AccessibilityElementInfoSetVisible(info, node.visible);
  OH_ArkUI_AccessibilityElementInfoSetFocusable(info, node.focusable);
  OH_ArkUI_AccessibilityElementInfoSetFocused(info, node.focused);
  OH_ArkUI_AccessibilityElementInfoSetAccessibilityFocused(
      info, provider.focused == node.id);
  OH_ArkUI_AccessibilityElementInfoSetEditable(info, node.editable);
  OH_ArkUI_AccessibilityElementInfoSetIsPassword(info, node.password);
  OH_ArkUI_AccessibilityElementInfoSetClickable(info, node.clickable);
  OH_ArkUI_AccessibilityElementInfoSetCheckable(info, node.checkable);
  OH_ArkUI_AccessibilityElementInfoSetChecked(info, node.checked);
  OH_ArkUI_AccessibilityElementInfoSetSelected(info, node.selected);
  OH_ArkUI_AccessibilityElementInfoSetScrollable(info, node.scrollable);
  // Only nodes a reader has something to say about or do with are stops of
  // their own; containers are not, but what is inside them still is.
  const bool meaningful = !node.name.empty() || !node.value.empty() ||
                          node.focusable || node.clickable || node.editable ||
                          node.checkable;
  OH_ArkUI_AccessibilityElementInfoSetAccessibilityLevel(
      info, node.id != provider.snapshot.root && meaningful ? "yes" : "no");
  std::vector<ArkUI_AccessibleAction> actions = {
      {ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_GAIN_ACCESSIBILITY_FOCUS, ""},
      {ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLEAR_ACCESSIBILITY_FOCUS, ""}};
  if (node.enabled && node.clickable) {
    actions.push_back({ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLICK, ""});
  }
  if (node.enabled && node.editable) {
    actions.push_back({ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_TEXT, ""});
  }
  if (node.scrollable) {
    actions.push_back({ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD, ""});
    actions.push_back({ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_BACKWARD, ""});
  }
  OH_ArkUI_AccessibilityElementInfoSetOperationActions(
      info, static_cast<int32_t>(actions.size()), actions.data());
  return true;
}

void SendEvent(Provider& provider, int64_t id,
               ArkUI_AccessibilityEventType type) {
  if (id < 0) return;
  const Node* node = GetNode(provider, id);
  if (!node) {
    return;
  }
  auto* info = OH_ArkUI_CreateAccessibilityElementInfo();
  auto* event = OH_ArkUI_CreateAccessibilityEventInfo();
  if (info && event && Fill(provider, *node, info)) {
    OH_ArkUI_AccessibilityEventSetEventType(event, type);
    OH_ArkUI_AccessibilityEventSetElementInfo(event, info);
    OH_ArkUI_SendAccessibilityAsyncEvent(provider.native, event, [](int32_t) {});
  }
  if (event) {
    OH_ArkUI_DestoryAccessibilityEventInfo(event);
  }
  if (info) {
    OH_ArkUI_DestoryAccessibilityElementInfo(info);
  }
}

void AddDescendants(const Provider& provider, const Node& node,
                    std::set<int64_t>& ids, bool recursive) {
  std::vector<int64_t> pending = node.children;
  while (!pending.empty()) {
    const int64_t child = pending.back();
    pending.pop_back();
    if (ids.insert(child).second && recursive) {
      if (const Node* descendant = GetNode(provider, child)) {
        pending.insert(pending.end(), descendant->children.begin(),
                       descendant->children.end());
      }
    }
  }
}

int32_t FindById(const char* instance, int64_t id,
                 ArkUI_AccessibilitySearchMode mode, int32_t,
                 ArkUI_AccessibilityElementInfoList* list) {
  std::lock_guard lock(Mutex());
  Provider* provider = Find(instance);
  const Node* node = provider ? GetNode(*provider, id) : nullptr;
  if (!node || !list) {
    return kFailure;
  }
  std::set<int64_t> ids = {node->id};
  // Logged as numbers only, never names or values. The first few queries
  // of each provider, then one in a hundred.
  const bool log = ++provider->queries <= 5 || provider->queries % 100 == 0;
  if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_PREDECESSORS) {
    const Node* parent = node->parent < 0 ? nullptr : GetNode(*provider, node->parent);
    while (parent && parent->id != node->id && ids.insert(parent->id).second) {
      parent = parent->parent < 0 ? nullptr : GetNode(*provider, parent->parent);
    }
  }
  if (mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_SIBLINGS) {
    if (node->parent >= 0) {
      if (const Node* parent = GetNode(*provider, node->parent)) {
        ids.insert(parent->children.begin(), parent->children.end());
      }
    }
  }
  if (mode & (ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_CHILDREN |
              ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_RECURSIVE_CHILDREN)) {
    AddDescendants(*provider, *node, ids,
        mode & ARKUI_ACCESSIBILITY_NATIVE_SEARCH_MODE_PREFETCH_RECURSIVE_CHILDREN);
  }
  for (int64_t selected : ids) {
    if (const Node* result = GetNode(*provider, selected)) {
      if (!Fill(*provider, *result,
                OH_ArkUI_AddAndGetAccessibilityElementInfo(list))) {
        return kFailure;
      }
    }
  }
  if (log) {
    LOG(WARNING) << "OHOS accessibility: query #" << provider->queries
                 << " id=" << id << " mode=" << static_cast<int>(mode)
                 << " answered=" << ids.size()
                 << " snapshot_nodes=" << provider->snapshot.nodes.size();
  }
  return kSuccess;
}

int32_t FindByText(const char* instance, int64_t id, const char* text,
                   int32_t, ArkUI_AccessibilityElementInfoList* list) {
  std::lock_guard lock(Mutex());
  Provider* provider = Find(instance);
  const Node* node = provider ? GetNode(*provider, id) : nullptr;
  if (!node || !text || !list) {
    return kFailure;
  }
  std::set<int64_t> ids = {node->id};
  AddDescendants(*provider, *node, ids, true);
  for (int64_t selected : ids) {
    const Node* result = GetNode(*provider, selected);
    if (result && (result->name.find(text) != std::string::npos ||
                   (!result->password &&
                    result->value.find(text) != std::string::npos))) {
      if (!Fill(*provider, *result,
                OH_ArkUI_AddAndGetAccessibilityElementInfo(list))) {
        return kFailure;
      }
    }
  }
  return kSuccess;
}

int32_t FindFocused(const char* instance, int64_t id,
                    ArkUI_AccessibilityFocusType type, int32_t,
                    ArkUI_AccessibilityElementInfo* info) {
  std::lock_guard lock(Mutex());
  Provider* provider = Find(instance);
  const Node* root = provider ? GetNode(*provider, id) : nullptr;
  if (!root) {
    return kFailure;
  }
  std::set<int64_t> ids = {root->id};
  AddDescendants(*provider, *root, ids, true);
  for (int64_t selected : ids) {
    const Node* node = GetNode(*provider, selected);
    if (node && ((type == ARKUI_ACCESSIBILITY_NATIVE_FOCUS_TYPE_ACCESSIBILITY &&
                  node->id == provider->focused) ||
                 (type == ARKUI_ACCESSIBILITY_NATIVE_FOCUS_TYPE_INPUT &&
                  node->focused))) {
      return Fill(*provider, *node, info) ? kSuccess : kFailure;
    }
  }
  return kFailure;
}

int32_t FindNext(const char* instance, int64_t id,
                 ArkUI_AccessibilityFocusMoveDirection direction, int32_t,
                 ArkUI_AccessibilityElementInfo* info) {
  std::lock_guard lock(Mutex());
  Provider* provider = Find(instance);
  const Node* node = provider ? GetNode(*provider, id) : nullptr;
  if (!node || (direction != ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_FORWARD &&
                direction != ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_BACKWARD)) {
    return kFailure;
  }
  const auto& order = provider->snapshot.order;
  auto found = std::find(order.begin(), order.end(), node->id);
  if (found == order.end()) {
    return kFailure;
  }
  int index = static_cast<int>(found - order.begin());
  const int step = direction == ARKUI_ACCESSIBILITY_NATIVE_DIRECTION_FORWARD
                       ? 1 : -1;
  for (index += step; index >= 0 && index < static_cast<int>(order.size());
       index += step) {
    const Node* next = GetNode(*provider, order[index]);
    if (next && next->visible &&
        (next->focusable || !next->name.empty() || next->editable)) {
      return Fill(*provider, *next, info) ? kSuccess : kFailure;
    }
  }
  return kFailure;
}

int32_t Execute(const char* instance, int64_t id,
                ArkUI_Accessibility_ActionType type,
                ArkUI_AccessibilityActionArguments* arguments, int32_t) {
  ActionCallback callback;
  Action action;
  int64_t previous_focus = -1;
  std::string text;
  {
    std::lock_guard lock(Mutex());
    Provider* provider = Find(instance);
    const Node* node = provider ? GetNode(*provider, id) : nullptr;
    if (!node || !provider->action) {
      return kFailure;
    }
    id = node->id;
    switch (type) {
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLICK:
        if (!node->enabled || !node->clickable) return kFailure;
        action = Action::kClick;
        break;
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SET_TEXT: {
        if (!node->enabled || !node->editable || !arguments) return kFailure;
        char* value = nullptr;
        if (OH_ArkUI_FindAccessibilityActionArgumentByKey(
                arguments, "ACTION_ARGU_SET_TEXT", &value) != kSuccess || !value) {
          return kFailure;
        }
        text = value;
        action = Action::kSetText;
        break;
      }
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_GAIN_ACCESSIBILITY_FOCUS:
        action = Action::kFocus;
        if (provider->focused != id) {
          previous_focus = provider->focused;
          SendEvent(*provider, provider->focused,
                    ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUS_CLEARED);
          provider->focused = id;
          SendEvent(*provider, id,
                    ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUSED);
        }
        break;
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLEAR_ACCESSIBILITY_FOCUS:
        action = Action::kClearFocus;
        if (provider->focused != id) return kFailure;
        provider->focused = -1;
        SendEvent(*provider, id,
                  ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUS_CLEARED);
        break;
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD:
      case ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_BACKWARD:
        if (!node->scrollable) return kFailure;
        action = type == ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_SCROLL_FORWARD
                     ? Action::kScrollForward : Action::kScrollBackward;
        break;
      default:
        return kFailure;
    }
    callback = provider->action;
  }
  if (previous_focus >= 0) {
    callback.Run(previous_focus, Action::kClearFocus, std::string());
  }
  callback.Run(id, action, text);
  return kSuccess;
}

int32_t ClearFocus(const char* instance) {
  int64_t id;
  {
    std::lock_guard lock(Mutex());
    Provider* provider = Find(instance);
    if (!provider) return kFailure;
    id = provider->focused;
    if (id < 0) return kSuccess;
  }
  return Execute(instance, id,
      ARKUI_ACCESSIBILITY_NATIVE_ACTION_TYPE_CLEAR_ACCESSIBILITY_FOCUS, nullptr, 0);
}

int32_t GetCursor(const char* instance, int64_t id, int32_t, int32_t* index) {
  std::lock_guard lock(Mutex());
  Provider* provider = Find(instance);
  const Node* node = provider ? GetNode(*provider, id) : nullptr;
  if (!node || !index || !node->editable || node->cursor < 0) return kFailure;
  *index = node->cursor;
  return kSuccess;
}

ArkUI_AccessibilityProviderCallbacksWithInstance callbacks = {
    FindById, FindByText, FindFocused, FindNext, Execute, ClearFocus, GetCursor};

}

bool RegisterProvider(const std::string& component_id,
                      OH_NativeXComponent* component, void* window) {
  ArkUI_AccessibilityProvider* native = nullptr;
  if (OH_NativeXComponent_GetNativeAccessibilityProvider(component, &native) != 0 ||
      !native) {
    LOG(WARNING) << "OHOS accessibility: no provider for " << component_id;
    return false;
  }
  {
    std::lock_guard lock(Mutex());
    auto existing = Providers().find(component_id);
    if (existing != Providers().end() && existing->second.native == native &&
        existing->second.window == window) {
      return true;
    }
    static uint64_t next_generation = 0;
    Provider provider;
    provider.native = native;
    provider.window = window;
    provider.generation = ++next_generation;
    provider.instance = component_id + ":" + std::to_string(provider.generation);
    Node root;
    root.id = kRootId;
    root.parent = kRootParentId;
    root.role = "Web";
    provider.snapshot.nodes.emplace(kRootId, std::move(root));
    provider.snapshot.order.push_back(kRootId);
    Providers().insert_or_assign(component_id, std::move(provider));
  }
  std::string instance;
  {
    std::lock_guard lock(Mutex());
    instance = Providers().at(component_id).instance;
  }
  const int32_t result = OH_ArkUI_AccessibilityProviderRegisterCallbackWithInstance(
      instance.c_str(), native, &callbacks);
  LOG(WARNING) << "OHOS accessibility: provider " << instance
               << " registered, result=" << result;
  if (result != kSuccess) {
    UnregisterProvider(component_id, window);
    return false;
  }
  return true;
}

void UnregisterProvider(const std::string& component_id, void* window) {
  std::lock_guard lock(Mutex());
  auto found = Providers().find(component_id);
  if (found != Providers().end() && found->second.window == window) {
    Providers().erase(found);
  }
}

uint64_t RequestedProviderGeneration(const std::string& component_id) {
  std::lock_guard lock(Mutex());
  auto found = Providers().find(component_id);
  return found != Providers().end() && found->second.requested
             ? found->second.generation : 0;
}

void PublishSnapshot(const std::string& component_id, uint64_t generation,
                     Snapshot snapshot,
                     ActionCallback action) {
  std::lock_guard lock(Mutex());
  auto found = Providers().find(component_id);
  if (found == Providers().end() || !found->second.requested ||
      found->second.generation != generation) return;
  Provider& provider = found->second;
  provider.action = std::move(action);
  if (provider.snapshot == snapshot) return;
  if (provider.snapshot.nodes.size() != snapshot.nodes.size()) {
    LOG(WARNING) << "OHOS accessibility: " << component_id << " snapshot "
                 << provider.snapshot.nodes.size() << " -> "
                 << snapshot.nodes.size() << " nodes";
  }
  if (!snapshot.nodes.contains(provider.focused)) {
    SendEvent(provider, provider.focused,
              ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_ACCESSIBILITY_FOCUS_CLEARED);
    provider.focused = -1;
  }
  provider.snapshot = std::move(snapshot);
  SendEvent(provider, provider.snapshot.root,
            ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_PAGE_CONTENT_UPDATE);
}

void ClearSnapshot(const std::string& component_id, uint64_t generation,
                   uint64_t owner) {
  std::lock_guard lock(Mutex());
  auto found = Providers().find(component_id);
  if (found == Providers().end() || found->second.generation != generation ||
      found->second.snapshot.owner != owner) return;
  Provider& provider = found->second;
  provider.action.Reset();
  provider.focused = -1;
  provider.snapshot = Snapshot();
  Node root;
  root.id = kRootId;
  root.parent = kRootParentId;
  root.role = "Web";
  provider.snapshot.nodes.emplace(kRootId, std::move(root));
  provider.snapshot.order.push_back(kRootId);
  SendEvent(provider, kRootId,
            ARKUI_ACCESSIBILITY_NATIVE_EVENT_TYPE_PAGE_CONTENT_UPDATE);
}

}
