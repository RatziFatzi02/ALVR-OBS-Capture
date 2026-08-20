# Building the OBS plugin on Windows

Install these first:

- Windows x64
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.28 or newer
- Ninja or the Visual Studio 2022 CMake generator
- OBS Studio and the source tree for the same OBS version

Run the build from PowerShell:

```powershell
Set-Location .\obs-plugin
.\build.ps1 `
    -ObsRoot 'C:\Program Files\obs-studio' `
    -ObsSourceDir 'C:\src\obs-studio-32.0.4'
```

The script writes the files to `obs-plugin\outputs\install\alvr-capture`.

The plugin and modified streamer communicate through `Local\ALVR-Capture-Prototype-v1`. This private protocol is not part of the ALVR API. Distribute the plugin only with the matching streamer source and binaries.
