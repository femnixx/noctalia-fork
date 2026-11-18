# Dynamic Notch Implementation Plan

## Overview
Transform Noctalia's full-width top bar into a centered, morphing Dynamic Notch while maintaining 100% feature parity.

---

## Confirmed Decisions

1. **Dimensions & Viewport Clamping**: `220×32` collapsed / `460×130` expanded, with text ellipsis truncation when collapsed. Dynamically clamp expanded width to `min(expanded_width, output_logical_width - 32)` and expanded height to `min(expanded_height, output_logical_height * 0.45)` for responsiveness.

2. **Corner Radius**: Separate configurable radii (`16px` collapsed / `22px` expanded) interpolated smoothly during morph.

3. **Auto-expand Triggers**: Media changes and notifications trigger expand, with configurable auto-collapse timeout (default `3.5s` unless hovered) and suppression when fullscreen window focused.

4. **Widget Slot Arrays**: Separate `collapsed_widgets` and `expanded_widgets` configurable arrays in TOML.

5. **`notchTopMargin`**: Default `6px` for floating pill on standard laptops, configurable to `0px` for physical camera notches.

6. **Responsive Input & Interruptible Easing**: Asymmetric easing (`EaseOutCubic` ~220ms expand, `EaseInCubic` ~180ms collapse) locked to `wl_surface::frame` callbacks. Interruptible mid-flight from current progress. Zero delay on enter/click, `180ms` debounce on leave. Immediate `wl_surface_set_input_region` updates every frame during morph.

7. **Widget Transition**: Opacity cross-fade (fade out collapsed `0%–40%`, fade in expanded `50%–100%`) without extra FBOs when idle.

---

## Phase 1: Core Surface & Geometry Changes

### 1.1 BarConfig Extensions (`src/config/config_types.h`)
Add Dynamic Notch configuration fields to `BarConfig`:
```cpp
// Dynamic Notch settings
bool dynamicNotch = false;                    // Enable Dynamic Notch mode
std::int32_t notchCollapsedWidth = 220;       // Collapsed pill width
std::int32_t notchCollapsedHeight = 32;       // Collapsed pill height
std::int32_t notchExpandedWidth = 460;        // Expanded pill width
std::int32_t notchExpandedHeight = 130;       // Expanded pill height
std::int32_t notchCollapsedRadius = 16;       // Collapsed corner radius
std::int32_t notchExpandedRadius = 22;        // Expanded corner radius
std::int32_t notchTopMargin = 6;              // Top margin from screen edge (6px default, 0 for physical notches)
bool notchHoverExpand = true;                 // Expand on hover
bool notchClickExpand = true;                 // Expand on click
std::int32_t notchExpandDelayMs = 0;          // Zero delay on hover/click
std::int32_t notchCollapseDelayMs = 180;      // 180ms debounce on pointer leave
std::int32_t notchAutoCollapseTimeoutMs = 3500; // Auto-collapse after 3.5s unless hovered
// Widget slot assignments for collapsed/expanded states (separate configurable arrays)
std::vector<std::string> notchCollapsedStart = {"workspaces"};
std::vector<std::string> notchCollapsedCenter = {"media"};
std::vector<std::string> notchCollapsedEnd = {"clock", "battery"};
std::vector<std::string> notchExpandedStart = {"workspaces", "launcher"};
std::vector<std::string> notchExpandedCenter = {"media", "volume", "brightness"};
std::vector<std::string> notchExpandedEnd = {"notifications", "tray", "control-center", "session", "clock", "battery"};
```

### 1.2 Config Schema (`src/config/schema/config_schema_bar.cpp`)
Add schema fields for all new Dynamic Notch config options with appropriate ranges and validation.

### 1.3 BarInstance Extensions (`src/shell/bar/bar_instance.h`)
Add Dynamic Notch state to `BarInstance`:
```cpp
// Dynamic Notch state
bool m_isNotchMode = false;
bool m_notchExpanded = false;
float m_notchExpandProgress = 0.0f;           // 0.0 = collapsed, 1.0 = expanded
AnimationManager::Id m_notchAnimationId = 0;
Timer m_notchCollapseTimer;                   // Debounce timer for pointer leave
Timer m_notchAutoCollapseTimer;               // Auto-collapse timeout timer
std::vector<std::unique_ptr<Widget>> m_notchCollapsedStartWidgets;
std::vector<std::unique_ptr<Widget>> m_notchCollapsedCenterWidgets;
std::vector<std::unique_ptr<Widget>> m_notchCollapsedEndWidgets;
std::vector<std::unique_ptr<Widget>> m_notchExpandedStartWidgets;
std::vector<std::unique_ptr<Widget>> m_notchExpandedCenterWidgets;
std::vector<std::unique_ptr<Widget>> m_notchExpandedEndWidgets;
Node* m_notchContentRoot = nullptr;           // Root for notch content (swaps on expand/collapse)
InputArea* m_notchHoverArea = nullptr;        // Hover detection area covering max expanded bounds
bool m_notchPointerInside = false;            // Track pointer inside notch area
bool m_notchFullscreenSuppressed = false;     // Suppress expand when fullscreen focused
```

### 1.4 LayerSurface Anchor Changes (`src/shell/bar/bar.cpp`)
**Key change**: In `createInstance()`, when `dynamicNotch` is enabled and position is "top":
- Change anchor from `TOP | LEFT | RIGHT` to `TOP` only
- Set explicit width/height based on current notch state (collapsed/expanded)
- Set margins: `marginTop = notchTopMargin`, `marginLeft = 0`, `marginRight = 0`
- This lets Wayland center the surface horizontally automatically

### 1.5 Dynamic Input Region (`src/shell/wayland/layer_surface.cpp` + `bar.cpp`)
- Add `setInputRegion(const std::vector<InputRect>& rects)` method to `LayerSurface`
- In `Bar::prepareFrame()` or `Bar::buildScene()`, compute the current interpolated notch bounds
- Call `surface->setInputRegion(notchBounds)` on every frame during animation, and when state settles
- When fully collapsed and idle, the input region matches the compact pill exactly
- Transparent padding around the notch never blocks clicks

---

## Phase 2: Morphing Animation System

### 2.1 Animation Driver (`src/shell/bar/bar.cpp`)
Add `animateNotchExpand(BarInstance&, bool expand)` method:
- Uses `AnimationManager::animate()` with `Easing::EaseOutCubic` (~220ms) for expand, `Easing::EaseInCubic` (~180ms) for collapse
- Animates `m_notchExpandProgress` from current value to 1.0 (expand) or 0.0 (collapse) — **interruptible mid-flight**
- On each tick: call `requestLayout()` + `requestRedraw()` on the instance
- On complete: if collapsed and no hover, stop frame callbacks (`m_animations.cancelForOwner(&m_notchProgress)`)

### 2.2 Geometry Interpolation (`src/shell/bar/bar.cpp`)
In `buildScene()` for notch mode:
```cpp
float w = lerp(collapsedWidth, expandedWidth, progress);
float h = lerp(collapsedHeight, expandedHeight, progress);
float r = lerp(collapsedRadius, expandedRadius, progress);
// Clamp to viewport
w = std::min(w, outputLogicalWidth - 32);
h = std::min(h, outputLogicalHeight * 0.45f);
surface->requestSize((uint32_t)w, (uint32_t)h);
// Update input region to match interpolated bounds every frame
```

### 2.3 Widget Layout Swapping with Cross-Fade (`src/shell/bar/bar.cpp`)
- Maintain two widget sets: collapsed and expanded
- In `buildScene()`, based on `m_notchExpandProgress`:
  - `progress < 0.4`: Show collapsed widgets (opacity `1.0 - progress/0.4`), hide expanded
  - `progress >= 0.5`: Show expanded widgets (opacity `(progress - 0.5)/0.5`), hide collapsed
  - `0.4 <= progress < 0.5`: Both hidden (transition gap)
- Use `m_notchContentRoot` as container; set widget opacity via style/color alpha
- No extra FBOs — pure scene graph opacity manipulation

### 2.4 Hover/Click Detection & Triggers (`src/shell/bar/bar.cpp`)
- `m_notchHoverArea` covers the full *maximum* expanded bounds (so hover works before expansion)
- On pointer enter: `m_notchPointerInside = true`, cancel collapse timer, start expand immediately (zero delay)
- On pointer leave: `m_notchPointerInside = false`, start `180ms` collapse debounce timer
- On click: toggle expand/collapse immediately
- Media playback change / notification arrival: trigger expand programmatically (if not fullscreen suppressed)
- Auto-collapse timer: `3.5s` after expand completes, cancelled on hover re-entry

---

## Phase 3: Widget Integration & Popup Re-anchoring

### 3.1 Widget Set Population (`src/shell/bar/bar.cpp`)
In `populateWidgets()` for notch mode:
- Create widgets for both collapsed and expanded slot configurations
- Use `WidgetFactory::create()` with appropriate names from config vectors
- Store in separate vectors in `BarInstance`

### 3.2 Media Widget Enhancements (`src/shell/bar/widgets/media_widget.*`)
- Collapsed: Show only playing indicator + title marquee (ellipsis)
- Expanded: Full playback controls, artwork, volume slider
- Add `setNotchMode(bool collapsed)` to switch layouts

### 3.3 Workspaces Widget (`src/shell/bar/widgets/workspaces_widget.*`)
- Collapsed: Compact pills (just numbers/icons)
- Expanded: Full labels + interactions

### 3.4 Popup Anchoring Updates (`src/shell/panel/panel_manager.cpp`)
**Key change**: Panels (Control Center, Calendar, Tray, etc.) currently anchor to bar edges.

For Dynamic Notch:
- In `PanelManager::openPanel()`, detect if source bar is in notch mode
- If notch mode: anchor panel to **center-bottom of the notch surface** instead of bar edges
- Use `LayerPopupParentContext` from bar with notch surface dimensions
- Panel reveal animation originates from notch center

### 3.5 Tray/Notification Popups (`src/shell/tray/tray_widget.cpp`, `src/shell/notification/`)
- Same re-anchoring: center under notch
- Update `PanelOpenRequest` anchorX/anchorY to notch center

---

## Phase 4: Configuration & Hot Reload

### 4.1 example.toml Updates
Add `[bar.main.dynamic_notch]` section with all new options documented.

### 4.2 Hot Reload Support (`src/shell/bar/bar.cpp`)
In `Bar::reload()`:
- Detect Dynamic Notch config changes
- If geometry fields changed (width/height/radius/margin): recreate LayerSurface
- If widget slot assignments changed: repopulate widget vectors
- If animation params changed: update timers
- Preserve expanded/collapsed state across reload where possible

### 4.3 Per-Monitor Overrides (`src/config/config_types.h` + schema)
Extend `BarMonitorOverride` with all Dynamic Notch fields for per-output tuning.

---

## Phase 5: Performance & Battery Life

### 5.1 Zero-Idle-CPU Guarantee
- When notch is **fully collapsed and idle** (no hover, no animation, no media playing, no pending notifications):
  - Cancel all notch-related animations (`m_animations.cancelForOwner(&m_notchProgress)`)
  - Stop frame callbacks: don't call `requestFrameTick()` unless animation active
  - Ensure `BarInstance::instanceNeedsFrameTick()` returns `false` when settled
  - Verify with `perf`/`powertop` that no wakeups occur

### 5.2 No Extra FBOs/Textures
- Reuse existing scene graph; no offscreen rendering
- Notch morphing is pure geometry + layout, no render-to-texture
- Blur region updates use existing `setBlurRegion()` path

### 5.3 Fractional Scaling
- All notch dimensions in logical pixels
- `LayerSurface` uses `wp_viewporter` for fractional scale (existing path)
- `RenderContext` scale synced before layout (existing path)

---

## File-by-File Change Summary

| File | Changes |
|------|---------|
| `src/config/config_types.h` | Add Dynamic Notch fields to `BarConfig` and `BarMonitorOverride` |
| `src/config/schema/config_schema_bar.cpp` | Add schema for all new fields |
| `src/shell/bar/bar_instance.h` | Add notch state, widget vectors, hover area, animation ID |
| `src/shell/bar/bar.cpp` | Core implementation: anchor change, animation, layout swap, input region, hover detection, popup context |
| `src/shell/wayland/layer_surface.h/cpp` | Add `setInputRegion()` method |
| `src/shell/bar/widgets/media_widget.*` | Collapsed/expanded layout modes |
| `src/shell/bar/widgets/workspaces_widget.*` | Compact pill mode |
| `src/shell/panel/panel_manager.cpp` | Re-anchor attached/floating panels to notch center |
| `src/shell/tray/tray_widget.cpp` | Re-anchor tray drawer |
| `src/shell/notification/` | Re-anchor notification toasts (if bar-attached) |
| `example.toml` | Document all new config options |

---

## Validation Checklist

- [ ] Bar compiles with `meson build && ninja -C build`
- [ ] Dynamic Notch enabled: bar becomes centered pill at top
- [ ] Hover expands notch smoothly (width/height/radius interpolate)
- [ ] Click toggles expand/collapse
- [ ] Media playing → auto-expands
- [ ] Notification arrives → auto-expands
- [ ] Collapsed state shows: workspaces, media indicator, clock, battery
- [ ] Expanded state shows: full media controls, volume/brightness, notifications, control center trigger, tray
- [ ] Control Center opens centered under notch
- [ ] Launcher opens centered under notch
- [ ] Tray drawer opens centered under notch
- [ ] Calendar/notification popups anchor to notch
- [ ] Input region matches visual bounds (click-through on transparent padding)
- [ ] Fractional scaling works (1.5x, 2x, etc.)
- [ ] Hot reload updates notch config without restart
- [ ] Zero CPU/GPU wakeups when collapsed and idle
- [ ] Memory stays ~84 MB
- [ ] Works on all supported compositors (Hyprland, Niri, Sway, etc.)

---

## Implementation Order

1. **Config types + schema** (foundation)
2. **BarInstance state + LayerSurface anchor change** (geometry)
3. **Animation driver + interpolation** (morphing)
4. **Widget set population + layout swap + cross-fade** (content)
5. **Hover/click detection + triggers** (interaction)
6. **Input region sync** (click-through)
7. **Popup re-anchoring** (panels, tray, notifications)
8. **Media/Workspaces widget adaptations** (widget-specific)
9. **Hot reload + example.toml** (config UX)
10. **Performance validation** (idle CPU, memory)