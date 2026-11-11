# Gap Table — QML Shell vs Noctalia v5

## My QML Shell Tabs & Features vs Noctalia v5

| My tab/feature | Noctalia already has it? | Gap / difference | Suggested approach |
|---|---|---|---|
| **Home** tab (MusicCard + weather card + clock + volume + brightness + battery cards) | Partially — bar has clock, weather, volume, brightness, battery widgets; Control Center has audio tab | Home is a dashboard of mini-cards; Noctalia spreads these across bar + Control Center | Config: compose bar widgets + Control Center tabs |
| **Music** tab (full album art + playback controls + lyrics) | Partially — Noctalia media widget + media OSD (PipeWire MPRIS) | Full-screen album art view + lyrics panel not in core | Luau plugin or C++ module for full-screen music view |
| **Clipboard** tab (history, paste actions) | Yes — Noctalia clipboard panel + history + IPC | Nearly identical | Config: enable clipboard widget on bar |
| **Alerts** tab (notifications with actions, dismiss, clear-all) | Yes — Noctalia notifications toast/history panel | Nearly identical | Config: enable notifications widget |
| **Shelf** tab (drag-and-drop file staging, copy path, open folder) | No — not in Noctalia core | Drag-and-drop staging area is unique | Luau plugin or C++ module |
| **Notes** tab (in-memory scratch text editor) | No — not in Noctalia core | Scratchpad is unique | Luau plugin |
| **Controls** tab — Output card (sink dropdown, mute, volume slider, pct) | Yes — Noctalia Control Center audio tab (PipeWire) | Nearly identical | Config |
| **Controls** tab — Input card (source dropdown, mute, volume slider, pct) | Yes — Noctalia Control Center audio tab | Nearly identical | Config |
| **Controls** tab — per-app volumes with per-app output picker | Partially — Noctalia audio tab shows app streams | Per-app output routing via headphones button dropdown not in core | C++ module (PipeWire direct) or Luau plugin |
| **Bluetooth** tab (power toggle, scan, connect/disconnect, battery) | Yes — Noctalia Control Center bluetooth tab + bar widget | Nearly identical | Config |
| **Bluetooth** tab — rfkill unblock | Partial — Noctalia bluetooth tab | rfkill unblock pill not in core | C++ module or Luau plugin calling rfkill |
| **Calendar** tab (month grid + today card) | Yes — Noctalia calendar tab + bar widget | Nearly identical | Config |
| **Search** tab (app launcher, fuzzy scoring, keyboard nav) | Yes — Noctalia launcher (vendored fzy) | Nearly identical | Config |
| **Wi-Fi** tab (on/off, scan, connect/disconnect/forget, password) | Yes — Noctalia network bar widget + control center network tab | Nearly identical | Config |
| **System** tab (CPU, memory, CPU temp, power draw, GPU bars) | Yes — Noctalia system monitor widget | Nearly identical | Config |
| **Weather** tab (city, icon, temp, description) | Yes — Noctalia weather widget | Nearly identical | Config |
| **Settings** tab (DND, notification history limit, theme pills, About) | Yes — Noctalia settings window + control center | Nearly identical | Config |
| Battery indicator (bar widget) | Yes — Noctalia battery bar widget | Nearly identical | Config |
| Top tab strip (14 tabs + second row of smaller tabs) | No — Noctalia uses separate bar, panels, launcher | Tab strip is a custom surface model | **Core C++ module** — separate surface with tab strip |
| Shelf drag-and-drop (Process/wl-copy/cp/rm/xdg-open) | No — not in Noctalia core | File staging is unique | Luau plugin or C++ module |
| Process/subprocess spawning (grim/slurp/nmcli/brightnessctl/sysstat.sh) | Partial — Noctalia uses pactl for audio, nmcli for network | Direct subprocess spawning for screenshot/sysstat | Luau plugin or C++ module |
| MPRIS media (playback controls, artwork, lyrics) | Yes — Noctalia media widget + MPRIS | Lyrics panel (lrclib.net) not in core | Luau plugin |

## Tabs That Map Directly to Noctalia (Config Only)

| My QML tab | Noctalia equivalent | Config path |
|---|---|---|
| Clipboard | `clipboard` bar widget + panel | `[bar.main.start]` / `[bar.main.end]` |
| Alerts | `notification` bar widget + notification panel | `[bar.main.end]` + `[notification]` |
| Calendar | `calendar` bar widget + calendar tab | `[bar.main.start]` + `[control_center.calendar]` |
| Search | `launcher` bar widget + launcher panel | `[bar.main.start]` |
| Wi-Fi | `network` bar widget + network tab | `[bar.main.end]` + `[control_center.network_tab]` |
| System | `sysmon` bar widget + system tab | `[bar.main.end]` + `[control_center.system_tab]` |
| Weather | `weather` bar widget + weather tab | `[bar.main.end]` + `[control_center.weather_tab]` |
| Settings | settings window + control center settings | `noctalia msg settings open` |
| Controls (output/input/mute/pct) | `audio` bar widget + audio tab | `[bar.main.end]` + `[control_center.audio_tab]` |
| Bluetooth | `bluetooth` bar widget + bluetooth tab | `[bar.main.end]` + `[control_center.bluetooth_tab]` |
| Battery indicator | `battery` bar widget | `[bar.main.end]` |

## Tabs That Need New Code (Luau Plugin or C++ Module)

| My QML tab | What's missing in Noctalia | Approach |
|---|---|---|
| Shelf | Drag-and-drop file staging surface | Luau plugin (file watcher + copy/rm/xdg-open) |
| Notes | Scratch text editor | Luau plugin |
| Music | Full-screen album art + lyrics panel | Luau plugin (MPRIS already available via PipeWire) |
| Controls (per-app output picker) | Per-app stream routing dropdown | C++ module (PipeWire `pactl move-sink-input` equivalent via libpipewire) |
| Bluetooth (rfkill unblock) | rfkill unblock pill | C++ module or Luau plugin calling `rfkill` |
| Top tab strip | Separate tabbed surface model | **Core C++ module** — biggest gap |

## Approach Summary

- **~10 tabs** map to existing Noctalia widgets/tabs via config only
- **~4 tabs** need Luau plugins (Shelf, Notes, Music lyrics, Bluetooth rfkill)
- **1 tab** needs C++ module (Controls per-app output picker via PipeWire)
- **1 surface** needs core C++ work (top tab strip model)
