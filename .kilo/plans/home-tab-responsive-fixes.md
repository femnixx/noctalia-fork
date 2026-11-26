# Home Tab Responsive Layout Fixes Plan

## Goal
Fix remaining visual/layout issues in the expanded Dynamic Notch Home tab to match the reference design and add missing functionality (Microphone card, working dropdowns, brightness slider).

## Current State (from screenshots)
- Duplicate top bar removed ✅
- Vertical clipping fixed ✅
- Weather details showing ✅
- Media card has progress bar + volume slider ✅
- **Remaining issues:** Column 3 slider overflow, Column 2 duplicate controls + missing album art, Column 4 clipping, Top bar tab text + missing sub-nav, Microphone card missing, Dropdowns/brightness non-functional

---

## Phase 1: Visual & Layout Fixes

### 1.1 Column 3: Fix Slider Overflow (Volume & Brightness)
**File:** `src/shell/bar/bar.cpp` (~lines 4185-4280)
- **Problem:** Volume/brightness slider tracks extend too far right, pushing percentage labels (26, 55) outside card borders
- **Fix:** 
  - Add right padding inside volume/brightness cards (increase `.paddingH` from 12px → 16px)
  - Set slider `maxWidth` or use `flexGrow=1` with `minWidth=40` so track shrinks to leave room for 2-3 digit label
  - Ensure label has `minWidth=32-40px` and right-aligns inside card

### 1.2 Column 2: Fix Media Card
**File:** `src/shell/bar/bar.cpp` (~lines 3999-4170)
- **Problems:**
  - Two sets of playback controls (duplicate Prev/Pause/Next)
  - Missing album art (shows tiny disc icon instead of 56x56px rounded square)
  - Second Play button's white circle spills outside card right border
- **Fix:**
  - Remove second control group entirely (keep only one centered row)
  - Shrink Pause button: `controlHeight=36px` (was 40), `glyphSize=1.2x` (was 1.3x), remove solid white circle background
  - Album art: ensure `setVisible(true)` when `hasImage()`, fallback glyph hidden
  - Track info: 3 lines (title, artist, device) with proper elide

### 1.3 Column 4: Fix Bluetooth Header Clipping
**File:** `src/shell/bar/bar.cpp` (~lines 4312-4370)
- **Problems:** Settings gear icon pushed off right edge, "BLUETOOTH" label too wide
- **Fix:**
  - Reduce header padding: `.paddingH=10px` (was 12px)
  - Add `flexGrow=1` to label with `ellipsize=End`, `maxLines=1`
  - Move icons closer: `gap=6px` (was 8px), fixed icon size `16px`
  - Ensure icons have `flexShrink=0` so they never get pushed

### 1.4 Top Header Bar: Fix Tabs & Add Sub-Nav Row
**File:** `src/shell/bar/bar.cpp` (~lines 3732-3860) - Global header in `buildNotchBentoLayout`
- **Problems:** 
  - Inactive tabs show text (Media, Clipboard, Notifications, Files) pushing clock/battery right
  - Missing green charging battery icon
  - Missing secondary sub-navigation row (Search + 6 tool icons)
  - Right utility icons not grouped in dark pill
- **Fix:**
  - **Tabs:** Active Home = vertical pill (icon stacked above "Home", navy bg, blue text). Inactive = icon-only circular ghost pills (no text)
  - **Center-Right:** Date/Time + Battery % + green charging icon inline before utility pill
  - **Far Right:** Utility icons (Lightning, Pencil, Pin, Pop-out, Settings) inside dark pill (`#26282C`, radius 18px, padding 6px 12px)
  - **Sub-Nav Row** (new, below header): Left-aligned group - Search (36x36 dark square, primary) + 6 tool icons (Calendar, Volume, Bluetooth, Activity, Terminal, Settings) as ghost pills, `gap=12px`

---

## Phase 2: Column 3 Restructure - Add Microphone Card

### 2.1 Update BarInstance Pointers
**File:** `src/shell/bar/bar_instance.h` (~lines 215-240)
- Add microphone card/widgets:
  ```cpp
  Flex* notchMicCard = nullptr;
  Slider* notchMicSlider = nullptr;
  Label* notchMicLabel = nullptr;
  Label* notchMicSourceLabel = nullptr;
  Button* notchMicMuteButton = nullptr;
  Button* notchMicDeviceSelector = nullptr;
  ```

### 2.2 Build 3-Card Stack in Column 3
**File:** `src/shell/bar/bar.cpp` (~lines 4185-4280)
- Column 3 becomes vertical ColumnLayout with 3 equal-height cards (Output, Mic) + shorter Brightness
- Each card: icon + slider + readout on top row, device dropdown pill below
- **Flex ratios:** Output `flexGrow=1`, Mic `flexGrow=1`, Brightness `flexGrow=0.8` (or use fixed heights with equal Output/Mic)
- **Device selectors:** Same recessed dropdown pill design for both output and input

### 2.3 Wire Microphone Service Integration
**Files:** `src/shell/bar/bar.cpp` (prepareNotchFrame), `src/dbus/audio/audio_service.h`
- Add `setDefaultSourceVolume()` / `getDefaultSource()` to AudioService if missing
- In `prepareNotchFrame`: sync mic slider value, mute state, device label from PipeWire/PulseAudio

---

## Phase 3: Fix Non-Working Interactive Controls

### 3.1 Audio Device Dropdown (Output & Input)
**Problem:** Native Wayland/Qt popup menus fail in layer-shell (clipped/trapped)
**Solution:** Inline expandable list inside card
- **File:** `src/shell/bar/bar.cpp` - replace `notchVolumeDeviceSelector` / `notchMicDeviceSelector` onClick
- **Behavior:** Click pill → toggle inline Flex below slider showing all PipeWire sinks/sources
- **Data source:** `PipeWireService::getSinks()` / `getSources()` (or `m_audio->getDevices()`)
- **Selection:** Click item → call `m_audio->setDefaultSink(id)` / `setDefaultSource(id)` → close list
- **Styling:** Dark recessed pill (`#1A1B1E`), radius 10px, items with radio-style selection

### 3.2 Brightness Slider System Integration
**Problem:** Slider moves visually but doesn't change backlight
**Root cause:** `BrightnessService::setBrightness(displayId, value)` requires display ID; slider callback doesn't pass it
**Fix:**
- **File:** `src/shell/bar/bar.cpp` (~line 4280) - update brightness slider `onValueChanged`
  ```cpp
  .onValueChanged = [&instance](double v) { 
      if (instance.m_brightness && !instance.m_brightness->displays().empty()) {
          instance.m_brightness->setBrightness(instance.m_brightness->displays()[0].id, v / 100.0);
      }
  }
  ```
- **Permission:** User must be in `video` group for `brightnessctl`/sysfs access (document in README)
- **Alternative:** Use `ddcutil` for external monitors via BrightnessService

---

## Data Flow Summary

```
Top Header (Global):
  Tabs (notchTabPills) → notchTabIndex → show/hide notchTabContents
  Time/Battery → prepareNotchFrame updates per-frame
  Utility Pill → static icons

Sub-Nav Row:
  Search + Tools → static icon buttons (tooltips only)

Column 1 (Time/Weather):
  notchTimeLabel/DateLabel → prepareNotchFrame (system clock)
  Weather labels → TODO: weather service integration

Column 2 (Media):
  notchMediaArt → prepareNotchFrame (MPRIS artUrl async load)
  notchMediaTrack/Artist/DeviceId → prepareNotchFrame (MPRIS metadata)
  Progress dots → prepareNotchFrame (positionUs/lengthUs)
  Transport buttons → MprisService previous/playPause/next
  Volume slider → MprisService setVolumeActive()

Column 3 (Audio/Mic/Brightness):
  Volume slider → AudioService setVolume()
  Volume dropdown → inline list → AudioService setDefaultSink()
  Mic slider → AudioService setDefaultSourceVolume()
  Mic dropdown → inline list → AudioService setDefaultSource()
  Brightness slider → BrightnessService setBrightness(displayId, value)

Column 4 (Bluetooth):
  Header → static
  Status/List → BluetoothService devices (TODO)
```

---

## Validation Checklist

1. **Build:** `meson compile -C build` succeeds
2. **Top Header:** Active Home tab = navy pill (icon+text), inactive = icon-only; time+battery inline; utility pill on right
3. **Sub-Nav:** Search square + 6 tools left-aligned, no spacer
4. **Column 1:** Clock (28px) + date; weather icon + temp + condition + high/low + city (all visible, no clipping)
5. **Column 2:** 56x56 album art (rounded), 3-line track info, single centered transport controls, media volume slider at bottom
6. **Column 3:** 3 cards stacked - Output (speaker+slider+dropdown), Mic (mic+slider+dropdown), Brightness (sun+slider); sliders leave room for labels
7. **Column 4:** BLUETOOTH label truncates, Refresh+Settings icons visible on right
8. **Dropdowns:** Click volume/mic device pill → inline list opens → select device → updates system audio
9. **Brightness:** Drag slider → screen brightness changes immediately
10. **Collapsed:** 32px pill, centered, visualizer works for any audio
11. **No crashes, 60fps, no memory leaks**

---

## Open Questions

1. **Weather data:** Currently hardcoded. Integrate with weather service? (Out of scope for layout fix)
2. **Bluetooth device list:** Currently "No paired devices". Integrate with BluetoothService? (Out of scope)
3. **Microphone mute button:** Add mute toggle to mic card? (Yes, mirror volume card)
4. **External monitor brightness:** BrightnessService uses `ddcutil` for externals? (Check existing implementation)

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/shell/bar/bar_instance.h` | Add mic card pointers |
| `src/shell/bar/bar.cpp` - `buildNotchBentoLayout` | Fix global header, sub-nav, column 2/3/4 layouts |
| `src/shell/bar/bar.cpp` - `prepareNotchFrame` | Wire mic slider, dropdown logic, brightness fix |
| `src/dbus/audio/audio_service.h/.cpp` | Add source volume/default source methods if missing |
| `src/system/brightness_service.h/.cpp` | Verify display ID handling |