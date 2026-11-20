#pragma once

#include "config/config_service.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/bar/widget.h"
#include "shell/panel/attached_panel_context.h"
#include "shell/wayland/layer_surface.h"
#include "ui/signal.h"
#include "dbus/mpris/mpris_service.h"
#include "pipewire/pipewire_service.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class Box;
class Button;
class Flex;
class Glyph;
class GridView;
class Image;
class Label;
class Node;
class ProgressBar;
class Slider;

// Forward declarations for services
class BrightnessService;
class INetworkService;
class BluetoothService;
class NotificationManager;
class IdleInhibitor;

struct BarCapsuleRun {
  Node* shell = nullptr;
  Box* bg = nullptr;
  Flex* container = nullptr;
  Node* content = nullptr;
  WidgetBarCapsuleSpec spec{};
  float contentScale = 1.0F;
  // Capsule geometry can exist without a painted fill or border.
  bool hasPaintedCapsuleBackground = false;
  std::vector<Widget*> widgets;
  // Hover highlight overlays, parallel to `widgets` for group runs; one shared box for single runs.
  std::vector<Box*> hoverBoxes;
  bool accordion = false;
  // Unfold animation time in milliseconds. Always overwritten from the group's accordion_duration
  // setting (default kBarRevealDurationMs) when the run is built.
  float accordionDurationMs = static_cast<float>(kBarRevealDurationMs);
  // Hover time in milliseconds before unfolding starts (expand only; collapse is immediate).
  float accordionDelayMs = static_cast<float>(kBarRevealDelayMs);
  // Pending delayed expand; cancelled when the pointer leaves, the state resolves, or the run dies.
  Timer accordionDelayTimer;
  // Clips accordion members to the reveal window (inside the capsule padding).
  Node* accordionClip = nullptr;
  BarAccordionDirection accordionDirection = BarAccordionDirection::End;
  // Index into `widgets` (visual order) of the always-visible member (config members[0]).
  std::size_t accordionVisibleIndex = 0;
  bool accordionExpanded = false;
  float accordionProgress = 0.0F; // 0 = collapsed, 1 = fully expanded
};

struct BarInstance {
  std::uint32_t outputName = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t outputLogicalX = 0;
  std::int32_t outputLogicalY = 0;
  std::int32_t outputLogicalWidth = 0;
  std::int32_t outputLogicalHeight = 0;
  std::size_t barIndex = 0;
  BarConfig barConfig;
  std::unique_ptr<LayerSurface> surface;
  // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  Node* slideRoot = nullptr;
  float slideHiddenDx = 0.0F;
  float slideHiddenDy = 0.0F;
  InputDispatcher inputDispatcher;
  // Gestures for the parts of the bar no widget covers. The sink is never mounted in the scene; it
  // is used only for its scroll-detent accumulator, so dead-zone scrolling quantizes like a widget.
  noctalia::bar::WidgetActionBindings deadZoneBindings;
  InputArea deadZoneAxisSink;
  float hideOpacity = 1.0F;
  // bar-hide/toggle IPC on non-autohide bars: release compositor exclusive zone until bar-show (v4 isVisible=false).
  bool ipcLayoutReleased = false;
  // bar-auto-hide-set off keeps autoHide true until the reveal completes; block hover helpers from replacing it.
  bool autoHideDisablePending = false;
  Timer autoHideDelayTimer;
  // smart_auto_hide: active workspace empty (or overview open) — keep the bar visible.
  bool smartAutoHidePinnedVisible = false;
  bool pointerInside = false;
  float lastPointerSx = 0.0F;
  float lastPointerSy = 0.0F;
  std::size_t attachedPopupCount = 0;
  // Last region sent for dead_zone.click_through; skips resending an unchanged region every layout pass.
  std::optional<std::vector<InputRect>> clickThroughInputRegion;

  // Bar background, shadow, and layout sections (start/center/end along main axis)
  Box* bg = nullptr;
  Box* shadow = nullptr;
  Node* shadowLeftClip = nullptr;
  Node* shadowRightClip = nullptr;
  Box* shadowLeft = nullptr;
  Box* shadowRight = nullptr;
  Node* contentClip = nullptr;        // Clips to pill bounds (Node for visual clipping only)
  // Unclipped layer between the bar background and contentClip; hosts the hover pills of
  // Unclipped layer between the bar background and contentClip; hosts the hover pills of
  // capsule-less widgets so they neither affect layout nor get clipped at section boundaries.
  Node* hoverUnderlay = nullptr;
  Node* startSlot = nullptr;
  Node* centerSlot = nullptr;
  Node* endSlot = nullptr;
  Flex* startSection = nullptr;
  Flex* centerSection = nullptr;
  Flex* endSection = nullptr;

  std::vector<std::unique_ptr<Widget>> startWidgets;
  std::vector<std::unique_ptr<Widget>> centerWidgets;
  std::vector<std::unique_ptr<Widget>> endWidgets;
  std::vector<BarCapsuleRun> startCapsuleRuns;
  std::vector<BarCapsuleRun> centerCapsuleRuns;
  std::vector<BarCapsuleRun> endCapsuleRuns;

  // Maps each widget's root node to its Widget so hover-change events resolve to the owning widget.
  std::unordered_map<const Node*, Widget*> widgetByRoot;
  Widget* hoverHighlightWidget = nullptr;
  // Peek-enabled widget that is revealed or waiting out its peek_delay_ms; null when none.
  Widget* peekWidget = nullptr;
  Timer widgetPeekDelayTimer;

  Signal<>::ScopedConnection paletteConn;
  std::optional<AttachedPanelGeometry> attachedPanelGeometry;

  // Dynamic Notch state
  bool isNotchMode = false;
  bool notchExpanded = false;
  float notchExpandProgress = 0.0F;           // 0.0 = collapsed, 1.0 = expanded
  AnimationManager::Id notchAnimationId = 0;
  Timer notchCollapseTimer;                   // Debounce timer for pointer leave (180ms grace)
  Timer notchExpandTimer;                     // Debounce timer for pointer enter (expand delay)
  Timer notchAutoCollapseTimer;               // Auto-collapse timeout timer
  // Fixed surface dimensions (set at startup, never changed during animation).
  // All pill geometry is rendered inside this fixed bounding box.
  std::uint32_t maxSurfaceW = 0;              // expandedWidth + shadowPadding
  std::uint32_t maxSurfaceH = 0;              // expandedHeight + shadowPadding + topMargin
  std::vector<std::unique_ptr<Widget>> notchCollapsedStartWidgets;
  std::vector<std::unique_ptr<Widget>> notchCollapsedCenterWidgets;
  std::vector<std::unique_ptr<Widget>> notchCollapsedEndWidgets;
  std::vector<std::unique_ptr<Widget>> notchExpandedStartWidgets;
  std::vector<std::unique_ptr<Widget>> notchExpandedCenterWidgets;
  std::vector<std::unique_ptr<Widget>> notchExpandedEndWidgets;
  Node* notchContentRoot = nullptr;           // Root for notch content (swaps on expand/collapse)
  // Section nodes for collapsed/expanded widget sets (mounted once in buildNotchScene)
  Flex* notchCollapsedStartSection = nullptr;
  Flex* notchCollapsedCenterSection = nullptr;
  Flex* notchCollapsedEndSection = nullptr;
  Flex* notchExpandedStartSection = nullptr;
  Flex* notchExpandedCenterSection = nullptr;
  Flex* notchExpandedEndSection = nullptr;
  InputArea* notchHoverArea = nullptr;        // Hover detection area covering current pill bounds
  bool notchPointerInside = false;            // Track pointer inside notch area
  bool notchFullscreenSuppressed = false;     // Suppress expand when fullscreen focused
  bool notchSliderActive = false;             // True while any slider is being dragged
  bool notchSubMenuOpen = false;              // True while a sub-tab/menu inside card is active

  // Collapsed pill custom elements (album art + visualizer)
  Node* notchCollapsedContainer = nullptr;    // Container for collapsed-pill elements
  Image* notchAlbumArt = nullptr;             // Album art thumbnail (20x20, radius 5)
  Glyph* notchFallbackGlyph = nullptr;        // Fallback music glyph when no media active
  std::vector<Box*> notchVisualizerBars;      // 5 animated visualizer bars
  AnimationManager::Id notchVisualizerAnimId = 0; // Visualizer pulse animation
  bool notchMediaPlaying = false;             // Current MPRIS playback state
  std::string notchLastArtUrl;                // Last loaded album art URL (avoid reload)

  // Concave "fang" fillets at top-left and top-right (concave inverse-rounded corners)
  // Removed: true concave corner cutouts not supported by current renderer (would render as floating circles)
  // Box* notchFangLeft = nullptr;
  // Box* notchFangRight = nullptr;

  // Bento Control Center layout (built once on init, updated per-frame)
  Node* notchBentoRoot = nullptr;             // Root for expanded Bento grid (inside contentClip)
  // Top Header Bar: horizontal tab pills (Home, Media, Clipboard, Notifications, Files)
  Flex* notchHeaderTabs = nullptr;
  std::vector<Button*> notchTabPills;
  std::size_t notchTabIndex = 0;              // Active tab index
  // Header status: Date & Time + Battery %
  Label* notchHeaderDateTime = nullptr;
  Label* notchHeaderBattery = nullptr;
  // Header action buttons: Power Profiles, Pin, Settings, Power Menu
  Flex* notchHeaderActions = nullptr;
  Button* notchPowerProfilesButton = nullptr;
  Button* notchPinButton = nullptr;
  Button* notchSettingsButton = nullptr;
  Button* notchPowerButton = nullptr;
  bool notchPinned = false;                   // Pin state - prevents auto-collapse when true
  // Main 4-Column Horizontal Bento Grid (y = 46px to bottom)
  Flex* notchBentoGrid = nullptr;
  // Column 1 (~22%): Clock, Date & System Info Card
  Flex* notchClockCard = nullptr;
  Label* notchTimeLabel = nullptr;
  Label* notchDateLabel = nullptr;
  Label* notchSystemInfoLabel = nullptr;
  // Column 2 (~34%): Full MPRIS Media Player Card
  Flex* notchMediaCard = nullptr;
  Image* notchMediaArt = nullptr;
  Glyph* notchMediaArtFallback = nullptr;
  Label* notchMediaTrack = nullptr;
  Label* notchMediaArtist = nullptr;
  Label* notchMediaProgress = nullptr;
  ProgressBar* notchMediaProgressBar = nullptr;  // Animated progress scrubber
  Button* notchMediaPrev = nullptr;
  Button* notchMediaPlayPause = nullptr;
  Button* notchMediaNext = nullptr;
  // Column 3 (~24%): Stacked Volume & Brightness Cards
  Flex* notchVolumeCard = nullptr;
  Flex* notchMicCard = nullptr;
  Flex* notchBrightnessCard = nullptr;
  Slider* notchVolumeSlider = nullptr;
  Label* notchVolumeLabel = nullptr;
  Label* notchVolumeSinkLabel = nullptr;
  Button* notchVolumeMuteButton = nullptr;
  Button* notchVolumeDeviceSelector = nullptr;
  Slider* notchMicSlider = nullptr;
  Label* notchMicLabel = nullptr;
  Label* notchMicSourceLabel = nullptr;
  Button* notchMicMuteButton = nullptr;
  Button* notchMicDeviceSelector = nullptr;
  Slider* notchBrightnessSlider = nullptr;
  Label* notchBrightnessLabel = nullptr;
  // Column 4 (~20%): 2x2 Quick Toggle Tiles (Wi-Fi, Bluetooth, DND, Caffeine)
  GridView* notchToggleGrid = nullptr;
  Button* notchWifiToggle = nullptr;
  Button* notchBluetoothToggle = nullptr;
  Button* notchDndToggle = nullptr;
  Button* notchCaffeineToggle = nullptr;
  // Toggle tile labels for color updates
  Label* notchWifiLabel = nullptr;
  Label* notchBluetoothLabel = nullptr;
  Label* notchDndLabel = nullptr;
  Label* notchCaffeineLabel = nullptr;

  // Service pointers for interactivity (set during initNotchScene)
  MprisService* m_mpris = nullptr;
  PipeWireService* m_audio = nullptr;
  BrightnessService* m_brightness = nullptr;
  INetworkService* m_network = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  NotificationManager* m_notifications = nullptr;
  IdleInhibitor* m_idleInhibitor = nullptr;
};