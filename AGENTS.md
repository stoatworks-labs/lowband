# AGENTS.md — Lowband

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short command
reference; this is the *why*. Read "What is actually verified" before you tell anybody
this works.

---

## What the plugin is

A Hi8 tape played back on a Video8 deck, as an FFGL 2.1 effect (`LB01`, shown as
`SW Lowband`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, a universal macOS
`.bundle` and (by CI, untried) a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/lowband`.

Built 2026-10-04 in one session at Allan's request ("a new resolume plugin that emulates a
hi8 tape being played back on a video8 player"), from `specs/SPEC-lowband.md`, with
`BRIEF.md` and `BRIEF-ADDENDUM.md`: colourunder for the plugin shape, the two rasters, the
harness, `--pipe`, verify, the sweep and CI; receipt for a CPU stage inside ProcessOpenGL;
nesolume's Amiga for worker threads; tinsel and graticule for the trap list.

---

## The one idea

**Hi8 moved the luma's FM carrier up and left the colour where it was.** Video8 records
luma as a frequency from 4.2 MHz (sync tip) to 5.4 MHz (peak white); Hi8 from 5.7 to
7.7 MHz, 2.0 MHz of deviation against 1.2, with a 0.47 µs emphasis against 1.3 µs. The
colour-under chroma (732.42 / 743.44 kHz) is common. A Video8 deck reads the Hi8 carrier
through its own channel, limiter, demodulator, map and de-emphasis; model that as an
actual FM signal and the picture falls out:

| the stage | what comes out |
| --- | --- |
| the deck's map: 1.2 MHz is sync-to-white, the tape swings 2.0 | **gain 5/3**: `--gain` 1.66675 against 1.66667 |
| Hi8's pre-emphasis, Video8's de-emphasis | a step lands half-way at once and **creeps with 1.3 µs**: the glow (`--emphasis`) |
| the same on the sync's trailing edge, read by the back-porch clamp | **the black lifts** 5 % PAL, 6 % NTSC (`--clamp`, against an analytic prediction) |
| the pre-emphasis overshoot at a bright edge clipped at 220 %, 10.1 MHz, where the Video8 band passes 0.053 | the limiter loses crossings: **a dip of a third of the step below black** (`--streak`) |
| white's carrier furthest outside the band, the noise in the band the same | **highlights break up first**: noise at white 2.6× black, a Video8 tape 1.02× (`--threshold`) |
| the colour-under band common to both | **the colour survives** (no check: it is the same code path for both tapes) |

### The pipeline (Engine.h has the detail)

    host --intake (GPU)--> P x N Y'UV at 13.5 MHz (701 x 576 PAL, 711 x 480 NTSC),
                           the area average of the host pixels; read back
    per line, eight lines in lock step, on up to eight workers:
      composite: sync (-S), blanking, the picture linearly between pixel centres;
                 a 4 us warm-up from the line before it in the SAME field
                 (two frame rows up), and the next line's sync after
      record:    Y low-pass (Butterworth 4) -> pre-emphasis shelf -> clips -> FM
                 (phase in turns, a folded Taylor cosine) at 40.5 MHz
      tape:      head clog (Wallace's loss as a 49-tap zero-phase FIR) ->
                 dropouts (-30 dB, raised-cosine edges) -> head-amplifier noise
                 (white, a 65,536-entry Gaussian table read by the PCG hash)
      deck:      RF high-pass (Butterworth 2) -> RF low-pass (Butterworth 8) ->
                 limiter + pulse count (each crossing's unit of area on a cubic
                 B-spline at a Newton-refined time) -> Y low-pass (Butterworth 8)
                 -> de-emphasis -> the deck's map from Hz to volts
      monitor:   3-sample box to 13.5 MHz at the deck's own delay -> back-porch
                 clamp per line -> sync AGC over the frame (optional)
      chroma:    U, V zero-phase low-pass, half amplitude at 0.5 MHz
    upload --display (GPU)--> host: nearest line, Catmull-Rom across, R'G'B', Mix

### How this differs from colourunder, ferric and old-cathode

- **colourunder** is VHS's colour-under chroma, head switch, tracking bar, dropout
  compensator, SP/LP/EP and generations. Its luma "FM" is a noise shape and a Gaussian
  band; its README says it has no pre-emphasis clipping and so no white streaking. Lowband
  has none of colourunder's mechanisms and colourunder none of lowband's: here the carrier
  is real, and the whole subject is a deck reading a carrier it was not built for.
- **ferric** is time-base error and the NR round trip; **old-cathode** composite to a CRT.
- Chroma here is a plain band-limit, the same for both tapes, on purpose: colourunder is
  the plugin for colour-under in depth.

---

## Decisions

- **The numbers and their sources** (ATTRIBUTIONS.md has the citations):
  - FM carriers and deviations: Sencore Tech Tip 189 (read with pdftotext), Wikipedia's Hi8
    and 8 mm articles, and vhs-decode's `video8.py` (which uses the same figures for PAL).
    Three sources agree. **Confirmed.**
  - Colour-under 46.875 fH / 47.25 fH: vhs-decode. One source; Sencore gives 743 kHz.
  - Emphasis tau 1.30 µs (Video8) / 0.47 µs (Hi8, "From spec") and the 11.5794 dB shelf:
    vhs-decode only. The shelf form `( 1 + s tau ) / ( 1 + s tau / x )` is this plugin's
    reading of a time constant and a gain; its corners (122/464 kHz, 339/1284 kHz) sit
    either side of vhs-decode's "mid" figures (274 and 760 kHz). **Unconfirmed.** No
    non-linear emphasis.
  - White clip 220 % of sync-to-white above the tip, dark clip 90 % of blanking-to-white
    below blanking: vhs-decode's comment on Hi8, with a question mark, applied to Video8 by
    assumption.
  - The deck's RF band: vhs-decode's filters for a capture of each format's RF (Video8
    1.9–7.0 MHz, Hi8 1.85–10.31 MHz) standing in for a deck's head, equaliser and RF
    filters. The orders (2 and 8) are chosen. **This is the number the look depends on
    most** — the streaks and the threshold both live at its upper edge — and it is the
    least sourced.
  - The Y low-pass after the demodulator: vhs-decode's 3.5 / 5.0 MHz, at order 8 rather
    than vhs-decode's 6 (to take the 2f carrier down below 0.1 % of white). The camcorder's
    record low-pass is assumed to be the same band, at order 4.
  - Writing speed: a 40 mm drum (Wikipedia) once a frame: 3.14 m/s PAL, 3.77 m/s NTSC.
- **A Hi8 deck plays a Video8 tape in Video8 mode** (Wikipedia: "All Hi8 equipment can
  record and play in the legacy Video8 format"); the format detection itself is not
  modelled. A Video8 deck has one mode. So of the four combinations only one is broken.
- **The noise is the head amplifier's**, white, added after the clog and before the
  deck's filters, and Tape Noise sets it as a carrier-to-noise ratio over the Video8
  deck's band (5.1 MHz) — a density, so the same setting is the same tape whichever deck
  or mode reads it. A Hi8 deck's wider band therefore lets more of it in.
- **The clog is Wallace's spacing loss** on the signal only (the amplifier's noise does
  not see it): the most physical knob for "a deck that reads high frequencies badly",
  and the one that pushes Hi8's carriers below threshold first. Up to 0.15 µm.
- **The clamp is per line** over the back porch from 1 µs after the sync to 0.5 µs before
  the picture, at the deck's compensated delay. **The AGC is over the frame**, from the
  mean sync depth (per-line AGC would band the picture with the noise).
- **Lines are independent** and start from steady state after a 4 µs warm-up from the line
  before them in the same field. That makes the worker count irrelevant to the result,
  and it is close to the truth: the filters' memory is a few microseconds.
- **A CPU engine.** The FM needs a running phase and the deck is a chain of IIR filters,
  neither of which a fragment shader does well; on the CPU it is exact and testable. The
  GPU does the two resamplings.
- **Defaults**, chosen on Resolume's demo clips (nine of them, contact sheets in the
  build session): Hi8 on Video8, PAL, Tape Noise 0.6 (24.4 dB), Head Clog 0.4 (0.06 µm),
  Dropouts 0.15, Sync AGC off. At 0.5 / 0.2 the look was glow and streaks only; at 0.6 /
  0.4 the highlights break into sparkle while the darks hold, which is the plugin's
  signature; at 0.7 / 0.55 it is snow.

---

## The traps (in the order they were hit)

**A lane change that missed one stride.** The batch went from four lanes to eight for
speed; the composite generator still wrote with a hard-coded stride of 4, so the picture
never reached the tape and the output was the blanking level with noise. Nothing crashed.
An edge probe (a black-to-white line through the engine) found it in one look. Every
`[ sample ][ lane ]` access now takes `L4`.

**A sampled pulse train aliases into the picture.** The first demodulator split each
crossing's unit of area between the two samples either side (a linear kernel, sinc² in
frequency). The 4th harmonic of white's 10.8 MHz crossing rate is 43.2 MHz, which aliases
to 2.7 MHz at 40.5 MHz: a matched Video8 deck showed a ±2 % ripple on white. A cubic
B-spline (sinc⁴) took it to ±0.7 %.

**A linear zero-crossing estimate has a pattern.** The remaining ±0.7 % was the linear
estimate of each crossing's time — up to 1 % of a sample out on a carrier 7.5 samples a
cycle, in a pattern that repeats every four crossings (2.7 MHz again). Two Newton steps on
the cubic through four samples took it to ±0.05 %.

**And the cubic leaves a slower one near the band edge.** On Hi8's 6.65 MHz carrier the
crossings slide past the sample grid every 1.65 µs, and the cubic's few-1e-4-sample error
shows as a ±0.1 % wobble at 0.6 MHz. Invisible in a picture, but it biased `--emphasis`'s
fit of the late tail (1.23 µs). The check now staggers the edge over eight phases of the
grid, down the frame, and averages: the wobble cancels, the tail (a sum of exponentials of
one tau) does not.

**The FM channel stretches the tail near the band edge.** Fitting the emphasis tail from
1.5 µs read 1.34 µs; from 3 µs, 1.305. The carrier climbs through the tail, and near the
Video8 band's edge the band's group delay climbs with it, drawing the early tail out. Real
dispersion, not a bug. The fit starts at 2.5 µs and its tolerance includes the group-delay
swing over the window.

**A hand-typed constant.** The shelf gain was typed as 3.7863 for 10^(11.5794/20); it is
3.7930. `--model` caught it on its first run, because it types the dB figure itself and
computes. The plugin now computes it too.

**The IIR state has to live in registers.** Run a sample at a time through a cascade whose
state is in memory and every recurrence waits on a store: 44 ms a frame on one worker. The
`RunLanes` template keeps a cascade's state in local arrays the compiler holds in
registers; the FM and crossing loops became flat passes; `LB_RESTRICT` (`__restrict`) on
the hot pointers was what let clang vectorise the FM loop at all (it was scalar,
`fcsel`s, with the two arrays possibly aliasing). 44 → 12 ms on one worker, 6 → 1.8 ms on
eight; then the B-spline put some of it back (2.6 ms at the defaults).

**The clamp removes any carrier offset**, so `--matched`'s first negative control (Hi8
written from Video8's sync tip) did not fail it: a constant frequency offset is a DC
offset, and the back porch takes it out. Correct physics, wrong control. The control is now
a demodulator that counts only rising crossings, which halves the gain.

**A step's 50 % point is not its DC group delay.** The deck compensates its own delay with
the chain's DC group delay (what a delay line would be set to). A small step's 50 % point
lands 0.24–0.37 px later because the Butterworths' group delay rises toward their corners.
`--registration` predicts the offset by running the same step through the baseband chain
in double and taking out the DC delays, and agrees to 0.07 px.

**A 320-wide host cannot see a tenth of a pixel.** `--registration` first measured in the
host picture and failed at 320×180, where a host pixel is 2.2 raster pixels and the display
point-samples. Registration is the deck's property, so it is measured on the deck's raster;
`--display` checks the display pass separately.

**GLSL's division is 2.5 ulp.** `--display` recomputes the display's Catmull-Rom and matrix
in double and first allowed float rounding of a correctly rounded division; at 960×540 it
failed by 1.4×. GLSL 4.10 s4.7.1 allows a division 2.5 ulp; at a position near 700 that is
1.5e-4 of a pixel, times the spline's slope. The tolerance is now that, per pixel, and every
raster sits under a third of it.

**Reverting a mutation does not always rebuild.** Apple's make compares mtimes to the
second: a source restored and touched in the same second as the mutated object's build was
not recompiled, and the "reverted" run still failed. Delete the object file.

**`FILAMENT_LOG_DIR`.** Diag.cpp came by way of colourunder, which (like gate) still names
the log-directory override after filament. Renamed `LOWBAND_LOG_DIR` here; gate's and
colourunder's are left as they are.

---

## What is actually verified

On this Mac (`tools/verify.sh`, fresh universal build, all green 2026-10-04):

- `--carrier`: counted crossings, every format and standard: Video8 PAL 4.1997 / 4.5600 /
  5.3999 MHz (tip, blanking, white), Hi8 PAL 5.6988 / 6.3001 / 7.7001.
- `--matched`: flat fields 0.05–0.25 back to 3.1e-4 on both decks, both standards.
- `--gain`: 1.66675 PAL, 1.66679 NTSC (5/3 = 1.66667); matched 0.99980–1.00003.
- `--emphasis`: 1.300–1.322 µs at the three rasters (1.30); matched decks no tail.
- `--clamp`: 0.0497 / 0.0581 against 0.0516 / 0.0603 predicted; Video8 tape −0.0003.
- `--threshold`: 2.62 against 2.17 (first order) for Hi8 on Video8, 1.02 against 1.00 for
  Video8. The measured ratio is a fifth above first-order theory: near the band edge the
  noise is not only a carrier-to-noise question (the asymmetric sidebands distort it too).
- `--streak`: Hi8 on Video8 dips 0.558 below black after a black-to-white edge; Hi8 on Hi8
  0.001.
- `--wallace`: the taps' response equals e^(−af) less the computed truncation to 1e-8;
  within 0.16 dB of the law from 4.2 to 10.1 MHz up to 0.15 µm.
- `--registration`, `--intake`, `--display`, `--dropout`, `--threads`, `--resize`,
  `--alpha` as in the README.
- `--names --model`: every name ≤ 16 characters and unique as Arena reduces it; every
  format and standard number against the one typed in the harness.
- The sweep: all 8 controls change the picture (also at CI's 160×90).
- The bundle: universal, `plugMain` exported, plist, ad-hoc signature; oxbow sees
  `SW Lowband` / `LB01` / effect and renders 120 frames.

**Assumed, not verified:** everything in "Decisions" marked unconfirmed; that the look
matches a real Video8 deck (no capture was found); the colour path beyond being common
to both tapes; anything about Resolume.

### Negative controls and the mutation test

Each physics check has a `model::Perturb` bit that must make it fail (`--negative` runs
them all; every one fails as it should, at every raster and on the software renderer):
Hi8 from Video8's tip → `--carrier`; rising crossings only → `--matched`; the tape's map
in the Video8 deck → `--gain`; the tape's emphasis in the Video8 deck → `--emphasis` and
`--clamp`; a Video8 band as wide as Hi8's → `--threshold` and `--streak`; the other
standard's writing speed → `--wallace`; no delay compensation → `--registration`; a
dropout two lines down → `--dropout`; noise seeded by worker → `--threads`; a resize that
restarts the clock → `--resize`.

Mutations, each made by hand, seen to fail, reverted: in the shipped GLSL, the intake's Y'
weight 0.587 → 0.578 fails `--intake` (8.1e-3 off against 2e-6); in the engine, one
B-spline weight's 4 → 5 fails all 18 of `--matched` and `--gain`. So the harness drives
the shaders and the engine that ship, not copies.

### Would this hold on another rasteriser, at another raster?

Every check passes at 320×180, 960×540 and 1280×720, and on Apple's software renderer at
320×180. Line by line:

- `--carrier`, `--wallace`, `--names`, `--model`: no GL; the engine and the model only.
  Tolerances: the pre-emphasis's settling (its step response, analytic) and the counting
  method's own worst linear-interpolation error over all phases; the taps' float rounding.
- `--matched`, `--gain`, `--emphasis`, `--clamp`, `--threshold`, `--streak`, `--dropout`,
  `--threads`: measured on the deck's picture (the plugin's upload), which is CPU arithmetic
  and the same on any GPU; the host raster only changes the intake, and these use flat or
  two-level pictures whose intake is exact. Tolerances: the 2f carrier residual through the
  8th-order low-pass (computed from the analogue prototype), the slowest tail's remainder,
  the band's group-delay swing, a second-order shape term (2 %) for `--clamp`, ±25 % of
  first-order FM theory for `--threshold`.
- `--registration`: on the deck's raster; tolerance a fifth of a 40.5 MHz sample plus a
  twentieth of a pixel for interpolating the 50 % point.
- `--intake`: the GPU's area average against an exact one in double: 2e-6 (a few ulps of a
  sum of floats under 1). Raster-independent by construction (integer overlaps).
- `--display`: GLSL 4.10's 2.5 ulp division, times the spline's slope at each pixel, plus
  2e-6. The software renderer passes under the same bound.
- `--resize`: four ulps of the value for the GPU's display pass (gate's rule); the deck is
  the CPU's and identical.
- `--alpha`: exact.

---

## Performance

`lbtest --bench` at the defaults: 3.9 / 4.2 / 5.3 ms a frame at 720p / 1080p / 4K (23–32 %
of a 60 fps frame) on this Mac while other builds ran. `lbtest --profile`: the engine is
14 ms on one worker, 2.6 ms on eight; with no clog and no noise 1.9 ms. The rest is the GPU
intake, the synchronous read-back (6.5 MB of RGBA32F) and the upload. Ways down, not taken:
a pixel-buffer read-back a frame late (a frame of latency — slowscan does it, an ordinary
effect should not), half floats for the transfers, a shorter clog FIR at small spacings.

**Windows is unmeasured.** The lane loops rely on auto-vectorisation; clang does it here,
MSVC may not, and then the engine could be several times slower. An SSE path for
`RunLanes` would settle it.

---

## Open questions

- **What a real Video8 deck shows.** The whole look is the model's prediction. A capture of
  a Hi8 tape on a Video8 camcorder would settle the band edge, the order of the filters and
  how much snow is "right".
- **The deck's band** (1.9–7.0 MHz, Butterworth 2 and 8) is a decoder's choice standing in
  for a head, an equaliser and RF filters. A Video8 service manual's RF luminance block
  would replace it.
- **The black depends a little on the picture.** The porch also holds the tail of the
  previous line's picture, so a bright line lifts the next line's porch: flat 0.3 against
  flat black moves the measured sync depth from 0.409 to 0.399 V. Real (a clamp's
  content-dependent error), small, and it makes `--gain`'s two-level line the honest way to
  measure the gain.
- **Sync AGC under-corrects** a Hi8 tape (to about 1.23×) because the sync is read through
  the same mismatched emphasis and measures short. That is what an AGC on sync amplitude
  would do; whether a real monitor's AGC behaves like this one (frame-averaged) is not known.

---

## Browser demo

`demo/` is the page at **lowband-demo.stoatworks-labs.com** (2026-10-04), on the fleet's kit
(`stoatworks-backend/resolume-demo`, vendored by its `sync.sh lowband`; never edit
`demo/vendor/`).

**What is the plugin's.** The version line, `kCommon` and the vertex, intake and display
bodies of `Shaders.cpp` are spliced into `demo/plugin.js` by `demo/tools/sync_shaders.py`,
tabs and comments included; `assemble` and the list of stages that take `kCommon` are
Shaders.cpp's. `demo/tools/check_shaders.py --dump DIR` compares every assembled stage with
`lbtest --dump-shaders DIR` byte for byte; `tools/verify.sh` runs it. The kit's `port()`
changes only the version line and the precision qualifiers. The buffers are the plugin's:
the intake P × N RGBA32F read back in floats, the deck's picture uploaded as the same; the
page refuses to start without `EXT_color_buffer_float`.

**What is a hand port.** Model.cpp, Controls.cpp and Clock.cpp (with the unit declared) in
`demo/model.js`, with the constructor's parameters (`DECLARATIONS`), the head of
`ProcessOpenGL` (`Instance.beginFrame`: the settings, the raster, the clock, the video frame)
and its two passes (`passes()`); Dsp.cpp in `demo/dsp.js`; Engine.cpp in `demo/engine.js`.
The plugin runs eight lines in lock step, one a SIMD lane; no lane reads another and a line's
seed is its row, so the port runs one line at a time with the same arithmetic. Every C++
`float` operation is one `Math.fround`, in the C++'s order (by Figueroa, double rounding is
innocuous for + − × ÷ when the wider type has 2p + 2 bits, so a float operation computed in
double and rounded IS the float operation); doubles stay doubles. The page splits each
frame's lines across Web Workers (`demo/engine-worker.js`, as many as the plugin's
`defaultThreads()`), does the clamp and the AGC on the main thread in line order, and shows
the last finished frame with the input it was made from; paused, a new frame starts only
when the clock, a parameter, the clip or the size changed. About 0.25 s of one core a
frame in node, 24–35 ms on eight workers on an M4 Max: some 30 frames of tape a second.

**`demo/tools/check_port.sh` checks the port against the C++, not a reader.** It cuts the
`ParamID` enum out of Lowband.h and the anonymous namespace, the whole constructor and the
whole of `ProcessOpenGL` out of Lowband.cpp at run time, pastes them unedited into
`demo/tools/refport.cpp`'s scaffolding (GL entry points and `ffglex` classes that RECORD
every uniform, texture and framebuffer, and a `glReadPixels` that hands the plugin an intake
the script wrote), compiles that with the plugin's own Engine, Model, Dsp, Controls and Clock,
and compares: the declarations and groups; every filter's float coefficients and steady
states, the rasters, the clog taps, sigma, the dropouts and line seeds; all 65,536 Gaussian
table entries; and over 10 scenarios and 25 frames (the defaults at 1280×720, matched decks,
NTSC with a full clog, Sync AGC and Mix, a Video8 tape on a Hi8 deck at 30 dropouts a
frame, out-of-range intakes, fractional options, the clock's backward jump, gap and stall,
a 7×3 host) the clock, the video frame, the settings, the sync depth, gain and porch, both
passes and **every one of 38,877,696 uploaded floats: bit-identical** at `-ffp-contract=off`,
and identical to the x86_64 slice (`-O3 -arch x86_64` under Rosetta). The same frame split
1, 3, 5 or 7 ways is the same picture. ~14 s. Mutations of the port caught (scratch copies):
a B-spline weight 4 → 5, one Newton step instead of two, the deck de-emphasising with the
tape's tau, 23 clog half-taps, the Gaussian table at i + 0.49, the noise seed's salt, Tape
Noise's 46 dB top to 45, Dropouts in the Deck group, Head Clog's default 0.41, the porch
window from 1.1 µs, one lost `fround` in the FM phase, the chroma's backward pass dropped,
the display's `MixAmount` renamed, the clock's jump 1/50 s. Not caught, and said: the AGC's
floor (5 % of the sync), which no reachable setting makes bind.

**What is not bit-identical, and why.**
- **The arm64 slice fuses multiply-adds** (clang's default `-ffp-contract=on` at `-O3`);
  JavaScript cannot. Against it 22.9 M of the 38.9 M floats differ in their last bits and
  450 by more than 1/255 (at most 2.6), every one of them in a scenario with tape noise and
  picture detail: the demodulator at its threshold, where one rounding can add or lose a
  zero crossing (a reading, not traced sample by sample). With no noise the largest
  difference is 6e-6. The plugin's own two slices differ from each other the same way.
- **libm is not JavaScript's Math.** `tan`, `sin`, `cos`, `exp`, `log`, `pow` differ in the
  last bit here and there (neither is correctly rounded: Apple's `cos(0.77)` and V8's
  `cos(0.1)` are each one ulp off), so the filter designs' doubles differ in their last bits;
  every float coefficient is identical. Group delays (only the deck's delay compensation
  uses them) differ by up to 5 ulp at DC and the blanking carriers, the compensated delay by
  2 ulp, the dropouts' ends by 1 ulp: compared in ulps and reported, never reaching a float
  or a sample in the scenarios. `erfc`, which JavaScript lacks, is the page's own (a series
  below 1, Lentz's continued fraction above); the table it feeds is identical.
- **`__divdc3` fuses too.** `std::complex` division goes to compiler-rt's `__divdc3`, whose
  arm64 build has three `fmadd`s whatever the plugin's own flags (found in its disassembly
  when the group delays disagreed by an ulp); `dsp.js` ports them with an exact BigInt FMA,
  checked against C's `fma` on 200,000 random triples.

**Measured once (2026-10-04)**, driven headlessly (Chrome 154, ANGLE on Metal, Apple M4 Max)
frame by frame at n / 60 from a fresh instance (`window.__lowbandDemo.hooks`: `fresh()`, and
`afterRender` to read the canvas and the held input inside the frame) against
`lbtest --pipe --fps 60` on the same input frames read back from the page, with the same
values `--set`: the defaults on the Synthetic scene at 320×180 over 30 frames, 3,075 of
6,912,000 channel values differ, each by 1/255; no noise, clog or dropouts, 10 frames, 789
of 2,304,000 by 1; NTSC, Sync AGC on, Mix 0.5 on Lights on black at 1280×720, 6 frames,
30,539 of 22,118,400 by 1; a Hi8 deck on colour bars at 640×360, 6 frames, 1,496 of
5,529,600 by 1. Never more than 1, alpha never. It can fail: the page at the defaults
against the pipe at Head Clog 0.41 (not 0.40) makes 335,226 values differ, 144 by more than
1, up to 38. Paused, the same frame twice is identical; from the defaults, Recording →
Video8 moves the picture a mean 31.8 levels, Deck → Hi8 30.9, Head Clog 1 25.4, Mix 0.5
15.7, Sync AGC 15.5, NTSC 13.3, Tape Noise 0 9.9, Dropouts 1 0.5. No console errors.

**What differs, each said on the page:** the frame rate (the last finished frame, a little
behind a playing clip); the clock is the kit's in declared seconds (no unit vote; paused
re-renders the same video frame; Restart is a backward clock, which steps on 1/60 s; the
kit caps a frame at 0.1 s, so the 0.5 s jump never happens), updated on every frame the page
draws; libm against Math; the input is a generated clip at 640×360–1920×1080 or the
visitor's own file; no sound (the plugin has none); the test hooks are off and the About
block is absent.

**Traps.** The kit redraws on every change, and a finished frame needs a redraw to be shown,
so a paused page must not take that redraw as a request: a frame key (time, size, clip,
parameters), not a flag: with a flag, a Step clicked between a frame finishing and its
redraw would never be computed. refport's `glGetIntegerv` writes one value except for `GL_VIEWPORT`:
`ClientTransfer` queries single ints, and colourunder's stub (four values each) would
overwrite the neighbours. Headless Chrome needs ANGLE on Metal (`--use-gl=angle
--use-angle=metal --enable-gpu --ignore-gpu-blocklist`); cdpshot.py's default has no WebGL2.

Deploy: push to main (`.github/workflows/deploy.yml`) or `cf-run npx wrangler deploy` from
the repo root. The host is a Worker **route** over a proxied `AAAA 100::` record made through
the API on 2026-10-04, not a custom domain: the zone is at Cloudflare's limit of 100. Delete
that record and the page goes dark while deploys stay green. Verify by content:
`curl -s 'https://lowband-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

---

## Not done

Released as v0.1.0 on 4 October 2026: registered in the fleet's three tables, so
`StoatworksAbout*.h`, `ATTRIBUTIONS.md` and the issue forms are GENERATED (`sync-about`,
`sync-attributions`, `sync-issue-templates` in stoatworks-backend) — edit the tables, not
the files.

- **Never loaded into Resolume on macOS.** On Windows, the Arena gate (plugin-bench, Arena
  7.27.1, Mesa llvmpipe, no GPU) passed a CI build: 14 controls match, 7 of 9 move the
  picture. Dropouts and Standard read inconclusive there, because the noise is fresh every
  frame and the gate compares single frames: the gate's limit, not a dead control
  (`tools/sweep.py` shows both live).
- Windows' CPU cost is unmeasured (MSVC may not vectorise the lane loops).
- No OpenFX port or presets.
