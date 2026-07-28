# AGENTS.md — Building & Installing the VST3/AU Plugin

This file is a quick-reference for AI agents and developers who need to build
and install the Nuked SC-55 VST3/AU plugin. For the standard SDL2 frontend
build, see [`documentation/BUILDING.md`](documentation/BUILDING.md). For the
plugin architecture and design rationale, see
[`documentation/VST-PORT.md`](documentation/VST-PORT.md).

## Upstream PRs

| PR | Repo | Branch | Status | Description |
|----|------|--------|--------|-------------|
| [#73](https://github.com/linoshkmalayil/Nuked-SC55-GUI-Float/pull/73) | linoshkmalayil/Nuked-SC55-GUI-Float | `fix/rom-loading-bugs` | **OPEN** — waiting for maintainer test & merge | Fix waverom corruption causing silent notes in JV-880 mode (two ROM loading bugs) |
| – | jcmoyer/Nuked-SC55 | – | Not needed | Bugs not present in this repo — they were introduced in GUI-Float fork |

## Prerequisites

- **CMake** ≥ 3.22
- **C++23** compiler (clang 19+, gcc 14+, or MSVC 19.39+)
- **JUCE** cloned at `external/JUCE`
  ```bash
  git clone git@github.com:juce-framework/JUCE.git external/JUCE
  ```
- **macOS only**: Xcode Command Line Tools (for AU + VST3 codesigning)

The plugin target (`nuked-sc55-vst`) does **not** require SDL2 or RtMidi — it
links only against `nuked-sc55-backend` and `nuked-sc55-common`.

## Build

### Configure (Release)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNUKED_ENABLE_VST=ON
```

If SDL2 is present on the system, the standard frontend will also be
configured — that is fine. If SDL2 is missing, the VST target still builds.

### Build the plugin

```bash
cmake --build build --target nuked-sc55-plugins --config Release -j
```

> **Important:** Always use the `nuked-sc55-plugins` target, **not**
> `nuked-sc55-vst`. The latter only builds the JUCE SharedCode static library
> and will *not* relink or reinstall the actual `.vst3` / `.component` bundles.
> `nuked-sc55-plugins` is an umbrella target that depends on both
> `nuked-sc55-vst_VST3` and `nuked-sc55-vst_AU`, so a single command produces
> and installs the final plugin bundles.

Build artifacts land under:

```
build/nuked-sc55-vst_artefacts/Release/VST3/Nuked SC-55.vst3
build/nuked-sc55-vst_artefacts/Release/AU/Nuked SC-55.component
```

### Incremental rebuild after source edits

```bash
cmake --build build --target nuked-sc55-plugins --config Release -j
```

CMake will only recompile changed translation units (`PluginProcessor.cpp`,
`PluginEditor.cpp`) and relink — typically a few seconds.

## Install

`COPY_PLUGIN_AFTER_BUILD TRUE` is set in `CMakeLists.txt`, so the build
**automatically copies** the bundles to the user plugin directories:

| Format | Install path (macOS) |
|---|---|
| VST3 | `~/Library/Audio/Plug-Ins/VST3/Nuked SC-55.vst3` |
| AU | `~/Library/Audio/Plug-Ins/Components/Nuked SC-55.component` |

No manual `cmake --install` step is needed for the plugin. If the copy fails
due to permissions, copy manually:

```bash
cp -R "build/nuked-sc55-vst_artefacts/Release/VST3/Nuked SC-55.vst3" ~/Library/Audio/Plug-Ins/VST3/
cp -R "build/nuked-sc55-vst_artefacts/Release/AU/Nuked SC-55.component" ~/Library/Audio/Plug-Ins/Components/
```

### Windows / Linux

- **Windows**: VST3 installs to `C:\Program Files\Common Files\VST3\`
- **Linux**: VST3 installs to `~/.vst3/` or `/usr/lib/vst3/`
- **AU is macOS-only** — it will not be built on other platforms.

## Codesigning (macOS)

The JUCE build signs bundles **adhoc** (no Developer ID). This is sufficient
for local testing but has caveats:

1. **Gatekeeper**: On first load, macOS may block the plugin. Open
   **System Settings → Privacy & Security** and click "Allow Anyway" for
   `Nuked SC-55`.
2. **DAW rescan**: After allowing, restart the DAW so it rescans plugins.
3. **Distribution**: For sharing builds, re-sign with a Developer ID:
   ```bash
   codesign --force --deep --sign "Developer ID Application: <Name>" \
     ~/Library/Audio/Plug-Ins/VST3/Nuked\ SC-55.vst3
   codesign --force --deep --sign "Developer ID Application: <Name>" \
     ~/Library/Audio/Plug-Ins/Components/Nuked\ SC-55.component
   ```

## Verification

### Confirm the bundles exist and are signed

```bash
codesign -dv ~/Library/Audio/Plug-Ins/VST3/Nuked\ SC-55.vst3
codesign -dv ~/Library/Audio/Plug-Ins/Components/Nuked\ SC-55.component
```

Expected: `Signature=adhoc`, `Format=bundle with Mach-O`, arm64 (or universal).

### Validate AU in Logic Pro / GarageBand

macOS ships `auval` to validate Audio Units:

```bash
auval -v aufx Nukd Sc55
```

- `aufx` = effect/audio-unit type (JUCE `IS_SYNTH` instruments still validate
  as `aufx` here)
- `Nukd` = manufacturer code (from `PLUGIN_MANUFACTURER_CODE`)
- `Sc55` = plugin code (from `PLUGIN_CODE`)

If `auval` fails, Logic Pro will refuse to load the AU. Common causes: missing
codesign, wrong architecture, or Info.plist issues.

### Force DAW rescan

- **Logic Pro**: Logic Pro → Plug-in Manager → find "Nuked SC-55" → rescan
- **Ableton Live**: Preferences → Plug-ins → "Rescan"
- **Reaper**: Options → Preferences → VST → "Clear cache / Re-scan"

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `JUCE not found at external/JUCE` | JUCE not cloned | `git clone git@github.com:juce-framework/JUCE.git external/JUCE` |
| Build succeeds but DAW doesn't see plugin | Wrong install path or needs rescan | Verify path in table above; force DAW rescan |
| AU fails `auval` | Codesign or arch mismatch | Run `auval -v aufx Nukd Sc55` and read the report |
| "Nuked SC-55 is damaged" on load | macOS Gatekeeper blocking adhoc signature | System Settings → Privacy & Security → Allow Anyway |
| Plugin loads but no sound | ROM not loaded | The plugin prompts for ROM files on first load — point it at a valid SC-55 romset |
| Build fails on C++23 features | Compiler too old | Use clang 19+, gcc 14+, or MSVC 19.39+ |

## CMake options reference

| Option | Default | Purpose |
|---|---|---|
| `NUKED_ENABLE_VST` | `ON` | Build the VST3/AU plugin target |
| `JUCE_DIR` | `external/JUCE` | Path to JUCE checkout |
| `NUKED_ENABLE_TESTS` | `OFF` | Enable the test suite (needs Catch2 + ROMs) |
| `CMAKE_BUILD_TYPE` | (none) | Use `Release` for distributable builds |

## Quick reference (copy-paste)

```bash
# One-time: ensure JUCE is present
git clone git@github.com:juce-framework/JUCE.git external/JUCE

# Configure + build + auto-install
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNUKED_ENABLE_VST=ON
cmake --build build --target nuked-sc55-plugins --config Release -j

# Verify install
ls ~/Library/Audio/Plug-Ins/VST3/ | grep -i nuked
ls ~/Library/Audio/Plug-Ins/Components/ | grep -i nuked

# Validate AU
auval -v aufx Nukd Sc55
```
