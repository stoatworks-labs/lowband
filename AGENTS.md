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

## Not done

- **Never loaded into Resolume**, on either platform. Never built on Windows.
- No GitHub repo, tag, release, website page, user guide, video, browser demo, OpenFX port
  or presets. `StoatworksAbout.h` (no `guide`) and `ATTRIBUTIONS.md` are provisional hand
  copies for the fleet's generators to replace.
- Landing it: move to `~/Projects/resolume/lowband`, create the public repo, register it in
  the three tables (projects.json, sync-about TARGETS, names.json), then the release brief.
