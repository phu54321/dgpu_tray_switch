# dGPU Tray Switch

A tiny tray app that switches a laptop's discrete GPU on or off, similar to G-Helper's Eco mode but for any laptop.

It doesn't use vendor ACPI calls. It enables or disables the dGPU's PnP device node, the same thing Device Manager does, and then restarts the NVIDIA services so they let go of the GPU and pick it up again. The app is written in C++ against the plain Win32 API, with no runtime dependencies.

## Features

- **Tray toggle**
  - Right-click the icon for a menu with status, Enable, Disable, (Manual) Restart NVIDIA services, Start with Windows, and Exit.
  - Left-click does nothing, so the dGPU can't be switched by accident.
- **Queued requests**
  - Clicks are never blocked while an operation is running. The app always moves toward the state you asked for most recently.
  - Flipping it back and forth during an operation ends up doing nothing.
- **Service handling**
  - Turning off: stops the NVIDIA services, disables the device, then starts the services again.
  - Turning on: enables the device, waits for the driver, then restarts the services.
- **Boot restore**
  - When started at logon, the app first enables the dGPU, then returns it to the last state you chose.
  - The services are restarted only if the device was actually toggled.
- **Sleep and hibernate**
  - The dGPU isn't touched before sleep, because Windows gives apps only about 2 seconds and doesn't wait for them.
  - After resume, the app runs the same restore as at boot: it enables the dGPU, then returns it to your chosen state. This happens even if the screen stays off, for example in clamshell mode.
- **Start with Windows**
  - Uses a Task Scheduler logon task with highest privileges, so there is no UAC prompt at logon.

Tray icon states:

| Color | Meaning |
|---|---|
| Green | dGPU on |
| Grey with a red slash | dGPU off (Eco) |
| Amber | Operation running or queued |
| Red badge | Device problem or no dGPU found |

## Build

You need CMake 3.20 or newer, plus MinGW-w64 (gcc), clang, or MSVC.

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_RC_COMPILER=windres
cmake --build build
```

This produces `build/dgpu_tray_switch.exe`, a single statically linked exe when built with MinGW.

To run controller regression tests with fake GPU and service APIs (no hardware
changes), configure with `-DDGPU_BUILD_TESTS=ON`, build, then run
`ctest --test-dir build --output-on-failure`.

The app icon is generated at build time by `tools/icongen.cpp`. It uses the same renderer as the tray icons in `src/icons.cpp`.

## Usage

1. Run `dgpu_tray_switch.exe`. It asks for administrator rights, because changing a device or a service needs them.
2. Right-click the tray icon and choose **Enable dGPU** or **Disable dGPU (Eco)**.
3. Tick **Start with Windows** in the menu to have it start at logon and restore your last choice.

Your last choice is remembered. A normal launch also brings the device to that state.

## Configuration

Settings are optional registry values under `HKCU\Software\DgpuTraySwitch`:

| Value | Type | Default | Purpose |
|---|---|---|---|
| `DesiredEnabled` | DWORD | (set by the app) | The state you chose last |
| `DeviceInstanceId` | SZ | first `PCI\VEN_10DE` display adapter | Choose the device explicitly, e.g. for AMD dGPUs or multi-GPU setups. Copy it from Device Manager → Details → Device instance path. |
| `Services` | SZ | `NVDisplay.ContainerLocalSystem;NvContainerLocalSystem` | `;`-separated services to restart |
| `NvDelayMs` | DWORD | `5000` | Wait after enabling before restarting services |

## Notes

- If a disable can't happen right away (for example, something still has the GPU open), Windows may report that a restart is required. The app tells you when that happens.
- Keep the display that's in use connected to the iGPU. Disabling a GPU that drives a screen turns that screen off.
- This changes device state system-wide. Use at your own risk.
