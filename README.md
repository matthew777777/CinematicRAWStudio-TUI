# Cinematic RAW Studio — TUI (`craw-tui`)

Terminal app to browse `.craw` takes and render them to edit-ready video or
lossless CinemaDNG. Apache-2.0, written from scratch on the
CinematicRAWStudio decoder; video encoding is done by FFmpeg (external
process, must be on `PATH`).

```
cmake -S . -B build -G Ninja && ninja -C build      # needs ../CinematicRAWStudio-Decoder
./build/develop_tests
./build/craw-tui ~/Movies/takes                      # or: craw-tui take.craw ...
```

## Interactive

Browser (Enter opens folders, imports takes) · media pool (Space marks) ·
clip metadata · export settings · render queue with progress.

| key | action |
|-----|--------|
| Enter | open folder / import take |
| Tab | switch browser / media pool |
| Space | mark take |
| a / A | queue marked (or current) / all takes |
| v (R) | render queue · x cancel |
| c g t | cycle codec, gamut, transfer |
| e / E | exposure −/+ ⅓ EV |
| l y h | lens-shading correction, highlight clipping, half resolution |
| b ? q | toggle browser, help, quit |

## Headless

```
craw-tui info -f take.craw
craw-tui export -f take.craw -F prores4444 -g sgamut3cine -t slog3 -o out/
craw-tui profiles
```

**Codecs** (shown when the FFmpeg build has the encoder): ProRes 4444,
ProRes 422 HQ, DNxHR HQX, HEVC 10-bit (x265 / VideoToolbox), H.264
(x264 / VideoToolbox), AV1 10-bit (SVT-AV1), CinemaDNG (lossless, no FFmpeg).

**Gamuts:** rec709, rec2020, p3d65, acescg (AP1), sgamut3cine, arriwg3.
**Transfers:** rec709, srgb, linear, slog3, logc3 (EI 800), hlg, pq
(scene 1.0 = 203 cd/m², BT.2408).

## Development pipeline

Black/white levels (per-frame dynamic levels when recorded) → lens-shading
gain map (LSHD chunk or inline map) → bilinear demosaic or 2×2 superpixel →
white balance from AsShotNeutral with neutral highlight clipping →
ForwardMatrix (or inverted ColorMatrix) to XYZ D50 → Bradford to the target
white → target primaries → exposure → transfer → rgb48le into FFmpeg, with
colour tags (primaries / transfer / matrix) on the output. Audio is muxed
from the take (PCM 24-bit in .mov, AAC in .mp4).
