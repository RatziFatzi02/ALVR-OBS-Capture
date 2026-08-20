# v20.14.1-capture.1

This release packages the ALVR OBS capture prototype for Windows x64.

## Package contents

- ALVR v20.14.1 streamer with the local D3D11 stereo capture producer
- OBS Studio source plugin with left, right, mixed and side-by-side views
- Preset and custom crops with centered zoom
- Three-slot producer ring and one-frame OBS GPU cache
- The ALVR PR #3333 shutdown changes adapted to v20.14.1

## Build checks

- The modified ALVR release streamer build completed.
- The OBS plugin built in RelWithDebInfo mode against OBS Studio 32.0.4.
- `git diff --check` passed.

## Limits

- This is an unofficial build with a private IPC protocol.
- It supports only Windows x64 and D3D11.
- It does not capture audio or HDR. It has no network transport, CPU readback or recording component.
- Testing does not cover a hardware matrix or long-running combined sessions.
