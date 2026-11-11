# Memory Baseline — Stock Noctalia v5 vs. dynamic-notch-niri QML Shell

## Measurements (single monitor, fresh start)

| Shell | RSS | PSS | Notes |
|-------|-----|-----|-------|
| Stock Noctalia v5 (build-debug) | 72.5 MB | 31.8 MB | `./build-debug/noctalia`, one monitor |
| dynamic-notch-niri QML shell | 321 MB | — | Existing shell, one monitor |

## Memory Savings Target

- Stock Noctalia uses ~4.4× less RSS than the QML shell (72.5 vs 321 MB)
- Target after porting: ~55 MB (one sixth of 321 MB)
- Headroom above stock Noctalia baseline: ~17 MB for added features

## Method

```
pid=$(pgrep -x noctalia | head -1)
awk '/^(Rss|Pss):/ {printf "%s %.1f MB\n", $1, $2/1024}' /proc/$pid/smaps_rollup
```

Measured 2026-11-11 on `noctalia-niri/build-debug` with `meson setup build-debug --buildtype=debug -Dcpp_std=c++23 -Dtests=auto`.
