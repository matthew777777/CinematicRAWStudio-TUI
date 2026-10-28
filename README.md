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

Imports CRAW-Fast, CRAW-Live, Live v2 and Desktop Compact (codec 4) automatically; the codec stored in
each frame selects the decoder. Rebuild after updating the sibling decoder
to pick up its WAV export fixes. CinemaDNG audio exports use integer PCM
(16-bit source stays 16-bit; float becomes 24-bit) with Broadcast WAV
timecode. In Resolve, place video and WAV at the same timeline start;
the automatic audio synchronization dialog has crashed with these takes.

## Easy install: Mac, Linux and Windows

Install the complete toolset once with the
[shared installer](https://github.com/matthew777777/CinematicRAWStudio-Spec/blob/main/GETTING_STARTED.md).
It fetches the sibling repos and dependencies automatically.

Mac/Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/matthew777777/CinematicRAWStudio-Spec/main/scripts/install.sh | bash
```

Windows PowerShell:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/matthew777777/CinematicRAWStudio-Spec/main/scripts/install.ps1)))
```

Open a new terminal, then use `craw` for the editor,
`craw mount "take.craw"` for Resolve, `craw unmount` to close mounts,
or `craw export "take.craw"` to save CinemaDNG and sound.


## Interactive

Browser (Enter opens folders, imports takes) · media pool (Space marks) ·
truecolour preview of the selected take (half-block cells, developed in the
background with the current look) · clip metadata · export settings or grade
sliders · render queue with progress.

| key | action |
|-----|--------|
| Enter | open folder / import take |
| Tab | switch browser / media pool |
| Space | mark take |
| a / A | queue marked (or current) / all takes |
| v (R) | render queue · x cancel |
| c f g t | cycle codec, frame rate, gamut, transfer |
| G | grade mode: exposure, temperature, tint, contrast, saturation, highlights, shadows, blacks (live preview; 0 resets) |
| , . / < > | preview frame ±1 / ±10 % |
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

Export is pipelined: several frames decode and develop concurrently while
frames stream to FFmpeg in order. Transfer curves use a table indexed by the
float bit pattern (no log/pow per sample; PSNR ~78 dB vs exact). On an
8-core Mac, 40 frames of 4080x2040 to ProRes 422 HQ take ~8 s, of which
FFmpeg's encoder alone needs ~4 s.

## Lossless desktop compaction

Run `craw compact in.craw out.craw` with the updated Spec desktop wrapper and
Decoder/Encoder tools. Open the output in the TUI normally; browser, preview,
CinemaDNG export and rendered exports use the shared decoder. Takes may contain
both Compact and original-codec frames when keeping an original frame is smaller.
Compaction preserves RAW samples, frame records and ancillary chunks. It is a
desktop operation; Compact is substantially slower to encode and decode than
Live v2. The phone recording codec is unchanged.
