# lowband

A Hi8 tape played back on a Video8 deck — the luma as a real FM signal, written by Hi8 at
5.7–7.7 MHz and read by a Video8 deck's channel and demodulator built for 4.2–5.4 MHz (5/3
too much contrast, a glow trailing every highlight from the mismatched emphasis, a lifted
black, black streaks after bright edges, highlights that break up first under tape noise;
the colour-under chroma is common to both, so the colour survives) — as an FFGL **effect**
(`SW Lowband`, `LB01`) for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the chain, a format's number, a filter or a check's
tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/lbtest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one;
  `--average` writes the mean of every frame instead)
- Set anything by name: `--set "Recording=0" --set "Tape Noise=0.8" --set "Head Clog=0.6"`
  (0..1 for sliders, the element index for options — Video8 0, Hi8 1; PAL 0, NTSC 1 —
  0/1 for Sync AGC)
- List parameters, kinds, defaults and ranges: `./build/lbtest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/lbtest --dump-shaders DIR`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between
  cues; an option and a boolean STEP (they hold the last cue at or before the frame); an
  event fires on its cue frame only. A cue naming no parameter is refused with exit 2, a
  partial frame at the end of stdin ends the stream cleanly, a failed render or a closed
  stdout exits 1 (SIGPIPE is ignored, so never 141):
  `ffmpeg -i clip.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/lbtest --pipe --size 1280x720 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mov`
- The engine's cost alone at 1, 2, 4 and 8 workers: `./build/lbtest --profile`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the reserved-word grep
  + every check at 320x180, 960x540 AND 1280x720 AND on the software renderer + --pipe +
  the sweep + the bundle + oxbow; ~10 min on this Mac)
- The recorded carriers, by counted crossings: `./build/lbtest --carrier`
- A matched deck gives a flat field back: `./build/lbtest --matched`
- Hi8 on Video8 has the gain 2.0 / 1.2: `./build/lbtest --gain`
- The mismatched emphasis's tail is the deck's 1.3 us: `./build/lbtest --emphasis`
- The lifted black, against its analytic prediction: `./build/lbtest --clamp`
- White noisier than black by the band's |H| ratio: `./build/lbtest --threshold`
- The streak after a bright edge: `./build/lbtest --streak`
- The clog is Wallace's 54.6 d / lambda dB: `./build/lbtest --wallace`
- A matched deck puts an edge back: `./build/lbtest --registration`
- The intake's area average and the display's read-back: `./build/lbtest --intake --display`
- A dropout disturbs its own line: `./build/lbtest --dropout`
- Any worker count gives the same picture: `./build/lbtest --threads`
- The state survives a resize: `./build/lbtest --resize`; alpha: `./build/lbtest --alpha`
- The checks can fail: `./build/lbtest --negative`; one perturbation verbosely:
  `./build/lbtest --perturb BITS --gain` (bits in `Model.h`)
- The plugin's numbers against the stated ones, no GL: `./build/lbtest --names --model`
- Every check takes `--size WxH`; CI runs them at 320x180
- CI's renderer, on this Mac: `LBTEST_RENDERER=software ./build/lbtest --display --size 320x180`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the state held: `./build/lbtest --bench`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Lowband.bundle`

## Notes
- **The deck, not the look.** `Model.{h,cpp}` holds the numbers: the two formats (FM
  carriers, deviation, emphasis, clips, the deck's bands), the two standards, the noise,
  the clog's taps, the dropouts. `Engine.{h,cpp}` is the chain. `Shaders.cpp` only brings
  the host frame onto the raster and takes the deck's picture back. A wrong format number
  is a fix in `Model.cpp`.
- **Two rates.** The picture at 13.5 MHz (701 pixels of PAL's active line, 711 of NTSC's);
  the FM chain at 40.5 MHz, exactly 2592 or 2574 samples a line. Pixel i's centre is at
  sample activeStart + 3 i + 1.
- **The CPU does the signal.** Read back, eight lines in lock step (`dsp::Cascade::kLanes`)
  per batch, batches shared between up to eight workers, uploaded. A line's arithmetic
  never depends on the worker count (`--threads`).
- **A cascade's state stays in registers** (`RunLanes`, templated on the section count);
  the hot loops are flat or lane-wise with `LB_RESTRICT`. Through memory the IIR chain was
  three times slower; without restrict clang did not vectorise the FM loop.
- **The demodulator** spreads each crossing's unit of area over four samples with a cubic
  B-spline, at a time refined by Newton steps on a cubic. The two-sample split and the
  linear estimate each left a ripple on white (2 %, then 0.7 %).
- **The deck's delay is compensated** with the DC group delay of the chain for its own
  format, as a delay line would be; a small step's 50 % point lands 0.24–0.37 px later,
  which `--registration` predicts from the filters' shapes.
- **Time is seconds since the first frame, in double** (clamp's Clock); only the video
  frame (25 or 29.97 Hz) that seeds the noise and the dropouts reads it.
- **Output alpha is 1 at Mix 1** (`mix( src.a, 1, Mix )`): a tape has no alpha.
- **`Perturb` bits and the `...ForTest` hooks are test hooks**, inert in the plugin.
- **Parameter names must be unique as Arena reduces them** (lower case, no spaces).
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen it.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `lowband_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `LB01`. Display name `SW Lowband`.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies (no `guide`);
  the first sync after registration replaces them.
- **Reverting a mutation:** Apple's make compares mtimes to the second. Delete the object
  (`build/CMakeFiles/lowband_core.dir/source/X.cpp.o`) or the reverted source is not rebuilt.

## Not done yet
- Never loaded into Resolume, never built on Windows; Windows' CPU cost unknown.
- No GitHub repo, release, website page, user guide, browser demo, OpenFX port or presets.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/lowband/lowband.YYYY-MM-DD.log
