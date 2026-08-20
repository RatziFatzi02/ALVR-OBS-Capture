# ALVR OBS Capture

ALVR OBS Capture connects a modified ALVR streamer to OBS through local D3D11 textures. It runs on Windows x64 and keeps the frame on the GPU.

The code starts from [ALVR v20.14.1](https://github.com/alvr-org/ALVR/tree/v20.14.1). The [original ALVR repository](https://github.com/alvr-org/ALVR) already covers headset support, networking, setup and troubleshooting. This page documents only the changes in this fork.

> [!WARNING]
> This is an unofficial prototype. It is not an ALVR release. The package needs the ALVR v20.14.1 headset client. Its OBS plugin was built against OBS Studio 32.0.4.

## What changed

### ALVR streamer

After ALVR composes its layers, the streamer copies the left and right eye into a three-slot D3D11 texture ring. The copy happens before ALVR's optional color correction. The streamer publishes a frame ID only after it has copied both eyes.

ALVR never waits for OBS. If OBS occupies every slot, ALVR skips that capture copy and continues streaming to the headset.

This fork also backports the shutdown changes from [ALVR PR #3333](https://github.com/alvr-org/ALVR/pull/3333) to the v20.14.1 interfaces. The backport joins the runtime threads before SteamVR unloads the driver DLL.

### OBS source plugin

The plugin adds an **ALVR Capture** source. It reads the shared textures directly, without window capture, CPU readback or another encoder.

Available views are Left Eye, Right Eye, left-dominant mix, right-dominant mix and Side-by-Side. Crop options cover 16:9, 16:10, 4:3 and custom values. Centered zoom ranges from 100 to 400 percent.

OBS keeps the last complete stereo frame in a GPU texture. If the newest ALVR slot is busy, OBS draws that cached frame instead of an empty source. The plugin uses OBS's sRGB path and does not apply a fixed gamma correction.

## Install the release package

1. Close ALVR, SteamVR and OBS Studio.
2. Extract the `ALVR` folder and run its `ALVR Dashboard.exe`. Do not merge it into a different ALVR version.
3. Copy the contents of `OBS-Plugin` into the OBS Studio installation directory. Keep the `obs-plugins` and `data` folders intact.
4. Restart OBS Studio and add **ALVR Capture** under **Sources**.

The source starts in Right Eye mode. ALVR and OBS must run in the same Windows session and on the same graphics adapter.

## Source and builds

The ALVR files remain in their upstream directories. The OBS code lives in [`obs-plugin`](obs-plugin). See [`docs/BUILDING-OBS-PLUGIN.md`](docs/BUILDING-OBS-PLUGIN.md) for its Windows build command.

Each release includes SHA-256 checksums. The ALVR streamer and OBS plugin compile, but this version has not been tested across a hardware matrix or in long-running sessions.

## License and attribution

The existing [MIT License](LICENSE) covers ALVR and this fork. The release package keeps the Android platform-tools notice file. Neither the ALVR project nor the OBS project endorses this fork.

The capture code and OBS plugin were written with the help of GPT-5.6.
