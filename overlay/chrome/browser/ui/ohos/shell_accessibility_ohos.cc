#include "chrome/browser/ui/ohos/shell_accessibility_ohos.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/supports_user_data.h"
#include "base/task/bind_post_task.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "components/ohos_accessibility/accessibility_provider_ohos.h"
#include "content/public/browser/browser_accessibility_state.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/scoped_accessibility_mode.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/accessibility/ax_action_data.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_data.h"
#include "ui/accessibility/platform/ax_platform_node_delegate.h"
#include "ui/accessibility/platform/browser_accessibility.h"
#include "ui/accessibility/platform/browser_accessibility_manager.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace chrome::ohos {
namespace {

constexpr char kModeKey[] = "ohos-web-accessibility";

// A node's id across every accessibility tree in the browser. Asked through
// the delegate interface, where it is public; BrowserAccessibility's own
// override is protected.
int64_t UniqueId(const ui::BrowserAccessibility* node) {
  return static_cast<const ui::AXPlatformNodeDelegate*>(node)
      ->GetUniqueId()
      .value();
}

ohos_accessibility::Snapshot EmptySnapshot() {
  ohos_accessibility::Snapshot snapshot;
  ohos_accessibility::Node root;
  root.role = "Web";
  snapshot.nodes.emplace(0, std::move(root));
  snapshot.order.push_back(0);
  return snapshot;
}

class PageMode : public base::SupportsUserData::Data,
                 public content::WebContentsObserver {
 public:
  explicit PageMode(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        mode_(content::BrowserAccessibilityState::GetInstance()
                  ->CreateScopedModeForWebContents(contents, ui::kAXModeComplete)) {
    static uint64_t next_owner = 0;
    owner_ = ++next_owner;
  }

  void SetProvider(std::string component, uint64_t generation) {
    if (component_ != component || generation_ != generation) Clear();
    component_ = std::move(component);
    generation_ = generation;
  }

  bool TakeDirty() { return std::exchange(dirty_, false); }
  void MarkDirty() { dirty_ = true; }
  uint64_t owner() const { return owner_; }

  void SetGeometry(const gfx::Rect& bounds, float density) {
    if (bounds_ != bounds || density_ != density) dirty_ = true;
    bounds_ = bounds;
    density_ = density;
  }

  void AccessibilityEventReceived(const ui::AXUpdatesAndEvents&) override {
    dirty_ = true;
  }

  void AccessibilityLocationChangesReceived(
      const ui::AXTreeID&, ui::AXLocationAndScrollUpdates&) override {
    dirty_ = true;
  }

  void PrimaryMainFrameWasResized(bool) override { dirty_ = true; }
  void PrimaryPageChanged(content::Page&) override { Clear(); }
  void OnVisibilityChanged(content::Visibility visibility) override {
    dirty_ = true;
    if (visibility != content::Visibility::VISIBLE) Clear();
  }
  void WebContentsDestroyed() override { Clear(); }

 private:
  void Clear() {
    dirty_ = true;
    if (!component_.empty()) {
      ohos_accessibility::ClearSnapshot(component_, generation_, owner_);
    }
  }

  std::string component_;
  uint64_t generation_ = 0;
  uint64_t owner_ = 0;
  bool dirty_ = true;
  gfx::Rect bounds_;
  float density_ = 0.0f;
  std::unique_ptr<content::ScopedAccessibilityMode> mode_;
};

struct Target {
  ui::AXTreeID tree;
  ui::AXNodeID node;
};

std::string ComponentRole(ax::mojom::Role role) {
  switch (role) {
    case ax::mojom::Role::kButton: return "Button";
    case ax::mojom::Role::kCheckBox: return "Checkbox";
    case ax::mojom::Role::kRadioButton: return "Radio";
    case ax::mojom::Role::kSwitch: return "Toggle";
    case ax::mojom::Role::kLink: return "Link";
    case ax::mojom::Role::kTextField:
    case ax::mojom::Role::kTextFieldWithComboBox: return "TextInput";
    case ax::mojom::Role::kImage: return "Image";
    case ax::mojom::Role::kSlider: return "Slider";
    case ax::mojom::Role::kStaticText: return "Text";
    case ax::mojom::Role::kRootWebArea: return "Web";
    default: return "Column";
  }
}

void Perform(gfx::AcceleratedWidget widget,
             const std::string& component, uint64_t generation,
             base::WeakPtr<content::WebContents> contents,
             ui::AXTreeID root_tree,
             const std::map<int64_t, Target>& targets,
             int64_t id, ohos_accessibility::Action action,
             const std::string& value) {
  if (ohos_accessibility::RequestedProviderGeneration(component) != generation ||
      !contents || GetAuraShellAccessibilityContents(widget) != contents.get() ||
      contents->GetVisibility() != content::Visibility::VISIBLE ||
      contents->GetPrimaryMainFrame()->GetAXTreeID() != root_tree) {
    return;
  }
  auto found = targets.find(id);
  if (found == targets.end()) return;
  auto* manager = ui::BrowserAccessibilityManager::FromID(found->second.tree);
  auto* node = manager ? manager->GetFromID(found->second.node) : nullptr;
  if (!node || UniqueId(node) != id) return;
  ui::AXActionData data;
  data.target_tree_id = found->second.tree;
  data.target_node_id = found->second.node;
  switch (action) {
    case ohos_accessibility::Action::kClick:
      data.action = ax::mojom::Action::kDoDefault;
      break;
    case ohos_accessibility::Action::kFocus:
      manager->ScrollToMakeVisible(*node, gfx::Rect());
      manager->SetAccessibilityFocus(*node);
      return;
    case ohos_accessibility::Action::kClearFocus:
      manager->ClearAccessibilityFocus(*node);
      return;
    case ohos_accessibility::Action::kSetText:
      data.action = ax::mojom::Action::kSetValue;
      data.value = value;
      break;
    case ohos_accessibility::Action::kScrollForward:
      data.action = ax::mojom::Action::kScrollForward;
      break;
    case ohos_accessibility::Action::kScrollBackward:
      data.action = ax::mojom::Action::kScrollBackward;
      break;
  }
  node->AccessibilityPerformAction(data);
}

}

void UpdateShellAccessibility(gfx::AcceleratedWidget widget,
                              content::WebContents* contents) {
  auto component = ui::GetOhosNativeSurfaceComponentIdForWidget(widget);
  if (!component) return;
  const uint64_t generation =
      ohos_accessibility::RequestedProviderGeneration(*component);
  if (!generation) return;
  const auto surface = ui::GetOhosNativeSurface(widget);
  if (!surface) return;
  auto snapshot = EmptySnapshot();
  if (!contents || contents->GetVisibility() != content::Visibility::VISIBLE) {
    ohos_accessibility::PublishSnapshot(*component, generation, std::move(snapshot), {});
    return;
  }
  if (!contents->GetUserData(kModeKey)) {
    contents->SetUserData(kModeKey, std::make_unique<PageMode>(contents));
  }
  auto* page_mode = static_cast<PageMode*>(contents->GetUserData(kModeKey));
  page_mode->SetProvider(*component, generation);
  page_mode->SetGeometry(surface->bounds, surface->density);
  if (!page_mode->TakeDirty()) return;
  snapshot.owner = page_mode->owner();
  const auto tree = contents->GetPrimaryMainFrame()->GetAXTreeID();
  auto* manager = ui::BrowserAccessibilityManager::FromID(tree);
  auto* root = manager ? manager->GetBrowserAccessibilityRoot() : nullptr;
  if (!root) {
    static int not_ready = 0;
    if (++not_ready <= 3 || not_ready % 100 == 0) {
      LOG(WARNING) << "OHOS accessibility: page tree not ready (manager="
                   << (manager != nullptr) << ", " << not_ready << " times)";
    }
    page_mode->MarkDirty();
    ohos_accessibility::PublishSnapshot(*component, generation, std::move(snapshot), {});
    return;
  }
  snapshot.nodes.clear();
  snapshot.order.clear();
  snapshot.root = UniqueId(root);
  std::map<int64_t, Target> targets;
  std::vector<std::pair<ui::BrowserAccessibility*, int64_t>> pending = {{root, -1}};
  while (!pending.empty()) {
    auto [accessible, parent] = pending.back();
    pending.pop_back();
    ohos_accessibility::Node node;
    node.id = UniqueId(accessible);
    if (snapshot.nodes.contains(node.id)) continue;
    node.parent = parent;
    node.role = ComponentRole(accessible->GetRole());
    node.name = accessible->GetStringAttribute(ax::mojom::StringAttribute::kName);
    node.description = accessible->GetStringAttribute(
        ax::mojom::StringAttribute::kDescription);
    node.password = accessible->HasState(ax::mojom::State::kProtected);
    if (!node.password) {
      node.value = accessible->GetStringAttribute(ax::mojom::StringAttribute::kValue);
    }
    node.bounds = accessible->GetBoundsRect(ui::AXCoordinateSystem::kScreenPhysicalPixels,
                                           ui::AXClippingBehavior::kUnclipped);
    node.bounds.Offset(-surface->bounds.x(), -surface->bounds.y());
    node.visible = !accessible->HasState(ax::mojom::State::kInvisible) &&
                   !node.bounds.IsEmpty();
    const int restriction = accessible->GetIntAttribute(ax::mojom::IntAttribute::kRestriction);
    node.enabled = restriction != static_cast<int>(ax::mojom::Restriction::kDisabled);
    node.focusable = accessible->HasState(ax::mojom::State::kFocusable);
    node.focused = accessible->IsFocused();
    node.editable = accessible->IsTextField() &&
                    restriction == static_cast<int>(ax::mojom::Restriction::kNone);
    node.clickable = accessible->HasDefaultAction();
    const int checked = accessible->GetIntAttribute(ax::mojom::IntAttribute::kCheckedState);
    node.checkable = checked != static_cast<int>(ax::mojom::CheckedState::kNone);
    node.checked = checked == static_cast<int>(ax::mojom::CheckedState::kTrue) ||
                   checked == static_cast<int>(ax::mojom::CheckedState::kMixed);
    node.selected = accessible->GetBoolAttribute(ax::mojom::BoolAttribute::kSelected);
    node.scrollable = accessible->GetIntAttribute(ax::mojom::IntAttribute::kScrollYMax) >
                          accessible->GetIntAttribute(ax::mojom::IntAttribute::kScrollYMin) ||
                      accessible->GetIntAttribute(ax::mojom::IntAttribute::kScrollXMax) >
                          accessible->GetIntAttribute(ax::mojom::IntAttribute::kScrollXMin);
    if (node.editable) {
      accessible->GetIntAttribute(ax::mojom::IntAttribute::kTextSelEnd, &node.cursor);
    }
    const size_t count = accessible->PlatformChildCount();
    for (size_t index = 0; index < count; ++index) {
      auto* child = accessible->PlatformGetChild(index);
      if (child) node.children.push_back(UniqueId(child));
    }
    for (size_t index = count; index > 0; --index) {
      if (auto* child = accessible->PlatformGetChild(index - 1)) {
        pending.emplace_back(child, node.id);
      }
    }
    targets.emplace(node.id, Target{accessible->GetTreeData().tree_id,
                                    accessible->GetId()});
    snapshot.order.push_back(node.id);
    snapshot.nodes.emplace(node.id, std::move(node));
  }
  ohos_accessibility::PublishSnapshot(
      *component, generation, std::move(snapshot), base::BindPostTaskToCurrentDefault(
          base::BindRepeating(&Perform, widget, *component, generation,
                              contents->GetWeakPtr(), tree,
                              std::move(targets))));
}

}
