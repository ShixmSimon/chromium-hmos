#ifndef COMPONENTS_OHOS_ACCESSIBILITY_ACCESSIBILITY_PROVIDER_OHOS_H_
#define COMPONENTS_OHOS_ACCESSIBILITY_ACCESSIBILITY_PROVIDER_OHOS_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "ui/gfx/geometry/rect.h"

struct OH_NativeXComponent;

namespace ohos_accessibility {

enum class Action { kClick, kFocus, kClearFocus, kSetText, kScrollForward,
                    kScrollBackward };

struct Node {
  int64_t id = 0;
  int64_t parent = -1;
  std::vector<int64_t> children;
  std::string role;
  std::string name;
  std::string value;
  std::string description;
  gfx::Rect bounds;
  bool enabled = true;
  bool visible = true;
  bool focusable = false;
  bool focused = false;
  bool editable = false;
  bool password = false;
  bool clickable = false;
  bool checkable = false;
  bool checked = false;
  bool selected = false;
  bool scrollable = false;
  int cursor = -1;

  bool operator==(const Node&) const = default;
};

struct Snapshot {
  uint64_t owner = 0;
  int64_t root = 0;
  std::vector<int64_t> order;
  std::map<int64_t, Node> nodes;
  bool operator==(const Snapshot&) const = default;
};

using ActionCallback =
    base::RepeatingCallback<void(int64_t, Action, const std::string&)>;

bool RegisterProvider(const std::string& component_id,
                      OH_NativeXComponent* component, void* window);
void UnregisterProvider(const std::string& component_id, void* window);
uint64_t RequestedProviderGeneration(const std::string& component_id);
void ClearSnapshot(const std::string& component_id, uint64_t generation,
                   uint64_t owner);
void PublishSnapshot(const std::string& component_id,
                     uint64_t generation,
                     Snapshot snapshot,
                     ActionCallback action);

}

#endif
