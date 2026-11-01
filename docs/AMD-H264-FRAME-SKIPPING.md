# AMD AMF H.264 frame-skipping fix

This change addresses periodic multi-second frozen pictures accompanied by almost zero encoded video bitrate on an AMD H.264 stream. Frame counters can keep advancing while the encoder produces pictures that repeat the previous image.

## Evidence and scope

On the tested Radeon RX 9070 system with Adrenalin 26.8.1 and AMF runtime 1.5.2.0, the ultra-low-latency usage preset reported `RateControlSkipFrameEnable=true`. During the original freezes, H.264 output included 13-byte P slices. A limited offline parser, using the captured SPS/PPS, verified that all 109 small slices in one capture skipped the entire picture.

The user reported that driver 25.12.1 did not exhibit the problem, that later drivers did, and that PICO Connect remained usable. These are comparison reports, not a measured driver-by-driver bisect. The exact AMD internal change and PICO Connect's encoder configuration remain unknown.

## Implementation

- Explicitly disable `AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE` for H.264. Check the setter and verify the effective value after encoder initialization; refuse initialization if it remains enabled.
- End each codec initialization switch branch explicitly. H.264 previously fell through into HEVC and AV1 initialization.
- Check query-timeout setters and use the existing non-timeout polling path if setting the timeout fails.
- Initialize the pre-analysis capability flag.
- Log the H.264 preset value and effective skip, HRD, target bitrate, peak bitrate and VBV values after initialization and the first dynamic update. Report rejected dynamic bitrate updates without logging every frame.

HRD remains enabled. This patch does not force filler data, guarantee that actual bitrate equals the requested bitrate, or change the headset protocol. Normal scene-dependent bitrate variation is expected.

## Validation on 2026-11-01

Windows release builds and the targeted source whitespace check succeeded. The complete local custom streamer was also rebuilt with its capture and audio extensions and installed separately from the original test worktree.

Same PC and headset; H.264, constant requested bitrate 500 Mbit/s, 90 Hz:

| Measurement | Original reference | Isolated patched streamer | Integrated custom streamer |
| --- | --- | --- | --- |
| Observation window | 60 s | 90 s | 90 s, including connection startup |
| Summary samples | 119 | 180 | 168 |
| Samples below 0.02 Mbit/s | 39 | 0 | 0 |
| Separate near-zero intervals | 5 | 0 | 0 |
| Minimum actual bitrate | 0.009152 Mbit/s | 138.77 Mbit/s | 20.95 Mbit/s |
| Median actual bitrate | 179.26 Mbit/s | 231.94 Mbit/s | 231.79 Mbit/s |
| Client FPS | 89-90 | 89-90 | 89-90 |
| Packet loss counter | 0 | 0 | 0 |

Both patched runs read back `skip=0`, target/peak 500,000,000 bit/s and VBV 6,111,110 bits, with AMF_OK results. The integrated run confirmed that SteamVR loaded the newly built driver and that the capture producer initialized.

The user also reported that the original visible freezes were absent during a personal check of the isolated patched streamer. Scene content and head movement were not controlled identically across runs. There was no new subjective confirmation during the integrated automated measurement. These short tests support the fix on this setup; they do not establish long-session stability or coverage of other GPUs, drivers, codecs, the OBS consumer or audio behavior.

The patch bundles frame-skipping control, switch fixes and timeout handling. The tests have not isolated the contribution of each change.
