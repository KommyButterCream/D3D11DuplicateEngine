# D3D11DuplicateEngine
Windows D3D11-based screen capture DLL using the DXGI Desktop Duplication API

# Info
Modular C++ screen capture engine for Windows built on Direct3D 11 and DXGI Desktop Duplication.
Provides desktop frame acquisition, shared texture output, dirty/move rect metadata, mouse pointer state tracking, and optional frame callback processing for external applications or viewer modules.

This project is designed as a DLL-based capture component and integrates shared sibling modules from `Core` and `D3D11Engine`.

# Features
- DXGI Desktop Duplication API-based desktop capture
- Automatic recovery from duplication access loss (lock screen, UAC secure desktop,
  resolution change, fullscreen transition, TDR, RDP connect) and from device removal
- Capture event callback and runtime statistics
- Unchanged-frame skipping: frames where only the pointer moved are neither copied nor published
- High resolution frame pacing driven by a periodic waitable timer
- Direct3D 11 device-backed frame acquisition
- Shared texture creation for zero-copy style interop
- Dirty rect and move rect metadata extraction
- Mouse pointer position and shape tracking
- Capture thread support with callback-based processing
- Modular integration with shared sibling libraries

# Pipeline reference
`docs/capture-pipeline.html` walks the whole flow in one page - initialization order, the
capture state machine, the per-frame pipeline with every early-exit path, the frame slot
ownership protocol, the shared texture handoff, and a table of every failure point with the
event it raises, the counter it bumps, and what the caller is expected to do about it.
Open it in a browser.

# Recovery and diagnostics
The capture thread runs a small state machine: `Idle -> Running -> Reconnecting -> Running`,
with `Faulted` as the terminal state.

- `AcquireNextFrame` failures (`DXGI_ERROR_ACCESS_LOST` and everything else) put the engine
  into `Reconnecting`. It releases the duplication objects and retries `DuplicateOutput` with
  a 50ms..500ms backoff, indefinitely. A lock screen or a UAC prompt therefore no longer
  ends the capture.
- If the D3D device itself is gone, an engine this object owns is discarded and recreated.
  An engine supplied by the caller is not recreated - its device is shared with other users,
  so the engine reports `DeviceRemoved` and enters `Faulted` for the host to handle.
- If the desktop resolution changed while reconnecting, frame resources are rebuilt and
  `ModeChanged` is reported. Textures and the shared handle handed out before that point
  are stale and must be re-acquired.
- `SetCaptureEventCallback` reports `AccessLost` / `Reconnecting` / `Reconnected` /
  `ModeChanged` / `DeviceRemoved` / `DeviceRecreated` / `Faulted`. It is called on the
  capture thread, so it must not block. `Reconnecting` is only reported periodically so a
  long lock screen does not flood the host.
- `GetStats()` returns captured / dropped / timeout / accessLost / reconnect /
  deviceRecreate / invalidRelease counters plus the last HRESULT. `GetCaptureState()` and
  `IsFaulted()` expose the state directly.
- `ReleaseLatestFrameHandle` no longer breaks into the debugger on a bad handle. An
  out-of-range slot or a double release is absorbed and counted in `invalidReleaseCount`,
  which is the counter to watch for caller bugs.

# Frame delivery
- `SetSkipUnchangedFrames` is on by default. Desktop Duplication also returns a frame when
  only the mouse pointer moved; those carry `frameInfo.LastPresentTime == 0` and the desktop
  image is byte-identical to the previous frame. Such frames are counted in
  `CaptureStats::skippedFrames` and are neither copied nor published, so an idle desktop
  costs almost nothing. Turn it off only if the consumer needs a frame for every pointer
  update as well.
  Note that `capturedFrames` is not comparable across the two settings: `AcquireNextFrame`
  coalesces presents, so a slower loop reports fewer, denser frames. What is guaranteed is
  that no visual content is lost, because a skipped frame carries no image change by contract.
- `SetTargetFps(0)` runs uncapped; `AcquireNextFrame` itself blocks, so this is not a busy
  loop. A non-zero value is paced by a periodic high resolution waitable timer
  (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Windows 10 1803+, falling back to a normal
  waitable timer). Measured at 2560x1440: 30 fps -> 33.3ms mean with 0.3ms standard
  deviation, 60 fps -> 16.7ms mean with about 2ms standard deviation.
- `StopThread` latency is bounded by the in-flight `AcquireNextFrame` timeout (500ms), not by
  the frame period; the pacing wait itself aborts immediately on a stop request.

# Dependencies
- Core
- D3D11Engine
- Windows Direct3D 11 / DXGI 1.2+
- C++20
- MSVC (Visual Studio 2022)

# Build Environment
- C++20
- MSVC (Visual Studio 2022)
- Windows 10/11 x64

# Project Structure
- `docs/capture-pipeline.html` : capture flow and failure handling reference (open in a browser)
- `D3D11DuplicateEngine/` : main DLL project sources and headers
- `D3D11DuplicateEngine/D3D11DuplicateEngine.h` : public engine interface
- `D3D11DuplicateEngine/D3D11DuplicateEngine.cpp` : DXGI duplication initialization and frame capture logic
- `D3D11DuplicateEngine/D3D11DuplicateThread.*` : capture worker thread implementation
- `D3D11DuplicateEngine/CommonTypes.h` : common capture result and frame-pool structures
- `Shaders/` : precompiled shader objects used by related D3D11 modules
- `D3D11DuplicateEngine.sln` : Visual Studio solution

# Repository Layout
This project expects `D3D11DuplicateEngine`, `Core`, and `D3D11Engine` to be placed under the same parent directory.

Example:
```text
Module/
+-- Core/
+-- D3D11Engine/
+-- D3D11DuplicateEngine/
```

The Visual Studio solution references shared projects by sibling paths:
- `../Core/Core/Core.vcxproj`
- `../D3D11Engine/D3D11Engine/D3D11Engine.vcxproj`

# Notes
- Shared libraries are managed as sibling repositories/projects, not as Git submodules.
- Open `D3D11DuplicateEngine.sln` with Visual Studio 2022.
- Build the x64 configuration to produce the D3D11DuplicateEngine DLL.
- The main target is a DLL for desktop capture based on DXGI Desktop Duplication.
- The current implementation captures a selected output and exposes the frame as a D3D11 texture.
- Shared handle output can be used by external viewer or processing modules.
