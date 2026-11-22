# Star Wars Episode I: Racer (N64) — Meta Quest 3 VR Edition

A **native, standalone Meta Quest 3 / Quest 3S VR port** of *Star Wars Episode I: Racer* (Nintendo 64), built via **static recompilation** (`N64Recomp` + `N64ModernRuntime`) and rendered natively on Vulkan (`RT64` + **OpenXR**).

**Not an emulator. Exclusive to Meta Quest 3 & Quest 3S.**  
Every hardware assumption, renderer pass, OpenXR composition layer, haptic profile, and performance target is built specifically for Quest 3's Snapdragon XR2 Gen 2 (Adreno 740 GPU), Touch Plus controllers, and pancake optics.

> **No game data is distributed in this repository.**  
> You supply your own legally obtained *Star Wars Episode I: Racer (USA)* N64 cartridge dump (`.z64`, `.v64`, or `.n64`). Recompiled code and APKs built from your ROM stay strictly on your own machine and headset.

---

## Why This Exists

The original Nintendo 64 release runs at ~20–30 fps in 320×240 (or 480p hi-res) with 2-player splitscreen caps. By statically translating the game's MIPS binary into native `arm64-v8a` machine code and hooking directly into RT64's Vulkan pipeline and RDRAM state, Quest 3 unlocks:

- **True Stereo 6DOF VR** at **90 Hz / 120 Hz** via native **OpenXR + Vulkan** (`XR_KHR_vulkan_enable2`)
- **Crisp OpenXR Quad Composition Layer** for menus, Watto's Shop, and curved cockpit HUD (sharp text on pancake lenses, independent of 3D eye-buffer scale)
- **Hardware Fixed Foveated Rendering (`XR_FB_foveation_vulkan`)** + Quest 3 sustained CPU/GPU performance profiles (`XR_EXT_performance_settings`)
- **VR Engine Enhancements** (modeled on community VR/recomp best practices): expanded head-tracked frustum culling so track walls never disappear when peeking out of the cockpit, world-space 3D lens flares, optional nose-bob / track-seam stabilization, and diegetic cockpit engine gauges
- **Physical Dual-Throttle Podracer Controls** wiring Touch Plus controllers into the ROM's native `RRDUAL` twin-stick podracing mode, with **asymmetric per-engine Touch Plus haptics** (feel your left or right turbine scrape, overheat, catch fire, and boost independently)
- **8-Player Full-Screen Online VR Multiplayer** (modeled on [MrSco/goldeneye-vr](https://github.com/MrSco/goldeneye-vr)): ENet + `libjuice` ICE/STUN/TURN (zero port-forwarding), LAN discovery, Cloudflare Worker WebSocket lobby (Public / Friends / Private codes), host migration, 6DOF spectator ride-along, 3D proximity voice chat, and custom multiplayer rulesets
- **In-Headset GEVR-Style VR Launcher, Force Ghosts & Mod Packs**: Time Trial holographic ghosts, RT64 ASTC HD texture packs + restored N64 textures, unlocked LucasArts hidden Debug Menu / Vehicle Stat Editor / `AI Level x10`, and 30-second roomscale Replay Theater

---

## Roadmap & Milestone Status

See [ROADMAP.md](ROADMAP.md) for the full technical architecture across all phases.

| Phase | Scope | Status |
|---|---|---|
| **Phase 1** | Clean standalone MIT repo, Quest 3 `arm64-v8a` Vulkan APK pipeline, macOS/Linux/Windows one-command `./build.sh` | **IN PROGRESS** |
| **Phase 2** | Native OpenXR Vulkan Stereo VR (`XR_KHR_vulkan_enable2`), per-eye asymmetric projections, 6DOF head tracking, Quad HUD layer, 90/120 Hz, FFR, Touch Plus controls & haptics | **PLANNED** |
| **Phase 3** | GEVR-style in-headset VR Launcher & pause lobby, Physical Dual-Throttle Levers (`RRDUAL`), diegetic cockpit HUD, ROM Debug Menu / Stat Editor, RT64 HD texture pack support | **PLANNED** |
| **Phase 4** | MrSco-style 8-player online multiplayer (ENet + `libjuice` ICE/STUN/TURN + LAN + WebSocket Lobby + Friends + Spatial Voice), Time Trial Force Ghosts & leaderboards | **PLANNED** |
| **Phase 5** | Application SpaceWarp (`XR_FB_space_warp`), single-pass `VK_KHR_multiview` optimization, spatial HRTF turbine audio, Elimination/Mirror/Reverse modes, Replay Theater | **PLANNED** |

---

## Requirements

### Your Own ROM
*Star Wars Episode I: Racer (USA)* N64 cartridge dump (`.z64`, `.v64`, or `.n64` — converted automatically during build or on-headset import):

| Dump | SHA-1 (`.z64` byte order) |
|---|---|
| `Star Wars Episode I - Racer (USA).z64` (`NEPE`) | `3542d5597c8a56ea8f5c63bceae97a24c4c08d58` |

### Build Host (macOS / Linux)
- **Git**, **Python 3**, **Clang / Clang++** (host tools), **Java 17+**
- **Android SDK** with:
  - Platform `android-34` (or `35`)
  - Build-Tools `34.0.0`
  - CMake `3.22.1` + Ninja
  - Android NDK (`25.1.8937393` or `28.2.13676358`)
- **Meta Quest 3 / Quest 3S** with Developer Mode enabled (for `adb install`)

---

## Building for Quest 3

```bash
git clone --recursive https://github.com/e-ee/Star-Wars-Episode-I-Racer-N64.git
cd Star-Wars-Episode-I-Racer-N64
./build.sh "/path/to/Star Wars Episode I - Racer (USA).z64"
```

The build script:
1. Validates your ROM SHA-1 and converts `.v64`/`.n64` to `baserom.z64` locally (ignored by git).
2. Applies submodule compatibility patches via `scripts/apply_patches.sh`.
3. Compiles host `N64Recomp` & `RSPRecomp` tools once into `build/recomp-tools/`.
4. Statically recompiles the game MIPS code & `aspMain` audio microcode locally.
5. Builds the `arm64-v8a` Vulkan + OpenXR Quest 3 APK at `android/app/build/outputs/apk/debug/app-debug.apk`.

Install to your Quest 3 over USB / Wi-Fi ADB:
```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

---

## Credits & Licensing

Licensed under the [MIT License](LICENSE). Built standing on the shoulders of:

- **[sp00nznet/racer](https://github.com/sp00nznet/racer)** — N64Recomp symbol map, patches, and desktop recompilation foundation (MIT)
- **[dguillot-gh/star-wars-racer-recomp](https://github.com/dguillot-gh/star-wars-racer-recomp)** — Android `arm64-v8a` RT64 Vulkan port & runtime fixes (MIT)
- **[MrSco/goldeneye-vr](https://github.com/MrSco/goldeneye-vr)** — Quest 3 OpenXR harness architecture, in-VR ImGui launcher UX, and ENet + `libjuice` + Cloudflare Worker multiplayer stack (MIT)
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp) / [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)** — static recompiler & libultra runtime (MIT)
- **[RT64](https://github.com/rt64/rt64) / [plume](https://github.com/renderbag/plume)** — high-level N64 renderer & Vulkan abstraction (MIT)
- **[SW_RACER_RE](https://github.com/tim-tim707/SW_RACER_RE)**, **[swe1r-patcher](https://github.com/JayFoxRox/swe1r-patcher)**, and **[SWE1R-Mods](https://github.com/louriccia/SWE1R-Mods)** — community research on Racer internals, multiplayer upgrades, and restored N64 textures

*Star Wars Episode I: Racer* is a trademark of Lucasfilm Ltd. Nintendo 64 is a trademark of Nintendo. This project is an independent, non-commercial open-source port containing zero copyrighted game assets.
