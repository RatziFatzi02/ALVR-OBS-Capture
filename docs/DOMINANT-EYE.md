# Dominant-eye projection

Both the Windows preview and OBS source now map eye textures into a common projection plane using active OpenVR projection bounds and eye translations. The selected eye supplies the overlap; the other eye supplies the missing outer region. A narrow edge blend replaces the previous wide central crossfade, and right dominance now selects the correct eye.

Default alignment distance is 1 m. Preview offers 0.5/1/2 m in its context menu; OBS exposes 0.25â€“20 m. Crop and zoom apply after fusion. Without valid runtime geometry the selected single eye is used. Canted eye axes currently use this fallback. Keep openvr_api.dll next to the preview EXE and in the plugin data directory; the build packaging installs both.

Symmetric frusta do not add directional FOV by themselves. Eye translation on the selected plane adds outer coverage; measured width factor on the tested ALVR headset was 1.026436 at 1 m. This is finite-plane stereo stitching, not depth reconstruction or a demonstrated pixel-exact reproduction of SteamVR. Near objects can remain discontinuous at the narrow seam.

Validation: Release preview and OBS builds passed. projection-test.cpp checks geometry, invalid bounds and compilation of the production preview shaders, plus optionally the active runtime. verify-mixed-effect.cpp renders and reads back the actual OBS effect on D3D11, checking both dominance modes and secondary outer edges. The old effect fails three right-dominance samples (exit 7); the new effect passes (exit 0). A live preview of the SteamVR library no longer showed the broad central double contours.

The streamer capture protocol and the AMD encoder fix are unchanged. Existing local audio extensions were retained in installed builds but are outside this patch.
