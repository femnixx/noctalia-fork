# Dynamic Notch Fixes Plan

## Issues to Fix

### 1. Notch Expand/Shrink Animation Issues
**Problem**: The notch animation calls `surface->requestSize()` during animation (bar.cpp:6488), but the design specifies a FIXED surface size at expanded dimensions (bar.cpp:749). This causes:
- Layout corruption after multiple expand/shrink cycles
- Notch disappearing when unfocused after second shrink
- Home tab being cut off to shrunken size

**Root Cause**: `animateNotchExpand` (line 6488) dynamically resizes the Wayland surface during animation, contradicting the fixed-surface design.

**Fix**: Remove dynamic surface resizing during animation. Keep surface fixed at expanded size. Only animate pill geometry (position/size) inside the fixed surface via `updateNotchGeometry`.

---

### 2. Visualizer Upside Down + Wrong Shape
**Problem**: 
- Visualizer bars grow from center (visY = centered) rather than from bottom baseline
- User wants: horizontal line shape at far right, bars grow UP from baseline when playing

**Current Code** (bar.cpp:5221-5238, 5794-5811):
```cpp
const float visY = (pillH - 16.0F * contentScale) * 0.5F; // centered vertically
bar->setSize(barW, 4.0F * contentScale); // base height; animation scales up
// Animation (line 585): h = 4.0F + 8.0F * sin(phase + i * 1.2F) - grows from center
```

**Fix**: 
- Position visualizer at bottom of pill (baseline at pill bottom - margin)
- Make bars grow UPWARD from baseline (decrease Y, increase height)
- Change to horizontal line arrangement (side by side) when idle, vertical bars when playing

---

### 3. Missing Thumbnail + Text Overflow + Media Controls Pushed Down
**Problems**:
- No album art thumbnail showing in collapsed or expanded state
- Track/artist text has no max word/character limit, overflows layout
- Media controls pushed down in expanded view

**Current Code** (bar.cpp:3880-3987):
- `notchMediaArt` loads at 68x68px but may not be visible due to layout
- `notchMediaTrack` and `notchMediaArtist` use `TextEllipsize::End` but no max lines/width constraint
- Flex layout with `flexGrow = 0.35F` for media card may not reserve enough space

**Fix**:
- Ensure album art loads and displays in both collapsed (20x20) and expanded (68x68) states
- Add max-width/max-lines constraints to track/artist labels
- Adjust media card layout to prevent controls from being pushed down

---

### 4. Media Card Layout Redesign
**New Requirements** (from user feedback):
- **Album art centered** in the media card (not left-aligned)
- **Remove progress bar** (thin 4px scrubber) and progress time label ("0:00 / 0:00")
- **Title and artist centered below album art** (replacing progress bar position)
- **Control buttons remain at bottom** (Previous, Play/Pause, Next)

**Current Layout**:
```
[Album Art] [Title]          <- horizontal row
[Artist]
[Progress Bar]
[Time Label]
[Prev] [Play/Pause] [Next]   <- bottom row
```

**New Layout**:
```
        [Album Art]          <- centered, larger?
        [Title]              <- centered below art
        [Artist]             <- centered below title
                             <- (no progress bar)
[Prev] [Play/Pause] [Next]   <- bottom row (unchanged)
```

---

### 5. Collapsed Notch Thumbnail Size Increase
**Requirement**: Make the album art thumbnail larger in the collapsed pill state.

**Current Code** (bar.cpp:5206): `const float artSize = 20.0F * contentScale;` - 20px thumbnail

**Fix**: Increase to larger size (e.g., 32px or 40px) for better visibility in collapsed state.

---

### 6. Expanded Notch Title/Artist Marquee Animation
**Requirement**: In expanded notch, display title and artist horizontally side-by-side with a continuous left-to-right scrolling marquee animation (like audio visualizer text scrolling).

**Current Layout**: Title and artist stacked vertically, centered, static.

**New Layout**:
```
[Title                    ]  <- scrolls left→right continuously
[Artist                   ]  <- scrolls left→right continuously (or combined)
```

**Implementation**:
- Change title/artist to horizontal layout (single line, side by side with separator)
- Enable `autoScroll: true` with `autoScrollSpeed` on the label(s)
- Or implement custom marquee animation using AnimationManager
- Only scroll when text exceeds available width

---

## Implementation Plan

### Phase 1: Fix Notch Animation (No Surface Resize)
- [ ] Remove `instance.surface->requestSize()` calls from `animateNotchExpand` (lines 6488, 6525)
- [ ] Remove `instance.surface->setExclusiveZone()` calls from animation (lines 6491, 6526) - keep exclusive zone fixed
- [ ] Verify `updateNotchGeometry` only animates pill position/size inside fixed surface
- [ ] Ensure input region tracks pill bounds correctly during animation

### Phase 9: Fix Expanded Notch Content Cut-Off During Animation
- [ ] Revert surface to FIXED expanded size (no height animation via requestSize)
- [ ] Update `computeNotchSurfaceSpec` to return expanded dimensions
- [ ] Update `createInstance` to use expanded surface size
- [ ] Keep animation only for pill geometry (position/size) inside fixed surface
- [ ] Keep input region tracking pill bounds for click-through

### Phase 2: Fix Visualizer (Baseline at Bottom, Horizontal Line → Vertical Bars)
- [ ] Change visualizer Y position to bottom of pill with margin (not centered)
- [ ] Modify animation to grow bars UPWARD from baseline (Y decreases, height increases)
- [ ] Change idle state to horizontal line (1px height, full width) at far right
- [ ] Change playing state to vertical bars growing upward from baseline
- [ ] Update both `layoutNotchWidgets` and `prepareNotchFrame` per-frame updates

### Phase 3: Fix Media Card (Thumbnail, Text Limits, Layout)
- [ ] Convert `file://` URLs to local paths using `mpris::cachedArtworkPath()` before `setSourceFile` (bar.cpp:5598-5612)
- [ ] Ensure `notchMediaArt` loads thumbnail correctly in expanded view (bar.cpp:5600-5605)
- [ ] Ensure `notchAlbumArt` loads thumbnail in collapsed view (bar.cpp:5592-5598)
- [ ] Add `maxLines: 1` and `maxWidth` constraints to `notchMediaTrack` and `notchMediaArtist`
- [ ] Adjust media card flex layout to reserve fixed space for controls at bottom
- [ ] Verify album art fallback (disc glyph) shows when no art available

### Phase 4: Media Card Layout Redesign
- [ ] Restructure `buildNotchBentoLayout` media card section (bar.cpp:3870-3992)
- [ ] Change media card to single vertical column (centered alignment)
- [ ] Place album art first (centered, possibly larger - e.g., 100x100px)
- [ ] Place track title below album art (centered, larger font)
- [ ] Place artist below title (centered, smaller font)
- [ ] Remove `notchMediaProgressBar` creation and progress time label
- [ ] Keep media control buttons at bottom (unchanged)
- [ ] Update per-frame updates in `prepareNotchFrame` to show/hide art vs fallback correctly

### Phase 5: Collapsed Notch Thumbnail Size Increase
- [ ] Increase `artSize` in `layoutNotchWidgets` (bar.cpp:5206) from 20px to 32-40px
- [ ] Adjust `artRadius` proportionally (e.g., 8-10px)
- [ ] Update `prepareNotchFrame` (bar.cpp:5782) to match
- [ ] Verify visual balance with visualizer on the right

### Phase 6: Expanded Notch Title/Artist Marquee Animation
- [ ] Change expanded media card layout: title and artist horizontal (side by side with " - " separator)
- [ ] Use single label or two labels with `autoScroll: true` and `autoScrollSpeed`
- [ ] Configure marquee: only scroll when text overflows, pause on hover, loop continuously
- [ ] Alternative: custom AnimationManager-based marquee for more control
- [ ] Update per-frame updates in `prepareNotchFrame` to set combined text

### Phase 9: Fix Expanded Notch Content Cut-Off During Animation
**Problem**: During expand animation, the surface height animates via `requestSize` (async), but `prepareNotchFrame` runs at frame callback time which can be before the compositor acknowledges the new size. The Bento content is laid out for the target size but the surface is still at the old size, causing clipping.

**Root Cause**: 
- Animation callback calls `requestSize` (async)
- Frame callback calls `prepareNotchFrame` -> `layoutNotchWidgets` 
- Configure callback fires later -> `buildScene` -> `layoutNotchWidgets` (with actual surface size)
- These can be out of sync, causing content to exceed surface bounds temporarily

**Fix**: 
- Remove `requestSize` from animation callback (surface height animation)
- Keep surface at FIXED expanded height (original design)
- Only animate pill geometry (position/size) inside the fixed surface via `updateNotchGeometry` and `layoutNotchWidgets`
- This matches the original fixed-surface design that avoids Wayland configure races
- Clicks outside pill pass through because input region tracks pill bounds (already implemented)

---

## Files to Modify
- `src/shell/bar/bar.cpp` - Main implementation file

## Validation
- Test multiple expand/shrink cycles - notch should not disappear or corrupt layout
- Verify visualizer: idle = horizontal line at far right bottom; playing = vertical bars growing upward
- Verify album art shows in both collapsed and expanded states
- Verify track/artist text truncates with ellipsis, doesn't overflow
- Verify media controls stay at bottom of media card
- **Verify new media card layout**: album art centered, title/artist centered below, no progress bar, controls at bottom
- **Verify collapsed thumbnail**: larger album art (32-40px) in collapsed pill
- **Verify marquee animation**: title/artist scroll horizontally left-to-right continuously in expanded notch