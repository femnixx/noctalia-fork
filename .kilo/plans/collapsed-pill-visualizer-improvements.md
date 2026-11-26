# Collapsed Pill Visualizer Improvements Plan

## Goal
Enhance the Dynamic Notch collapsed pill (Music tab) to:
1. **Visualize ANY audio** via PipeWire spectrum (not just MPRIS players)
2. **Show album art** for MPRIS-tracked players (current behavior, acceptable)
3. **Display title + artist** with horizontal marquee scrolling between album art (left) and visualizer bars (right)

## Current Architecture

### Files Involved
- `src/shell/bar/bar.cpp` - Main notch logic (`prepareNotchFrame`, `layoutNotchWidgets`, `updateNotchGeometry`)
- `src/shell/bar/bar_instance.h` - Notch state (`notchAlbumArt`, `notchVisualizerBars`, `notchMediaTrack`, etc.)
- `src/shell/bar/widgets/audio_visualizer_widget.cpp/h` - PipeWire-based visualizer widget
- `src/app/application.h` - `PipeWireSpectrum` available at `m_audioSpectrum`

### Current State
- **Collapsed pill** (lines 4456-4498, 4688-4734): Shows album art (left) + 5 fake sine-wave visualizer bars (right)
- **MPRIS tracking** (lines 5085-5150): Fetches artUrl, title, artist from active MPRIS player
- **Visualizer animation** (lines 6285-6357): Fake sine-wave animation triggered by `notchMediaPlaying` (MPRIS only)
- **Expanded tab** (line 3951): `notchMediaTrack` Label with marquee exists in expanded Music tab only

## Implementation Plan

### Phase 1: Replace Fake Visualizer with PipeWire Spectrum
**Files:** `bar.cpp`, `bar_instance.h`

1. **Add `PipeWireSpectrum*` to `BarInstance`** (bar_instance.h:283 area)
   ```cpp
   PipeWireSpectrum* m_audioSpectrum = nullptr;
   ```

2. **Wire service in `initNotchScene`** (bar.cpp ~line 4203)
   ```cpp
   instance.m_audioSpectrum = m_audioSpectrum;  // from Bar::m_audioSpectrum
   ```

3. **Replace `startNotchVisualizerAnimation` / `stopNotchVisualizerAnimation`** (bar.cpp 6285-6357)
   - Remove fake sine-wave animation
   - Create an `AudioVisualizerWidget` instance for collapsed pill (or reuse visualizer bars with PipeWire data)
   - Simpler approach: Keep the 5 `Box*` bars, but drive their heights from `PipeWireSpectrum::values()` per frame in `prepareNotchFrame`

4. **Per-frame spectrum sync in `prepareNotchFrame`** (around line 5350)
   - If `instance.m_audioSpectrum` exists and has active listener
   - Pull spectrum values → map to 5 bars → update bar heights/positions

### Phase 2: Add Title/Artist Label to Collapsed Pill
**Files:** `bar.cpp`, `bar_instance.h`

1. **Add `Label* notchCollapsedMediaTrack = nullptr`** to `BarInstance` (bar_instance.h ~line 189)

2. **Create label in `initNotchScene`** (bar.cpp ~line 4470, after album art)
   ```cpp
   instance.notchCollapsedMediaTrack = static_cast<Label*>(instance.notchCollapsedContainer->addChild(
       ui::label({
           .text = "",
           .fontSize = Style::fontSizeBody * contentScale,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxWidth = 0,  // calculated in layout
           .maxLines = 1,
           .ellipsize = TextEllipsize::End,
           .autoScroll = true,
           .autoScrollSpeed = 30.0F * contentScale,
           .autoScrollOnlyWhenHovered = false,
       })
   ));
   ```

3. **Position in `layoutNotchWidgets`** (bar.cpp ~line 4696-4713)
   - Place between album art (right edge + gap) and visualizer (left edge - gap)
   - Calculate `maxWidth` dynamically: `visX - (artX + artSize + gap)`

4. **Update text in `prepareNotchFrame`** (bar.cpp ~line 5137)
   - Reuse existing `trackTitle` / `trackArtist` from MPRIS
   - Set combined text: `"Title — Artist"` on `notchCollapsedMediaTrack`
   - Show/hide based on whether MPRIS has metadata

5. **Visibility logic** (bar.cpp ~line 4700-4713)
   - Show label when `collapsed && collapsedOpacity > 0 && !trackTitle.empty()`
   - Hide when expanded or no metadata

### Phase 3: Album Art - Keep MPRIS Only
No changes needed. Current logic at lines 5103-5118 already handles this correctly.

## Data Flow

```
PipeWireSpectrum (system-wide audio) 
    → Bar::m_audioSpectrum 
    → BarInstance::m_audioSpectrum 
    → prepareNotchFrame() per-frame 
    → spectrum.values() 
    → 5 Box* bars height/position update

MPRIS activePlayer 
    → artUrl, title, artist 
    → notchAlbumArt (async texture load) 
    → notchCollapsedMediaTrack.setText() 
    → notchMediaTrack (expanded tab)
```

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| No audio playing | Visualizer bars idle (2px line), label hidden, album art hidden, fallback glyph shown |
| MPRIS playing (Spotify, etc.) | Full: art + title/artist marquee + real visualizer |
| Non-MPRIS audio (browser, games) | Visualizer works (PipeWire), no art, no title/artist, fallback glyph shown |
| Track change | Art reloads async, label text updates, visualizer continues seamlessly |
| Expand notch | Collapsed container fades out, expanded Music tab shows full media card |

## Validation Steps

1. Build succeeds: `meson compile -C build`
2. Run noctalia, play audio via:
   - Spotify (MPRIS) → verify art + title/artist + visualizer
   - Firefox YouTube (non-MPRIS) → verify visualizer works, glyph shown, no title/artist
   - Pause → visualizer idles to horizontal line
3. Expand notch → Music tab shows full media card with same data
4. No crashes, no memory leaks, smooth 60fps animation

## Open Questions (Resolved)

- ✅ Visualizer source: **PipeWire spectrum** (real audio levels)
- ✅ Album art: **MPRIS only** (current behavior acceptable)
- ✅ Title/artist layout: **Between art + visualizer with marquee**

## Implementation Status: COMPLETED

All phases implemented:

1. ✅ Added `m_audioSpectrum` to BarInstance + wired in initNotchScene
2. ✅ Replaced fake visualizer animation with PipeWire-driven bar updates in prepareNotchFrame
3. ✅ Added collapsed media track label + positioning in layoutNotchWidgets
4. ✅ Updated label text from MPRIS in prepareNotchFrame
5. ✅ Removed old `startNotchVisualizerAnimation` / `stopNotchVisualizerAnimation` functions
6. ✅ Fixed compilation error: changed `PipeWireSpectrum::ListenerId` to `std::uint64_t` in bar_instance.h (forward declaration issue)

## Key Changes Made

### bar_instance.h
- Added `PipeWireSpectrum* m_audioSpectrum = nullptr;` to BarInstance
- Added `Label* notchCollapsedMediaTrack = nullptr;` for collapsed pill title/artist
- Added `std::uint64_t notchSpectrumListenerId = 0;` for spectrum listener
- Forward declared `class PipeWireSpectrum;`

### bar.cpp
- Wired `instance.m_audioSpectrum = m_audioSpectrum` in `initNotchScene`
- Created `notchCollapsedMediaTrack` label with marquee in `initNotchScene`
- Added PipeWire spectrum change listener (5 bands) in `initNotchScene`
- Updated `layoutNotchWidgets` to position label between album art and visualizer
- Updated `prepareNotchFrame` to:
  - Pull spectrum values per-frame and update 5 visualizer bars
  - Set collapsed media track label text from MPRIS metadata
  - Handle audioActive/idle state for visualizer
- Removed `startNotchVisualizerAnimation` / `stopNotchVisualizerAnimation` functions
- Updated frame tick callback to only tick notch animations (not visualizer)

### bar.h
- Removed declarations for `startNotchVisualizerAnimation` / `stopNotchVisualizerAnimation`