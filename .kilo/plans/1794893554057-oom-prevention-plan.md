# OOM Prevention Plan for Noctalia Build

## Problem Analysis

**Root Cause**: Ninja build runs with full parallelism (18 jobs) on a C++23 codebase with heavy template metaprogramming. Each compilation process in debug mode consumes 2-4GB RAM, leading to 36-72GB total demand.

**Evidence from Logs**:
- Electron processes killed with 1.5TB virtual memory, 2-5GB RSS
- btop coredump from libc OOM under memory exhaustion
- Build succeeded in configuration but OOM'd during compilation

## Proposed Solutions (Priority Order)

### 1. Limit Ninja Parallel Jobs (Immediate Fix)
- Add `-j<N>` to meson compile/ninja commands
- Recommended: `-j4` or `-j8` based on available RAM
- Can be set via `NINJAFLAGS` env var or meson `--jobs`

### 2. Use Release Build for Development
- Release builds use less memory per compilation unit
- Add `-O2` optimization without debug symbols
- Trade-off: no debug info, but faster iteration with lower memory

### 3. Configure Meson Build Options
- Disable LTO for debug builds (`-Db_lto=false`)
- Enable unity builds (`-Dunity=on`) to reduce template instantiations
- Use `ccache` to avoid recompilation

### 4. Linker Optimization
- Use `mold` linker instead of `ld` or `lld` (faster, less memory)
- Add `-fuse-ld=mold` to link args

### 5. System-Level Mitigations
- Add swap file (4-8GB) for burst protection
- Consider `ulimit -v` to prevent runaway processes

## Implementation Tasks

### Task 1: Update justfile with Job Limits
```bash
# Add NINJAFLAGS or -j flag to build commands
meson compile -C build-{{m}} -j 4 noctalia noctalia-pam-helper
```

### Task 2: Add Meson Build Options for Memory Efficiency
```meson
# In meson.build or via command line
-Db_lto=false (for debug)
-Dunity=on (reduces template bloat)
```

### Task 3: Configure Mold Linker
```meson
# Add to meson.build link_args
add_project_link_arguments('-fuse-ld=mold', language: 'cpp')
```

### Task 4: Optional - Add Swap Configuration (System Level)
```bash
# Create 8GB swap file if not present
sudo fallocate -l 8G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
```

## Validation Plan

1. Test build with `-j4` and verify completion without OOM
2. Compare build times: debug vs release with job limits
3. Verify binary functionality with limited parallelism
4. Monitor memory usage during build with `watch -n 1 free -h`

## Open Questions

1. What is the physical RAM on the build machine? (Determines optimal `-j` value)
2. Is swap currently configured? (Check with `swapon --show`)
3. Should we default to release builds for development, or keep debug with job limits?
4. Are there specific translation units causing peak memory? (Could isolate with unity builds)

## Rollout Strategy

1. **Phase 1** (Immediate): Add `-j4` to justfile build command, test
2. **Phase 2** (If needed): Add `-Db_lto=false` and `-Dunity=on` to meson config
3. **Phase 3** (If needed): Switch default build mode to release for dev
4. **Phase 4** (System): Add swap file if persistent issues