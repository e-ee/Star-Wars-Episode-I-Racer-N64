# Technical Roadmap — Star Wars Episode I: Racer (Quest 3 Edition)

This document captures the 5-phase engineering plan for building the definitive Meta Quest 3 VR version of *Star Wars Episode I: Racer (N64)*.

---

## Phase 1 — Clean MIT Foundation & Quest 3 APK Pipeline
- Standalone MIT repository (`com.swe1r.quest3`), zero proprietary dependencies.
- Cross-platform `./build.sh` supporting macOS (`x86_64` & `arm64`), Linux, and Windows (`build.ps1`), including macOS `dxc-macos` zlib shim + host `file_to_c` compilation.
- Vendored Khronos OpenXR headers and Meta Quest `libopenxr_loader.so` (`arm64-v8a`).
- Verified ROM pipeline (`N64Recomp` + `fix_fallthroughs.py` + `post_recomp_patches.py` + `gen_lookup_table.py` + `RSPRecomp` `aspMain.toml`).

## Phase 2 — Native Quest 3 OpenXR Vulkan Stereo VR
- **Quest 3 Manifest & Activity**: Immersive VR categories (`IMMERSIVE_HMD`, `com.oculus.intent.category.VR`), `quest3|quest3s` target metadata, Vulkan 1.1+ requirement, flat PNG launcher icon (`ic_launcher_quest.png`) so Meta Horizon OS displays crisp library art.
- **`plume` External Vulkan Hooks (`XR_KHR_vulkan_enable2`)**:
  - OpenXR-negotiated `VkInstance`, `VkPhysicalDevice`, and `VkDevice` via `volk`.
  - Offscreen / headless swapchain mode bypassing `VkSurfaceKHR` crashes during pause/Universal Menu transitions, with explicit GPU image blits + `VkImageMemoryBarrier` transitions into acquired OpenXR swapchain images (`VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` on release).
- **RT64 Per-Eye Stereo & HUD Separation**:
  - Extract near/far planes from N64 perspective matrices; rebuild true asymmetric per-eye OpenXR frustum projections (`XrFovf`).
  - Apply 6DOF head pose + per-eye IPD offset relative to yaw-recenter origin.
  - Render 3D perspective scenes per eye and isolate 2D HUD / menus into a dedicated transparent OpenXR `XrCompositionLayerQuad` layer.
- **Quest 3 Hardware Extensions**: `XR_FB_display_refresh_rate` (72 / 90 / 120 Hz), `XR_FB_foveation_vulkan` (hardware FFR), `XR_EXT_performance_settings`, `XR_FB_color_space`.
- **Touch Plus Controls & Asymmetric Haptics**: Full OpenXR action set bound to `/interaction_profiles/oculus/touch_controller`, N64 input bridge, and rumble routing to left/right controllers.

## Phase 3 — GEVR-Style Launcher, Dual-Throttle Levers & Community Mod Features
- **3D/Quad In-Headset ImGui VR Launcher & Pause Menu** (modeled on `goldeneye-vr`): Play · Host · Join · Ghosts · Mods & Texture Packs · Settings · On-headset ROM picker.
- **Physical Dual-Throttle VR Levers (`RRDUAL`)**: Grab left & right virtual thruster controls in 3D space, push/pull differential thrust to carve turns, pull back to air-brake, twist wrists to roll, and punch forward to fire Boost.
- **Asymmetric Engine Telemetry Haptics**: Read live RDRAM pod state (`0x80118F90` / `'Test'` struct) for per-engine RPM rumble, canyon scrapes, engine fire warning, and boost harmonics.
- **Diegetic Cockpit Frame & Rear Holo-Mirror**: Optional 3D cockpit struts with glowing heat/boost/speed readouts.
- **Engine Enhancements**:
  - Head-tracked frustum cull expansion + world-space 3D lens flares + nose-bob smoothing.
  - Unlocked ROM Hidden Debug Menu (`Debug Level`, `Edit Vehicle Stats`, `AI Level x10`, unlock all pods/tracks).
  - RT64 ASTC-compressed HD Texture Packs & `louriccia/SWE1R-Mods` Restored N64 Textures (plus optional downloadable packs from `Mods & Packs`).

## Phase 4 — MrSco-Style 8-Player Online Multiplayer & Time Trial Ghosts
- **Network Stack (ported from `goldeneye-vr`)**:
  - **ENet** reliable/unreliable channels over UDP (`27007`) + **LAN UDP discovery** (`27008`) + **Direct IP**.
  - **`libjuice` ICE / STUN / Cloudflare TURN** NAT traversal (playable over normal Wi-Fi & mobile hotspots without router port-forwarding).
  - **Cloudflare Worker Lobby Service + WebSocket Signaling**: Public server browser, Friends-only lobbies, and Private room codes.
- **Breaking the 2-Player Splitscreen Cap → 8-Player Full-Screen VR**:
  - Every Quest 3 renders full-screen 3D VR locally while RDRAM hooks inject up to 7 remote players' pod positions, orientation matrices, engine flames, spark/fire states, and collision flags at 60+ Hz with smooth render interpolation.
- **Lobby & Race Features**: Host migration, late-join 6DOF Spectator Ride-Along, in-race pause lobby & next-track voting, Friends & Invites, and 3D Spatial Proximity Voice Chat.
- **Multiplayer Rulesets**: Upgraded Pods Allowed (`swe1r-patcher` style) vs. Stock vs. Equalized Max-Tier; Full Contact vs. Non-Collision Ghost Racing; AI Grid Fill.
- **Time Trial "Force Ghosts"**: Record, save, share, and race against holographic ghost replays of personal bests and leaderboard records.

## Phase 5 — Quest 3 Hyper-Optimization & Extra Modes
- **Single-Pass `VK_KHR_multiview` in RT64** to halve CPU/shader draw overhead across both eyes.
- **Application SpaceWarp (`XR_FB_space_warp`)** feeding RT64 velocity buffers for locked 120 Hz at high supersampling.
- **Spatial HRTF Pod Audio**: Stereo/3D positioned rival turbines with Doppler shift.
- **New Modes & Replay Theater**: Elimination / Knockout races, Mirror & Reverse Track variants, and a 30-second instant 6DOF roomscale Replay Theater.
