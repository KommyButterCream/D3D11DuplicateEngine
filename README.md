# D3D11DuplicateEngine
Windows D3D11-based screen capture DLL using the DXGI Desktop Duplication API

# Info
Modular C++ screen capture engine for Windows built on Direct3D 11 and DXGI Desktop Duplication.
Provides desktop frame acquisition, shared texture output, dirty/move rect metadata, mouse pointer state tracking, and optional frame callback processing for external applications or viewer modules.

This project is designed as a DLL-based capture component and integrates shared sibling modules from `Core`, `D3D11Engine`, and `D3D11ImageIO`.

# Features
- DXGI Desktop Duplication API-based desktop capture
- Direct3D 11 device-backed frame acquisition
- Shared texture creation for zero-copy style interop
- Dirty rect and move rect metadata extraction
- Mouse pointer position and shape tracking
- Capture thread support with callback-based processing
- Optional texture export/debug support via D3D11 image I/O
- Modular integration with shared sibling libraries

# Dependencies
- Core
- D3D11Engine
- D3D11ImageIO
- Windows Direct3D 11 / DXGI 1.2+
- C++20
- MSVC (Visual Studio 2022)

# Build Environment
- C++20
- MSVC (Visual Studio 2022)
- Windows 10/11 x64

# Project Structure
- `D3D11DuplicateEngine/` : main DLL project sources and headers
- `D3D11DuplicateEngine/D3D11DuplicateEngine.h` : public engine interface
- `D3D11DuplicateEngine/D3D11DuplicateEngine.cpp` : DXGI duplication initialization and frame capture logic
- `D3D11DuplicateEngine/D3D11DuplicateThread.*` : capture worker thread implementation
- `D3D11DuplicateEngine/CommonTypes.h` : shared capture result and callback-related structures
- `Shaders/` : precompiled shader objects used by related D3D11 modules
- `D3D11DuplicateEngine.sln` : Visual Studio solution

# Repository Layout
This project expects `D3D11DuplicateEngine`, `Core`, `D3D11Engine`, and `D3D11ImageIO` to be placed under the same parent directory.

Example:
```text
Module/
+-- Core/
+-- D3D11Engine/
+-- D3D11ImageIO/
+-- D3D11DuplicateEngine/
```

The Visual Studio solution references shared projects by sibling paths:
- `../Core/Core/Core.vcxproj`
- `../D3D11Engine/D3D11Engine/D3D11Engine.vcxproj`
- `../D3D11ImageIO/D3D11ImageIO/D3D11ImageIO.vcxproj`

# Notes
- Shared libraries are managed as sibling repositories/projects, not as Git submodules.
- Open `D3D11DuplicateEngine.sln` with Visual Studio 2022.
- Build the x64 configuration to produce the D3D11DuplicateEngine DLL.
- The main target is a DLL for desktop capture based on DXGI Desktop Duplication.
- The current implementation captures a selected output and exposes the frame as a D3D11 texture.
- Shared handle output can be used by external viewer or processing modules.
