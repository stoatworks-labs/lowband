# Attributions

Lowband is built on other people's work. This file lists what that work is, who did it,
and what it is doing here.

This is a provisional hand copy in the shape `stoatworks-backend/scripts/sync-attributions.py`
generates; the first sync after the project is registered replaces it.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Plugin shape, harness, --pipe contract and verify — Stoatworks colourunder

<https://github.com/stoatworks-labs/colourunder>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the About block, the Diag logger), the harness's GL
plumbing, PNG writer, parameters by name, `--pipe` with its cue sheet and SIGPIPE ignored,
the software-renderer switch, `verify.sh`, the sweep and the negative-control pattern are
colourunder's, which had them from gate, toner, filament and the rest of the fleet before it.

### The clock and the two raster timings — Stoatworks clamp and standards, by way of colourunder

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

Clock.{h,cpp} is clamp's (standards' before it). The 625/50 and 525/59.94 line, porch,
sync and active-line figures, and their sources, are clamp's table as colourunder carries it.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, by way of gate and colourunder.

### Client-memory pixel transfers — Stoatworks receipt

<https://github.com/stoatworks-labs/receipt>  
Licence: MIT  
Copyright: Stoatworks Labs

The guard that unbinds a host's pixel buffer objects and row lengths around the read-back
and the upload is receipt's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there is no other
way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample. Part of the
upstream SDK tree rather than something this plugin calls.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The FM carriers — Sencore Tech Tip 189, "Comparison of VCR Formats"; Wikipedia, "Hi8" and "8 mm video format"

<https://docs.ampnuts.ru/eevblog.docs/Sencore/Sencore_Tech_Tips/TT189p%20-%20Comparison%20Of%20VCR%20Formats.pdf>

8 mm: sync tip 4.2 MHz, peak white 5.4 MHz, 1.2 MHz deviation; Hi-8: 5.7 MHz, 7.7 MHz,
2.0 MHz; chroma under at 743 kHz for both. Confirmed by three sources (the tech tip's table
read with pdftotext; both Wikipedia articles). The tech tip is an NTSC document.

### The PAL figures, the colour-under carriers, the emphasis and the decks' bands — vhs-decode

<https://github.com/oyvindln/vhs-decode> (`vhsdecode/format_defs/video8.py`; GPL-3.0: figures cited, no code used)

The same FM carriers for PAL and NTSC; colour-under at 46.875 fH (PAL, 732.42 kHz) and
47.25 fH (NTSC, 743.44 kHz); the main emphasis time constant, Video8 1.30 µs ("1.25~1.35µs",
VHS's) and Hi8 0.47 µs ("From spec"), with a shelf of 11.5794 dB for both; Hi8's white clip
at 220 % and dark clip at 90 % (from comments marked with a question mark); and the filters
a decoder puts on a capture of each format's RF (Video8 1.9 to 7.0 MHz, Hi8 1.85 to
10.31 MHz) and on the demodulated Y (3.5 and 5.0 MHz). Several are marked tuned or uncertain
in the file itself. The decoder's RF filters stand in for a deck's head, equaliser and RF
filters, which no source here describes. One source.

### The writing speed — Wikipedia, "8 mm video format"

A 40 mm head drum turning once a frame: 3.14 m/s (PAL), 3.77 m/s (NTSC). The article gives
"3.75 meters per second", which is the NTSC figure.

### The spacing loss — R. L. Wallace, Jr., "The Reproduction of Magnetically Recorded Signals", Bell System Technical Journal 30(4), 1145–1173, October 1951

A head reading through a spacing d loses e^(−2πd/λ), 54.6 d/λ dB. The citation was checked
against the BSTJ table of contents; the law itself is the textbook one.

### What a Video8 deck shows on a Hi8 tape — US patent 4,949,195; forum summaries

Sony's mode-discrimination patent says only that a high-band recording on a standard
machine "tends to be disturbed"; forum summaries say snow and streaking, as with S-VHS on
VHS. No first-hand capture was found. The picture this plugin makes is the model's
prediction, not a match to footage.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Video8 (1985) and Hi8 (1989), Sony

The 8 mm formats: the FM luma, the colour-under chroma, and the Hi8 tape that every Video8
camcorder owner tried in the wrong machine once. Built from what the formats are rather
than from anyone's implementation.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.470-6, BT.1700; SMPTE 170M** — the 625/50 and 525/59.94 rasters.
- **ITU-R BT.601** — the 13.5 MHz sampling, the Y' weights, and the U and V scalings used for both standards.
- **IEC 60843 (8 mm video)** — through the sources above; not read directly.
- **The OpenGL Shading Language 4.10, s4.7.1** — the 2.5 ulp a division may cost, which `lbtest --display` allows for.
- **PCG (M. E. O'Neill, Harvey Mudd College, 2014)** — the pcg_hash output mix used for the noise and the dropouts, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
