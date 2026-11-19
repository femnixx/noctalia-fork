#include "shell/bar/bar.h"

#include "compositors/compositor_platform.h"
#include "shell/wayland/foreign_toplevels.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tooltip/tooltip_placement.h"
#include "shell/wayland/layer_shell.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/surface_shadow.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "system/brightness_service.h"
#include "notification/notification_manager.h"
#include "lib/render/core/color.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <pwd.h>
#include <fstream>

namespace {

  constexpr Logger kLog("bar");
  constexpr std::chrono::milliseconds kWorkspaceRevealDebounce{80};
  constexpr std::chrono::milliseconds kWorkspacePeekHold{450};
  constexpr std::int32_t kAutoHideTriggerPx = 3;
  constexpr float kAutoHideSlideExtraPx = 4.0F;

  std::string activeWorkspaceId(const std::vector<Workspace>& workspaces) {
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        if (!workspace.id.empty()) {
          return workspace.id;
        }
        if (!workspace.name.empty()) {
          return workspace.name;
        }
        return std::to_string(workspace.index);
      }
    }
    return {};
  }

  bool barConfigUsesSlideSurface(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  bool barSupportsSlideBehavior(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  bool barPointerHideAllowed(const BarInstance& instance) noexcept {
    if (instance.barConfig.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return instance.barConfig.autoHide;
  }

  bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (assignment.minimizeState.has_value() && assignment.minimizeState->minimized) {
        continue;
      }
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    if (platform.hasUnminimizedToplevels(output) == false) {
      return false;
    }
    return active->occupied;
  }

  bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }

  int barAutoHideEdgeGutter(const BarConfig& cfg) noexcept {
    if (!barConfigUsesSlideSurface(cfg) || cfg.marginEdge <= 0) {
      return 0;
    }
    return cfg.marginEdge;
  }

  std::vector<InputRect> barAutoHideSurfaceInputRegion(const BarConfig& cfg, int surfW, int surfH, bool fullSurface) {
    if (surfW <= 0 || surfH <= 0) {
      return {};
    }
    if (fullSurface) {
      return {InputRect{0, 0, surfW, surfH}};
    }

    const int strip = std::min(kAutoHideTriggerPx, cfg.position == "left" || cfg.position == "right" ? surfW : surfH);
    if (cfg.position == "bottom") {
      return {InputRect{0, surfH - strip, surfW, strip}};
    }
    if (cfg.position == "left") {
      return {InputRect{0, 0, strip, surfH}};
    }
    if (cfg.position == "right") {
      return {InputRect{surfW - strip, 0, strip, surfH}};
    }
    return {InputRect{0, 0, surfW, strip}};
  }

  bool pointInsideNode(const Node* node, float sceneX, float sceneY) {
    if (node == nullptr) {
      return false;
    }
    float localX = 0.0F;
    float localY = 0.0F;
    if (!Node::mapFromScene(node, sceneX, sceneY, localX, localY)) {
      return false;
    }
    return localX >= 0.0F && localX < node->width() && localY >= 0.0F && localY < node->height();
  }

  HitTestOutset crossAxisOutsetToSlot(const Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return {};
    }

    float nodeX = 0.0F;
    float nodeY = 0.0F;
    float slotX = 0.0F;
    float slotY = 0.0F;
    Node::absolutePosition(node, nodeX, nodeY);
    Node::absolutePosition(slot, slotX, slotY);

    if (isVertical) {
      return {
          .left = std::max(0.0F, nodeX - slotX),
          .top = 0.0F,
          .right = std::max(0.0F, (slotX + slot->width()) - (nodeX + node->width())),
          .bottom = 0.0F,
      };
    }

    return {
        .left = 0.0F,
        .top = std::max(0.0F, nodeY - slotY),
        .right = 0.0F,
        .bottom = std::max(0.0F, (slotY + slot->height()) - (nodeY + node->height())),
    };
  }

  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return;
    }

    if (dynamic_cast<InputArea*>(node) != nullptr || node->clipChildren()) {
      node->setHitTestOutset(crossAxisOutsetToSlot(node, slot, isVertical));
    }

    for (const auto& child : node->children()) {
      applyBarWidgetHitTargets(child.get(), slot, isVertical);
    }
  }

  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY) {
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr
          || widget->isBarClickThrough()
          || widget->outerNode() == nullptr
          || !widget->outerNode()->visible()) {
        continue;
      }
      // Bounds only, never Node::hitTest: hit outsets deliberately extend a widget's clickable
      // band past its ink (bar.cpp applies the hover pill padding that way), and treating that
      // band as "on a widget" would carve unreachable holes out of the dead zone.
      if (pointInsideNode(widget->outerNode(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough()) {
        continue;
      }
      auto* root = widget != nullptr ? widget->outerNode() : nullptr;
      auto* bounds = widget != nullptr ? widget->layoutBoundsNode() : nullptr;
      if (root == nullptr || bounds == nullptr || bounds == root || root->parent() != bounds || !bounds->visible()) {
        continue;
      }
      if (pointInsideNode(bounds, sceneX, sceneY)) {
        return widget;
      }
    }
    return nullptr;
  }

  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY) {
    if (auto* widget = widgetAtPoint(instance.endWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    if (auto* widget = widgetAtPoint(instance.centerWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    return widgetAtPoint(instance.startWidgets, sceneX, sceneY);
  }

  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo) {
    if (instance.surface == nullptr) {
      return {0.0F, 0.0F};
    }
    const auto* surface = instance.surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const auto mTop = static_cast<float>(surface->marginTop());
    const auto mRight = static_cast<float>(surface->marginRight());
    const auto mBottom = static_cast<float>(surface->marginBottom());
    const auto mLeft = static_cast<float>(surface->marginLeft());
    const auto surfW = static_cast<float>(surface->width());
    const auto surfH = static_cast<float>(surface->height());
    const auto outputW = static_cast<float>(outputInfo.effectiveLogicalWidth());
    const auto outputH = static_cast<float>(outputInfo.effectiveLogicalHeight());

    float x = mLeft;
    float y = mTop;
    if (aRight && !aLeft) {
      x = std::max(0.0F, outputW - mRight - surfW);
    }

    if (aBottom && !aTop) {
      y = std::max(0.0F, outputH - mBottom - surfH);
    }
    return {x, y};
  }

  bool isBarDeadZone(const BarInstance& instance, float sceneX, float sceneY) {
    if (widgetAtPoint(instance, sceneX, sceneY) != nullptr) {
      return false;
    }
    return pointInsideNode(instance.startSection, sceneX, sceneY)
        || pointInsideNode(instance.centerSection, sceneX, sceneY)
        || pointInsideNode(instance.endSection, sceneX, sceneY)
        || pointInsideNode(instance.sceneRoot.get(), sceneX, sceneY);
  }

  // Bar tooltips stay suppressed while any panel opened from the bar is alive.
  void suppressTooltipForOpenPanel(std::string_view panelId) {
    TooltipManager::instance().suppressBarTooltipsForPanel(panelId);
  }

  // The dead zone has no widget to anchor to, so a panel action anchors at the pointer instead.
  void openPanelAtBarPointer(
      BarInstance& instance, float sx, float sy, CompositorPlatform* platform, std::string_view sourceBarName,
      std::string_view panelId, std::string_view context, bool toggle
  ) {
    auto& panelManager = PanelManager::instance();
    if (toggle && panelManager.isOpenPanel(panelId)) {
      panelManager.closePanel();
      return;
    }

    float anchorX = sx;
    float anchorY = sy;
    if (platform != nullptr && instance.output != nullptr) {
      if (const auto* out = platform->findOutputByWl(instance.output); out != nullptr && out->hasUsableGeometry()) {
        const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(instance, *out);
        anchorX += surfaceX;
        anchorY += surfaceY;
      }
    }
    panelManager.openPanel(
        std::string(panelId),
        PanelOpenRequest{
            .output = instance.output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasAnchorPosition = true,
            .context = context,
            .sourceBarName = sourceBarName,
        }
    );
  }

  bool dispatchBarDeadZoneGesture(
      BarInstance& instance, noctalia::bar::Gesture gesture, float sx, float sy, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    const auto* action = instance.deadZoneBindings.find(gesture);
    if (action == nullptr) {
      return false;
    }

    if (action->kind == noctalia::bar::WidgetAction::Kind::Ipc && noctalia::bar::isAnchoredPanelVerb(action->verb)) {
      const auto args = noctalia::bar::parsePanelVerbArgs(action->args);
      if (args.panelId.empty()) {
        kLog.error(
            "bar.{}.dead_zone.actions.{}: \"{}\" needs a panel id", instance.barConfig.name, gestureConfigKey(gesture),
            action->verb
        );
        return false;
      }
      openPanelAtBarPointer(
          instance, sx, sy, platform, instance.barConfig.name, args.panelId, args.panelContext,
          action->verb == "panel-toggle"
      );
      return true;
    }

    // The dispatcher reports the command and the failure itself; repeating it here would double
    // every line on a held scroll.
    return dispatcher.run(*action, IpcInvocationContext{.barName = instance.barConfig.name, .output = instance.output});
  }

  bool handleBarDeadZoneButton(
      BarInstance& instance, float sx, float sy, std::uint32_t button, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }
    const auto gesture = noctalia::bar::gestureForButton(button);
    if (!gesture.has_value()) {
      return false;
    }
    return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
  }

  bool handleBarDeadZoneAxis(
      BarInstance& instance, float sx, float sy, const PointerEvent& event, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    // Routed through a scene-less InputArea for detent accumulation. Cycle actions fire once per
    // gesture; other actions fire for every step, matching a widget's `scroll_repeat = "auto"`.
    instance.deadZoneAxisSink.setOnAxisHandler([&](const InputArea::PointerData& data) {
      const auto gesture = noctalia::bar::gestureForScroll(data.axis, data.scrollSteps());
      if (!gesture.has_value()) {
        return false;
      }
      const auto* action = instance.deadZoneBindings.find(*gesture);
      if (!data.scrollStepStartsGesture() && action != nullptr && dispatcher.cycles(*action)) {
        return true;
      }
      return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
    });
    return instance.deadZoneAxisSink.dispatchAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
  }

  std::uint32_t positionToAnchor(const std::string& position) {
    if (position == "bottom") {
      return LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right;
    }
    if (position == "left") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    }
    if (position == "right") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    }
    // Default: top
    return LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right;
  }

  // Hover highlight: peak fill alpha of the widget-foreground tint, and the cross-axis inset
  // (logical px, content-scaled) of per-member pills inside capsule groups.
  constexpr float kWidgetHoverFillAlpha = 0.1F;
  constexpr float kGroupHoverCrossInset = 2.0F;

  // Sizes a run's hover overlays after the capsule geometry is final. Single runs (and plain
  // widgets) get one overlay covering the whole shell; group runs get one pill per member so
  // only the hovered member lights up.
  void placeCapsuleHoverBoxes(
      BarCapsuleRun& run, bool isVertical, float shellW, float shellH, float contentX, float contentY,
      float capsuleRadius, float widgetHoverPadding
  ) {
    if (run.hoverBoxes.empty()) {
      return;
    }
    if (run.container == nullptr) {
      Box* box = run.hoverBoxes.front();
      if (box == nullptr) {
        return;
      }
      box->setPosition(0.0F, 0.0F);
      box->setSize(shellW, shellH);
      box->setRadius(capsuleRadius);
      return;
    }
    const float scale = run.contentScale;
    const float crossInset = kGroupHoverCrossInset * scale;
    // Use the normal per-widget inset, independent of the shared capsule's configurable
    // padding, so grouped and standalone widgets receive the same hover treatment.
    const float mainInset = widgetHoverPadding * scale;
    const float shellMain = isVertical ? shellH : shellW;
    const float shellCross = isVertical ? shellW : shellH;
    const float contentMain = isVertical ? contentY : contentX;
    const float crossExtent = std::max(0.0F, shellCross - 2.0F * crossInset);
    for (std::size_t i = 0; i < run.widgets.size() && i < run.hoverBoxes.size(); ++i) {
      // outerNode(), not root(): the outer gesture area is what the run container lays out, so it
      // carries the member's position; root() sits at (0,0) inside it.
      Node* member = run.widgets[i] != nullptr ? run.widgets[i]->outerNode() : nullptr;
      Box* box = run.hoverBoxes[i];
      if (member == nullptr || box == nullptr) {
        continue;
      }
      // Skip hidden members — Flex leaves them at stale (0,0) geometry.
      if (!member->visible() || !member->participatesInLayout()) {
        box->setSize(0.0F, 0.0F);
        continue;
      }
      const float rootStart = contentMain + (isVertical ? member->y() : member->x());
      const float rootExtent = isVertical ? member->height() : member->width();
      const float mainStart = std::max(0.0F, rootStart - mainInset);
      const float mainExtent = std::max(0.0F, std::min(shellMain, rootStart + rootExtent + mainInset) - mainStart);
      if (isVertical) {
        box->setPosition(crossInset, mainStart);
        box->setSize(crossExtent, mainExtent);
      } else {
        box->setPosition(mainStart, crossInset);
        box->setSize(mainExtent, crossExtent);
      }
      const float pillRadius = std::max(0.0F, std::min(box->width(), box->height()) * 0.5F);
      box->setRadius(std::min(pillRadius, std::max(0.0F, capsuleRadius - crossInset)));
    }
  }

  struct CapsuleMemberGeometry {
    std::size_t widgetIndex = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float sliceStart = 0.0F;
    float sliceEnd = 0.0F;

    float mainStart(bool isVertical) const noexcept { return isVertical ? y : x; }
    float mainEnd(bool isVertical) const noexcept { return mainStart(isVertical) + (isVertical ? height : width); }
  };

  std::vector<CapsuleMemberGeometry>
  capsuleMemberSlices(const BarCapsuleRun& run, bool isVertical, float widgetHoverPadding) {
    std::vector<CapsuleMemberGeometry> members;
    if (run.shell == nullptr) {
      return members;
    }
    members.reserve(run.widgets.size());

    float shellX = 0.0F;
    float shellY = 0.0F;
    Node::absolutePosition(run.shell, shellX, shellY);
    for (std::size_t i = 0; i < run.widgets.size(); ++i) {
      Widget* widget = run.widgets[i];
      Node* root = widget != nullptr ? widget->outerNode() : nullptr;
      if (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible()) {
        continue;
      }

      float rootX = 0.0F;
      float rootY = 0.0F;
      Node::absolutePosition(root, rootX, rootY);
      members.push_back(
          CapsuleMemberGeometry{
              .widgetIndex = i,
              .x = rootX - shellX,
              .y = rootY - shellY,
              .width = root->width(),
              .height = root->height(),
          }
      );
    }

    const float shellMain = isVertical ? run.shell->height() : run.shell->width();
    for (std::size_t i = 0; i < members.size(); ++i) {
      auto& member = members[i];
      const float memberStart = member.mainStart(isVertical);
      const float memberEnd = member.mainEnd(isVertical);
      const float pad = widgetHoverPadding * run.widgets[member.widgetIndex]->contentScale();
      float before = pad;
      float after = pad;

      if (i > 0) {
        before = std::min(before, std::max(0.0F, (memberStart - members[i - 1].mainEnd(isVertical)) * 0.5F));
      }
      if (i + 1 < members.size()) {
        after = std::min(after, std::max(0.0F, (members[i + 1].mainStart(isVertical) - memberEnd) * 0.5F));
      }

      if (members.size() > 1) {
        if (i == 0) {
          before = after;
        }
        if (i + 1 == members.size()) {
          after = before;
        }
      }
      if (run.hasPaintedCapsuleBackground) {
        if (i == 0) {
          before = std::min(before, std::max(0.0F, memberStart));
        }
        if (i + 1 == members.size()) {
          after = std::min(after, std::max(0.0F, shellMain - memberEnd));
        }
      }
      member.sliceStart = memberStart - before;
      member.sliceEnd = memberEnd + after;
    }
    return members;
  }

  // Extends each member's hit target across the capsule padding and half the gap to its
  // neighbors, so hover/click coverage matches the capsule ink instead of stopping at the
  // content edge. Runs after applyBarWidgetHitTargets (which replaces outsets each layout).
  void extendCapsuleHitTargets(std::vector<BarCapsuleRun>& runs, bool isVertical, float widgetHoverPadding) {
    for (auto& run : runs) {
      if (run.shell == nullptr || run.hoverBoxes.empty()) {
        continue;
      }

      // Single runs (capsule or ghost pill): the hover overlay rect is the hit region.
      if (run.container == nullptr) {
        Widget* widget = !run.widgets.empty() ? run.widgets.front() : nullptr;
        Box* box = run.hoverBoxes.front();
        auto* area = widget != nullptr ? widget->outerNode() : nullptr;
        if (area == nullptr || box == nullptr || !area->visible() || !area->participatesInLayout()) {
          continue;
        }
        const float areaStart = isVertical ? area->y() : area->x();
        const float areaEnd = areaStart + (isVertical ? area->height() : area->width());
        const float boxStart = isVertical ? box->y() : box->x();
        const float boxEnd = boxStart + (isVertical ? box->height() : box->width());
        auto outset = area->hitTestOutset();
        if (isVertical) {
          outset.top += std::max(0.0F, areaStart - boxStart);
          outset.bottom += std::max(0.0F, boxEnd - areaEnd);
        } else {
          outset.left += std::max(0.0F, areaStart - boxStart);
          outset.right += std::max(0.0F, boxEnd - areaEnd);
        }
        area->setHitTestOutset(outset);
        continue;
      }

      // Tile only laid-out members; hidden ones keep stale geometry, and members clipped out of a
      // collapsed accordion are input-suppressed so their slice belongs to the visible member.
      const auto laidOut = capsuleMemberSlices(run, isVertical, widgetHoverPadding);
      const float shellMain = isVertical ? run.shell->height() : run.shell->width();
      float minSliceStart = 0.0F;
      float maxSliceEnd = shellMain;
      for (const auto& member : laidOut) {
        const std::size_t i = member.widgetIndex;
        Node* area = run.widgets[i]->outerNode();
        const float memberStart = member.mainStart(isVertical);
        const float memberEnd = member.mainEnd(isVertical);
        const float sliceStart = member.sliceStart;
        const float sliceEnd = member.sliceEnd;
        minSliceStart = std::min(minSliceStart, sliceStart);
        maxSliceEnd = std::max(maxSliceEnd, sliceEnd);

        auto outset = area->hitTestOutset();
        const float before = std::max(0.0F, memberStart - sliceStart);
        const float after = std::max(0.0F, sliceEnd - memberEnd);
        if (isVertical) {
          outset.top += before;
          outset.bottom += after;
        } else {
          outset.left += before;
          outset.right += after;
        }
        area->setHitTestOutset(outset);
      }
      auto shellOutset = run.shell->hitTestOutset();
      if (isVertical) {
        shellOutset.top += std::max(0.0F, -minSliceStart);
        shellOutset.bottom += std::max(0.0F, maxSliceEnd - shellMain);
      } else {
        shellOutset.left += std::max(0.0F, -minSliceStart);
        shellOutset.right += std::max(0.0F, maxSliceEnd - shellMain);
      }
      run.shell->setHitTestOutset(shellOutset);
    }
  }

  struct BarVisualGeometry {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };

  struct BarSurfaceSpec {
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::int32_t exclusiveZone = 0;
  };

  BarSurfaceSpec computeBarSurfaceSpec(const BarConfig& barConfig, const ShadowStyle& shadowConfig) {
    const bool vertical = (barConfig.position == "left" || barConfig.position == "right");
    const bool isBottom = barConfig.position == "bottom";
    const bool isRight = barConfig.position == "right";
    const std::int32_t mEnds = barConfig.marginEnds.px();
    const std::int32_t mEdge = barConfig.marginEdge;
    const auto sb = surface_shadow::bleed(barConfig.shadow, shadowConfig);
    const int edgeGutter = barAutoHideEdgeGutter(barConfig);
    const auto concave = barConcaveShape(barConfig);
    const int insetL = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.left)));
    const int insetT = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.top)));
    const int insetR = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.right)));
    const int insetB = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.bottom)));
    // Reserve room for a concave-corner spike on the inner edge (opaque bar material),
    // in addition to the shadow bleed that renders beyond the spike tips.
    const int concaveBulge = static_cast<int>(std::lround(concave.innerBulge));

    const std::int32_t edgeMargin = barEdgeLayerMargin(barConfig, shadowConfig);

    BarSurfaceSpec spec;
    if (!vertical) {
      spec.marginLeft = std::max(0, mEnds - sb.left - insetL);
      spec.marginRight = std::max(0, mEnds - sb.right - insetR);
      if (isBottom) {
        if (edgeGutter > 0) {
          // Surface reaches the screen edge (no layer margin); the margin is folded
          // into the surface as a gutter on the edge side. Do not add the edge-side
          // bleed here — it lives inside the gutter, not beyond it.
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginBottom = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + std::min(mEdge, sb.down));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.down + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginTop = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(std::min(mEdge, sb.up) + barConfig.thickness + sb.down + concaveBulge);
        }
      }
    } else {
      spec.marginTop = std::max(0, mEnds - sb.up - insetT);
      spec.marginBottom = std::max(0, mEnds - sb.down - insetB);
      if (isRight) {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginRight = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + std::min(mEdge, sb.right));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.right + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginLeft = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(std::min(mEdge, sb.left) + barConfig.thickness + sb.right + concaveBulge);
        }
      }
    }

    spec.exclusiveZone = barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    return spec;
  }

  // Compute surface spec for Dynamic Notch mode.
  // The LayerSurface is sized to the FIXED expanded bounding box at startup and
  // never resized during hover/expand/collapse animations (avoids Wayland configure
  // races on Niri that clip rounded corners into sharp rectangles).
  // The pill is rendered dynamically inside this fixed surface.
  BarSurfaceSpec computeNotchSurfaceSpec(const BarConfig& barConfig, const ShadowStyle& shadowConfig, bool /*expanded*/) {
    const auto sb = surface_shadow::bleed(barConfig.shadow, shadowConfig);

    // Fixed surface = expanded pill bounds + shadow padding on all sides.
    // Surface width/height are constant; pill position/size animate inside.
    // Enforce minimum 860×340 even if config has stale smaller values.
    const float expandedW = std::max(static_cast<float>(barConfig.notchExpandedWidth), 860.0F);
    const float expandedH = std::max(static_cast<float>(barConfig.notchExpandedHeight), 340.0F);

    BarSurfaceSpec spec;
    spec.marginTop = barConfig.notchTopMargin;
    spec.marginLeft = 0;
    spec.marginRight = 0;
    spec.marginBottom = 0;

    spec.surfaceWidth = static_cast<std::uint32_t>(expandedW + static_cast<float>(sb.left) + static_cast<float>(sb.right));
    spec.surfaceHeight = static_cast<std::uint32_t>(expandedH + static_cast<float>(sb.up) + static_cast<float>(sb.down) + static_cast<float>(barConfig.notchTopMargin));

    // Exclusive zone = 0 so windows extend to top edge; notch floats on top (LAYER_TOP).
    spec.exclusiveZone = 0;

    return spec;
  }

  float
  barInnerSurfaceExtension(const BarConfig& cfg, const ShadowStyle& shadow, float surfaceWidth, float surfaceHeight) {
    const auto base = computeBarSurfaceSpec(cfg, shadow);
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const float current = isVertical ? surfaceWidth : surfaceHeight;
    const auto normal = static_cast<float>(isVertical ? base.surfaceWidth : base.surfaceHeight);
    return std::max(0.0F, current - normal);
  }

  // Returns true when two bar configs would produce an identical layer-shell
  // surface (same anchor, size, exclusive zone, namespace). When true, an
  // existing BarInstance can be retained on reload and only its widget tree
  // rebuilt — avoiding the screen-shift caused by destroying and recreating
  // the exclusive zone.
  bool barConfigSurfaceFieldsEqual(
      const BarConfig& a, const BarConfig& b, const ShadowStyle& previousShadow, const ShadowStyle& nextShadow
  ) {
    const bool sameShadowSurface =
        (!a.shadow && !b.shadow) || surface_shadow::sameSurfaceMetrics(previousShadow, nextShadow);
    return a.name == b.name
        && a.position == b.position
        && a.enabled == b.enabled
        && a.autoHide == b.autoHide
        && a.smartAutoHide == b.smartAutoHide
        && a.reserveSpace == b.reserveSpace
        && a.layer == b.layer
        && a.thickness == b.thickness
        // Corner radii feed the concave-corner bulge, which changes the surface size.
        && a.radiusTopLeft == b.radiusTopLeft
        && a.radiusTopRight == b.radiusTopRight
        && a.radiusBottomLeft == b.radiusBottomLeft
        && a.radiusBottomRight == b.radiusBottomRight
        && a.concaveEdgeCorners == b.concaveEdgeCorners
        && a.marginEnds == b.marginEnds
        && a.marginEdge == b.marginEdge
        && a.shadow == b.shadow
        && sameShadowSurface
        && a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it = std::ranges::find(next, oldBar.name, &BarConfig::name);
      if (it != next.end()) {
        preserved.push_back(oldBar.name);
      }
    }

    if (preserved.size() > next.size()) {
      return true;
    }
    for (std::size_t i = 0; i < preserved.size(); ++i) {
      if (next[i].name != preserved[i]) {
        return true;
      }
    }
    return false;
  }

  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShadowStyle& shadow, float surfaceWidth, float surfaceHeight,
      float innerSurfaceExtension = 0.0F
  ) {
    const auto barThickness = static_cast<float>(cfg.thickness);
    const auto marginEnds = static_cast<float>(cfg.marginEnds.px());
    const auto marginEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const auto sbi = surface_shadow::bleed(cfg.shadow, shadow);
    const auto concave = barConcaveShape(cfg);
    const float insetL = std::ceil(std::max(0.0F, concave.logicalInset.left));
    const float insetT = std::ceil(std::max(0.0F, concave.logicalInset.top));
    const float insetR = std::ceil(std::max(0.0F, concave.logicalInset.right));
    const float insetB = std::ceil(std::max(0.0F, concave.logicalInset.bottom));
    const auto bleedLeft = static_cast<float>(sbi.left);
    const auto bleedRight = static_cast<float>(sbi.right);
    const auto bleedUp = static_cast<float>(sbi.up);
    const auto bleedDown = static_cast<float>(sbi.down);
    // For bottom/right bars the inner edge is the origin side, so the concave spike
    // pushes the body inward by its bulge. Top/left bars grow away from the origin
    // and need no body shift. Gutter (auto-hide) placements derive from the surface
    // size, which already includes the bulge, so they shift automatically.
    const float concaveBulge = concave.innerBulge;

    if (isVertical) {
      // Vertical bar: edge gap is left/right, ends inset is top/bottom.
      const float y = std::min(marginEnds, bleedUp) + insetT;
      float x = isRight ? (bleedLeft + concaveBulge) : std::min(marginEdge, bleedLeft);
      if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
        // The gutter equals marginEdge and sits between the screen edge and the bar.
        // Position the bar exactly marginEdge from the edge so it matches the
        // non-auto-hide placement; the edge-side shadow bleeds into the gutter.
        if (isRight) {
          x = surfaceWidth - static_cast<float>(gutter) - barThickness;
        } else {
          x = static_cast<float>(gutter);
        }
      } else if (isRight) {
        x += innerSurfaceExtension;
      }
      return {
          .x = x,
          .y = y,
          .width = barThickness,
          .height = surfaceHeight - y - std::min(marginEnds, bleedDown) - insetB,
      };
    }

    // Horizontal bar: edge gap is top/bottom, ends inset is left/right.
    const float x = std::min(marginEnds, bleedLeft) + insetL;
    float y = isBottom ? (bleedUp + concaveBulge) : std::min(marginEdge, bleedUp);
    if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isBottom) {
        y = surfaceHeight - static_cast<float>(gutter) - barThickness;
      } else {
        y = static_cast<float>(gutter);
      }
    } else if (isBottom) {
      y += innerSurfaceExtension;
    }
    return {
        .x = x,
        .y = y,
        .width = surfaceWidth - x - std::min(marginEnds, bleedRight) - insetR,
        .height = barThickness,
    };
  }

  InputRect barContentInputRegion(const BarConfig& cfg, const ShadowStyle& shadow, int surfW, int surfH) {
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH));
    const auto barVisual = computeBarVisualGeometry(
        cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH), innerSurfaceExtension
    );
    return InputRect{
        static_cast<int>(barVisual.x), static_cast<int>(barVisual.y), static_cast<int>(barVisual.width),
        static_cast<int>(barVisual.height)
    };
  }

  // Click-through dead zone: one rect per interactive widget, spanning the bar's thickness and padded
  // along the bar by the capsule padding that widens widget hit targets past their bounds. Spacers and
  // passive widgets are dead zone (see widgetAtPoint), so they pass through too.
  std::vector<InputRect> barWidgetsInputRegion(const BarInstance& instance, const InputRect& bar) {
    const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
    const float pad = instance.barConfig.widgetCapsulePadding;
    std::vector<InputRect> rects;
    for (const auto* widgets : {&instance.startWidgets, &instance.centerWidgets, &instance.endWidgets}) {
      for (const auto& widget : *widgets) {
        const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
        if (node == nullptr
            || widget->isBarClickThrough()
            || !node->visible()
            || node->width() <= 0.0F
            || node->height() <= 0.0F) {
          continue;
        }
        float x = 0.0F;
        float y = 0.0F;
        Node::absolutePosition(node, x, y);
        if (isVertical) {
          const int top = std::max(bar.y, static_cast<int>(std::floor(y - pad)));
          const int bottom = std::min(bar.y + bar.height, static_cast<int>(std::ceil(y + node->height() + pad)));
          if (bottom > top) {
            rects.push_back(InputRect{bar.x, top, bar.width, bottom - top});
          }
        } else {
          const int left = std::max(bar.x, static_cast<int>(std::floor(x - pad)));
          const int right = std::min(bar.x + bar.width, static_cast<int>(std::ceil(x + node->width() + pad)));
          if (right > left) {
            rects.push_back(InputRect{left, bar.y, right - left, bar.height});
          }
        }
      }
    }
    return rects;
  }

  std::pair<float, float> computeAutoHideHiddenDelta(
      bool isVertical, bool isBottom, bool isRight, float w, float h, float contentLeft, float contentTop,
      float contentRight, float contentBottom
  ) {
    const float k = kAutoHideSlideExtraPx;
    if (!isVertical) {
      if (isBottom) {
        return {0.0F, (h - contentTop) + k};
      }
      return {0.0F, -(contentBottom + k)};
    }
    if (isRight) {
      return {(w - contentLeft) + k, 0.0F};
    }
    return {-(contentRight + k), 0.0F};
  }

  void
  applyBarShadowStyle(BarInstance& instance, const ShadowStyle& shadowConfig, float surfaceWidth, float surfaceHeight) {
    if (instance.shadow == nullptr) {
      return;
    }

    const auto concave = barConcaveShape(instance.barConfig);
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight);
    const auto barVisual =
        computeBarVisualGeometry(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight, innerSurfaceExtension);
    // Shadow follows the same shape as the background: the body expanded outward by
    // the concave inset into the visual rect, so concave spikes cast a matching shadow.
    const float barAreaW = barVisual.width + concave.logicalInset.left + concave.logicalInset.right;
    const float barAreaH = barVisual.height + concave.logicalInset.top + concave.logicalInset.bottom;
    const float bgOpacity = std::clamp(instance.barConfig.backgroundOpacity, 0.0F, 1.0F);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const float shadowX = barVisual.x - concave.logicalInset.left + static_cast<float>(shadowOff.x);
    const float shadowY = barVisual.y - concave.logicalInset.top + static_cast<float>(shadowOff.y);
    RoundedRectStyle shadowStyle = surface_shadow::style(
        shadowConfig, bgOpacity,
        surface_shadow::Shape{.corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii}
    );

    const bool panelShadowExclusion = instance.attachedPanelGeometry.has_value()
        && instance.attachedPanelGeometry->width > 0.0F
        && instance.attachedPanelGeometry->height > 0.0F;
    if (panelShadowExclusion) {
      const auto& attached = *instance.attachedPanelGeometry;
      const float convexRadius = std::max(0.0F, attached.cornerRadius);
      const float bulgeRadius = std::max(0.0F, attached.bulgeRadius);
      const std::string_view barPosition = instance.barConfig.position;
      const auto corners = attached_panel::cornerShapes(barPosition);
      const auto pickRadius = [&](CornerShape shape) {
        return shape == CornerShape::Concave ? bulgeRadius : convexRadius;
      };
      shadowStyle.shadowExclusion = true;
      shadowStyle.shadowExclusionOffsetX = shadowX - attached.x;
      shadowStyle.shadowExclusionOffsetY = shadowY - attached.y;
      shadowStyle.shadowExclusionWidth = attached.width;
      shadowStyle.shadowExclusionHeight = attached.height;
      shadowStyle.shadowExclusionCorners = corners;
      shadowStyle.shadowExclusionLogicalInset = attached_panel::logicalInset(barPosition, bulgeRadius);
      shadowStyle.shadowExclusionRadius =
          Radii{pickRadius(corners.tl), pickRadius(corners.tr), pickRadius(corners.br), pickRadius(corners.bl)};
    }

    auto configureShadow = [&](Box* node, float x, float y) {
      if (node == nullptr) {
        return;
      }
      node->setStyle(shadowStyle);
      node->setZIndex(-1);
      node->setPosition(x, y);
      node->setSize(barAreaW, barAreaH);
    };

    instance.shadow->setHitTestVisible(false);
    instance.shadow->setVisible(true);
    configureShadow(instance.shadow, shadowX, shadowY);

    if (instance.shadowLeftClip != nullptr) {
      instance.shadowLeftClip->setVisible(false);
    }
    if (instance.shadowRightClip != nullptr) {
      instance.shadowRightClip->setVisible(false);
    }
  }

  void layoutBarSections(
      BarInstance& instance, Renderer& renderer, float barAreaW, float barAreaH, float padding, bool isVertical
  ) {
    const float slotCross = isVertical ? barAreaW : barAreaH;

    // Capsule cross-size is a fraction of the bar thickness (capsule_thickness), the same for every capsule
    // regardless of per-widget content scale. The max() guard keeps a thin bar from yielding a 0px capsule.
    const float capsuleCross = std::max(1.0F, std::round(slotCross * instance.barConfig.capsuleThickness));

    auto layoutWidgets = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
          tray->setCapsuleCross(capsuleCross);
        }
        if (widget->root() != nullptr) {
          widget->layout(renderer, barAreaW, barAreaH);
        }
      }
    };
    layoutWidgets(instance.startWidgets);
    layoutWidgets(instance.centerWidgets);
    layoutWidgets(instance.endWidgets);

    auto finalizeCapsules = [isVertical, capsuleCross, widgetHoverPadding = instance.barConfig.widgetCapsulePadding,
                             &renderer](std::vector<BarCapsuleRun>& runs) {
      for (auto& run : runs) {
        Node* shell = run.shell;
        Box* bg = run.bg;
        Node* content = run.content;
        if (shell == nullptr || bg == nullptr || content == nullptr) {
          continue;
        }
        if (run.container != nullptr) {
          run.container->layout(renderer);
        }

        bool hasVisibleContent = false;
        bool hasCapsuleContent = false;
        for (Widget* widget : run.widgets) {
          if (widget == nullptr || widget->root() == nullptr) {
            continue;
          }
          hasVisibleContent = hasVisibleContent || widget->root()->visible();
          hasCapsuleContent = hasCapsuleContent || widget->shouldShowBarCapsule();
        }
        const bool hasPaintedFill = resolveColorSpec(scaleAlpha(run.spec.fill, run.spec.opacity)).a > 0.0F;
        const bool hasPaintedBorder =
            run.spec.border.has_value() && run.spec.borderWidth > 0.0F && resolveColorSpec(*run.spec.border).a > 0.0F;
        run.hasPaintedCapsuleBackground = hasCapsuleContent && (hasPaintedFill || hasPaintedBorder);

        shell->setVisible(hasVisibleContent);
        const float scale = run.contentScale;
        const float iw = content->width();
        const float ih = content->height();
        auto memberMainPos = [isVertical](const Node* node) { return isVertical ? node->y() : node->x(); };
        auto memberMainExtent = [isVertical](const Node* node) { return isVertical ? node->height() : node->width(); };
        auto clearAccordionSuppression = [&run] {
          for (Widget* widget : run.widgets) {
            if (widget != nullptr) {
              widget->setBarPointerSuppressed(false);
            }
          }
        };
        if (!hasCapsuleContent) {
          shell->setSize(iw, ih);
          if (run.accordionClip != nullptr) {
            run.accordionClip->setPosition(0.0F, 0.0F);
            run.accordionClip->setSize(iw, ih);
          }
          content->setPosition(0.0F, 0.0F);
          bg->setVisible(false);
          bg->setPosition(0.0F, 0.0F);
          bg->setSize(iw, ih);
          placeCapsuleHoverBoxes(run, isVertical, iw, ih, 0.0F, 0.0F, std::min(iw, ih) * 0.5F, widgetHoverPadding);
          // No pill to hover: the group renders full-size, so nothing may stay input-suppressed.
          if (run.accordion) {
            clearAccordionSuppression();
          }
          continue;
        }
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        const float fullMain = isVertical ? ih : iw;
        // Collapsed accordion: the shell only spans its always-visible member; the rest are clipped
        // out and revealed as the hover progress lerps the main extent up to the full content size.
        const bool accordionStartDir = run.accordion && run.accordionDirection == BarAccordionDirection::Start;
        float revealMain = fullMain;
        if (run.accordion) {
          const Node* visibleMember = nullptr;
          auto laidOut = [](const Widget* widget) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            return node != nullptr && node->visible() && node->participatesInLayout() ? node : nullptr;
          };
          if (run.accordionVisibleIndex < run.widgets.size()) {
            visibleMember = laidOut(run.widgets[run.accordionVisibleIndex]);
          }
          if (visibleMember == nullptr) {
            // The configured first member is hidden: anchor on the laid-out member at the pill's fixed edge.
            for (Widget* widget : run.widgets) {
              if (const Node* node = laidOut(widget); node != nullptr) {
                visibleMember = node;
                if (!accordionStartDir) {
                  break;
                }
              }
            }
          }
          if (visibleMember != nullptr) {
            const float visMain = memberMainExtent(visibleMember);
            const float progress = std::clamp(run.accordionProgress, 0.0F, 1.0F);
            revealMain = visMain + (fullMain - visMain) * progress;
          }
          // Match Flex's logical-pixel positions so right-anchored siblings keep a stable rounding phase.
          revealMain = std::ceil(revealMain);
        }
        // Cross-size is the fixed capsuleCross, independent of per-widget content scale: scaling a widget
        // enlarges its glyph/text inside the fixed-height pill rather than resizing the capsule (so a
        // differently scaled member can't grow or split its capsule group). The main axis is content plus
        // per-widget padding, so an icon-only widget reads as a near-circular pill at the default padding
        // and widens as padding increases.
        const float shellMain = revealMain + 2.0F * padMain;
        const float shellCross = capsuleCross;
        const float shellW = isVertical ? shellCross : shellMain;
        const float shellH = isVertical ? shellMain : shellCross;
        // Start-direction accordions pin the content's end edge, so the always-visible last member
        // stays put while the hidden ones unfold off the pill's leading edge.
        const float contentMain = accordionStartDir ? shellMain - padMain - fullMain : padMain;
        const float contentX = isVertical ? (shellW - iw) * 0.5F : contentMain;
        const float contentY = isVertical ? contentMain : (shellH - ih) * 0.5F;
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0F, 0.0F);
        bg->setSize(shellW, shellH);
        if (run.accordionClip != nullptr) {
          // Reveal window: [padMain, padMain + revealMain] in shell coordinates, for both directions.
          if (isVertical) {
            run.accordionClip->setPosition(0.0F, padMain);
            run.accordionClip->setSize(shellCross, revealMain);
          } else {
            run.accordionClip->setPosition(padMain, 0.0F);
            run.accordionClip->setSize(revealMain, shellCross);
          }
          content->setPosition(contentX - (isVertical ? 0.0F : padMain), contentY - (isVertical ? padMain : 0.0F));
        } else {
          content->setPosition(contentX, contentY);
        }
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        const float capsuleRadius = radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                                            : std::max(0.0F, std::min(shellW, shellH) * 0.5F);
        bg->setRadius(capsuleRadius);
        if (run.container == nullptr) {
          placeCapsuleHoverBoxes(
              run, isVertical, shellW, shellH, contentX, contentY, capsuleRadius, widgetHoverPadding
          );
        }
        // Members outside the reveal window are clipped out; while collapsed (or collapsing) the
        // non-primary members are pointer-suppressed by reveal window so their hidden slots don't
        // capture clicks. While expanded, every valid layout member stays unsuppressed so hover
        // tracking and member interactions remain active across the reveal animation.
        if (run.accordion) {
          for (Widget* widget : run.widgets) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            if (widget == nullptr || node == nullptr || !node->visible() || !node->participatesInLayout()) {
              if (widget != nullptr) {
                widget->setBarPointerSuppressed(false);
              }
              continue;
            }
            if (run.accordionExpanded) {
              widget->setBarPointerSuppressed(false);
            } else {
              const float memberStart = contentMain + memberMainPos(node);
              const float memberEnd = memberStart + memberMainExtent(node);
              const bool memberFullyVisible = memberStart >= padMain - 0.5F && memberEnd <= padMain + revealMain + 0.5F;
              widget->setBarPointerSuppressed(!memberFullyVisible);
            }
          }
        }
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    // When bar touches screen edge, put the padding inside the sections, and extend the hit targets of
    // the first/last widgets to cover the area. So clicking on the screen edge still triggers the widget.
    const bool screenEdgeClick = instance.barConfig.marginEnds.px() == 0 && padding > 0;
    const float paddingInsideSection = screenEdgeClick ? padding : 0.0F;
    const float contentMainStart = screenEdgeClick ? 0.0F : padding;
    const float contentMainEnd =
        std::max(contentMainStart, (isVertical ? barAreaH : barAreaW) - (screenEdgeClick ? 0.0F : padding));
    const float contentMainSpan = std::max(0.0F, contentMainEnd - contentMainStart);

    auto configureSlot = [&](Node* slot, float mainOffset, float mainSize) {
      slot->setClipChildren(true);
      if (isVertical) {
        slot->setPosition(0.0F, mainOffset);
        slot->setSize(slotCross, mainSize);
      } else {
        slot->setPosition(mainOffset, 0.0F);
        slot->setSize(mainSize, slotCross);
      }
    };

    auto configureSection = [&](Flex* section, FlexJustify justify) {
      section->setJustify(justify);
      section->layout(renderer);
    };

    auto laneExpands = [](const Flex* section) {
      return std::ranges::any_of(section->children(), [](const auto& child) { return child->flexGrow() > 0.0F; });
    };

    // Flex only hands out free space once the lane's main size is known, so an expanding lane is laid out again
    // at its slot size. Not when its content overflows the slot: the natural size keeps the overflow clipping
    // from the far end, which is what keeps the end lane's last widgets reachable.
    auto stretchLaneToSlot = [&](Flex* section, float slotMain) {
      const float naturalMain = isVertical ? section->height() : section->width();
      if (!laneExpands(section) || slotMain <= naturalMain) {
        return;
      }
      section->arrange(
          renderer,
          isVertical ? LayoutRect{.width = section->width(), .height = slotMain}
                     : LayoutRect{.width = slotMain, .height = section->height()}
      );
    };

    if (screenEdgeClick) {
      if (isVertical) {
        instance.startSection->setPadding(paddingInsideSection, 0.0F, 0.0F, 0.0F);
        instance.endSection->setPadding(0.0F, 0.0F, paddingInsideSection, 0.0F);
      } else {
        instance.startSection->setPadding(0.0F, 0.0F, 0.0F, paddingInsideSection);
        instance.endSection->setPadding(0.0F, paddingInsideSection, 0.0F, 0.0F);
      }
    } else {
      instance.startSection->setPadding(0.0F);
      instance.endSection->setPadding(0.0F);
    }

    configureSection(instance.startSection, FlexJustify::Start);
    configureSection(instance.centerSection, FlexJustify::Center);
    configureSection(instance.endSection, FlexJustify::End);

    // Anchor mode: if a center widget is flagged as the anchor, pin its center to the
    // bar midline so surrounding siblings growing/shrinking cannot drift it sideways.
    const Node* anchorNode = nullptr;
    for (const auto& widget : instance.centerWidgets) {
      if (widget != nullptr && widget->isAnchor() && widget->layoutBoundsNode() != nullptr) {
        anchorNode = widget->layoutBoundsNode();
        break;
      }
    }

    const float barMidline = contentMainStart + contentMainSpan * 0.5F;
    const float centerNaturalMain = isVertical ? instance.centerSection->height() : instance.centerSection->width();

    // Sections sit on whole pixels; a fractional origin blurs every widget in the section.
    float centerSlotStart;
    float centerSlotMain;
    float centerSectionOffset; // offset of section origin within its slot along main axis
    if (anchorNode != nullptr) {
      const float anchorOffsetInSection = isVertical ? anchorNode->y() : anchorNode->x();
      const float anchorSpan = isVertical ? anchorNode->height() : anchorNode->width();
      const float anchorCenterInSection = anchorOffsetInSection + anchorSpan * 0.5F;
      // Place the section so that the anchor's center sits at barMidline.
      float desiredSectionStart = barMidline - anchorCenterInSection;
      // Clamp so the section stays within the content area.
      const float maxStart = contentMainEnd - centerNaturalMain;
      desiredSectionStart = std::clamp(
          std::round(desiredSectionStart), contentMainStart, std::max(contentMainStart, std::floor(maxStart))
      );
      centerSlotStart = desiredSectionStart;
      centerSlotMain = std::min(centerNaturalMain, contentMainEnd - centerSlotStart);
      centerSectionOffset = 0.0F;
    } else {
      centerSlotMain = std::min(contentMainSpan, centerNaturalMain);
      centerSlotStart = contentMainStart + std::max(0.0F, std::round((contentMainSpan - centerSlotMain) * 0.5F));
      centerSectionOffset = std::round((centerSlotMain - centerNaturalMain) * 0.5F);
    }
    // Whole pixels for the end slot too: sections can be fractionally wide at bar scales like 1.05.
    const float centerSlotEnd = std::ceil(centerSlotStart + centerSlotMain);
    float startSlotMain;
    float endSlotMain;
    if (!instance.centerWidgets.empty()) {
      startSlotMain = std::max(0.0F, centerSlotStart - contentMainStart);
      endSlotMain = std::max(0.0F, contentMainEnd - centerSlotEnd);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
      configureSlot(instance.endSlot, centerSlotEnd, endSlotMain);
    } else {
      // Allow start/end sections to take the full width if center is empty
      const float startNaturalMain = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNaturalMain = isVertical ? instance.endSection->height() : instance.endSection->width();

      // Prioritize end section, because control center is likely to be there, and we don't
      // want it to be clipped by a super long start section, so the user loses access to settings.
      endSlotMain = std::min(std::ceil(endNaturalMain), contentMainSpan);
      startSlotMain = std::min(startNaturalMain, contentMainSpan - endSlotMain);
      // With no center lane, expanding lanes split what the two leave free.
      const bool startExpands = laneExpands(instance.startSection);
      const bool endExpands = laneExpands(instance.endSection);
      if (startExpands || endExpands) {
        const float spare = std::max(0.0F, contentMainSpan - startSlotMain - endSlotMain);
        const float share = startExpands && endExpands ? std::floor(spare * 0.5F) : spare;
        startSlotMain += startExpands ? share : 0.0F;
        endSlotMain += endExpands ? share : 0.0F;
      }
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, contentMainStart + startSlotMain, 0.0F);
      configureSlot(instance.endSlot, contentMainEnd - endSlotMain, endSlotMain);
    }

    stretchLaneToSlot(instance.startSection, startSlotMain);
    stretchLaneToSlot(instance.endSection, endSlotMain);

    if (isVertical) {
      instance.startSection->setPosition(std::round((slotCross - instance.startSection->width()) * 0.5F), 0.0F);
      instance.centerSection->setPosition(
          std::round((slotCross - instance.centerSection->width()) * 0.5F), centerSectionOffset
      );
      instance.endSection->setPosition(
          std::round((slotCross - instance.endSection->width()) * 0.5F),
          std::floor(endSlotMain - instance.endSection->height())
      );
    } else {
      instance.startSection->setPosition(0.0F, std::round((slotCross - instance.startSection->height()) * 0.5F));
      instance.centerSection->setPosition(
          centerSectionOffset, std::round((slotCross - instance.centerSection->height()) * 0.5F)
      );
      instance.endSection->setPosition(
          std::floor(endSlotMain - instance.endSection->width()),
          std::round((slotCross - instance.endSection->height()) * 0.5F)
      );
    }

    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.centerSection, instance.centerSlot, isVertical);
    applyBarWidgetHitTargets(instance.endSection, instance.endSlot, isVertical);
    extendCapsuleHitTargets(instance.startCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.centerCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.endCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);

    // Ghost pills for capsule-less widgets: positioned on the bar-level underlay with the
    // metrics an enabled capsule would have (capsuleCross across the bar, capsule padding
    // along it). Runs after sections are positioned so absolute coordinates are final; the
    // widget's hit target is widened to match the pill.
    if (instance.hoverUnderlay != nullptr) {
      float underlayX = 0.0F;
      float underlayY = 0.0F;
      Node::absolutePosition(instance.hoverUnderlay, underlayX, underlayY);
      auto placeGhostPills = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& widget : widgets) {
          Box* box = widget->barHoverBox();
          Node* root = widget->outerNode();
          if (box == nullptr || root == nullptr || widget->barCapsuleShell() != nullptr) {
            continue;
          }
          if (!root->visible() || !root->participatesInLayout()) {
            box->setSize(0.0F, 0.0F);
            continue;
          }
          float rootX = 0.0F;
          float rootY = 0.0F;
          Node::absolutePosition(root, rootX, rootY);
          rootX -= underlayX;
          rootY -= underlayY;
          const float mainExtent = isVertical ? root->height() : root->width();
          const float pad = mainExtent > 0.5F ? widget->barCapsuleSpec().padding * widget->contentScale() : 0.0F;
          const float hoverW = isVertical ? capsuleCross : root->width() + 2.0F * pad;
          const float hoverH = isVertical ? root->height() + 2.0F * pad : capsuleCross;
          box->setPosition(
              isVertical ? rootX + (root->width() - capsuleCross) * 0.5F : rootX - pad,
              isVertical ? rootY - pad : rootY + (root->height() - capsuleCross) * 0.5F
          );
          box->setSize(hoverW, hoverH);
          box->setRadius(widget->resolvedBarCapsuleRadius(hoverW, hoverH));
          if (auto* area = dynamic_cast<InputArea*>(root)) {
            auto outset = area->hitTestOutset();
            if (isVertical) {
              outset.top += pad;
              outset.bottom += pad;
            } else {
              outset.left += pad;
              outset.right += pad;
            }
            area->setHitTestOutset(outset);
          }
        }
      };
      placeGhostPills(instance.startWidgets);
      placeGhostPills(instance.centerWidgets);
      placeGhostPills(instance.endWidgets);

      auto placeGroupHoverPills = [&](std::vector<BarCapsuleRun>& runs) {
        for (auto& run : runs) {
          if (run.container == nullptr || run.shell == nullptr) {
            continue;
          }
          float shellX = 0.0F;
          float shellY = 0.0F;
          Node::absolutePosition(run.shell, shellX, shellY);
          shellX -= underlayX;
          shellY -= underlayY;
          const float shellMainStart = isVertical ? shellY : shellX;
          const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
          for (Widget* widget : run.widgets) {
            Node* root = widget != nullptr ? widget->outerNode() : nullptr;
            Box* box = widget != nullptr ? widget->barHoverBox() : nullptr;
            if (box != nullptr
                && (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible())) {
              box->setSize(0.0F, 0.0F);
            }
          }

          const auto laidOut = capsuleMemberSlices(run, isVertical, instance.barConfig.widgetCapsulePadding);
          for (const auto& member : laidOut) {
            Widget* widget = run.widgets[member.widgetIndex];
            Box* box = widget->barHoverBox();
            if (box == nullptr) {
              continue;
            }

            const float rootX = shellX + member.x;
            const float rootY = shellY + member.y;
            const float mainStart = shellMainStart + member.sliceStart;
            const float mainEnd = shellMainStart + member.sliceEnd;
            const float mainExtent = std::max(0.0F, mainEnd - mainStart);
            const float hoverW = isVertical ? capsuleCross : mainExtent;
            const float hoverH = isVertical ? mainExtent : capsuleCross;
            box->setPosition(
                isVertical ? rootX + (member.width - capsuleCross) * 0.5F : mainStart,
                isVertical ? mainStart : rootY + (member.height - capsuleCross) * 0.5F
            );
            box->setSize(hoverW, hoverH);
            box->setRadius(
                radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(hoverW, hoverH)
                                        : widget->resolvedBarCapsuleRadius(hoverW, hoverH)
            );
          }
        }
      };
      placeGroupHoverPills(instance.startCapsuleRuns);
      placeGroupHoverPills(instance.centerCapsuleRuns);
      placeGroupHoverPills(instance.endCapsuleRuns);
    }
    if (screenEdgeClick) {
      auto extendHitTestOutsetToScreenEdge = [&](Node* node, bool start) {
        if (node == nullptr) {
          return;
        }
        auto hitTestOutset = node->hitTestOutset();
        if (start) {
          if (isVertical) {
            hitTestOutset.top += paddingInsideSection;
          } else {
            hitTestOutset.left += paddingInsideSection;
          }
        } else {
          if (isVertical) {
            hitTestOutset.bottom += paddingInsideSection;
          } else {
            hitTestOutset.right += paddingInsideSection;
          }
        }
        node->setHitTestOutset(hitTestOutset);
      };
      if (!instance.startWidgets.empty()) {
        auto* widget = instance.startWidgets.front().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), true);
        extendHitTestOutsetToScreenEdge(widget->root(), true);
      }
      if (!instance.endWidgets.empty()) {
        auto* widget = instance.endWidgets.back().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), false);
        extendHitTestOutsetToScreenEdge(widget->root(), false);
      }
    }
  }

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

  // Walks up from the hovered area to the bar widget that owns it, if any.
  Widget* widgetFromHoveredArea(BarInstance& instance, InputArea* hoveredArea) {
    for (const Node* node = hoveredArea; node != nullptr; node = node->parent()) {
      if (auto it = instance.widgetByRoot.find(node); it != instance.widgetByRoot.end()) {
        return it->second;
      }
    }
    return nullptr;
  }

  // An accordion run wants to be expanded while one of its widgets is hovered or, once it has started
  // unfolding, while the pointer stays anywhere over its capsule (gaps and padding included).
  bool accordionRunWantsExpansion(const BarInstance& instance, const BarCapsuleRun& run, const Widget* target) {
    if (target != nullptr && std::ranges::contains(run.widgets, target)) {
      return true;
    }
    return (run.accordionExpanded || run.accordionProgress > 0.0F)
        && instance.pointerInside
        && pointInsideNode(run.shell, instance.lastPointerSx, instance.lastPointerSy);
  }

  // The area the tooltip manager may show a tooltip for: none when the owning widget disables tooltips.
  InputArea* tooltipAreaForHover(BarInstance& instance, InputArea* hoveredArea) {
    const Widget* owner = widgetFromHoveredArea(instance, hoveredArea);
    return owner != nullptr && !owner->showsTooltip() ? nullptr : hoveredArea;
  }

  bool widgetsWantSecondTicks(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->wantsSecondTicks();
    });
  }

} // namespace

Bar::Bar() = default;

bool Bar::initialize(const BarServices& services) {
  m_platform = &services.platform;
  m_config = &services.config;
  m_notifications = services.notifications;
  m_tray = services.tray;
  m_audio = services.audio;
  m_easyEffects = services.easyEffects;
  m_upower = services.upower;
  m_sysmon = services.sysmon;
  m_powerProfiles = services.powerProfiles;
  m_network = services.network;
  m_modem = services.modem;
  m_externalIp = services.externalIp;
  m_idleInhibitor = services.idleInhibitor;
  m_mpris = services.mpris;
  m_audioSpectrum = services.audioSpectrum;
  m_httpClient = services.httpClient;
  m_weatherService = services.weather;
  m_renderContext = services.renderContext;
  m_nightLight = services.nightLight;
  m_themeService = services.theme;
  m_bluetooth = services.bluetooth;
  m_brightness = services.brightness;
  m_lockKeys = services.lockKeys;
  m_clipboard = services.clipboard;
  m_fileWatcher = services.fileWatcher;
  m_screenshots = services.screenshots;
  m_scriptApi = services.scriptApi;

  m_widgetFactory = std::make_unique<WidgetFactory>(services);

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_config->addReloadCallback(
      [this] {
        const auto& cfg = m_config->config();
        if (cfg.bars == m_lastBars
            && cfg.widgets == m_lastWidgets
            && cfg.shell.shadow == m_lastShadow
            && cfg.plugins == m_lastPlugins) {
          return;
        }
        reload();
      },
      "bar"
  );

  return true;
}

BarServices Bar::services() const {
  return {
      .platform = *m_platform,
      .config = *m_config,
      .notifications = m_notifications,
      .tray = m_tray,
      .audio = m_audio,
      .easyEffects = m_easyEffects,
      .upower = m_upower,
      .sysmon = m_sysmon,
      .powerProfiles = m_powerProfiles,
      .network = m_network,
      .modem = m_modem,
      .externalIp = m_externalIp,
      .idleInhibitor = m_idleInhibitor,
      .mpris = m_mpris,
      .audioSpectrum = m_audioSpectrum,
      .httpClient = m_httpClient,
      .weather = m_weatherService,
      .renderContext = m_renderContext,
      .nightLight = m_nightLight,
      .theme = m_themeService,
      .bluetooth = m_bluetooth,
      .brightness = m_brightness,
      .lockKeys = m_lockKeys,
      .clipboard = m_clipboard,
      .fileWatcher = m_fileWatcher,
      .screenshots = m_screenshots,
      .scriptApi = m_scriptApi,
  };
}

void Bar::onSecondTick() {
  // Without a widget that renders seconds, a full bar update once a minute is
  // enough, so skip the other 59 ticks instead of re-laying out every surface.
  const auto minute = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
  const bool minuteChanged = minute != m_lastTickMinute;
  m_lastTickMinute = minute;
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    if (!minuteChanged && !instanceWantsSecondTicks(*inst)) {
      continue;
    }
    inst->surface->requestUpdate();
  }
}

void Bar::reload() {
  noctalia::profiling::ScopedTimer t(kLog, "bar: reload (all instances)");
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_widgetFactory = std::make_unique<WidgetFactory>(services());

  if (recreateForOrder) {
    kLog.info("bar order changed; recreating layer-shell surfaces");
    closeAllInstances();
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying bar surfaces for order change: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
    syncInstances();
    return;
  }

  // Look up new bar configs by name.
  std::unordered_map<std::string, std::pair<const BarConfig*, std::size_t>> newBarsByName;
  newBarsByName.reserve(m_lastBars.size());
  for (std::size_t i = 0; i < m_lastBars.size(); ++i) {
    newBarsByName[m_lastBars[i].name] = {&m_lastBars[i], i};
  }

  // Exclusive-zone geometry on an output depends on the order its bar surfaces are
  // created: bars on the same edge stack in creation order, and bars on adjacent
  // edges (e.g. top + left) compete for the shared corner the same way. Rebuilding
  // one bar's surface in place while recreating another would commit them out of
  // config order and reshuffle that geometry. So if any bar on an output needs a
  // surface recreate, recreate every bar on that output — syncInstances rebuilds
  // them in config order. Scoped per output so other monitors are untouched.
  const auto needsSurfaceRecreate = [&](const BarInstance& inst) -> bool {
    auto it = newBarsByName.find(inst.barConfig.name);
    if (it == newBarsByName.end()) {
      return true;
    }
    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return true;
    }
    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return true;
    }
    return !barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow);
  };
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instUp : m_instances) {
    if (needsSurfaceRecreate(*instUp)) {
      outputsNeedingRecreate.insert(instUp->outputName);
    }
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below). Any bar on an output flagged above is destroyed so the
  // whole output is rebuilt in config order.
  bool destroyedAny = false;
  std::erase_if(m_instances, [&](const std::unique_ptr<BarInstance>& instUp) {
    auto& inst = *instUp;
    auto it = newBarsByName.find(inst.barConfig.name);
    auto destroy = [&] {
      if (inst.surface != nullptr) {
        m_surfaceMap.erase(inst.surface->wlSurface());
      }
      if (m_hoveredInstance == &inst) {
        m_hoveredInstance = nullptr;
      }
      destroyedAny = true;
      return true;
    };
    if (outputsNeedingRecreate.contains(inst.outputName)) {
      return destroy();
    }
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return destroy();
    }

    inst.barIndex = it->second.second;
    rebuildInstanceContents(inst, resolved);
    return false;
  });

  // Drain pending Wayland events for the just-destroyed surfaces before
  // creating new ones. Without this, the roundtrip inside LayerSurface::initialize
  // reads stale closures for dead proxies, which libwayland drops without freeing.
  if (destroyedAny && wl_display_roundtrip(m_platform->display()) < 0) {
    const int roundtripErrno = errno;
    kLog.error(
        "Wayland roundtrip failed after destroying stale bar surfaces: {}",
        m_platform->wayland().describeDisplayError(roundtripErrno)
    );
  }

  syncInstances();
}

void Bar::closeAllInstances() {
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_instances.clear();
}

void Bar::onOutputChange() { syncInstances(); }

void Bar::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  bool anyChanged = false;
  for (const auto& output : m_platform->outputs()) {
    const std::string activeId = activeWorkspaceId(m_platform->workspaces(output.output));
    if (activeId.empty()) {
      continue;
    }

    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (!last.empty() && last != activeId) {
      m_pendingWorkspaceRevealOutputs.insert(output.name);
      anyChanged = true;
    }
    last = activeId;
  }

  if (anyChanged) {
    m_workspaceRevealDebounce.start(kWorkspaceRevealDebounce, [this] { applyPendingWorkspaceReveal(); });
  }
  scheduleSmartAutoHideReevaluation();
}

void Bar::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this] {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Bar::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    BarInstance* instance = instanceUp.get();
    if (instance == nullptr
        || !instance->barConfig.enabled
        || !instance->barConfig.smartAutoHide
        || instance->surface == nullptr) {
      continue;
    }

    const bool wantsPinned = smartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0F || pinnedChanged) {
        revealAutoHideBar(*instance);
        syncBarAutoHideInputRegion(*instance);
        syncBarSurfaceChrome(*instance);
        needsRedraw = true;
      }
    } else if (
        !instance->pointerInside
        && instance->attachedPopupCount == 0
        && !suppressAutoHide
        && (instance->hideOpacity > 0.0F || pinnedChanged)
        && (!instance->barConfig.showOnWorkspaceSwitch || !isWorkspacePeekActive())
    ) {
      startHideFadeOut(*instance);
      needsRedraw = true;
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

bool Bar::isWorkspacePeekActive() const noexcept {
  return m_workspaceRevealDebounce.active() || m_workspacePeekHideTimer.active();
}

void Bar::applyPendingWorkspaceReveal() {
  if (m_platform == nullptr) {
    return;
  }

  const auto pendingOutputs = std::move(m_pendingWorkspaceRevealOutputs);
  m_pendingWorkspaceRevealOutputs.clear();

  std::vector<BarInstance*> peeked;
  peeked.reserve(m_instances.size());
  for (const std::uint32_t outputName : pendingOutputs) {
    for (const auto& instanceUp : m_instances) {
      auto* instance = instanceUp.get();
      if (instance == nullptr
          || instance->outputName != outputName
          || !instance->barConfig.enabled
          || !instance->barConfig.isAutoHideEnabled()
          || !instance->barConfig.showOnWorkspaceSwitch
          || instance->surface == nullptr) {
        continue;
      }

      revealAutoHideBar(*instance);
      if (instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        peeked.push_back(instance);
      }
    }
  }

  if (peeked.empty()) {
    return;
  }

  m_workspacePeekHideTimer.start(kWorkspacePeekHold, [this, peeked = std::move(peeked)] {
    for (BarInstance* instance : peeked) {
      if (instance == nullptr || !instance->barConfig.isAutoHideEnabled() || instance->pointerInside) {
        continue;
      }
      if (instance->barConfig.smartAutoHide && instance->smartAutoHidePinnedVisible) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        startHideFadeOut(*instance, true);
      }
    }
  });
}

void Bar::refresh() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
      if (inst->animations.hasActive() || instanceNeedsFrameTick(*inst)) {
        inst->surface->requestRedraw();
      }
    }
  }
}

void Bar::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Bar::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Bar::setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback) {
  m_autoHideSuppressionCallback = std::move(callback);
}

void Bar::reevaluateAutoHide() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr
        || !barPointerHideAllowed(*instance)
        || instance->pointerInside
        || instance->attachedPopupCount > 0) {
      continue;
    }
    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
    if (suppressAutoHide || instance->hideOpacity <= 0.001F) {
      continue;
    }
    startHideFadeOut(*instance);
  }
}

void Bar::rearmTooltipForHoveredWidget() {
  if (m_hoveredInstance == nullptr || !m_hoveredInstance->pointerInside) {
    return;
  }
  auto* hovered = m_hoveredInstance->inputDispatcher.hoveredArea();
  if (hovered == nullptr || m_hoveredInstance->surface == nullptr) {
    return;
  }
  // Same path a real hover takes, so the usual show delay still applies — the
  // tooltip must not blink into existence the instant the panel disappears.
  TooltipManager::instance().onBarHoverChange(
      tooltipAreaForHover(*m_hoveredInstance, hovered), m_hoveredInstance->surface->layerSurface(),
      m_hoveredInstance->output
  );
}

void Bar::reevaluateAutoHideAfterPopup() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    wl_surface* const surface = instance->surface->wlSurface();
    instance->pointerInside = m_platform != nullptr
        && surface != nullptr
        && m_platform->hasPointerPosition()
        && m_platform->lastPointerSurface() == surface;
    if (instance->pointerInside) {
      instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
      instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
      instance->inputDispatcher.pointerEnter(
          instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
      );
      m_hoveredInstance = instance.get();
    } else {
      instance->inputDispatcher.pointerLeave();
      if (m_hoveredInstance == instance.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    instance->surface->requestRedraw();
  }
  reevaluateAutoHide();
}

bool Bar::isRunning() const noexcept {
  return std::ranges::any_of(m_instances, [](const auto& inst) { return inst->surface && inst->surface->isRunning(); });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > 0.5F;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > 0.5F;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  return barSupportsSlideBehavior(instance.barConfig) || !instance.ipcLayoutReleased;
}

bool Bar::isVisible() const noexcept {
  return std::ranges::any_of(m_instances, [this](const auto& inst) { return instanceEffectivelyVisible(*inst); });
}

void Bar::clearInstancePointerState(BarInstance& instance) {
  instance.pointerInside = false;
  instance.inputDispatcher.pointerLeave();
  if (m_hoveredInstance == &instance) {
    m_hoveredInstance = nullptr;
  }
}

void Bar::setInstanceIpcVisible(BarInstance& instance, bool visible) {
  if (instance.surface == nullptr) {
    return;
  }
  if (barSupportsSlideBehavior(instance.barConfig)) {
    if (visible) {
      revealAutoHideBar(instance);
    } else {
      startHideFadeOut(instance, true);
    }
    return;
  }
  if (instance.slideRoot == nullptr) {
    return;
  }
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(1.0F);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  const float current = instance.hideOpacity;
  const float target = visible ? 1.0F : 0.0F;
  instance.animations.animate(
      current, target, Style::animNormal, visible ? Easing::EaseOutCubic : Easing::EaseInQuad,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this] {
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      },
      instance.slideRoot
  );
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::applyIpcVisibility(bool visible) {
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    setInstanceIpcVisible(*instance, visible);
    syncBarSurfaceChrome(*instance);
  }
  syncIdleInhibitorAnchors();
}

void Bar::syncIdleInhibitorAnchors() {
  if (m_idleInhibitor != nullptr) {
    m_idleInhibitor->resyncAnchorSurfaces();
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02F;
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
  return barShouldReserveExclusiveZone(
      instance.barConfig, instance.ipcLayoutReleased, barContentVisuallyShown(instance)
  );
}

void Bar::syncBarExclusiveZone(BarInstance& instance) {
  if (instance.surface == nullptr || m_config == nullptr) {
    return;
  }
  const std::int32_t zone = shouldReserveExclusiveZone(instance)
      ? reservedBarExclusiveZone(instance.barConfig, m_config->config().shell.shadow)
      : 0;
  instance.surface->setExclusiveZone(zone);
}

void Bar::syncBarSurfaceChrome(BarInstance& instance) {
  syncBarExclusiveZone(instance);
  applyBarCompositorBlur(instance);
}

std::optional<LayerPopupParentContext> Bar::popupParentContextForSurface(wl_surface* surface) const noexcept {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr || instance->surface == nullptr) {
    return std::nullopt;
  }

  auto* layerSurface = instance->surface->layerSurface();
  const auto width = instance->surface->width();
  const auto height = instance->surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  // For Dynamic Notch mode, anchor popups to center-bottom of notch
  if (instance->isNotchMode) {
    const auto& cfg = instance->barConfig;
    const float progress = instance->notchExpandProgress;
    const float notchW = lerp(static_cast<float>(cfg.notchCollapsedWidth), static_cast<float>(cfg.notchExpandedWidth), progress);
    const float notchH = lerp(static_cast<float>(cfg.notchCollapsedHeight), static_cast<float>(cfg.notchExpandedHeight), progress);
    const float maxW = std::min(notchW, static_cast<float>(instance->outputLogicalWidth - 32));
    const float maxH = std::min(notchH, static_cast<float>(instance->outputLogicalHeight) * 0.45F);

    return LayerPopupParentContext{
        .surface = instance->surface->wlSurface(),
        .attach = LayerShell::popupAttach(layerSurface),
        .output = instance->output,
        .width = static_cast<std::uint32_t>(maxW),
        .height = static_cast<std::uint32_t>(maxH),
    };
  }

  return LayerPopupParentContext{
      .surface = instance->surface->wlSurface(),
      .attach = LayerShell::popupAttach(layerSurface),
      .output = instance->output,
      .width = width,
      .height = height,
  };
}

std::optional<LayerPopupParentContext> Bar::preferredPopupParentContext(wl_output* output) const noexcept {
  BarInstance* instance = instanceForOutput(output);
  if (instance == nullptr && !m_instances.empty()) {
    instance = m_instances.front().get();
  }
  return instance != nullptr && instance->surface != nullptr
      ? popupParentContextForSurface(instance->surface->wlSurface())
      : std::nullopt;
}

std::vector<InputRect> Bar::surfaceRectsForOutput(wl_output* output) const {
  std::vector<InputRect> rects;
  if (m_platform == nullptr || output == nullptr) {
    return rects;
  }

  const WaylandOutput* wlOutput = m_platform->findOutputByWl(output);
  if (wlOutput == nullptr) {
    return rects;
  }
  if (!wlOutput->hasUsableGeometry()) {
    return rects;
  }
  const std::int32_t outputW = wlOutput->effectiveLogicalWidth();
  const std::int32_t outputH = wlOutput->effectiveLogicalHeight();

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (!instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    const auto* surface = instance->surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const std::int32_t mTop = surface->marginTop();
    const std::int32_t mRight = surface->marginRight();
    const std::int32_t mBottom = surface->marginBottom();
    const std::int32_t mLeft = surface->marginLeft();
    // surface->width()/height() may be 0 before configure; fall back to BarConfig
    // thickness so we still publish a sensible exclusion for fresh surfaces.
    const auto surfW = static_cast<std::int32_t>(surface->width());
    const auto surfH = static_cast<std::int32_t>(surface->height());

    std::int32_t rectW = surfW;
    std::int32_t rectH = surfH;
    std::int32_t rectX = 0;
    std::int32_t rectY = 0;

    if (aLeft && aRight) {
      rectW = std::max(0, outputW - mLeft - mRight);
      rectX = mLeft;
    } else if (aRight) {
      rectX = std::max(0, outputW - mRight - rectW);
    } else {
      rectX = mLeft;
    }

    if (aTop && aBottom) {
      rectH = std::max(0, outputH - mTop - mBottom);
      rectY = mTop;
    } else if (aBottom) {
      rectY = std::max(0, outputH - mBottom - rectH);
    } else {
      rectY = mTop;
    }

    if (rectW > 0 && rectH > 0) {
      rects.push_back(InputRect{rectX, rectY, rectW, rectH});
    }
  }

  return rects;
}

std::vector<wl_surface*> Bar::allBarSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instanceAcceptsPointerInput(*instance)) {
      if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
        surfaces.push_back(s);
      }
    }
  }
  return surfaces;
}

std::vector<wl_surface*> Bar::caffeineAnchorSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr || !instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    if (!barContentVisuallyShown(*instance)) {
      continue;
    }
    if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
      surfaces.push_back(s);
    }
  }
  return surfaces;
}

bool Bar::canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return false;
  }
  return barSupportsSlideBehavior(instance->barConfig) || instanceEffectivelyVisible(*instance);
}

std::optional<BarConfig> Bar::configForBar(wl_output* output, std::string_view barName) const {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !instance->barConfig.enabled) {
    return std::nullopt;
  }
  return instance->barConfig;
}

LayerShellLayer Bar::highestLayerForOutput(wl_output* output) const noexcept {
  LayerShellLayer highest = LayerShellLayer::Top;
  for (const auto& instance : m_instances) {
    if (instance->output != output || !instance->barConfig.enabled) {
      continue;
    }
    const LayerShellLayer layer = layerShellLayerFromConfig(instance->barConfig.layer);
    if (static_cast<std::uint32_t>(layer) > static_cast<std::uint32_t>(highest)) {
      highest = layer;
    }
  }
  return highest;
}

bool Bar::isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !barSupportsSlideBehavior(instance->barConfig)) {
    return true;
  }
  constexpr float kSettledThreshold = 0.999F;
  return instance->hideOpacity >= kSettledThreshold;
}

void Bar::revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance != nullptr) {
    revealAutoHideBar(*instance);
  }
}

void Bar::setAttachedPanelGeometry(
    wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry
) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr) {
    return;
  }

  instance->attachedPanelGeometry = geometry;
  if (instance->surface != nullptr && instance->surface->width() > 0 && instance->surface->height() > 0) {
    applyBarShadowStyle(
        *instance, m_config->config().shell.shadow, static_cast<float>(instance->surface->width()),
        static_cast<float>(instance->surface->height())
    );
    instance->surface->requestRedraw();
  }
}

void Bar::beginAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  ++instance->attachedPopupCount;
}

void Bar::endAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  if (instance->attachedPopupCount > 0) {
    --instance->attachedPopupCount;
  }
  if (instance->attachedPopupCount > 0) {
    return;
  }
  instance->pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == surface;
  if (instance->pointerInside) {
    instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
    instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
    instance->inputDispatcher.pointerEnter(
        instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
    );
  } else {
    instance->inputDispatcher.pointerLeave();
  }
  if (instance->surface != nullptr) {
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (!barPointerHideAllowed(*instance) || instance->pointerInside) {
    return;
  }
  const bool suppressAutoHide =
      (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
  if (!suppressAutoHide) {
    startHideFadeOut(*instance);
  }
}

void Bar::show() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::hide() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_wasVisibleBeforeOverlaySuppress = isVisible();
  hide();
}

void Bar::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_wasVisibleBeforeOverlaySuppress) {
    show();
  }
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::ranges::any_of(m_instances, [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
    }
    applyIpcVisibility(false);
    return;
  }

  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& bars = m_config->config().bars;

  const auto geometryChanged = [](const BarInstance& instance, const WaylandOutput& output) {
    return instance.outputLogicalX != output.logicalX
        || instance.outputLogicalY != output.logicalY
        || instance.outputLogicalWidth != output.effectiveLogicalWidth()
        || instance.outputLogicalHeight != output.effectiveLogicalHeight();
  };

  std::erase_if(m_lastActiveWorkspaceByOutput, [&outputs](const auto& pair) {
    return std::ranges::find(outputs, pair.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (last.empty()) {
      last = activeWorkspaceId(m_platform->workspaces(output.output));
    }
  }

  // Layer-shell surfaces are output-bound, but some compositors do not reposition an existing surface after that
  // output moves. Recreate every bar on an affected output to preserve their config-order stacking.
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instance : m_instances) {
    const auto output = std::ranges::find(outputs, instance->outputName, &WaylandOutput::name);
    if (output != outputs.end() && output->done && output->hasUsableGeometry() && geometryChanged(*instance, *output)) {
      outputsNeedingRecreate.insert(output->name);
    }
  }

  // Remove instances for outputs that no longer exist or whose output geometry changed.
  std::erase_if(m_instances, [&outputs, &outputsNeedingRecreate, this](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    const bool recreate = found && outputsNeedingRecreate.contains(inst->outputName);
    if (!found || recreate) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    if (!found || recreate) {
      if (inst->surface != nullptr) {
        m_surfaceMap.erase(inst->surface->wlSurface());
      }
      if (m_hoveredInstance == inst.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    return !found || recreate;
  });

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done || !output.hasUsableGeometry()) {
        continue;
      }

      bool exists = std::ranges::any_of(m_instances, [&output, barIdx](const auto& inst) {
        return inst->outputName == output.name && inst->barIndex == barIdx;
      });
      if (!exists) {
        auto resolved = ConfigService::resolveForOutput(bars[barIdx], output);
        if (!resolved.enabled) {
          continue;
        }
        createInstance(output, barIdx, resolved);
      }
    }
  }

  syncIdleInhibitorAnchors();
  reevaluateSmartAutoHide();
}

void Bar::createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig) {
  auto instance = std::make_unique<BarInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->outputLogicalX = output.logicalX;
  instance->outputLogicalY = output.logicalY;
  instance->outputLogicalWidth = output.effectiveLogicalWidth();
  instance->outputLogicalHeight = output.effectiveLogicalHeight();
  instance->barConfig = barConfig;
  instance->barIndex = barIndex;

  // Check if Dynamic Notch mode is enabled for this bar (top position only)
  const bool isNotchMode = barConfig.dynamicNotch && barConfig.position == "top";
  instance->isNotchMode = isNotchMode;

  std::uint32_t anchor;
  BarSurfaceSpec surfaceSpec;

  if (isNotchMode) {
    // Dynamic Notch: centered pill at top, anchored to TOP only
    anchor = LayerShellAnchor::Top;
    surfaceSpec = computeNotchSurfaceSpec(barConfig, m_config->config().shell.shadow, false); // Start collapsed
    instance->notchExpanded = false;
    instance->notchExpandProgress = 0.0F;
  } else {
    anchor = positionToAnchor(barConfig.position);
    surfaceSpec = computeBarSurfaceSpec(barConfig, m_config->config().shell.shadow);
  }

  kLog.info(
      "creating #{} \"{}\" on {} ({}), thickness={} position={} reserve_space={} exclusive_zone={} notch={}", barIndex,
      barConfig.name, output.connectorName, output.description, barConfig.thickness, barConfig.position,
      barConfig.reserveSpace, surfaceSpec.exclusiveZone, isNotchMode
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-bar-" + barConfig.name,
      .layer = isNotchMode ? LayerShellLayer::Overlay : layerShellLayerFromConfig(barConfig.layer),
      .anchor = anchor,
      .width = surfaceSpec.surfaceWidth,
      .height = surfaceSpec.surfaceHeight,
      .exclusiveZone = surfaceSpec.exclusiveZone,
      .marginTop = surfaceSpec.marginTop,
      .marginRight = surfaceSpec.marginRight,
      .marginBottom = surfaceSpec.marginBottom,
      .marginLeft = surfaceSpec.marginLeft,
      .keyboard = isNotchMode ? LayerShellKeyboard::OnDemand : LayerShellKeyboard::None,
      .defaultWidth = 1920,
      .defaultHeight = surfaceSpec.surfaceHeight,
      .prewarmBlur = barConfig.compositorBlur,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst](std::uint32_t width, std::uint32_t height) {
    buildScene(*inst, width, height);
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([inst](float deltaMs) {
    tickWidgets(inst->startWidgets, deltaMs);
    tickWidgets(inst->centerWidgets, deltaMs);
    tickWidgets(inst->endWidgets, deltaMs);
    // Also tick notch animation if active
    if (inst->isNotchMode && inst->notchAnimationId != 0) {
      inst->animations.tick(deltaMs);
    }
  });

  instance->surface->setAnimationManager(&instance->animations);
  populateWidgets(*instance);

  // Store fixed surface dimensions for pill positioning during animation.
  if (isNotchMode) {
    inst->maxSurfaceW = surfaceSpec.surfaceWidth;
    inst->maxSurfaceH = surfaceSpec.surfaceHeight;
  }

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  // Enable custom input region for notch surface so the pill-shaped
  // input region (updated per-frame) is respected. With clickThrough=true,
  // the custom input region set via setInputRegion() is used.
  if (isNotchMode) {
    inst->surface->setClickThrough(true);
  }

  // Render initial frame to commit a buffer so the surface is mapped and receives pointer events.
  if (isNotchMode) {
    buildScene(*inst, surfaceSpec.surfaceWidth, surfaceSpec.surfaceHeight);
  }

  m_surfaceMap[instance->surface->wlSurface()] = instance.get();
  m_instances.push_back(std::move(instance));
}

void Bar::destroyInstance(std::uint32_t outputName) {
  std::erase_if(m_instances, [outputName, this](const auto& inst) {
    if (inst->surface != nullptr) {
      m_surfaceMap.erase(inst->surface->wlSurface());
    }
    if (m_hoveredInstance == inst.get()) {
      m_hoveredInstance = nullptr;
    }
    return inst->outputName == outputName;
  });
}

void Bar::populateWidgets(BarInstance& instance) {
  instance.deadZoneBindings.resolve(
      noctalia::bar::WidgetActionBindings::Inputs{
          .widgetDefaults = noctalia::bar::deadZoneGestureDefaults(),
          .widgetActions = &instance.barConfig.deadZone.actions,
          .widgetContext = "bar." + instance.barConfig.name + ".dead_zone",
          .widgetName = instance.barConfig.name,
          .widgetType = "dead zone",
      }
  );
  {
    std::string summary;
    for (const auto gesture : noctalia::bar::allGestures()) {
      const auto* action = instance.deadZoneBindings.find(gesture);
      if (action == nullptr) {
        continue;
      }
      if (!summary.empty()) {
        summary += ", ";
      }
      summary += std::format("{}={}", gestureConfigKey(gesture), action->commandLine());
    }
    // Runs once per bar per reload, mirroring the per-widget line: the first thing to check when a
    // dead zone binding does not fire.
    kLog.debug("bar.{}.dead_zone: {}", instance.barConfig.name, summary.empty() ? "no bindings" : summary);
  }

  const auto& widgetConfigs = m_config->config().widgets;
  const auto labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  const std::string barFontFamily = (instance.barConfig.fontFamily && !instance.barConfig.fontFamily->empty())
      ? *instance.barConfig.fontFamily
      : m_config->config().shell.fontFamily;
  // Creates one widget for `name`. When `groupSpec` is set the widget is a member of a capsule group and
  // takes the group's capsule style + foreground; otherwise it resolves its own per-widget/bar capsule.
  auto createWidget = [&](const std::string& name, const WidgetBarCapsuleSpec* groupSpec,
                          const std::optional<ColorSpec>* groupForeground, std::vector<std::unique_ptr<Widget>>& dest) {
    const WidgetConfig* wcPtr = nullptr;
    if (auto it = widgetConfigs.find(name); it != widgetConfigs.end()) {
      wcPtr = &it->second;
    }
    const std::string_view widgetType =
        wcPtr != nullptr && !wcPtr->type.empty() ? std::string_view(wcPtr->type) : std::string_view(name);
    const CommonWidgetOptions options =
        resolveCommonWidgetOptions(instance.barConfig, wcPtr, widgetType, instance.barConfig.scale);
    if (!options.enabled) {
      return;
    }
    auto widget = m_widgetFactory->create(
        name, instance.output, options.contentScale, instance.barConfig.position, instance.barConfig.name,
        static_cast<float>(instance.barConfig.widgetSpacing), options.enableScroll
    );
    if (widget == nullptr) {
      return;
    }
    widget->setConfigName(name);
    widget->applyCommonOptions(options, labelFontWeight, barFontFamily, std::format("widget.{}", name));
    widget->setActionContext(
        IpcInvocationContext{
            .widgetName = name,
            .widgetType = wcPtr != nullptr ? wcPtr->type : name,
            .barName = instance.barConfig.name,
            .output = instance.output,
        }
    );
    widget->resolveGestureBindings(
        wcPtr != nullptr ? wcPtr->type : name, wcPtr, &instance.barConfig.actions,
        std::format("bar.{}", instance.barConfig.name), &m_actionDispatcher
    );
    widget->setBarCapsuleSpec(groupSpec != nullptr ? *groupSpec : options.capsule);
    if (options.color.has_value()) {
      widget->setWidgetForeground(options.color);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetForeground(*groupForeground);
    } else if (instance.barConfig.widgetColor.has_value()) {
      widget->setWidgetForeground(instance.barConfig.widgetColor);
    }
    if (options.iconColor.has_value()) {
      widget->setWidgetIconColor(options.iconColor);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetIconColor(*groupForeground);
    } else if (instance.barConfig.widgetIconColor.has_value()) {
      widget->setWidgetIconColor(instance.barConfig.widgetIconColor);
    }
    dest.push_back(std::move(widget));
  };

  // Expands a lane's entries: group tokens become contiguous member widgets sharing the group's capsule.
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest) {
    for (const auto& name : names) {
      if (isCapsuleGroupToken(name)) {
        const BarCapsuleGroupStyle* group = findBarCapsuleGroupStyle(instance.barConfig, capsuleGroupTokenId(name));
        if (group == nullptr) {
          kLog.warn("bar.{}: lane entry \"{}\" has no matching capsule_group", instance.barConfig.name, name);
          continue;
        }
        if (!group->enabled) {
          continue;
        }
        const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(instance.barConfig, *group);
        for (const auto& member : group->members) {
          createWidget(member, &groupSpec, &group->foreground, dest);
        }
        continue;
      }
      createWidget(name, nullptr, nullptr, dest);
    }
  };

  createWidgets(instance.barConfig.startWidgets, instance.startWidgets);
  createWidgets(instance.barConfig.centerWidgets, instance.centerWidgets);
  createWidgets(instance.barConfig.endWidgets, instance.endWidgets);

  // Populate notch widget sets if in notch mode
  if (instance.isNotchMode) {
    createWidgets(instance.barConfig.notchCollapsedStart, instance.notchCollapsedStartWidgets);
    createWidgets(instance.barConfig.notchCollapsedCenter, instance.notchCollapsedCenterWidgets);
    createWidgets(instance.barConfig.notchCollapsedEnd, instance.notchCollapsedEndWidgets);
    createWidgets(instance.barConfig.notchExpandedStart, instance.notchExpandedStartWidgets);
    createWidgets(instance.barConfig.notchExpandedCenter, instance.notchExpandedCenterWidgets);
    createWidgets(instance.barConfig.notchExpandedEnd, instance.notchExpandedEndWidgets);
  }

#ifndef NDEBUG
  // Prepend a red "debug" pill to the end section if running a debug build
  auto debugWidget = m_widgetFactory->create(
      "debug_indicator", instance.output, instance.barConfig.scale, instance.barConfig.position,
      instance.barConfig.name, static_cast<float>(instance.barConfig.widgetSpacing)
  );
  if (debugWidget != nullptr) {
    debugWidget->setConfigName("debug_indicator");
    debugWidget->setFontScale(instance.barConfig.fontScale);
    debugWidget->setLabelFontWeight(labelFontWeight);
    debugWidget->setLabelFontFamily(barFontFamily);
    debugWidget->create();
    instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const bool hoverHighlight = instance.barConfig.hoverHighlight;

  instance.widgetByRoot.clear();
  instance.hoverHighlightWidget = nullptr;
  instance.peekWidget = nullptr;
  instance.widgetPeekDelayTimer.stop();
  if (instance.hoverUnderlay != nullptr) {
    while (!instance.hoverUnderlay->children().empty()) {
      instance.hoverUnderlay->removeChild(instance.hoverUnderlay->children().back().get());
    }
  }

  // Hover overlay: sits above the capsule fill (same zIndex, later sibling) and below the
  // content; fill/visibility are driven by the hover animation.
  auto addHoverBox = [hoverHighlight](Widget& widget, Node& shell) -> Box* {
    if (!hoverHighlight || !widget.wantsBarHoverHighlight()) {
      return nullptr;
    }
    Box* boxPtr = nullptr;
    shell.addChild(
        ui::box({
            .out = &boxPtr,
            .fill = scaleAlpha(widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.0F),
            .visible = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
    widget.setBarHoverBox(boxPtr);
    return boxPtr;
  };

  auto attach = [&](std::vector<std::unique_ptr<Widget>>& widgets, std::vector<BarCapsuleRun>& capsuleRuns,
                    Flex* section) {
    if (section == nullptr) {
      return;
    }

    for (auto& widget : widgets) {
      widget->setAnimationManager(&instance.animations);
      widget->setUpdateCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestUpdate();
        }
      });
      widget->setRedrawCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestRedraw();
        }
      });
      widget->setFrameTickRequestCallback([surface = instance.surface.get()] {
        if (surface != nullptr) {
          surface->requestFrameTick();
        }
      });
      if (auto* plugin = dynamic_cast<PluginWidget*>(widget.get()); plugin != nullptr) {
        plugin->setUpdateDeferralCallback([] {
          auto* panel = PanelManager::current();
          return panel != nullptr && panel->isPanelTransitionActive();
        });
      }
      widget->setPanelToggleCallback([this, inst = &instance](
                                         std::string_view panelId, std::string_view context,
                                         std::optional<float> anchorSurfaceX, std::optional<float> anchorSurfaceY,
                                         Widget::PanelActivation activation
                                     ) {
        float anchorX = inst->lastPointerSx;
        float anchorY = inst->lastPointerSy;
        if (anchorSurfaceX.has_value()) {
          anchorX = *anchorSurfaceX;
        }
        if (anchorSurfaceY.has_value()) {
          anchorY = *anchorSurfaceY;
        }
        if (m_platform != nullptr && inst->output != nullptr) {
          if (const auto* out = m_platform->findOutputByWl(inst->output); out != nullptr && out->hasUsableGeometry()) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        PanelOpenRequest request{
            .output = inst->output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasExplicitAnchor = anchorSurfaceX.has_value() || anchorSurfaceY.has_value(),
            .hasAnchorPosition = true,
            .context = context,
            .sourceBarName = inst->barConfig.name
        };
        if (activation == Widget::PanelActivation::Open) {
          PanelManager::instance().openPanel(std::string(panelId), request);
        } else {
          PanelManager::instance().togglePanel(std::string(panelId), request);
        }
        if (PanelManager::instance().isOpenPanel(panelId)) {
          suppressTooltipForOpenPanel(panelId);
        }
      });
      if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
        tray->setHoverOverlayParent(instance.hoverUnderlay);
      }

      widget->create();
      if (widget->outerNode() != nullptr) {
        instance.widgetByRoot[widget->outerNode()] = widget.get();
      }
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      // No capsule: the hover pill lives on the bar-level underlay (unclipped, no layout
      // footprint) and is positioned after sections are laid out.
      if (hoverHighlight
          && instance.hoverUnderlay != nullptr
          && !widget.isBarClickThrough()
          && !widget.noGapAroundMe()) {
        addHoverBox(widget, *instance.hoverUnderlay);
      }
      auto* added = section->addChild(widget.releaseRoot());
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
      if (widget.expandsInLane()) {
        added->setFlexGrow(1.0F);
      }
    };

    auto addSingleCapsule = [&](Widget& widget) {
      const auto& cap = widget.barCapsuleSpec();
      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget.contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, cap.borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));
      Box* hoverPtr = addHoverBox(widget, *shellPtr);
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      if (auto* area = dynamic_cast<InputArea*>(widget.root())) {
        area->setTooltipAnchorNode(shellPtr);
      }
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.outerNode(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .widgets = {&widget},
              .hoverBoxes = hoverPtr != nullptr ? std::vector<Box*>{hoverPtr} : std::vector<Box*>{},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
      // The shell, not the widget root, is the lane's flex child.
      if (widget.expandsInLane()) {
        added->setFlexGrow(1.0F);
      }
    };

    // Members of the same group share one resolved style by construction (see resolveWidgetBarCapsuleSpec),
    // so adjacency + matching group ID is sufficient to merge. Per-widget scale does not split the group:
    // the run is sized from its largest member's scale below so a differently scaled member still fits.
    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      return firstSpec.enabled
          && nextSpec.enabled
          && !first.isAnchor()
          && !next.isAnchor()
          && !firstSpec.group.empty()
          && firstSpec.group == nextSpec.group;
    };

    std::size_t index = 0;
    while (index < widgets.size()) {
      auto& widget = widgets[index];
      if (widget->root() == nullptr) {
        ++index;
        continue;
      }

      const auto& cap = widget->barCapsuleSpec();
      if (!cap.enabled) {
        addPlainWidget(*widget);
        ++index;
        continue;
      }

      if (widget->isAnchor() || cap.group.empty()) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      std::size_t runEnd = index + 1;
      while (runEnd < widgets.size()
             && widgets[runEnd] != nullptr
             && widgets[runEnd]->root() != nullptr
             && canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
        ++runEnd;
      }

      if (runEnd - index < 2) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget->contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, cap.borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));

      // Visual order: with accordion + Start direction the always-visible first member sits last,
      // so the hidden members unfold before it along the lane's main axis. Flex children,
      // run.widgets and run.hoverBoxes must all follow this same order.
      const bool accordion = cap.accordion && (runEnd - index >= 2);
      const bool accordionStart = accordion && cap.accordionDirection == BarAccordionDirection::Start;
      std::vector<std::size_t> memberOrder;
      memberOrder.reserve(runEnd - index);
      if (accordionStart) {
        for (std::size_t memberIndex = index + 1; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
        memberOrder.push_back(index);
      } else {
        for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
      }

      std::vector<Box*> hoverBoxes;
      if (hoverHighlight) {
        hoverBoxes.reserve(memberOrder.size());
        for (const std::size_t memberIndex : memberOrder) {
          hoverBoxes.push_back(
              instance.hoverUnderlay != nullptr ? addHoverBox(*widgets[memberIndex], *instance.hoverUnderlay) : nullptr
          );
        }
      }

      auto inner = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = cap.widgetSpacing.value_or(widgetSpacing),
          }
      );
      Flex* innerPtr = inner.get();
      // Accordion members are clipped to the reveal window, not the padded shell: the capsule
      // padding is wider than the member gap at larger padding values, which would otherwise leave
      // the next member's leading edge showing inside the collapsed pill.
      Node* clipPtr = nullptr;
      if (accordion) {
        auto clip = ui::node({});
        clipPtr = clip.get();
        clipPtr->setClipChildren(true);
        clipPtr->addChild(std::move(inner));
        shellPtr->addChild(std::move(clip));
      } else {
        shellPtr->addChild(std::move(inner));
      }

      BarCapsuleRun run;
      run.shell = shellPtr;
      run.bg = bgPtr;
      run.container = innerPtr;
      run.content = innerPtr;
      run.spec = cap;
      run.contentScale = widget->contentScale();
      run.hoverBoxes = std::move(hoverBoxes);
      run.accordion = accordion;
      run.accordionDurationMs = cap.accordionDurationMs.value_or(static_cast<float>(kBarRevealDurationMs));
      run.accordionDelayMs = cap.accordionDelayMs.value_or(static_cast<float>(kBarRevealDelayMs));
      run.accordionClip = clipPtr;
      run.accordionDirection = cap.accordionDirection;
      run.accordionVisibleIndex = accordionStart ? memberOrder.size() - 1 : 0;

      for (const std::size_t memberIndex : memberOrder) {
        auto& member = widgets[memberIndex];
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (auto* area = dynamic_cast<InputArea*>(member->root())) {
          area->setTooltipAnchorNode(shellPtr);
        }
        if (member->noGapAroundMe()) {
          innerPtr->setChildGapExcluded(added, true);
        }
      }

      capsuleRuns.push_back(std::move(run));
      section->addChild(std::move(shell));
      index = runEnd;
    }
  };

  attach(instance.startWidgets, instance.startCapsuleRuns, instance.startSection);
  attach(instance.centerWidgets, instance.centerCapsuleRuns, instance.centerSection);
  attach(instance.endWidgets, instance.endCapsuleRuns, instance.endSection);
}

void Bar::updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);
  if (target != nullptr && (target->barHoverBox() == nullptr || !target->wantsBarHoverTint())) {
    target = nullptr;
  }
  if (target == instance.hoverHighlightWidget) {
    return;
  }
  if (instance.hoverHighlightWidget != nullptr) {
    animateWidgetHoverHighlight(instance, *instance.hoverHighlightWidget, false);
  }
  instance.hoverHighlightWidget = target;
  if (target != nullptr) {
    animateWidgetHoverHighlight(instance, *target, true);
  }
}

void Bar::animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered) {
  Box* box = widget.barHoverBox();
  if (box == nullptr) {
    return;
  }
  const ColorSpec fill = widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
  instance.animations.cancelForOwner(box);
  instance.animations.animate(
      widget.barHoverProgress(), hovered ? 1.0F : 0.0F, Style::animFast, Easing::EaseOutCubic,
      [&widget, box, fill](float progress) {
        widget.setBarHoverProgress(progress);
        box->setVisible(progress > 0.001F);
        box->setFill(scaleAlpha(fill, kWidgetHoverFillAlpha * progress));
      },
      {}, box
  );
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::beginAccordionAnimation(BarInstance& instance, BarCapsuleRun& run, bool expanded) {
  run.accordionExpanded = expanded;
  instance.animations.cancelForOwner(run.shell);
  instance.animations.animate(
      run.accordionProgress, expanded ? 1.0F : 0.0F, run.accordionDurationMs, Easing::EaseOutCubic,
      [&run, surface = instance.surface.get()](float progress) {
        run.accordionProgress = progress;
        if (surface != nullptr) {
          surface->requestLayout();
        }
      },
      {}, run.shell
  );
}

void Bar::updateAccordionExpansion(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);

  bool changed = false;
  auto updateRuns = [&](std::vector<BarCapsuleRun>& runs) {
    for (auto& run : runs) {
      if (!run.accordion || run.shell == nullptr) {
        run.accordionDelayTimer.stop();
        continue;
      }
      const bool want = accordionRunWantsExpansion(instance, run, target);
      if (want == run.accordionExpanded) {
        run.accordionDelayTimer.stop();
        continue;
      }
      if (!want) {
        // Collapse stays immediate; a pending expand is void once the pointer leaves.
        run.accordionDelayTimer.stop();
        beginAccordionAnimation(instance, run, false);
        changed = true;
        continue;
      }
      // Expand honors accordion_delay_ms: arm a one-shot and re-verify hover when it fires, so a
      // pointer that already moved on never unfolds the group.
      if (run.accordionDelayMs <= 0.0F) {
        beginAccordionAnimation(instance, run, true);
        changed = true;
        continue;
      }
      if (run.accordionDelayTimer.active()) {
        continue;
      }
      run.accordionDelayTimer.start(
          std::chrono::milliseconds(static_cast<int>(run.accordionDelayMs)), [this, &instance, &run] {
            const Widget* nowTarget = widgetFromHoveredArea(instance, instance.inputDispatcher.hoveredArea());
            if (run.accordionExpanded || !accordionRunWantsExpansion(instance, run, nowTarget)) {
              return;
            }
            beginAccordionAnimation(instance, run, true);
            if (instance.surface != nullptr) {
              instance.surface->requestLayout();
            }
          }
      );
    }
  };
  updateRuns(instance.startCapsuleRuns);
  updateRuns(instance.centerCapsuleRuns);
  updateRuns(instance.endCapsuleRuns);
  // animate() only queues; the first tick needs a frame requested here.
  if (changed && instance.surface != nullptr) {
    instance.surface->requestLayout();
  }
}

void Bar::beginWidgetPeekAnimation(BarInstance& instance, Widget& widget, bool expanded) {
  widget.setPeekExpanded(expanded);
  instance.animations.cancelForOwner(widget.peekAnimationOwner());
  instance.animations.animate(
      widget.peekProgress(), expanded ? 1.0F : 0.0F, static_cast<float>(kBarRevealDurationMs), Easing::EaseOutCubic,
      [&widget, surface = instance.surface.get()](float progress) {
        widget.setPeekProgress(progress);
        if (surface != nullptr) {
          surface->requestLayout();
        }
      },
      {}, widget.peekAnimationOwner()
  );
  // animate() only queues; the first tick needs a frame requested here.
  if (instance.surface != nullptr) {
    instance.surface->requestLayout();
  }
}

void Bar::updateWidgetPeek(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);
  if (target != nullptr && !target->peekEnabled()) {
    target = nullptr;
  }
  if (target == instance.peekWidget) {
    return;
  }
  instance.widgetPeekDelayTimer.stop();
  // Collapse stays immediate; a pending reveal is void once the pointer moves on.
  if (instance.peekWidget != nullptr && instance.peekWidget->peekExpanded()) {
    beginWidgetPeekAnimation(instance, *instance.peekWidget, false);
  }
  instance.peekWidget = target;
  if (target == nullptr) {
    return;
  }
  if (target->peekDelayMs() <= 0.0F) {
    beginWidgetPeekAnimation(instance, *target, true);
    return;
  }
  instance.widgetPeekDelayTimer.start(
      std::chrono::milliseconds(static_cast<int>(target->peekDelayMs())), [this, &instance] {
        Widget* now = widgetFromHoveredArea(instance, instance.inputDispatcher.hoveredArea());
        if (now == nullptr || now != instance.peekWidget || now->peekExpanded()) {
          return;
        }
        beginWidgetPeekAnimation(instance, *now, true);
      }
  );
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  noctalia::profiling::ScopedTimer t(kLog, std::format("bar: rebuild contents [{}]", newConfig.name));

  const bool restartHideDelay =
      instance.autoHideDelayTimer.active() && instance.barConfig.autoHideDelayMs != newConfig.autoHideDelayMs;
  if (restartHideDelay) {
    instance.autoHideDelayTimer.stop();
  }

  // Drop any pointer hover/capture state pointing into the widgets we're about
  // to destroy. Hover will be re-acquired on the next pointer motion.
  instance.inputDispatcher.pointerLeave();

  instance.barConfig = newConfig;

  // Detach old widget root nodes from their sections and destroy the widgets.
  // Widgets release their root into the section on creation, so the section
  // owns those nodes — clearing the section frees the scene tree.
  auto clearChildren = [](Node* node) {
    if (node == nullptr) {
      return;
    }
    while (!node->children().empty()) {
      node->removeChild(node->children().back().get());
    }
  };
  clearChildren(instance.startSection);
  clearChildren(instance.centerSection);
  clearChildren(instance.endSection);
  instance.startWidgets.clear();
  instance.centerWidgets.clear();
  instance.endWidgets.clear();
  instance.startCapsuleRuns.clear();
  instance.centerCapsuleRuns.clear();
  instance.endCapsuleRuns.clear();

  // Refresh section-level layout knobs that may have changed (gap; direction
  // doesn't change because position is part of the surface-fields gate).
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  if (instance.startSection != nullptr) {
    instance.startSection->setGap(widgetSpacing);
  }
  if (instance.centerSection != nullptr) {
    instance.centerSection->setGap(widgetSpacing);
  }
  if (instance.endSection != nullptr) {
    instance.endSection->setGap(widgetSpacing);
  }

  populateWidgets(instance);
  attachWidgetsToSections(instance);

  applyBackgroundPalette(instance);
  syncBarSurfaceChrome(instance);

  if (instance.surface != nullptr) {
    // Re-run buildScene at the current surface size so radii / styling pick
    // up changes. The first-frame branch is skipped because sceneRoot is
    // already in place.
    const auto w = instance.surface->width();
    const auto h = instance.surface->height();
    if (w > 0 && h > 0) {
      buildScene(instance, w, h);
    }
    instance.surface->requestLayout();
  }
  if (restartHideDelay
      && !instance.pointerInside
      && instance.attachedPopupCount == 0
      && barPointerHideAllowed(instance)) {
    startHideFadeOut(instance);
  }
}

void Bar::tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) { ::tickWidgets(widgets, deltaMs); }

bool Bar::widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsNeedFrameTick(widgets);
}

bool Bar::instanceNeedsFrameTick(const BarInstance& instance) {
  return widgetsNeedFrameTick(instance.startWidgets)
      || widgetsNeedFrameTick(instance.centerWidgets)
      || widgetsNeedFrameTick(instance.endWidgets);
}

bool Bar::widgetsWantSecondTicks(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsWantSecondTicks(widgets);
}

bool Bar::instanceWantsSecondTicks(const BarInstance& instance) {
  return widgetsWantSecondTicks(instance.startWidgets)
      || widgetsWantSecondTicks(instance.centerWidgets)
      || widgetsWantSecondTicks(instance.endWidgets);
}

bool Bar::wantsSecondTicks() const {
  return std::ranges::any_of(m_instances, [](const auto& instance) {
    return instance != nullptr && instanceWantsSecondTicks(*instance);
  });
}

void Bar::applyBackgroundPalette(BarInstance& instance) {
  if (instance.bg == nullptr) {
    return;
  }
  auto style = instance.bg->style();
  style.fill = colorForRole(ColorRole::Surface, instance.barConfig.backgroundOpacity);
  style.border = resolveColorSpec(instance.barConfig.border);
  style.borderWidth = instance.barConfig.borderWidth;
  instance.bg->setStyle(style);
}

void Bar::syncBarAutoHideInputRegion(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  if (!instanceAcceptsPointerInput(instance)) {
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion({});
    return;
  }
  if (barConfigUsesSlideSurface(instance.barConfig)) {
    const bool fullSurface = instance.pointerInside
        || instance.attachedPopupCount > 0
        || instance.hideOpacity > 0.5F
        || (instance.barConfig.smartAutoHide && instance.smartAutoHidePinnedVisible);
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, fullSurface));
    return;
  }
  const auto barRect = barContentInputRegion(instance.barConfig, m_config->config().shell.shadow, surfW, surfH);
  if (instance.barConfig.deadZone.clickThrough) {
    auto rects = barWidgetsInputRegion(instance, barRect);
    if (instance.clickThroughInputRegion != rects) {
      instance.surface->setInputRegion(rects);
      instance.clickThroughInputRegion = std::move(rects);
    }
    return;
  }
  instance.clickThroughInputRegion.reset();
  instance.surface->setInputRegion({barRect});
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (instance.autoHideDisablePending) {
    return;
  }
  if (!barSupportsSlideBehavior(instance.barConfig) || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.autoHideDelayTimer.stop();
  instance.ipcLayoutReleased = false;
  instance.animations.cancelForOwner(instance.slideRoot);
  const float current = instance.hideOpacity;
  wl_output* output = instance.output;
  const std::string barName = instance.barConfig.name;
  const auto notifyAttachedPanel = [output, barName] {
    PanelManager::instance().onAttachedBarRevealSettled(output, barName);
  };

  constexpr float kSettledThreshold = 0.999F;
  if (current >= kSettledThreshold) {
    const int surfW = static_cast<int>(instance.surface->width());
    const int surfH = static_cast<int>(instance.surface->height());
    instance.clickThroughInputRegion.reset();
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
    syncBarSurfaceChrome(instance);
    instance.surface->requestRedraw();
    notifyAttachedPanel();
    return;
  }

  instance.animations.animate(
      current, 1.0F, Style::animNormal, Easing::EaseOutCubic,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      notifyAttachedPanel, instance.slideRoot
  );
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  instance.clickThroughInputRegion.reset();
  instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::syncBarSlideLayerTransform(BarInstance& instance) const {
  if (instance.slideRoot == nullptr) {
    return;
  }
  if (instance.barConfig.autoHide
      || instance.barConfig.smartAutoHide
      || instance.ipcLayoutReleased
      || instance.hideOpacity < 0.999F) {
    const float t = 1.0F - instance.hideOpacity;
    instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
  } else {
    instance.slideRoot->setPosition(0.0F, 0.0F);
  }
}

void Bar::applyBarCompositorBlur(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  if (!instance.barConfig.compositorBlur || !barContentVisuallyShown(instance)) {
    instance.surface->clearBlurRegion();
    return;
  }

  if (instance.bg == nullptr) {
    return;
  }
  float absX = 0.0F;
  float absY = 0.0F;
  Node::absolutePosition(instance.bg, absX, absY);
  const int px = static_cast<int>(std::lround(absX));
  const int py = static_cast<int>(std::lround(absY));
  const int pw = static_cast<int>(std::lround(std::max(0.0F, instance.bg->width())));
  const int ph = static_cast<int>(std::lround(std::max(0.0F, instance.bg->height())));
  const auto concave = barConcaveShape(instance.barConfig);
  // The bg node is the visual rect; tessellateShape expects the body rect and
  // expands it outward by logicalInset itself. With all-convex corners and no
  // inset it takes its own fast path down to a plain rounded rect.
  const int insetL = static_cast<int>(std::lround(concave.logicalInset.left));
  const int insetT = static_cast<int>(std::lround(concave.logicalInset.top));
  const int insetR = static_cast<int>(std::lround(concave.logicalInset.right));
  const int insetB = static_cast<int>(std::lround(concave.logicalInset.bottom));
  auto blurStrips = Surface::tessellateShape(
      px + insetL, py + insetT, pw - insetL - insetR, ph - insetT - insetB, concave.corners, concave.logicalInset,
      concave.radii
  );
  instance.surface->setBlurRegion(blurStrips);
}

void Bar::startHideFadeOut(BarInstance& instance, bool immediate) {
  if (instance.autoHideDisablePending || instance.smartAutoHidePinnedVisible) {
    return;
  }
  if (!immediate
      && instance.barConfig.autoHide
      && !instance.barConfig.smartAutoHide
      && instance.barConfig.autoHideDelayMs > 0) {
    if (!instance.autoHideDelayTimer.active()) {
      instance.autoHideDelayTimer.start(
          std::chrono::milliseconds(instance.barConfig.autoHideDelayMs), [this, inst = &instance] {
            if (inst->pointerInside || inst->attachedPopupCount > 0 || !barPointerHideAllowed(*inst)) {
              return;
            }
            const bool suppressed =
                (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*inst) : false;
            if (!suppressed) {
              startHideFadeOut(*inst, true);
            }
          }
      );
    }
    return;
  }
  instance.autoHideDelayTimer.stop();
  const float current = instance.hideOpacity;
  instance.animations.animate(
      current, 0.0F, Style::animNormal, Easing::EaseInQuad,
      [this, inst = &instance](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this] {
        if (inst->surface == nullptr) {
          return;
        }
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        inst->surface->requestRedraw();
      },
      instance.slideRoot
  );
  syncBarSurfaceChrome(instance);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

// Compute responsive notch dimensions based on monitor size and widget content
struct NotchDimensions {
  float collapsedWidth;
  float collapsedHeight;
  float expandedWidth;
  float expandedHeight;
  float collapsedRadius;
  float expandedRadius;
};

Bar::NotchDimensions Bar::computeNotchDimensions(const BarInstance& instance, [[maybe_unused]] float progress) const {
  const auto& cfg = instance.barConfig;
  const float outputWidth = static_cast<float>(instance.outputLogicalWidth);
  const float outputHeight = static_cast<float>(instance.outputLogicalHeight);

  // Viewport-responsive clamping — works at all laptop resolutions & fractional scales
  // Collapsed: max 45% of screen width, min 200px
  const float collapsedWidth = std::clamp(
      static_cast<float>(cfg.notchCollapsedWidth),
      200.0F,
      outputWidth * 0.45F
  );
  const float collapsedHeight = static_cast<float>(cfg.notchCollapsedHeight); // fixed 32px

  // Expanded: clamp between 660-900px width, 310-55% screen height
  // Generous horizontal proportions for NotchX × Noctalia Bento Dashboard
  // Enforce minimum 900×360 even if config has stale smaller values.
  const float expandedWidth = std::clamp(
      std::max(static_cast<float>(cfg.notchExpandedWidth), 900.0F),
      660.0F,
      outputWidth - 40.0F
  );
  const float expandedHeight = std::clamp(
      std::max(static_cast<float>(cfg.notchExpandedHeight), 360.0F),
      310.0F,
      outputHeight * 0.55F
  );

  const float collapsedRadius = static_cast<float>(cfg.notchCollapsedRadius);
  const float expandedRadius = static_cast<float>(cfg.notchExpandedRadius);

  return {collapsedWidth, collapsedHeight, expandedWidth, expandedHeight, collapsedRadius, expandedRadius};
}

// Build scene for Dynamic Notch mode - SPLIT INTO INIT + UPDATE
// One-time initialization: creates all UI nodes, mounts widgets.
// The LayerSurface is FIXED at expanded dimensions; pill position/size animate inside it.
// Build Bento Control Center layout inside the notch (called once on init/config reload).
// Creates the full widget tree: header row with horizontal tab pills + status + actions,
// and main 4-column horizontal Bento grid.
// The tree is pre-allocated; per-frame updates only change visibility/bounds/data.
static void buildNotchBentoLayout(BarInstance& instance, [[maybe_unused]] Renderer& renderer, float contentScale) {
  const float headerH = 36.0F * contentScale;
  const float headerY = 12.0F * contentScale;       // 12px from top of pill
  const float gridY = 56.0F * contentScale;          // 12 + 36 + 8 = 56px from top
  const float innerPad = 16.0F * contentScale;       // 16px left/right/bottom padding
  const float colGap = 12.0F * contentScale;         // 12px gap between columns
  const float controlH = Style::controlHeight * contentScale;

  // Root for expanded Bento grid (inside contentClip, positioned each frame)
  instance.notchBentoRoot = instance.contentClip->addChild(ui::node({}));

  // ===== TOP HEADER BAR (y = 12px, height = 36px, full pill width) =====
  auto headerBar = instance.notchBentoRoot->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = 10.0F * contentScale,
          .padding = std::nullopt,
          .paddingV = 10.0F * contentScale,
          .paddingH = innerPad,
      })
  );
  headerBar->setPosition(0.0F, headerY);
  // headerBar width will be set per-frame to pillW

  // ---- LEFT: Horizontal tab pills (Home, Media, Clipboard, Notifications, Files) ----
  instance.notchHeaderTabs = static_cast<Flex*>(headerBar->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .gap = 6.0F * contentScale,
          .paddingH = 8.0F * contentScale,  // Additional horizontal padding for tabs
      })
  ));

  constexpr std::array<std::pair<const char*, const char*>, 5> kTabPills = {{
      {"home", "Home"},
      {"music", "Media"},
      {"clipboard", "Clipboard"},
      {"bell", "Notifications"},
      {"folder", "Files"},
  }};
  instance.notchTabPills.clear();
  instance.notchTabPills.reserve(kTabPills.size());
  for (std::size_t i = 0; i < kTabPills.size(); ++i) {
    const auto& [glyph, label] = kTabPills[i];
    auto btn = static_cast<Button*>(instance.notchHeaderTabs->addChild(
        ui::button({
            .text = label,
            .glyph = glyph,
            .fontSize = Style::fontSizeCaption * contentScale,
            .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
            .controlHeight = headerH,
            .variant = (i == 0) ? ButtonVariant::Primary : ButtonVariant::Ghost,
            .paddingV = 4.0F * contentScale,
            .paddingH = 12.0F * contentScale,
            .radius = 8.0F * contentScale,
            .onClick = [&instance, i]() { instance.notchTabIndex = i; },
        })
    ));
    instance.notchTabPills.push_back(btn);
  }

  // ---- CENTER-RIGHT: Compact status readout (Date & Time + Battery %) ----
  auto statusContainer = static_cast<Flex*>(headerBar->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .gap = 8.0F * contentScale,
      })
  ));
  instance.notchHeaderDateTime = static_cast<Label*>(statusContainer->addChild(
      ui::label({
          .text = "00:00",
          .fontSize = Style::fontSizeCaption * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  instance.notchHeaderBattery = static_cast<Label*>(statusContainer->addChild(
      ui::label({
          .text = "100%",
          .fontSize = Style::fontSizeCaption * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // ---- FAR RIGHT: Circular icon buttons (Power Profile, Pin, Settings, Power Menu) ----
  instance.notchHeaderActions = static_cast<Flex*>(headerBar->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .gap = 4.0F * contentScale,
          .paddingH = 8.0F * contentScale,  // Additional horizontal padding for action buttons
      })
  ));
  const float iconBtnSize = headerH;
  instance.notchPowerProfilesButton = static_cast<Button*>(instance.notchHeaderActions->addChild(
      ui::button({
          .text = "",
          .glyph = "balanced",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = iconBtnSize,
          .variant = ButtonVariant::Ghost,
          .radius = iconBtnSize * 0.5F,
          .width = iconBtnSize,
          .height = iconBtnSize,
          .onClick = [&instance]() { /* TODO: open power profiles */ },
      })
  ));
  instance.notchPinButton = static_cast<Button*>(instance.notchHeaderActions->addChild(
      ui::button({
          .text = "",
          .glyph = "pin",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = iconBtnSize,
          .variant = instance.notchPinned ? ButtonVariant::Primary : ButtonVariant::Ghost,
          .radius = iconBtnSize * 0.5F,
          .width = iconBtnSize,
          .height = iconBtnSize,
          .onClick = [&instance]() {
            instance.notchPinned = !instance.notchPinned;
            if (instance.notchPinButton != nullptr) {
              instance.notchPinButton->setVariant(instance.notchPinned ? ButtonVariant::Primary : ButtonVariant::Ghost);
            }
          },
      })
  ));
  instance.notchSettingsButton = static_cast<Button*>(instance.notchHeaderActions->addChild(
      ui::button({
          .text = "",
          .glyph = "settings",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = iconBtnSize,
          .variant = ButtonVariant::Ghost,
          .radius = iconBtnSize * 0.5F,
          .width = iconBtnSize,
          .height = iconBtnSize,
          .onClick = [&instance]() { /* TODO: open settings */ },
      })
  ));
  instance.notchPowerButton = static_cast<Button*>(instance.notchHeaderActions->addChild(
      ui::button({
          .text = "",
          .glyph = "shutdown",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = iconBtnSize,
          .variant = ButtonVariant::Ghost,
          .radius = iconBtnSize * 0.5F,
          .width = iconBtnSize,
          .height = iconBtnSize,
          .onClick = [&instance]() { /* TODO: open power menu */ },
      })
  ));

  // ===== MAIN 4-COLUMN HORIZONTAL BENTO GRID (y = 56px, height = pillH - 76px, width = pillW - 32px) =====
  // Positioned per-frame in layoutNotchWidgets to track pill bounds.
  // Outer padding: 16px left/right/bottom, 12px top below header (gridY = 56px)
  // Internal gap: 12px between all cards/columns
auto mainContent = instance.notchBentoRoot->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Start,
          .gap = colGap,
          .paddingV = innerPad,
          .paddingH = innerPad,
      })
  );
  instance.notchBentoGrid = static_cast<Flex*>(mainContent);
  mainContent->setPosition(innerPad, gridY);
  // mainContent size set per-frame in layoutNotchWidgets

// Column flexGrow proportions: 0.21, 0.35, 0.24, 0.20 (sum = 1.0)
   // With 3 gaps of 12px = 36px total gap space subtracted from usable width.

   // Helper lambda for native card styling (SurfaceContainer fill, 1px OutlineVariant border, 16px radius)
   auto styleCard = [contentScale](Flex& card) {
     card.setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
     card.setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
     card.setRadius(Style::scaledRadiusXl(contentScale));
     card.setPadding(Style::cardPadding * contentScale);
   };

   // ---- Column 1 (21%): Clock, Date & System Info Card ----
   instance.notchClockCard = static_cast<Flex*>(mainContent->addChild(
       ui::flex(FlexDirection::Vertical, ui::FlexProps{
           .align = FlexAlign::Start,
           .justify = FlexJustify::SpaceBetween,
           .gap = 8.0F * contentScale,
           .flexGrow = 0.21F,
       })
   ));
   styleCard(*instance.notchClockCard);
   instance.notchTimeLabel = static_cast<Label*>(instance.notchClockCard->addChild(
       ui::label({
           .text = "00:00",
           .fontSize = 42.0F * contentScale,
           .fontWeight = FontWeight::Bold,
           .color = colorSpecFromRole(ColorRole::OnSurface),
       })
   ));
   instance.notchDateLabel = static_cast<Label*>(instance.notchClockCard->addChild(
       ui::label({
           .text = "Monday, January 1",
           .fontSize = Style::fontSizeBody * contentScale,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
       })
   ));
   instance.notchSystemInfoLabel = static_cast<Label*>(instance.notchClockCard->addChild(
       ui::label({
           .text = "user@hostname • up 2h 34m • Niri",
           .fontSize = Style::fontSizeCaption * contentScale,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
       })
   ));

   // ---- Column 2 (35%): Full MPRIS Media Player Card ----
   instance.notchMediaCard = static_cast<Flex*>(mainContent->addChild(
       ui::flex(FlexDirection::Vertical, ui::FlexProps{
           .align = FlexAlign::Stretch,
           .gap = 12.0F * contentScale,
           .flexGrow = 0.35F,
       })
   ));
   styleCard(*instance.notchMediaCard);
  // Album art + track info row: 76x76px album art on left, track info on right
  auto mediaTopRow = instance.notchMediaCard->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .gap = 12.0F * contentScale,
      })
  );
  instance.notchMediaArt = static_cast<Image*>(mediaTopRow->addChild(
      ui::image({
          .fit = ImageFit::Cover,
          .radius = 12.0F * contentScale,  // 12px radius
          .width = 68.0F * contentScale,
          .height = 68.0F * contentScale,
      })
  ));
  instance.notchMediaArt->setVisible(false);
  instance.notchMediaArtFallback = static_cast<Glyph*>(mediaTopRow->addChild(
      ui::glyph({
          .glyph = "disc",
          .glyphSize = 38.0F * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  instance.notchMediaArtFallback->setVisible(true);

  auto mediaTextCol = static_cast<Flex*>(mediaTopRow->addChild(
      ui::flex(FlexDirection::Vertical, ui::FlexProps{
          .align = FlexAlign::Start,
          .justify = FlexJustify::Center,
          .gap = 2.0F * contentScale,
          .flexGrow = 1.0F,
      })
  ));
  instance.notchMediaTrack = static_cast<Label*>(mediaTextCol->addChild(
      ui::label({
          .text = "Nothing Playing",
          .fontSize = Style::fontSizeBody * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .ellipsize = TextEllipsize::End,
      })
  ));
  instance.notchMediaArtist = static_cast<Label*>(mediaTextCol->addChild(
      ui::label({
          .text = "",
          .fontSize = Style::fontSizeCaption * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .ellipsize = TextEllipsize::End,
      })
  ));

  // Thin 4px progress scrubber bar (placed between track info and control buttons)
  // Using a custom progress bar that shows album art as background when available
  instance.notchMediaProgressBar = static_cast<ProgressBar*>(instance.notchMediaCard->addChild(
      std::make_unique<ProgressBar>()
  ));
  instance.notchMediaProgressBar->setOrientation(ProgressBarOrientation::Horizontal);
  instance.notchMediaProgressBar->setRadius(2.0F * contentScale);
  instance.notchMediaProgressBar->setFillColor(colorSpecFromRole(ColorRole::Primary));
  instance.notchMediaProgressBar->setTrackColor(colorSpecFromRole(ColorRole::SurfaceContainerHighest));
  instance.notchMediaProgressBar->setProgress(0.0F);
  instance.notchMediaProgressBar->setSize(0.0F, 4.0F * contentScale);
  instance.notchMediaProgressBar->setFlexGrow(1.0F);

  instance.notchMediaProgress = static_cast<Label*>(instance.notchMediaCard->addChild(
      ui::label({
          .text = "0:00 / 0:00",
          .fontSize = Style::fontSizeMini * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::End,
      })
  ));

  // Media control buttons (Previous, Play/Pause, Next) - centered at bottom
  auto mediaControls = instance.notchMediaCard->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = 12.0F * contentScale,
          .paddingV = 4.0F * contentScale,
      })
  );
  instance.notchMediaPrev = static_cast<Button*>(mediaControls->addChild(
      ui::button({
          .text = "",
          .glyph = "media-prev",
          .glyphSize = Style::baseGlyphSize * 1.1F * contentScale,
          .controlHeight = controlH,
          .variant = ButtonVariant::Ghost,
          .radius = Style::scaledRadiusLg(contentScale),
          .onClick = [&instance]() { if (instance.m_mpris) instance.m_mpris->previousActive(); },
      })
  ));
  instance.notchMediaPlayPause = static_cast<Button*>(mediaControls->addChild(
      ui::button({
          .text = "",
          .glyph = "media-play",
          .glyphSize = Style::baseGlyphSize * 1.3F * contentScale,
          .controlHeight = controlH,
          .variant = ButtonVariant::Primary,
          .radius = Style::scaledRadiusLg(contentScale),
          .onClick = [&instance]() { if (instance.m_mpris) instance.m_mpris->playPauseActive(); },
      })
  ));
  instance.notchMediaNext = static_cast<Button*>(mediaControls->addChild(
      ui::button({
          .text = "",
          .glyph = "media-next",
          .glyphSize = Style::baseGlyphSize * 1.1F * contentScale,
          .controlHeight = controlH,
          .variant = ButtonVariant::Ghost,
          .radius = Style::scaledRadiusLg(contentScale),
          .onClick = [&instance]() { if (instance.m_mpris) instance.m_mpris->nextActive(); },
      })
  ));

  // ---- Column 3 (25%): Stacked Speaker + Microphone + Brightness Cards ----
  auto column3 = static_cast<Flex*>(mainContent->addChild(
       ui::flex(FlexDirection::Vertical, ui::FlexProps{
           .align = FlexAlign::Stretch,
           .gap = colGap,
           .flexGrow = 0.25F,
       })
   ));

// Volume Card (Speaker/Output)
  instance.notchVolumeCard = static_cast<Flex*>(column3->addChild(
      ui::flex(FlexDirection::Vertical, ui::FlexProps{
          .align = FlexAlign::Stretch,
          .gap = 4.0F * contentScale,  // Reduced from 8.0F to match brightness slider position
          .padding = std::nullopt,
          .paddingV = 8.0F * contentScale,
          .flexGrow = 1.0F,
      })
  ));
  styleCard(*instance.notchVolumeCard);

  // Header row: Mute button | Volume % | Device name (flex, ellipsis) | Device selector (⋮)
  auto volumeHeader = instance.notchVolumeCard->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = 8.0F * contentScale,
      })
  );

  // Mute button (clickable icon button, toggles mute)
  instance.notchVolumeMuteButton = static_cast<Button*>(volumeHeader->addChild(
      ui::button({
          .text = "",
          .glyph = "volume-high",
          .glyphSize = Style::baseGlyphSize * contentScale,
          .controlHeight = 28.0F * contentScale,
          .variant = ButtonVariant::Ghost,
          .radius = 8.0F * contentScale,
          .width = 28.0F * contentScale,
          .height = 28.0F * contentScale,
      })
  ));
  instance.notchVolumeMuteButton->setOnClick([&instance]() {
    if (instance.m_audio) {
      auto& audio = *instance.m_audio;
      bool newMute = !audio.state().sinks.empty() && !audio.state().sinks[0].muted;
      audio.setSinkMuted(instance.m_audio->state().defaultSinkId, newMute);
    }
  });

  // Volume percentage label
  instance.notchVolumeLabel = static_cast<Label*>(volumeHeader->addChild(
      ui::label({
          .text = "50%",
          .fontSize = Style::fontSizeCaption * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // Device name label (flex=1, minWidth=0, ellipsis) - clickable to open device picker
  instance.notchVolumeSinkLabel = static_cast<Label*>(volumeHeader->addChild(
      ui::label({
          .text = "Built-in Audio",
          .fontSize = Style::fontSizeCaption * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .minWidth = 0.0F,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  ));
  instance.notchVolumeSinkLabel->setOnClick([&instance](const InputArea::PointerData&) {
    if (instance.m_audio) {
      instance.notchSubMenuOpen = true;
      // TODO: open output device picker popup
    }
  });

  // Device selector dropdown button (⋮) - opens device picker
instance.notchVolumeDeviceSelector = static_cast<Button*>(volumeHeader->addChild(
      ui::button({
          .text = "",
          .glyph = "menu",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = 28.0F * contentScale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Select output device",
          .radius = 8.0F * contentScale,
          .width = 28.0F * contentScale,
          .height = 28.0F * contentScale,
          .onClick = [&instance]() {
            if (instance.m_audio) {
              instance.notchSubMenuOpen = true;
            }
          },
      })
  ));

  // Add header row to card

  // Volume slider
  instance.notchVolumeSlider = static_cast<Slider*>(instance.notchVolumeCard->addChild(
      ui::slider({
          .minValue = 0.0,
          .maxValue = 100.0,
          .step = 1.0,
          .value = 50.0,
          .trackHeight = 4.0F * contentScale,
          .thumbSize = 14.0F * contentScale,
          .controlHeight = controlH,
          .flexGrow = 1.0F,
          .onValueChanged = [&instance](double v) {
            if (instance.notchVolumeLabel != nullptr) {
              char buf[16];
              std::snprintf(buf, sizeof(buf), "%.0f%%", v);
              instance.notchVolumeLabel->setText(buf);
            }
            if (instance.m_audio) instance.m_audio->setSinkVolume(instance.m_audio->state().defaultSinkId, static_cast<float>(v) / 100.0F);
          },
          .onDragEnd = [&instance]() { /* Drag end handled by polling dragging() */ },
      })
  ));

// Microphone Card (Input)
  instance.notchMicCard = static_cast<Flex*>(column3->addChild(
      ui::flex(FlexDirection::Vertical, ui::FlexProps{
          .align = FlexAlign::Stretch,
          .gap = 4.0F * contentScale,  // Reduced to match brightness slider position
          .padding = std::nullopt,
          .paddingV = 8.0F * contentScale,
          .flexGrow = 1.0F,
      })
  ));
  styleCard(*instance.notchMicCard);

  // Header row: Mute button | Volume % | Device name (flex, ellipsis) | Device selector (⋮)
  auto micHeader = instance.notchMicCard->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = 8.0F * contentScale,
      })
  );

  // Mute button (clickable icon button, toggles mute)
  instance.notchMicMuteButton = static_cast<Button*>(micHeader->addChild(
      ui::button({
          .text = "",
          .glyph = "microphone",
          .glyphSize = Style::baseGlyphSize * contentScale,
          .controlHeight = 28.0F * contentScale,
          .variant = ButtonVariant::Ghost,
          .radius = 8.0F * contentScale,
          .width = 28.0F * contentScale,
          .height = 28.0F * contentScale,
      })
  ));
  instance.notchMicMuteButton->setOnClick([&instance]() {
    if (instance.m_audio) {
      auto& audio = *instance.m_audio;
      bool newMute = !audio.state().sources.empty() && !audio.state().sources[0].muted;
      audio.setSourceMuted(instance.m_audio->state().defaultSourceId, newMute);
    }
  });

  // Microphone volume percentage label
  instance.notchMicLabel = static_cast<Label*>(micHeader->addChild(
      ui::label({
          .text = "50%",
          .fontSize = Style::fontSizeCaption * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // Device name label (flex=1, minWidth=0, ellipsis) - clickable to open device picker
  instance.notchMicSourceLabel = static_cast<Label*>(micHeader->addChild(
      ui::label({
          .text = "Built-in Microphone",
          .fontSize = Style::fontSizeCaption * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .minWidth = 0.0F,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  ));
  instance.notchMicSourceLabel->setOnClick([&instance](const InputArea::PointerData&) {
    if (instance.m_audio) {
      instance.notchSubMenuOpen = true;
      // TODO: open input device picker popup
    }
  });

// Device selector dropdown button (⋮) - opens device picker
  instance.notchMicDeviceSelector = static_cast<Button*>(micHeader->addChild(
      ui::button({
          .text = "",
          .glyph = "menu",
          .glyphSize = Style::baseGlyphSize * 0.9F * contentScale,
          .controlHeight = 28.0F * contentScale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Select input device",
          .radius = 8.0F * contentScale,
          .width = 28.0F * contentScale,
          .height = 28.0F * contentScale,
          .onClick = [&instance]() {
            if (instance.m_audio) {
              instance.notchSubMenuOpen = true;
            }
          },
      })
  ));

  // Add header row to card

  // Microphone slider
  instance.notchMicSlider = static_cast<Slider*>(instance.notchMicCard->addChild(
      ui::slider({
          .minValue = 0.0,
          .maxValue = 100.0,
          .step = 1.0,
          .value = 50.0,
          .trackHeight = 4.0F * contentScale,
          .thumbSize = 14.0F * contentScale,
          .controlHeight = controlH,
          .flexGrow = 1.0F,
          .onValueChanged = [&instance](double v) {
            if (instance.notchMicLabel != nullptr) {
              char buf[16];
              std::snprintf(buf, sizeof(buf), "%.0f%%", v);
              instance.notchMicLabel->setText(buf);
            }
            if (instance.m_audio) instance.m_audio->setSourceVolume(instance.m_audio->state().defaultSourceId, static_cast<float>(v) / 100.0F);
          },
          .onDragEnd = [&instance]() { /* Drag end handled by polling dragging() */ },
      })
  ));

  // Brightness Card
  instance.notchBrightnessCard = static_cast<Flex*>(column3->addChild(
      ui::flex(FlexDirection::Vertical, ui::FlexProps{
          .align = FlexAlign::Stretch,
          .gap = 8.0F * contentScale,
          .padding = std::nullopt,
          .paddingV = 8.0F * contentScale,
          .flexGrow = 1.0F,
      })
  ));
  styleCard(*instance.notchBrightnessCard);

  // Header row: Sun icon | Brightness %
  auto brightnessHeader = instance.notchBrightnessCard->addChild(
      ui::flex(FlexDirection::Horizontal, ui::FlexProps{
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = 8.0F * contentScale,
      })
  );
  brightnessHeader->addChild(
      ui::glyph({
          .glyph = "sun",
          .glyphSize = Style::baseGlyphSize * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  instance.notchBrightnessLabel = static_cast<Label*>(brightnessHeader->addChild(
      ui::label({
          .text = "50%",
          .fontSize = Style::fontSizeCaption * contentScale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // Brightness slider
  instance.notchBrightnessSlider = static_cast<Slider*>(instance.notchBrightnessCard->addChild(
      ui::slider({
          .minValue = 0.0,
          .maxValue = 100.0,
          .step = 1.0,
          .value = 50.0,
          .trackHeight = 4.0F * contentScale,
          .thumbSize = 14.0F * contentScale,
          .controlHeight = controlH,
          .flexGrow = 1.0F,
          .onValueChanged = [&instance](double v) {
            if (instance.notchBrightnessLabel != nullptr) {
              char buf[16];
              std::snprintf(buf, sizeof(buf), "%.0f%%", v);
              instance.notchBrightnessLabel->setText(buf);
            }
            if (instance.m_brightness) instance.m_brightness->setBrightness(instance.m_brightness->displays()[0].id, static_cast<float>(v) / 100.0F);
          },
          .onDragEnd = [&instance]() { /* Drag end handled by polling dragging() */ },
      })
  ));

  // ---- Column 4 (20%): 2x2 Quick Toggle Tiles (Wi-Fi, Bluetooth, DND, Caffeine) ----
  instance.notchToggleGrid = static_cast<GridView*>(mainContent->addChild(
      std::make_unique<GridView>()
  ));
  instance.notchToggleGrid->setColumns(2);
  instance.notchToggleGrid->setColumnGap(8.0F * contentScale);
  instance.notchToggleGrid->setRowGap(8.0F * contentScale);
  instance.notchToggleGrid->setPadding(innerPad);
  instance.notchToggleGrid->setStretchItems(true);
  instance.notchToggleGrid->setUniformCellSize(true);
  instance.notchToggleGrid->setFlexGrow(0.23F);

auto createToggleTile = [&](const char* glyphOn, const char* glyphOff, const char* label, const char* tooltip,
                              std::function<bool()> getActive, std::function<void(bool)> setActive,
                              Label** labelPtrOut) {
    // Create a vertical flex container for icon + label (stacked vertically)
    auto tileNode = ui::flex(FlexDirection::Vertical, ui::FlexProps{
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = 4.0F * contentScale,
    });
    auto labelNode = static_cast<Label*>(tileNode->addChild(
        ui::label({
            .text = label,
            .fontSize = Style::fontSizeCaption * 0.85F * contentScale,
            .color = getActive() ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface),
        })
    ));
    *labelPtrOut = labelNode;
    // Wrap in a button for click handling - use button's glyph property instead of separate glyph node
    auto btn = static_cast<Button*>(instance.notchToggleGrid->addChild(
        ui::button({
            .text = "",
            .glyph = getActive() ? glyphOn : glyphOff,
            .glyphSize = Style::baseGlyphSize * 1.3F * contentScale,
            .controlHeight = 64.0F * contentScale,  // Taller for vertical stacking
            .variant = getActive() ? ButtonVariant::Ghost : ButtonVariant::Default,
            .tooltip = tooltip,
            .radius = Style::scaledRadiusLg(contentScale),
        })
    ));
    // Add the flex content as button content (icon is now on the button itself)
    btn->addChild(std::move(tileNode));
    // Store label node reference for color updates
    Label* labelPtr = labelNode;
    // Set onClick handler
    btn->setOnClick([&instance, getActive, setActive, btn, glyphOn, glyphOff, labelPtr]() {
      bool newState = !getActive();
      setActive(newState);
      if (btn != nullptr) {
        btn->setVariant(newState ? ButtonVariant::Ghost : ButtonVariant::Default);
        // Update glyph on the button itself
        btn->setGlyph(newState ? glyphOn : glyphOff);
        // Update label color for contrast (onPrimary when active, onSurface when inactive)
        if (labelPtr != nullptr) {
          labelPtr->setColor(newState ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
        }
      }
    });
    return btn;
  };

  // Wi-Fi tile (shows SSID when connected)
  instance.notchWifiToggle = createToggleTile(
      "wifi", "wifi-off", "Wi-Fi", "Toggle Wi-Fi",
      [&instance]() { return instance.m_network ? instance.m_network->state().wirelessEnabled : false; },
      [&instance](bool on) { if (instance.m_network) instance.m_network->setWirelessEnabled(on); },
      &instance.notchWifiLabel
  );
  // Bluetooth tile
  instance.notchBluetoothToggle = createToggleTile(
      "bluetooth", "bluetooth-off", "BT", "Toggle Bluetooth",
      [&instance]() { return instance.m_bluetooth ? instance.m_bluetooth->state().powered : false; },
      [&instance](bool on) { if (instance.m_bluetooth) instance.m_bluetooth->setPowered(on); },
      &instance.notchBluetoothLabel
  );
  // DND tile
  instance.notchDndToggle = createToggleTile(
      "bell-off", "bell", "DND", "Toggle DND",
      [&instance]() { return instance.m_notifications ? instance.m_notifications->doNotDisturb() : false; },
      [&instance](bool on) { if (instance.m_notifications) instance.m_notifications->toggleDoNotDisturb(); },
      &instance.notchDndLabel
  );
  // Caffeine tile
  instance.notchCaffeineToggle = createToggleTile(
      "caffeine-on", "caffeine-off", "Caffeine", "Toggle Caffeine",
      [&instance]() { return instance.m_idleInhibitor ? instance.m_idleInhibitor->enabled() : false; },
      [&instance](bool on) { if (instance.m_idleInhibitor) instance.m_idleInhibitor->setEnabled(on); },
      &instance.notchCaffeineLabel
  );
 }

// Never calls surface->requestSize() after startup (avoids Wayland configure races).
void Bar::initNotchScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::initNotchScene");
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  // Assign service pointers for notch interactivity
  instance.m_mpris = m_mpris;
  instance.m_audio = m_audio;
  instance.m_brightness = m_brightness;
  instance.m_network = m_network;
  instance.m_bluetooth = m_bluetooth;
  instance.m_notifications = m_notifications;
  instance.m_idleInhibitor = m_idleInhibitor;

  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float progress = instance.notchExpandProgress;
  [[maybe_unused]] const auto& cfg = instance.barConfig;

  // Compute responsive notch dimensions
  const NotchDimensions dims = computeNotchDimensions(instance, progress);

  // Interpolate geometry
  const float notchW = lerp(dims.collapsedWidth, dims.expandedWidth, progress);
  const float notchH = lerp(dims.collapsedHeight, dims.expandedHeight, progress);

  // Clamp to viewport
  const float maxW = std::min(notchW, static_cast<float>(instance.outputLogicalWidth - 32));
  const float maxH = std::min(notchH, static_cast<float>(instance.outputLogicalHeight) * 0.55F);

  // NOTE: No surface->requestSize() here — the LayerSurface is fixed at expanded size
  // (maxSurfaceW/maxSurfaceH) set at startup in createInstance(). The pill animates
  // inside this fixed box via position/size changes only.

  // Debug: print computed dimensions
  kLog.debug("Notch: targetExpandedW={} targetExpandedH={} maxSurfaceW={} maxSurfaceH={} progress={} maxW={} maxH={}",
      notchW, notchH, instance.maxSurfaceW, instance.maxSurfaceH, progress, maxW, maxH);

  // Compute initial pill position (centered horizontally, topMargin from top).
  const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
  const float pillY = static_cast<float>(cfg.notchTopMargin);

  // Update input region to current pill rect (surface-local coordinates)
  updateNotchInputRegion(instance, pillX, pillY, maxW, maxH);

  if (instance.sceneRoot != nullptr) {
    return; // Already initialized
  }

  instance.sceneRoot = ui::node({});
  instance.sceneRoot->setAnimationManager(&instance.animations);
  instance.sceneRoot->setSize(w, h);

  auto slide = ui::node({});
  slide->setParticipatesInLayout(false);
  instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

  // Notch background (rounded pill) — positioned dynamically inside fixed surface.
  instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));

  // ===== SET INITIAL BG STYLE WITH PROPER RADII (flat top, rounded bottom) =====
  // Ensures rounded corners from the very first frame, before any hover/updateNotchGeometry call.
{
    const float collapsedRadius = static_cast<float>(cfg.notchCollapsedRadius);
    const Radii notchRadii{0.0F, 0.0F, collapsedRadius, collapsedRadius};
    const CornerShapes notchCorners{
        CornerShape::Convex, CornerShape::Convex, CornerShape::Convex, CornerShape::Convex
    };
    const RoundedRectStyle bgStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, 1.0F),
        .border = Color{0.0F, 0.0F, 0.0F, 0.0F},
        .fillMode = FillMode::Solid,
        .corners = notchCorners,
        .logicalInset = {0, 0, 0, 0},
        .radius = notchRadii,
        .softness = 0.0F,
        .borderWidth = 0.0F,
    };
    instance.bg->setStyle(bgStyle);
    const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
    const float pillY = static_cast<float>(cfg.notchTopMargin);
    instance.bg->setStyle(bgStyle);
    instance.bg->setPosition(pillX, pillY);
    instance.bg->setSize(maxW, maxH);
    // ALWAYS visible in both collapsed and expanded states
    instance.bg->setVisible(true);
    instance.bg->setOpacity(1.0F);
  }

  // Shadow
  if (cfg.shadow && surface_shadow::enabled(cfg.shadow, shadowConfig)) {
    instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(
        ui::box({
            .configure = [](Box& shadow) { shadow.setHitTestVisible(false); },
        })
    ));

    auto leftClip = ui::node({});
    leftClip->setClipChildren(true);
    leftClip->setZIndex(-1);
    instance.shadowLeftClip = instance.slideRoot->addChild(std::move(leftClip));
    instance.shadowLeft = static_cast<Box*>(instance.shadowLeftClip->addChild(ui::box()));

    auto rightClip = ui::node({});
    rightClip->setClipChildren(true);
    rightClip->setZIndex(-1);
    instance.shadowRightClip = instance.slideRoot->addChild(std::move(rightClip));
    instance.shadowRight = static_cast<Box*>(instance.shadowRightClip->addChild(ui::box()));
  }

  // Content clip — clips to pill bounds, positioned dynamically each frame.
  auto contentClip = ui::node({});
  contentClip->setClipChildren(true);
  instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

  // Hover underlay — covers entire surface (for input region purposes).
  auto hoverUnderlay = ui::node({});
  hoverUnderlay->setHitTestVisible(false);
  hoverUnderlay->setSize(w, h);
  instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

// ===== HOVER DETECTION AREA — Place AFTER contentClip/underlay so it's ON TOP in z-order =====
  // This ensures it receives pointer enter/leave/motion events for the entire pill area.
  // acceptedButtons = 0 means it won't consume button clicks/drags, which pass through to child widgets.
  // hitTestVisible = false so it doesn't intercept pointer events from reaching child widgets.
  // Expand/collapse is driven by Wayland pointer events in Bar::onPointerEvent.
  instance.notchHoverArea = static_cast<InputArea*>(instance.slideRoot->addChild(
      ui::inputArea({
          .acceptedButtons = 0,
          .hitTestVisible = false,
      })
  ));
  instance.notchHoverArea->setPosition(pillX, pillY);
  instance.notchHoverArea->setSize(maxW, maxH);

  // Track pointer position within the hover area to prevent false leave events
  // when moving between notchHoverArea and child widgets inside contentClip.
  instance.notchHoverArea->setOnMotion([inst = &instance](const InputArea::PointerData& data) {
    // Update last known pointer position within the notch area
    inst->lastPointerSx = data.localX;
    inst->lastPointerSy = data.localY;
    // Ensure pointerInside stays true while moving within hover area
    inst->notchPointerInside = true;
    inst->notchCollapseTimer.stop();
  });
instance.notchHoverArea->setOnClick([this, inst = &instance](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT && inst->barConfig.notchClickExpand) {
      animateNotchExpand(*inst, !inst->notchExpanded);
    }
  });

  // Create notch content root (will hold collapsed/expanded widgets)
  instance.notchContentRoot = instance.contentClip->addChild(ui::node({}));

  const float contentScale = static_cast<float>(cfg.scale);

  // ===== COLLAPSED PILL: album art (left) + visualizer (right) =====
  auto collapsedContainer = ui::node({});
  collapsedContainer->setParticipatesInLayout(false);
  instance.notchCollapsedContainer = instance.notchContentRoot->addChild(std::move(collapsedContainer));

  // Album art thumbnail (far left, vertically centered, 22x22px, radius 6px).
  instance.notchAlbumArt = static_cast<Image*>(instance.notchCollapsedContainer->addChild(
      ui::image({
          .fit = ImageFit::Cover,
          .radius = 6.0F * contentScale,
          .width = 22.0F * contentScale,
          .height = 22.0F * contentScale,
      })
  ));
  instance.notchAlbumArt->setVisible(false);

  // Fallback music glyph (shown when no album art is available).
  instance.notchFallbackGlyph = static_cast<Glyph*>(instance.notchCollapsedContainer->addChild(
      ui::glyph({
          .glyph = "music",
          .glyphSize = Style::baseGlyphSize * 0.6F * contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  instance.notchFallbackGlyph->setVisible(false);

  // Visualizer bars: 5 thin vertical bars on the far right.
  // Animated at 60fps when MPRIS playback is active (locked to wl_surface::frame).
  // When paused/stopped: rest at flat 4px dots, stop frame callbacks.
  constexpr int kVisualizerBars = 5;
  const float barW = 3.0F * contentScale;
  [[maybe_unused]] const float barGap = 3.0F * contentScale;
  for (int i = 0; i < kVisualizerBars; ++i) {
    Node* barNode = instance.notchCollapsedContainer->addChild(
        ui::box({
            .fill = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .radius = barW * 0.5F,
            .width = barW,
            .height = 4.0F * contentScale,
        })
    );
    instance.notchVisualizerBars.push_back(static_cast<Box*>(barNode));
  }

  // ===== EXPANDED BENTO CONTROL CENTER (built once, updated per-frame) =====
  buildNotchBentoLayout(instance, renderer, contentScale);

  // Set initial opacity for expanded Bento root to 0 (hidden when collapsed)
  if (instance.notchBentoRoot != nullptr) {
    instance.notchBentoRoot->setOpacity(0.0F);
    instance.notchBentoRoot->setVisible(false);
  }

  // Collapsed container is visible initially (collapsed state); expanded sections fade in later.
  if (instance.notchCollapsedContainer != nullptr) {
    instance.notchCollapsedContainer->setVisible(true);
    instance.notchCollapsedContainer->setOpacity(1.0F);
  }

  // Layout all sections once
  instance.notchContentRoot->layout(renderer);

// Position the notch background pill at its dynamic bounds inside the fixed surface.
  // pillX = (maxSurfaceW - currentPillW) * 0.5f (centered horizontally)
  // pillY = topMargin (flush to top edge of surface content area)
  if (instance.bg != nullptr) {
    instance.bg->setPosition(pillX, pillY);
    instance.bg->setSize(maxW, maxH);
  }
  if (instance.contentClip != nullptr) {
    instance.contentClip->setPosition(pillX, pillY);
    instance.contentClip->setSize(maxW, maxH);
  }

  // Wire up InputDispatcher for this instance
  instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
  instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_platform->setCursorShape(serial, shape);
  });

  instance.surface->setSceneRoot(instance.sceneRoot.get());
}


// Per-frame geometry updates: position, size, opacity (no node creation).
// The LayerSurface is FIXED at expanded dimensions — we never call requestSize() here.
// Pill position/size animate inside the fixed box; input region tracks the pill rect.
void Bar::updateNotchGeometry(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::updateNotchGeometry");
  if (m_renderContext == nullptr || instance.surface == nullptr || instance.sceneRoot == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float progress = instance.notchExpandProgress;
  const auto& cfg = instance.barConfig;
  const float contentScale = static_cast<float>(cfg.scale);

  // Compute responsive notch dimensions
  const NotchDimensions dims = computeNotchDimensions(instance, progress);

  // Interpolate geometry
  const float notchW = lerp(dims.collapsedWidth, dims.expandedWidth, progress);
  const float notchH = lerp(dims.collapsedHeight, dims.expandedHeight, progress);
  const float notchR = lerp(dims.collapsedRadius, dims.expandedRadius, progress);

  // Clamp to viewport
  const float maxW = std::min(notchW, static_cast<float>(instance.outputLogicalWidth - 32));
  const float maxH = std::min(notchH, static_cast<float>(instance.outputLogicalHeight) * 0.45F);

  // NOTE: No surface->requestSize() — surface is fixed at maxSurfaceW/maxSurfaceH.
  // The pill animates within this fixed box.

  // Update input region to current pill rect (surface-local coordinates)
  {
    const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
    const float pillY = static_cast<float>(cfg.notchTopMargin);
    updateNotchInputRegion(instance, pillX, pillY, maxW, maxH);
  }

  // Update root size
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  // Update notch background (pill shape).
  // Pill centered horizontally inside fixed surface: pillX = (maxSurfaceW - pillW) * 0.5f
  // pillY = topMargin (flush to top edge when 0).
  // Hardware-style notch: flat top corners (radius 0), rounded bottom corners.
  if (instance.bg != nullptr) {
    const float bottomRadius = notchR;
    const Radii notchRadii{0.0F, 0.0F, bottomRadius, bottomRadius};  // tl=0, tr=0, br=radius, bl=radius
    const CornerShapes notchCorners{
        CornerShape::Convex,  // top-left: flat
        CornerShape::Convex,  // top-right: flat
        CornerShape::Convex,  // bottom-right: rounded
        CornerShape::Convex   // bottom-left: rounded
    };
    const RoundedRectStyle bgStyle{
        .fill = rgba(0.0F, 0.0F, 0.0F, 1.0F),  // Pure black #000000
        .border = Color{0.0F, 0.0F, 0.0F, 0.0F},  // No border
        .fillMode = FillMode::Solid,
        .corners = notchCorners,
        .logicalInset = {0, 0, 0, 0},
        .radius = notchRadii,
        .softness = 0.0F,
        .borderWidth = 0.0F,
    };
    instance.bg->setStyle(bgStyle);
    const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
    const float pillY = static_cast<float>(cfg.notchTopMargin);
    instance.bg->setStyle(bgStyle);
    instance.bg->setPosition(pillX, pillY);
    instance.bg->setSize(maxW, maxH);
// ALWAYS visible in both collapsed and expanded states
  instance.bg->setVisible(true);
  instance.bg->setOpacity(1.0F);
  }

  // Update shadow
  if (instance.shadow != nullptr) {
    applyBarShadowStyle(instance, shadowConfig, w, h);
  }

  // Update content clip to pill bounds.
  if (instance.contentClip != nullptr) {
    const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
    const float pillY = static_cast<float>(cfg.notchTopMargin);
    instance.contentClip->setPosition(pillX, pillY);
    instance.contentClip->setSize(maxW, maxH);
  }

  // Update hover area to exact current pill rect (surface-local coords).
  if (instance.notchHoverArea != nullptr) {
    const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
    const float pillY = static_cast<float>(cfg.notchTopMargin);
    instance.notchHoverArea->setPosition(pillX, pillY);
    instance.notchHoverArea->setSize(maxW, maxH);
  }

  // Layout widgets with cross-fade opacity inside pill bounds.
  const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
  const float pillY = static_cast<float>(cfg.notchTopMargin);
  layoutNotchWidgets(instance, renderer, pillX, pillY, maxW, maxH, progress);

  // Sync blur region to pill bounds
  if (cfg.compositorBlur && instance.bg != nullptr) {
    float absX = 0.0F;
    float absY = 0.0F;
    Node::absolutePosition(instance.bg, absX, absY);
    const int px = static_cast<int>(std::lround(absX));
    const int py = static_cast<int>(std::lround(absY));
    const int pw = static_cast<int>(std::lround(std::max(0.0F, instance.bg->width())));
    const int ph = static_cast<int>(std::lround(std::max(0.0F, instance.bg->height())));
    const CornerShapes blurCorners{
        CornerShape::Convex,  // top-left
        CornerShape::Convex,  // top-right
        CornerShape::Convex,  // bottom-right
        CornerShape::Convex   // bottom-left
    };
    const Radii blurRadii{notchR, notchR, notchR, notchR};
    auto blurStrips = Surface::tessellateShape(
        px, py, pw, ph,
        blurCorners,
        {0, 0, 0, 0},
        blurRadii
    );
    instance.surface->setBlurRegion(blurStrips);
  } else {
    instance.surface->clearBlurRegion();
  }

  // Update palette connection
  instance.paletteConn = paletteChanged().connect([inst = &instance] {
    applyBackgroundPalette(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
}

// Layout notch widgets with cross-fade between collapsed/expanded states
// Calls Widget::layout() (which invokes Widget::doLayout()) so glyphs/labels are
// Per-frame layout for Dynamic Notch: updates visibility, opacity, and positions
// of pre-built collapsed/expanded content. The Bento tree is built once in
// initNotchScene; this function only updates cross-fade and responsive bounds.
void Bar::layoutNotchWidgets(BarInstance& instance, Renderer& renderer, [[maybe_unused]] float pillX, [[maybe_unused]] float pillY, float pillW, float pillH, float progress) {
  if (instance.notchContentRoot == nullptr) return;

  const float contentScale = static_cast<float>(instance.barConfig.scale);

  const bool collapsed = progress < 0.35F;
  const float collapsedOpacity = collapsed ? (1.0F - progress / 0.35F) : 0.0F;
  const float expandedOpacity = progress > 0.5F ? ((progress - 0.5F) / 0.5F) : 0.0F;

  // Pill content bounds (contentClip is at pillX, pillY with size maxW, maxH).
  // Coordinates here are relative to contentClip origin (0,0 = pill top-left).

  // ---------- Collapsed pill: album art (left) + visualizer (right) ----------
  if (instance.notchCollapsedContainer != nullptr) {
    instance.notchCollapsedContainer->setVisible(collapsed || collapsedOpacity > 0.0F);
    instance.notchCollapsedContainer->setOpacity(collapsedOpacity);
    instance.notchCollapsedContainer->setPosition(0.0F, 0.0F);
    instance.notchCollapsedContainer->setSize(pillW, pillH);
  }

  const float artSize = 20.0F * contentScale;
  const float artRadius = 5.0F * contentScale;
  const float artX = 10.0F * contentScale;
  const float artY = (pillH - artSize) * 0.5F;
  const bool artVisible = collapsed && collapsedOpacity > 0.0F &&
      instance.notchAlbumArt != nullptr && instance.notchAlbumArt->hasImage();
  const bool glyphVisible = collapsed && collapsedOpacity > 0.0F && !artVisible;

  if (instance.notchAlbumArt != nullptr) {
    instance.notchAlbumArt->setVisible(artVisible);
    instance.notchAlbumArt->setPosition(artX, artY);
    instance.notchAlbumArt->setSize(artSize, artSize);
    instance.notchAlbumArt->setRadius(artRadius);
  }
  if (instance.notchFallbackGlyph != nullptr) {
    instance.notchFallbackGlyph->setVisible(glyphVisible);
    instance.notchFallbackGlyph->setPosition(artX, artY);
  }

  // Visualizer bars: far right, vertically centered.
  // 5 bars, 3px wide, 3px gap, maxHeight 16px, radius 1.5px, 10px from right edge.
  if (!instance.notchVisualizerBars.empty()) {
    constexpr int kVisualizerBars = 5;
    const float barW = 3.0F * contentScale;
    const float barGap = 3.0F * contentScale;
    const float totalVisW = kVisualizerBars * barW + (kVisualizerBars - 1) * barGap;
    const float visX = pillW - 10.0F * contentScale - totalVisW;
    const float visY = (pillH - 16.0F * contentScale) * 0.5F; // centered for max 16px height
    const bool visVisible = collapsed && collapsedOpacity > 0.0F;
    for (std::size_t i = 0; i < kVisualizerBars && i < instance.notchVisualizerBars.size(); ++i) {
      Box* bar = instance.notchVisualizerBars[i];
      bar->setVisible(visVisible);
      bar->setPosition(visX + static_cast<float>(i) * (barW + barGap), visY);
      bar->setSize(barW, 4.0F * contentScale); // base height; animation will scale up to 16px
      bar->setRadius(barW * 0.5F);
    }
  }

  // ---------- Expanded Bento Control Center ----------
  if (!collapsed) {
    // Show Bento root, hide collapsed container
    if (instance.notchBentoRoot != nullptr) {
      instance.notchBentoRoot->setVisible(true);
      instance.notchBentoRoot->setOpacity(expandedOpacity);
    }
    if (instance.notchCollapsedContainer != nullptr) {
      instance.notchCollapsedContainer->setVisible(false);
    }

    // Position and size header bar (full pill width, 36px height at y=12px)
    if (instance.notchHeaderTabs != nullptr && instance.notchHeaderTabs->parent() != nullptr) {
      instance.notchHeaderTabs->parent()->setSize(pillW, 36.0F * contentScale);
    }

    // Position and size main content grid (pillW - 32px width, pillH - 72px height, at 16px from left, 56px from top)
    if (instance.notchBentoGrid != nullptr) {
      // pillH - headerH(36) - headerGap(8) - topPad(16) - bottomPad(16) = pillH - 72
      // But we need 16px clear bottom padding for 24px rounded corners
      // So: pillH - headerH(36) - headerGap(8) - topPad(16) - bottomPad(16) = pillH - 76
      instance.notchBentoGrid->setSize(pillW - 32.0F * contentScale, pillH - 76.0F * contentScale);
    }

    // The Bento grid uses Flex/GridView layout — just trigger a layout pass
    // with the current pill bounds so it reflows responsively.
    if (instance.notchBentoRoot != nullptr) {
      instance.notchBentoRoot->layout(renderer);
    }
  } else {
    // Collapsed: hide Bento root, show collapsed container
    if (instance.notchBentoRoot != nullptr) {
      instance.notchBentoRoot->setVisible(false);
      instance.notchBentoRoot->setOpacity(0.0F);
    }
    if (instance.notchCollapsedContainer != nullptr) {
      instance.notchCollapsedContainer->setVisible(collapsedOpacity > 0.0F);
      instance.notchCollapsedContainer->setOpacity(collapsedOpacity);
    }
  }
}

void Bar::buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  // Handle Dynamic Notch mode separately
  if (instance.isNotchMode) {
    initNotchScene(instance, width, height);
    return;
  }

  uiAssertNotRendering("Bar::buildScene");
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto shadowOffset = shadowDirectionOffset(shadowConfig.direction);
  const float shadowSize = surface_shadow::enabled(instance.barConfig.shadow, shadowConfig)
      ? static_cast<float>(surface_shadow::kBlurRadius)
      : 0.0F;
  const auto shadowOffsetX = static_cast<float>(shadowOffset.x);
  const auto shadowOffsetY = static_cast<float>(shadowOffset.y);
  const bool isBottom = instance.barConfig.position == "bottom";
  const bool isRight = instance.barConfig.position == "right";
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto concave = barConcaveShape(instance.barConfig);

  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaX = barVisual.x;
  const float barAreaY = barVisual.y;
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = ui::node({});
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);

    auto slide = ui::node({});
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Bar background
    instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));

    // Shadow — bar shape copy rendered with large SDF softness to simulate a blurred drop shadow.
    if (shadowSize > 0.0F) {
      instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [](Box& shadow) { shadow.setHitTestVisible(false); },
          })
      ));

      auto leftClip = ui::node({});
      leftClip->setClipChildren(true);
      leftClip->setZIndex(-1);
      instance.shadowLeftClip = instance.slideRoot->addChild(std::move(leftClip));
      instance.shadowLeft = static_cast<Box*>(instance.shadowLeftClip->addChild(ui::box()));

      auto rightClip = ui::node({});
      rightClip->setClipChildren(true);
      rightClip->setZIndex(-1);
      instance.shadowRightClip = instance.slideRoot->addChild(std::move(rightClip));
      instance.shadowRight = static_cast<Box*>(instance.shadowRightClip->addChild(ui::box()));
    }
    // Note: shadow is inserted before bar sections so it renders below them (z=-1 is set below).

    auto contentClip = ui::node({});
    contentClip->setClipChildren(true);
    instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

    auto hoverUnderlay = ui::node({});
    hoverUnderlay->setHitTestVisible(false);
    hoverUnderlay->setSize(static_cast<float>(w), static_cast<float>(h));
    instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

    auto makeSlot = [&instance] {
      auto slot = ui::node({});
      slot->setClipChildren(true);
      return instance.contentClip->addChild(std::move(slot));
    };
    instance.startSlot = makeSlot();
    instance.centerSlot = makeSlot();
    instance.endSlot = makeSlot();

    // Create section boxes
    auto makeSection = [widgetSpacing, isVertical] {
      auto section = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = widgetSpacing,
          }
      );
      section->setMirrorInRtl(false);
      return section;
    };

    instance.startSection = static_cast<Flex*>(instance.startSlot->addChild(makeSection()));
    instance.centerSection = static_cast<Flex*>(instance.centerSlot->addChild(makeSection()));
    instance.endSection = static_cast<Flex*>(instance.endSlot->addChild(makeSection()));

    attachWidgetsToSections(instance);

    // Wire up InputDispatcher for this instance
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([this, inst = &instance](InputArea* /*old*/, InputArea* next) {
      if (next != nullptr) {
        next->setTooltipPlacement(tooltipPlacementAwayFromEdge(inst->barConfig.position));
      }
      TooltipManager::instance().onBarHoverChange(
          tooltipAreaForHover(*inst, next), inst->surface->layerSurface(), inst->output
      );
      updateWidgetHoverHighlight(*inst, next);
      updateWidgetPeek(*inst, next);
      updateAccordionExpansion(*inst, next);
    });

    if (instance.barConfig.smartAutoHide && m_platform != nullptr) {
      instance.smartAutoHidePinnedVisible = smartAutoHideWantsPinnedVisible(*m_platform, instance.output);
    }
    if (barConfigUsesSlideSurface(instance.barConfig)) {
      instance.slideRoot->setOpacity(1.0F);
      const bool startHidden =
          instance.barConfig.smartAutoHide ? !instance.smartAutoHidePinnedVisible : instance.barConfig.autoHide;
      instance.hideOpacity = startHidden ? 0.0F : 1.0F;
    } else {
      instance.slideRoot->setOpacity(1.0F);
      instance.hideOpacity = 1.0F;
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    const RoundedRectStyle bgStyle{
        .fill = colorForRole(ColorRole::Surface, instance.barConfig.backgroundOpacity),
        .border = resolveColorSpec(instance.barConfig.border),
        .fillMode = FillMode::Solid,
        .corners = concave.corners,
        .logicalInset = concave.logicalInset,
        .radius = concave.radii,
        .softness = 0.0F,
        .borderWidth = instance.barConfig.borderWidth,
    };
    instance.bg->setStyle(bgStyle);
    // (barAreaX/Y/W/H) is the body; the shader expands outward by logicalInset into
    // the visual rect, so the node must be sized to the visual rect.
    instance.bg->setPosition(barAreaX - concave.logicalInset.left, barAreaY - concave.logicalInset.top);
    instance.bg->setSize(
        barAreaW + concave.logicalInset.left + concave.logicalInset.right,
        barAreaH + concave.logicalInset.top + concave.logicalInset.bottom
    );
  }

  instance.paletteConn = paletteChanged().connect([inst = &instance] {
    applyBackgroundPalette(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
  if (instance.contentClip != nullptr) {
    instance.contentClip->setPosition(barAreaX, barAreaY);
    instance.contentClip->setSize(barAreaW, barAreaH);
  }

  applyBarShadowStyle(instance, shadowConfig, w, h);

  layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);

  float contentLeft = barAreaX;
  float contentTop = barAreaY;
  float contentRight = barAreaX + barAreaW;
  float contentBottom = barAreaY + barAreaH;
  if (instance.shadow != nullptr) {
    const float sx = barAreaX + shadowOffsetX;
    const float sy = barAreaY + shadowOffsetY;
    contentLeft = std::min(contentLeft, sx);
    contentTop = std::min(contentTop, sy);
    contentRight = std::max(contentRight, sx + barAreaW);
    contentBottom = std::max(contentBottom, sy + barAreaH);
  }
  // Concave spikes extend past the body on the inner edge; include them so the bar
  // slides fully off-screen when hidden.
  contentLeft -= concave.logicalInset.left;
  contentTop -= concave.logicalInset.top;
  contentRight += concave.logicalInset.right;
  contentBottom += concave.logicalInset.bottom;
  const auto hiddenDelta = computeAutoHideHiddenDelta(
      isVertical, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom
  );
  instance.slideHiddenDx = hiddenDelta.first;
  instance.slideHiddenDy = hiddenDelta.second;
  syncBarSlideLayerTransform(instance);

  // A rebuilt scene may sit on a new surface, so always send the region once.
  instance.clickThroughInputRegion.reset();
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
}

void Bar::updateWidgets(BarInstance& instance) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  auto updateSection = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
    for (auto& widget : widgets) {
      if (widget->root() == nullptr) {
        continue;
      }
      widget->update(renderer);
      widget->layout(renderer, barAreaW, barAreaH);
    }
  };

  updateSection(instance.startWidgets);
  updateSection(instance.centerWidgets);
  updateSection(instance.endWidgets);
  layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);
  if (instance.barConfig.deadZone.clickThrough) {
    syncBarAutoHideInputRegion(instance);
  }
}

// Prepare frame for Dynamic Notch mode
void Bar::prepareNotchFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  // Initialize scene on first frame
  if (instance.sceneRoot == nullptr) {
    initNotchScene(instance, instance.surface->width(), instance.surface->height());
    // Request initial frame tick to ensure the surface is mapped and receives pointer events
    if (instance.surface != nullptr) {
      instance.surface->requestFrameTick();
    }
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  // Compute current pill dimensions for widget layout constraints.
  const float progress = instance.notchExpandProgress;
  const NotchDimensions dims = computeNotchDimensions(instance, progress);
  const float notchW = lerp(dims.collapsedWidth, dims.expandedWidth, progress);
  const float notchH = lerp(dims.collapsedHeight, dims.expandedHeight, progress);
  [[maybe_unused]] const float maxW = std::min(notchW, static_cast<float>(instance.outputLogicalWidth - 32));
  [[maybe_unused]] const float maxH = std::min(notchH, static_cast<float>(instance.outputLogicalHeight) * 0.45F);

  // Request frame tick during animation or when visualizer is playing
  if (instance.surface != nullptr && (instance.notchAnimationId != 0 || instance.notchMediaPlaying)) {
    instance.surface->requestFrameTick();
  }

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);

    // Track MPRIS media state for collapsed-pill visualizer + album art + Bento media card.
    bool mediaPlaying = false;
    std::string artUrl;
    std::string trackTitle;
    std::string trackArtist;
    std::int64_t positionUs = 0;
    std::int64_t lengthUs = 0;
    if (m_mpris != nullptr) {
      if (auto player = m_mpris->activePlayer()) {
        mediaPlaying = player->playbackStatus == "Playing";
        artUrl = player->artUrl;
        trackTitle = player->title;
        trackArtist = joinedArtists(player->artists);
        positionUs = player->positionUs;
        lengthUs = player->lengthUs;
      }
    }

    // Load album art when it changes (texture allocation only on track change, not per-frame).
    if (instance.notchAlbumArt != nullptr && artUrl != instance.notchLastArtUrl) {
      instance.notchAlbumArt->clear(renderer);
      if (!artUrl.empty()) {
        instance.notchAlbumArt->setSourceFile(renderer, artUrl, 64, true, true);
      }
      instance.notchLastArtUrl = artUrl;
    }
    // Also update expanded Bento media card album art
    if (instance.notchMediaArt != nullptr && artUrl != instance.notchLastArtUrl) {
      instance.notchMediaArt->clear(renderer);
      if (!artUrl.empty()) {
        instance.notchMediaArt->setSourceFile(renderer, artUrl, 128, true, true);
      }
    }

    // Update Bento media card labels
    if (instance.notchMediaTrack != nullptr && !trackTitle.empty()) {
      instance.notchMediaTrack->setText(trackTitle);
    }
    if (instance.notchMediaArtist != nullptr && !trackArtist.empty()) {
      instance.notchMediaArtist->setText(trackArtist);
    }
    // Update media progress scrubber bar and timestamp
    if (instance.notchMediaProgressBar != nullptr && lengthUs > 0) {
      float progress01 = lengthUs > 0 ? static_cast<float>(positionUs) / static_cast<float>(lengthUs) : 0.0F;
      progress01 = std::clamp(progress01, 0.0F, 1.0F);
      instance.notchMediaProgressBar->setProgress(progress01);
    }
    if (instance.notchMediaProgress != nullptr && lengthUs > 0) {
      auto formatTime = [](std::int64_t us) -> std::string {
        std::int64_t secs = us / 1000000;
        std::int64_t mins = secs / 60;
        secs %= 60;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs));
        return std::string(buf);
      };
      std::string posStr = formatTime(positionUs);
      std::string lenStr = formatTime(lengthUs);
      instance.notchMediaProgress->setText(posStr + " / " + lenStr);
    }
    // Show/hide album art vs fallback in Bento media card
    if (instance.notchMediaArt != nullptr && instance.notchMediaArtFallback != nullptr) {
      bool hasArt = instance.notchMediaArt->hasImage();
      instance.notchMediaArt->setVisible(hasArt);
      instance.notchMediaArtFallback->setVisible(!hasArt);
    }

    // Start/stop visualizer animation based on playback state.
    if (mediaPlaying != instance.notchMediaPlaying) {
      instance.notchMediaPlaying = mediaPlaying;
      if (mediaPlaying) {
        startNotchVisualizerAnimation(instance);
      } else {
        stopNotchVisualizerAnimation(instance);
      }
    }

    // Update header date/time, battery, and pin button
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&now_c, &tm);
    char timeBuf[16], dateBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tm);
    std::strftime(dateBuf, sizeof(dateBuf), "%a %b %d", &tm);
    if (instance.notchHeaderDateTime != nullptr) {
      instance.notchHeaderDateTime->setText(std::string(timeBuf) + "  " + std::string(dateBuf));
    }
    // TODO: Update battery percentage from UPower
    if (instance.notchHeaderBattery != nullptr) {
      instance.notchHeaderBattery->setText("100%");
    }
    // Update pin button variant
    if (instance.notchPinButton != nullptr) {
      instance.notchPinButton->setVariant(instance.notchPinned ? ButtonVariant::Primary : ButtonVariant::Ghost);
    }

    // Update clock/date in Bento clock card (Column 1)
    if (instance.notchTimeLabel != nullptr || instance.notchDateLabel != nullptr) {
      if (instance.notchTimeLabel != nullptr) instance.notchTimeLabel->setText(timeBuf);
      if (instance.notchDateLabel != nullptr) instance.notchDateLabel->setText(dateBuf);
    }

    // Update system info in Column 1 (username@hostname • uptime • compositor)
    if (instance.notchSystemInfoLabel != nullptr) {
      // Get username
      std::string username = "user";
      if (auto* pw = getpwuid(getuid())) {
        username = pw->pw_name;
      } else if (const char* envUser = getenv("USER")) {
        username = envUser;
      }

      // Get hostname
      char hostname[256] = {0};
      std::string hostnameStr = "hostname";
      if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostnameStr = hostname;
      }

      // Get uptime
      std::string uptimeStr = "up ?";
      std::ifstream uptimeFile("/proc/uptime");
      if (uptimeFile) {
        double uptimeSeconds;
        uptimeFile >> uptimeSeconds;
        int64_t totalSecs = static_cast<int64_t>(uptimeSeconds);
        int64_t days = totalSecs / 86400;
        int64_t hours = (totalSecs % 86400) / 3600;
        int64_t mins = (totalSecs % 3600) / 60;
        std::ostringstream oss;
        if (days > 0) oss << days << "d ";
        if (hours > 0 || days > 0) oss << hours << "h ";
        oss << mins << "m";
        uptimeStr = "up " + oss.str();
      }

      // Get compositor name
      std::string compositor = "Niri"; // Default

      std::string sysInfo = username + "@" + hostnameStr + " • " + uptimeStr + " • " + compositor;
      instance.notchSystemInfoLabel->setText(sysInfo);
    }

    // Update collapsed-pill element visibility/positions every frame (not just in layout phase).
    // Ensures the music glyph and visualizer bars stay visible when collapsed.
    {
      const bool collapsed = progress < 0.35F;
      const float collapsedOpacity = collapsed ? (1.0F - progress / 0.35F) : 0.0F;
      const float expandedOpacity = progress > 0.5F ? ((progress - 0.5F) / 0.5F) : 0.0F;
      const float contentScale = static_cast<float>(instance.barConfig.scale);
      const float pillW = std::max(0.0F, notchW);
      const float pillH = std::max(0.0F, notchH);

      if (instance.notchCollapsedContainer != nullptr) {
        instance.notchCollapsedContainer->setVisible(collapsed || collapsedOpacity > 0.0F);
        instance.notchCollapsedContainer->setOpacity(collapsedOpacity);
        instance.notchCollapsedContainer->setPosition(0.0F, 0.0F);
        instance.notchCollapsedContainer->setSize(pillW, pillH);
      }

      // Album art visible only when it has a loaded texture; fallback glyph otherwise.
      const float artSize = 20.0F * contentScale;
      const float artRadius = 5.0F * contentScale;
      const float artX = 10.0F * contentScale;
      const float artY = (pillH - artSize) * 0.5F;
      const bool artVisible = collapsed && collapsedOpacity > 0.0F &&
          instance.notchAlbumArt != nullptr && instance.notchAlbumArt->hasImage();
      const bool glyphVisible = collapsed && collapsedOpacity > 0.0F && !artVisible;

      if (instance.notchAlbumArt != nullptr) {
        instance.notchAlbumArt->setVisible(artVisible);
        instance.notchAlbumArt->setPosition(artX, artY);
        instance.notchAlbumArt->setSize(artSize, artSize);
        instance.notchAlbumArt->setRadius(artRadius);
      }
      if (instance.notchFallbackGlyph != nullptr) {
        instance.notchFallbackGlyph->setVisible(glyphVisible);
        instance.notchFallbackGlyph->setPosition(artX, artY);
      }

      // Visualizer bars: far right, vertically centered, 10px from right edge.
      // 5 bars, 3px wide, 3px gap, maxHeight 16px, radius 1.5px.
      if (!instance.notchVisualizerBars.empty()) {
        constexpr int kVisualizerBars = 5;
        const float barW = 3.0F * contentScale;
        const float barGap = 3.0F * contentScale;
        const float totalVisW = kVisualizerBars * barW + (kVisualizerBars - 1) * barGap;
        const float visX = pillW - 10.0F * contentScale - totalVisW;
        const float visY = (pillH - 16.0F * contentScale) * 0.5F; // centered for max 16px
        const bool visVisible = collapsed && collapsedOpacity > 0.0F;
        for (std::size_t i = 0; i < kVisualizerBars && i < instance.notchVisualizerBars.size(); ++i) {
          Box* bar = instance.notchVisualizerBars[i];
          bar->setVisible(visVisible);
          bar->setPosition(visX + static_cast<float>(i) * (barW + barGap), visY);
          bar->setSize(barW, 4.0F * contentScale); // base height; animation scales up
          bar->setRadius(barW * 0.5F);
        }
      }

      // Cross-fade Bento root opacity
      if (instance.notchBentoRoot != nullptr) {
        instance.notchBentoRoot->setOpacity(expandedOpacity);
        instance.notchBentoRoot->setVisible(!collapsed);
      }
    }

    // Sync toggle tile states from services (live startup states)
    if (instance.notchWifiToggle != nullptr && instance.m_network) {
      bool wifiActive = instance.m_network->state().wirelessEnabled;
      instance.notchWifiToggle->setVariant(wifiActive ? ButtonVariant::Primary : ButtonVariant::Default);
      instance.notchWifiToggle->setGlyph(wifiActive ? "wifi" : "wifi-off");
      if (instance.notchWifiLabel != nullptr) {
        instance.notchWifiLabel->setColor(wifiActive ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
      }
    }
    if (instance.notchBluetoothToggle != nullptr && instance.m_bluetooth) {
      bool btActive = instance.m_bluetooth->state().powered;
      instance.notchBluetoothToggle->setVariant(btActive ? ButtonVariant::Primary : ButtonVariant::Default);
      instance.notchBluetoothToggle->setGlyph(btActive ? "bluetooth" : "bluetooth-off");
      if (instance.notchBluetoothLabel != nullptr) {
        instance.notchBluetoothLabel->setColor(btActive ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
      }
    }
    if (instance.notchDndToggle != nullptr && instance.m_notifications) {
      bool dndActive = instance.m_notifications->doNotDisturb();
      instance.notchDndToggle->setVariant(dndActive ? ButtonVariant::Primary : ButtonVariant::Default);
      instance.notchDndToggle->setGlyph(dndActive ? "bell-off" : "bell");
      if (instance.notchDndLabel != nullptr) {
        instance.notchDndLabel->setColor(dndActive ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
      }
    }
    if (instance.notchCaffeineToggle != nullptr && instance.m_idleInhibitor) {
      bool caffeineActive = instance.m_idleInhibitor->enabled();
      instance.notchCaffeineToggle->setVariant(caffeineActive ? ButtonVariant::Primary : ButtonVariant::Default);
      instance.notchCaffeineToggle->setGlyph(caffeineActive ? "caffeine-on" : "caffeine-off");
      if (instance.notchCaffeineLabel != nullptr) {
        instance.notchCaffeineLabel->setColor(caffeineActive ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::OnSurface));
      }
    }

    // Sync microphone mute state and volume
    if (instance.notchMicMuteButton != nullptr && instance.m_audio) {
      bool micMute = instance.m_audio->state().sources.empty() || instance.m_audio->state().sources[0].muted;
      instance.notchMicMuteButton->setGlyph(micMute ? "microphone-off" : "microphone");
      instance.notchMicMuteButton->setVariant(micMute ? ButtonVariant::Default : ButtonVariant::Ghost);
    }
    if (instance.notchMicLabel != nullptr && instance.m_audio) {
      float micVol = instance.m_audio->state().sources.empty() ? 0.0F : instance.m_audio->state().sources[0].volume;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.0f%%", micVol * 100.0F);
      instance.notchMicLabel->setText(buf);
    }
    if (instance.notchMicSlider != nullptr && instance.m_audio) {
      float micVol = instance.m_audio->state().sources.empty() ? 0.0F : instance.m_audio->state().sources[0].volume;
      instance.notchMicSlider->setValue(micVol * 100.0);
    }

    // Sync speaker mute state and volume
    if (instance.notchVolumeMuteButton != nullptr && instance.m_audio) {
      bool volMute = instance.m_audio->state().sinks.empty() || instance.m_audio->state().sinks[0].muted;
      instance.notchVolumeMuteButton->setGlyph(volMute ? "volume-off" : "volume-high");
      instance.notchVolumeMuteButton->setVariant(volMute ? ButtonVariant::Default : ButtonVariant::Ghost);
    }
    if (instance.notchVolumeLabel != nullptr && instance.m_audio) {
      float vol = instance.m_audio->state().sinks.empty() ? 0.0F : instance.m_audio->state().sinks[0].volume;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.0f%%", vol * 100.0F);
      instance.notchVolumeLabel->setText(buf);
    }
    if (instance.notchVolumeSlider != nullptr && instance.m_audio) {
      float vol = instance.m_audio->state().sinks.empty() ? 0.0F : instance.m_audio->state().sinks[0].volume;
      instance.notchVolumeSlider->setValue(vol * 100.0);
    }

    return;
  }

  if (!needsLayout) {
    return;
  }

  // Update notch geometry on layout changes (size/geometry)
  updateNotchGeometry(instance, instance.surface->width(), instance.surface->height());
}

void Bar::prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  // Handle Dynamic Notch mode
  if (instance.isNotchMode) {
    prepareNotchFrame(instance, needsUpdate, needsLayout);
    return;
  }

  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateWidgets(instance);
    return;
  }

  if (!needsLayout) {
    return;
  }

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    for (auto& widget : instance.startWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.centerWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.endWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    layoutBarSections(instance, renderer, barAreaW, barAreaH, padding, isVertical);
  }
  // Click-through input follows the widgets, which move whenever a widget resizes.
  if (instance.barConfig.deadZone.clickThrough) {
    syncBarAutoHideInputRegion(instance);
  }
}

bool Bar::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  BarInstance* targetInstance = nullptr;
  if (event.surface != nullptr) {
    targetInstance = instanceForSurface(event.surface);
  } else {
    targetInstance = m_hoveredInstance;
  }

  auto routeWidgetPopups = [&](BarInstance& instance) {
    auto routeGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget != nullptr && widget->onPointerEvent(event)) {
          return true;
        }
      }
      return false;
    };
    return routeGroup(instance.startWidgets) || routeGroup(instance.centerWidgets) || routeGroup(instance.endWidgets);
  };
  if (targetInstance != nullptr) {
    if (!instanceAcceptsPointerInput(*targetInstance)) {
      clearInstancePointerState(*targetInstance);
      return false;
    }
    if (routeWidgetPopups(*targetInstance)) {
      return true;
    }
  } else {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && instanceAcceptsPointerInput(*instance) && routeWidgetPopups(*instance)) {
        return true;
      }
    }
  }

  if (targetInstance != nullptr && targetInstance->attachedPopupCount > 0) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_hoveredInstance = targetInstance;
      targetInstance->pointerInside = true;
      break;
    case PointerEvent::Type::Leave:
      targetInstance->pointerInside = false;
      if (m_hoveredInstance == targetInstance) {
        m_hoveredInstance = nullptr;
      }
      break;
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.pressed) {
        const auto sx = static_cast<float>(event.sx);
        const auto sy = static_cast<float>(event.sy);
        if (!handleBarDeadZoneButton(*targetInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
          openPanelAtBarPointer(
              *targetInstance, sx, sy, m_platform, targetInstance->barConfig.name, "control-center", "home", true
          );
        }
        return true;
      }
      break;
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    BarInstance* const entered = m_hoveredInstance;
    entered->lastPointerSx = static_cast<float>(event.sx);
    entered->lastPointerSy = static_cast<float>(event.sy);
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    if (barSupportsSlideBehavior(m_hoveredInstance->barConfig) && m_hoveredInstance->sceneRoot != nullptr) {
      revealAutoHideBar(*m_hoveredInstance);
    }
    // Notch-specific: drive expand on pointer enter for Dynamic Notch mode.
    if (entered->isNotchMode && entered->barConfig.notchHoverExpand && !isFullscreenFocused()) {
      animateNotchExpand(*entered, true);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();
      updateAccordionExpansion(*m_hoveredInstance, nullptr);
      updateWidgetPeek(*m_hoveredInstance, nullptr);
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*m_hoveredInstance) : false;
      if (barPointerHideAllowed(*m_hoveredInstance) && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    // Notch-specific: drive collapse on pointer leave for Dynamic Notch mode.
    if (targetInstance != nullptr && targetInstance->isNotchMode) {
      targetInstance->pointerInside = false;
      // 180ms grace timer before collapsing — prevents flicker on accidental cursor slip
      if (targetInstance->barConfig.notchCollapseDelayMs > 0) {
        targetInstance->notchCollapseTimer.start(
            std::chrono::milliseconds(targetInstance->barConfig.notchCollapseDelayMs),
            [this, targetInstance]() {
              if (!targetInstance->notchPinned && !targetInstance->pointerInside && targetInstance->isNotchMode && targetInstance->notchExpanded &&
                  !targetInstance->notchSliderActive && !targetInstance->notchSubMenuOpen) {
                animateNotchExpand(*targetInstance, false);
              }
            }
        );
      } else {
        if (!targetInstance->notchPinned && !targetInstance->notchSliderActive && !targetInstance->notchSubMenuOpen) {
          animateNotchExpand(*targetInstance, false);
        }
      }
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr) {
      break;
    }
    BarInstance* const hovered = m_hoveredInstance;
    hovered->lastPointerSx = static_cast<float>(event.sx);
    hovered->lastPointerSy = static_cast<float>(event.sy);
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    updateAccordionExpansion(*hovered, hovered->inputDispatcher.hoveredArea());
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr) {
      break;
    }
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    bool pressed = event.pressed;
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(
        sx, sy, event.button, pressed, event.serial, event.time, event.touch
    );
    if (pressed
        && !consumed
        && handleBarDeadZoneButton(*m_hoveredInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
      consumed = true;
    }

    break;
  }
  case PointerEvent::Type::Axis: {
    if (m_hoveredInstance == nullptr) {
      break;
    }
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    const bool axisConsumed = m_hoveredInstance->inputDispatcher.pointerAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    if (!axisConsumed) {
      handleBarDeadZoneAxis(*m_hoveredInstance, sx, sy, event, m_platform, m_actionDispatcher);
    }
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return consumed;
}

BarInstance* Bar::instanceForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceMap.find(surface);
  return it != m_surfaceMap.end() ? it->second : nullptr;
}

BarInstance* Bar::instanceForOutput(wl_output* output) const noexcept { return instanceForBar(output, {}); }

BarInstance* Bar::instanceForBar(wl_output* output, std::string_view barName) const noexcept {
  if (output == nullptr) {
    return nullptr;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (barName.empty() || instance->barConfig.name == barName) {
      return instance.get();
    }
  }
  return nullptr;
}

std::optional<std::string> Bar::collectBarIpcInstances(
    std::optional<std::string> barName, std::optional<std::string> monitorSelector,
    std::vector<BarInstance*>& instancesOut
) {
  instancesOut.clear();

  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  if (barName.has_value()) {
    const bool knownBar = std::ranges::contains(m_config->config().bars, *barName, &BarConfig::name);
    if (!knownBar) {
      if (!monitorSelector.has_value()) {
        monitorSelector = std::move(barName);
        barName = std::nullopt;
      } else {
        std::vector<std::string> knownBars;
        knownBars.reserve(m_config->config().bars.size());
        for (const auto& bar : m_config->config().bars) {
          knownBars.push_back(bar.name);
        }
        const std::string suffix =
            knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
        return "error: unknown bar \"" + std::string(*barName) + "\"" + suffix + "\n";
      }
    }
  }

  const auto matchesBar = [&](const BarInstance& instance) {
    return !barName.has_value() || instance.barConfig.name == *barName;
  };

  if (!monitorSelector.has_value()) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && matchesBar(*instance)) {
        instancesOut.push_back(instance.get());
      }
    }
    if (instancesOut.empty()) {
      if (barName.has_value()) {
        return "error: no instances matched bar \"" + std::string(*barName) + "\"\n";
      }
      return "error: no bar instances are active\n";
    }
    return std::nullopt;
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector(*monitorSelector);
  std::vector<std::string> outputMatches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      outputMatches.push_back(output.connectorName);
    }
  }

  std::ranges::sort(knownOutputs);
  knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
  std::ranges::sort(outputMatches);
  outputMatches.erase(std::ranges::unique(outputMatches).begin(), outputMatches.end());

  if (outputMatches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += '\n';
    return error;
  }
  if (outputMatches.size() > 1) {
    return "error: monitor selector \""
        + selector
        + "\" matched multiple outputs: "
        + StringUtils::join(outputMatches, ", ")
        + "\n";
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output == nullptr || !matchesBar(*instance)) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it != m_platform->outputs().end() && it->connectorName == outputMatches.front()) {
      instancesOut.push_back(instance.get());
    }
  }

  if (instancesOut.empty()) {
    std::string error = "error: no instances matched";
    if (barName.has_value()) {
      error += " bar \"" + std::string(*barName) + "\"";
    }
    error += " on \"" + outputMatches.front() + "\"\n";
    return error;
  }

  return std::nullopt;
}

namespace {

  std::optional<std::string> parseBarVisibilityIpcArgs(
      std::string_view command, std::string_view args, std::optional<std::string>& barName,
      std::optional<std::string>& monitorSelector
  ) {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() > 2) {
      return "error: usage: " + std::string(command) + " [bar-name] [monitor-selector]\n";
    }
    barName = std::nullopt;
    monitorSelector = std::nullopt;
    if (!parts.empty() && !parts[0].empty()) {
      barName = parts[0];
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
      monitorSelector = parts[1];
    }
    return std::nullopt;
  }

} // namespace

// Dynamic Notch member function implementations

void Bar::animateNotchExpand(BarInstance& instance, bool expand) {
  if (!instance.isNotchMode) return;

  const float from = instance.notchExpandProgress;
  const float to = expand ? 1.0F : 0.0F;
  if (std::abs(from - to) < 0.01F) return;

  // Cancel any existing animation (enables mid-flight interruption/reversal)
  if (instance.notchAnimationId != 0) {
    instance.animations.cancel(instance.notchAnimationId);
    instance.notchAnimationId = 0;
  }

  // Native refresh-rate pacing: ~210ms expand (EaseOutCubic), ~170ms collapse (EaseInQuad)
  // Frame callbacks driven by wl_surface::frame ensure smooth pacing at 60–144 Hz.
  const float durationMs = expand ? 210.0F : 170.0F;
  const Easing easing = expand ? Easing::EaseOutCubic : Easing::EaseInQuad;

  instance.notchAnimationId = instance.animations.animate(
      from, to, durationMs, easing,
      [this, &instance](float progress) {
        instance.notchExpandProgress = progress;
        if (instance.surface != nullptr) {
          instance.surface->requestLayout();
          instance.surface->requestRedraw();
          // Keep frame callbacks alive for the entire animation duration
          instance.surface->requestFrameTick();
        }
        // Update input region every frame to match exact pill rect
        if (instance.surface != nullptr) {
          const NotchDimensions dims = computeNotchDimensions(instance, progress);
          const float notchW = lerp(dims.collapsedWidth, dims.expandedWidth, progress);
          const float notchH = lerp(dims.collapsedHeight, dims.expandedHeight, progress);
          const float maxW = std::min(notchW, static_cast<float>(instance.outputLogicalWidth - 32));
          const float maxH = std::min(notchH, static_cast<float>(instance.outputLogicalHeight) * 0.45F);
          const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
          const float pillY = static_cast<float>(instance.barConfig.notchTopMargin);
          kLog.debug("[NOTCH_DEBUG] animate progress={} notchW={} notchH={} maxW={} maxH={} pillX={} pillY={}",
              progress, notchW, notchH, maxW, maxH, pillX, pillY);
          updateNotchInputRegion(instance, pillX, pillY, maxW, maxH);
        }
      },
      [this, &instance, expand]() {
        instance.notchAnimationId = 0;
        instance.notchExpanded = expand;
        kLog.debug("[NOTCH_DEBUG] animate complete: expand={} notchProgress={}", expand, instance.notchExpandProgress);
        // Final input region update
        if (instance.surface != nullptr) {
          const NotchDimensions dims = computeNotchDimensions(instance, expand ? 1.0F : 0.0F);
          const float notchW = expand ? dims.expandedWidth : dims.collapsedWidth;
          const float notchH = expand ? dims.expandedHeight : dims.collapsedHeight;
          const float maxW = std::min(notchW, static_cast<float>(instance.outputLogicalWidth - 32));
          const float maxH = std::min(notchH, static_cast<float>(instance.outputLogicalHeight) * 0.45F);
          const float pillX = (static_cast<float>(instance.maxSurfaceW) - maxW) * 0.5F;
          const float pillY = static_cast<float>(instance.barConfig.notchTopMargin);
          updateNotchInputRegion(instance, pillX, pillY, maxW, maxH);
        }
        // Start auto-collapse timer if expanded and not hovered and not pinned
        if (expand && !instance.notchPinned && !instance.notchPointerInside && instance.barConfig.notchAutoCollapseTimeoutMs > 0) {
          instance.notchAutoCollapseTimer.start(
              std::chrono::milliseconds(instance.barConfig.notchAutoCollapseTimeoutMs),
              [this, &instance]() {
                if (!instance.notchPinned && !instance.notchPointerInside && instance.isNotchMode && !instance.notchSliderActive && !instance.notchSubMenuOpen) {
                  animateNotchExpand(instance, false);
                }
              }
          );
        }
        // Stop frame callbacks when animation completes (idle CPU/GPU near zero)
        // Exception: keep frame callbacks alive if visualizer is actively playing
        if (instance.surface != nullptr && !instance.notchMediaPlaying) {
          instance.surface->discardPendingFrameCallback();
        }
      },
      &instance.notchExpandProgress
  );

  // Request initial frame tick to start the animation loop
  if (instance.surface != nullptr) {
    instance.surface->requestFrameTick();
  }
}

void Bar::updateNotchInputRegion(BarInstance& instance, float pillX, float pillY, float pillW, float pillH) {
  if (!instance.isNotchMode || instance.surface == nullptr) return;

  // Use the EXACT current pill bounds (based on expansion progress) for the input region.
  // When collapsed: use collapsed pill rect (200x32)
  // When expanded: use expanded bounds (900x360)
  // This ensures clicks on expanded Bento cards reach the surface,
  // and clicks outside the collapsed pill pass through to windows below.
  const bool expanded = instance.notchExpandProgress > 0.5F;
  const NotchDimensions dims = computeNotchDimensions(instance, expanded ? 1.0F : 0.0F);
  const float fx = pillX;
  const float fy = 0.0F;
  const float fw = expanded ? dims.expandedWidth : dims.collapsedWidth;
  const float fh = expanded ? dims.expandedHeight : dims.collapsedHeight;
  const int ix = static_cast<int>(std::lround(pillX));
  const int iy = 0;
  const int iw = static_cast<int>(std::lround(fw));
  const int ih = static_cast<int>(std::lround(fh));
  const InputRect rect{ix, iy, iw, ih};

  instance.surface->setInputRegion({rect});
}

void Bar::startNotchVisualizerAnimation(BarInstance& instance) {
  if (instance.notchVisualizerAnimId != 0) {
    instance.animations.cancel(instance.notchVisualizerAnimId);
    instance.notchVisualizerAnimId = 0;
  }
  if (instance.notchVisualizerBars.empty()) return;

  constexpr int kBars = 5;
  const float contentScale = static_cast<float>(instance.barConfig.scale);
  instance.notchVisualizerAnimId = instance.animations.animate(
      0.0F, 6.283185F, 1500.0F, Easing::Linear,
      [this, &instance, contentScale](float phase) {
        for (std::size_t i = 0; i < kBars && i < instance.notchVisualizerBars.size(); ++i) {
          float h = 4.0F + 8.0F * std::sin(phase + static_cast<float>(i) * 1.2F);
          h = std::clamp(h, 1.0F, 16.0F);  // maxHeight = 16px
          instance.notchVisualizerBars[i]->setSize(3.0F * contentScale, h);
        }
        // Keep frame callbacks alive while visualizer is animating
        if (instance.surface != nullptr) {
          instance.surface->requestFrameTick();
        }
      },
      [this, &instance]() {
        instance.notchVisualizerAnimId = 0;
        if (instance.notchMediaPlaying) {
          startNotchVisualizerAnimation(instance);
        }
      },
      this
  );
}

void Bar::stopNotchVisualizerAnimation(BarInstance& instance) {
  if (instance.notchVisualizerAnimId != 0) {
    instance.animations.cancel(instance.notchVisualizerAnimId);
    instance.notchVisualizerAnimId = 0;
  }
  const float contentScale = static_cast<float>(instance.barConfig.scale);
  const float barW = 3.0F * contentScale;
  const float barH = 4.0F * contentScale;  // resting flat at 4px
  for (auto* bar : instance.notchVisualizerBars) {
    bar->setSize(barW, barH);
  }
  // Stop frame callbacks when visualizer stops (unless notch is expanded)
  if (instance.surface != nullptr && !instance.notchExpanded) {
    instance.surface->discardPendingFrameCallback();
  }
}

bool Bar::isFullscreenFocused() const {
  if (m_platform == nullptr) return false;
  return m_platform->foreignToplevels().hasFullscreenToplevel();
}

void Bar::triggerNotchExpand(BarInstance& instance) {
  if (!instance.isNotchMode || instance.notchExpanded) return;
  if (instance.barConfig.notchHoverExpand && !isFullscreenFocused()) {
    animateNotchExpand(instance, true);
  }
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|smart|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string& value = parts[0];
  bool enabled = false;
  bool smart = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else if (value == "smart") {
    smart = true;
  } else {
    return "error: invalid value (use on/off/smart, true/false, 1/0)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  auto applyTransientAutoHide = [this, enabled, smart](BarInstance& instance) {
    auto applySurfaceSpec = [this](BarInstance& inst) {
      if (inst.surface == nullptr) {
        return;
      }
      const auto spec = computeBarSurfaceSpec(inst.barConfig, m_config->config().shell.shadow);
      inst.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      inst.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    };

    instance.ipcLayoutReleased = false;
    instance.autoHideDisablePending = false;
    instance.autoHideDelayTimer.stop();
    instance.animations.cancelForOwner(instance.slideRoot);

    instance.barConfig.autoHide = enabled;
    instance.barConfig.smartAutoHide = smart;

    if (enabled || smart) {
      applySurfaceSpec(instance);
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0F);
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      if (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) {
        revealAutoHideBar(instance);
      } else {
        startHideFadeOut(instance, true);
      }
      return;
    }

    if (instance.barConfig.autoHide && instance.hideOpacity < 0.999F) {
      const float current = instance.hideOpacity;
      instance.autoHideDisablePending = true;
      instance.animations.animate(
          current, 1.0F, Style::animNormal, Easing::EaseOutCubic,
          [this, inst = &instance](float v) {
            inst->hideOpacity = v;
            syncBarSlideLayerTransform(*inst);
            syncBarSurfaceChrome(*inst);
          },
          [this, inst = &instance, applySurfaceSpec] {
            inst->autoHideDisablePending = false;
            inst->barConfig.autoHide = false;
            applySurfaceSpec(*inst);
            syncBarSlideLayerTransform(*inst);
            syncBarAutoHideInputRegion(*inst);
            syncBarSurfaceChrome(*inst);
            if (inst->surface != nullptr) {
              inst->surface->requestRedraw();
            }
          },
          instance.slideRoot
      );
      if (instance.surface != nullptr) {
        instance.surface->requestRedraw();
      }
      return;
    }

    instance.barConfig.autoHide = false;
    instance.autoHideDisablePending = false;
    instance.hideOpacity = 1.0F;
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setOpacity(1.0F);
    }
    applySurfaceSpec(instance);
    syncBarSlideLayerTransform(instance);
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  for (BarInstance* instance : targets) {
    applyTransientAutoHide(*instance);
  }
  return "ok\n";
}

std::string Bar::setBarLayerIpc(std::string_view args) {
  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-layer-set <top|overlay> [bar-name] [monitor-selector]\n";
  }

  const std::string& layer = parts[0];
  if (layer != "top" && layer != "overlay") {
    return "error: invalid layer (use top or overlay)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const LayerShellLayer shellLayer = layerShellLayerFromConfig(layer);
  for (BarInstance* instance : targets) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    instance->surface->setLayer(shellLayer);
    instance->barConfig.layer = layer;
  }

  return "ok\n";
}

TaskbarWidget* Bar::findTaskbarWidget(const IpcInvocationContext& context) const {
  const auto findIn = [&context](const std::vector<std::unique_ptr<Widget>>& widgets) -> TaskbarWidget* {
    for (const auto& widget : widgets) {
      if (widget->configName() != context.widgetName) {
        continue;
      }
      if (auto* taskbar = dynamic_cast<TaskbarWidget*>(widget.get()); taskbar != nullptr) {
        return taskbar;
      }
    }
    return nullptr;
  };

  for (const auto& instance : m_instances) {
    if (instance->barConfig.name != context.barName || instance->output != context.output) {
      continue;
    }
    for (const auto* section : {&instance->startWidgets, &instance->centerWidgets, &instance->endWidgets}) {
      if (auto* taskbar = findIn(*section); taskbar != nullptr) {
        return taskbar;
      }
    }
  }
  return nullptr;
}

void Bar::registerIpc(IpcService& ipc) {
  // Widget gesture actions dispatch through the same registry.
  m_actionDispatcher.setIpcService(&ipc);

  ipc.bindCycle(noctalia::cli::msg::taskbarCycle, [this, &ipc](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() != 1 || (parts[0] != "next" && parts[0] != "prev")) {
      return "error: taskbar-cycle requires <next|prev>\n";
    }
    // Order comes from the taskbar's own model (pins, grouping, per-monitor filter), so the
    // target is a widget instance rather than a global window list.
    const auto& context = ipc.invocationContext();
    if (!context.has_value() || context->widgetName.empty()) {
      return "error: taskbar-cycle must be invoked from a taskbar widget gesture\n";
    }
    auto* taskbar = findTaskbarWidget(*context);
    if (taskbar == nullptr) {
      return "error: no taskbar widget named '" + context->widgetName + "' on bar '" + context->barName + "'\n";
    }
    taskbar->cycleAdjacent(parts[0] == "next" ? 1 : -1);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::barShow, [this](const std::string& args) -> std::string { return showBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barHide, [this](const std::string& args) -> std::string { return hideBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barToggle, [this](const std::string& args) -> std::string {
    return toggleBarIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barReserveToggle, [this](const std::string& args) -> std::string {
    return toggleBarReserveSpaceIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barAutoHideSet, [this](const std::string& args) -> std::string {
    return setBarAutoHideIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barLayerSet, [this](const std::string& args) -> std::string {
    return setBarLayerIpc(args);
  });
}

std::string Bar::showBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-show", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::hideBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-hide", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (!barSupportsSlideBehavior(instance->barConfig)) {
      instance->ipcLayoutReleased = true;
    }
    setInstanceIpcVisible(*instance, false);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const bool anyEffectivelyVisible = std::ranges::any_of(targets, [this](const BarInstance* instance) {
    return instance != nullptr && instanceEffectivelyVisible(*instance);
  });

  if (anyEffectivelyVisible) {
    for (BarInstance* instance : targets) {
      if (!barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
      setInstanceIpcVisible(*instance, false);
      syncBarSurfaceChrome(*instance);
    }
    return "ok\n";
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarReserveSpaceIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-reserve-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (instance != nullptr) {
      instance->barConfig.reserveSpace = !instance->barConfig.reserveSpace;
      syncBarExclusiveZone(*instance);
    }
  }
  return "ok\n";
}
