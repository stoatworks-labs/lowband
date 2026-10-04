// The page's port of the CPU engine (demo/model.js, dsp.js, engine.js)
// against the plugin's own C++.
//
//   node demo/tools/check_port.mjs [--keep DIR] [--quick]
//
// Run by demo/tools/check_port.sh, which tools/verify.sh calls. --quick runs
// three of the ten scenarios; --keep leaves the reference builds, intakes and
// uploads in DIR; LB_ALL=1 prints every difference, not the first sixteen.
//
// What it does. It pastes four pieces of the plugin into refport.cpp's
// markers, unedited -- the ParamID enum from Lowband.h; from Lowband.cpp the
// anonymous namespace (loc, bindTextures, ClientTransfer, defaultThreads),
// the whole of Lowband::Lowband() and the whole of Lowband::ProcessOpenGL --
// and compiles them with the plugin's own Engine.cpp, Model.cpp, Dsp.cpp,
// Controls.cpp and Clock.cpp. refport.cpp's GL and ffglex stand-ins record
// instead of drawing, and its glReadPixels hands ProcessOpenGL an intake this
// script wrote (as if the GPU's intake pass had produced it). Recorded: every
// parameter the constructor declares; the designs (every filter's float
// coefficients, steady states and group delays, the clog taps, the noise
// sigma, the dropouts, the line seeds, all 65,536 Gaussian table entries);
// and per frame the clock, the video frame, the settings, the sync depth,
// gain and porch, the deck's delay, every buffer allocated, the read-back,
// every pass with its textures and uniforms, and THE DECK'S PICTURE AS
// UPLOADED, every float. The port then runs the same scenarios, the frame's
// lines split across "workers" the way the page splits them, and the two
// are compared: every int exact, every float and double by its bits.
//
// What it cannot. It says nothing about the shaders (check_shaders.py does),
// nor about plugin.js's GL half beyond the plan's names and uniforms: the
// WebGL objects, the read-back's layout, the canvas. The page against
// `lbtest --pipe` on the same input is the end-to-end check of those
// (AGENTS.md, "Browser demo"). The video frames stay far below 2^32, so the
// high halves of the 64-bit seeds are exercised only at zero.

import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..');
const model = await import(join(REPO, 'demo', 'model.js'));
const dsp = await import(join(REPO, 'demo', 'dsp.js'));
const engineJs = await import(join(REPO, 'demo', 'engine.js'));

const quick = process.argv.includes('--quick');
const read = (p) => readFileSync(join(REPO, p), 'utf8');

function cut(text, start, end, label) {
  const a = text.indexOf(start);
  if (a < 0) throw new Error(`${label}: start marker not found`);
  const b = text.indexOf(end, a + start.length);
  if (b < 0) throw new Error(`${label}: end marker not found`);
  return text.slice(a, b);
}

// --- the reference: the plugin's text, recorded ---------------------------
const header = read('source/Lowband.h');
const source = read('source/Lowband.cpp');
const enumText = cut(header, '\tenum ParamID : FFUInt32', '\nprivate:', 'ParamID enum');
const anonText = cut(source, 'namespace\n{\nstd::string glStringOrUnknown', '} // namespace\n', 'anonymous namespace') + '} // namespace\n';
const ctorText = cut(source, 'Lowband::Lowband()', '//---------------------------------------------------------------------------\nFFResult Lowband::InitGL', 'constructor');
const processText = cut(source, 'FFResult Lowband::ProcessOpenGL', '//---------------------------------------------------------------------------\nFFResult Lowband::DeInitGL', 'ProcessOpenGL');

let ref = readFileSync(join(HERE, 'refport.cpp'), 'utf8');
for (const [marker, text] of [['//@@ENUM@@', enumText], ['//@@ANON@@', anonText], ['//@@CONSTRUCTOR@@', ctorText], ['//@@PROCESS@@', processText]]) {
  if (!ref.includes(marker)) throw new Error(`refport.cpp has no ${marker}`);
  ref = ref.replace(marker, () => text);
}

const keepAt = process.argv.indexOf('--keep');
const keep = keepAt >= 0 ? process.argv[keepAt + 1] : null;
const dir = keep ?? mkdtempSync(join(tmpdir(), 'lb-port-'));
writeFileSync(join(dir, 'refport_gen.cpp'), ref);

function build(tag, flags) {
  const exe = join(dir, `refport-${tag}`);
  const sources = ['Engine.cpp', 'Model.cpp', 'Dsp.cpp', 'Controls.cpp', 'Clock.cpp'].map((f) => join(REPO, 'source', f));
  const result = spawnSync('c++', ['-std=c++17', ...flags, '-Wall', '-Wno-unused-function', '-pthread', '-I', join(REPO, 'source'),
    join(dir, 'refport_gen.cpp'), ...sources, '-o', exe], { encoding: 'utf8' });
  if (result.status !== 0) {
    console.log(`FAIL  the reference (${flags.join(' ')}) did not compile:`);
    console.log(result.stderr.split('\n').slice(0, 30).join('\n'));
    process.exit(1);
  }
  return exe;
}

// --- the scenarios ------------------------------------------------------------
// ParamID order: Recording, Tape Noise, Dropouts, Deck, Standard, Head Clog,
// Sync AGC, Mix. Options by element index (Video8 0, Hi8 1; PAL 0, NTSC 1).

/** A small seeded generator for the pictures, so a scenario is the same every run. */
function prng(seed) {
  let s = seed >>> 0;
  return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return model.hash(s) / 4294967296; };
}

/** An intake as the GPU's pass would hand it over: P x N, ( Y', U, V, A ), line 0 first. */
function picture(kind, P, N, frame) {
  const pix = new Float32Array(P * N * 4);
  const rand = prng(0x5eed + frame * 7919 + P);
  for (let r = 0; r < N; r += 1) {
    for (let i = 0; i < P; i += 1) {
      const o = (r * P + i) * 4;
      let y = 0;
      let u = 0;
      let v = 0;
      const x = i / P;
      const yy = r / N;
      switch (kind) {
        case 'scene': {
          // A ramp, a bright box, fine stripes, a hard edge, moving with the frame.
          y = 0.15 + 0.6 * x;
          if (yy > 0.2 && yy < 0.45 && x > 0.3 + 0.01 * frame && x < 0.55 + 0.01 * frame) y = 1.0;
          if (yy > 0.6 && yy < 0.8) y = ((i >> 1) & 1) ? 0.9 : 0.1;
          if (yy >= 0.8) y = x < 0.5 ? 0.0 : 1.0;
          y += 0.02 * Math.sin(r * 0.37 + frame);
          u = 0.2 * Math.sin(x * 9 + yy * 3);
          v = 0.25 * Math.cos(x * 5 - yy * 7 + frame * 0.1);
          break;
        }
        case 'flat': y = yy < 1 / 3 ? 0.05 : yy < 2 / 3 ? 0.15 : 0.25; break;
        case 'edges': y = (Math.floor(x * (6 + (r % 5))) & 1) ? 1.0 : 0.0; u = y * 0.1; v = -y * 0.1; break;
        case 'wild': y = -0.2 + 1.4 * rand(); u = -0.6 + 1.2 * rand(); v = -0.6 + 1.2 * rand(); break;
        case 'white': y = 1.0; break;
        default: y = 0.0;
      }
      pix[o] = y;
      pix[o + 1] = u;
      pix[o + 2] = v;
      pix[o + 3] = 1.0;
    }
  }
  return pix;
}

const D = [...model.DEFAULTS];
const withP = (changes) => { const p = [...D]; for (const [k, v] of Object.entries(changes)) p[model.PT[k]] = v; return p; };
const ALL = [
  { name: 'the defaults (Hi8 on Video8, PAL, noise, dropouts, clog) on a moving scene, 1280x720', W: 1280, H: 720, kind: 'scene', p: D, t: [0, 0.1, 0.2, 0.45] },
  { name: 'Video8 on Video8, matched, nothing on: flat fields', W: 320, H: 180, kind: 'flat',
    p: withP({ RECORDING: 0, DECK: 0, TAPE_NOISE: 0, DROPOUTS: 0, HEAD_CLOG: 0 }), t: [0] },
  { name: 'Hi8 on Hi8, matched, NTSC, Head Clog 1 and Tape Noise 0.8: edges', W: 960, H: 540, kind: 'edges',
    p: withP({ RECORDING: 1, DECK: 1, STANDARD: 1, HEAD_CLOG: 1, TAPE_NOISE: 0.8, DROPOUTS: 0 }), t: [0, 1 / 30] },
  { name: 'Hi8 on Video8, NTSC, Sync AGC on, Mix 0.5: the scene', W: 640, H: 360, kind: 'scene',
    p: withP({ STANDARD: 1, SYNC_AGC: 1, MIX: 0.5 }), t: [0, 0.25] },
  { name: 'Video8 tape on a Hi8 deck (Video8 mode), Dropouts 1 (30 a frame), Tape Noise 1', W: 1920, H: 1080, kind: 'scene',
    p: withP({ RECORDING: 0, DECK: 1, DROPOUTS: 1, TAPE_NOISE: 1 }), t: [0, 0.04, 0.08] },
  { name: 'out-of-range Y and colour, clog 0.123, Tape Noise 0.0001', W: 320, H: 180, kind: 'wild',
    p: withP({ HEAD_CLOG: 0.123, TAPE_NOISE: 0.0001 }), t: [0] },
  { name: 'fractional options (lround), Sync AGC 0.49 off then 0.5 on', W: 64, H: 36, kind: 'scene',
    p: withP({ RECORDING: 0.6, DECK: 0.49, STANDARD: 0.51, SYNC_AGC: 0.49, MIX: 0.25 }), t: [0] },
  { name: 'Sync AGC at exactly 0.5, white', W: 64, H: 36, kind: 'white', p: withP({ SYNC_AGC: 0.5 }), t: [0] },
  { name: 'the clock: a backward jump (Restart), an over-long gap, a stall', W: 320, H: 180, kind: 'scene',
    p: withP({ DROPOUTS: 0.6 }), t: [0, 0.1, 0.2, 0.0, 0.05, 2.0, 2.4, 2.4, 3.1] },
  { name: 'black, the defaults, NTSC', W: 7, H: 3, kind: 'black', p: withP({ STANDARD: 1 }), t: [0] },
].map((s) => ({ ...s, p: s.p.map(Math.fround) }));
const SCENARIOS = quick ? [ALL[0], ALL[2], ALL[8]].map((s) => ({ ...s, t: s.t.slice(0, 3) })) : ALL;

// --- the port, run ------------------------------------------------------------
const bitsOf = (() => {
  const view = new DataView(new ArrayBuffer(8));
  return {
    f: (v) => { view.setFloat32(0, v); return view.getUint32(0).toString(16).padStart(8, '0'); },
    d: (v) => { view.setFloat64(0, v); return view.getBigUint64(0).toString(16).padStart(16, '0'); },
  };
})();

/** The page's split: rows in contiguous chunks, one a worker. */
function runPort(engines, pix, P, N, s, workers) {
  const drops = engineJs.frameDropouts(s, N);
  const parts = [];
  for (let w = 0; w < workers; w += 1) {
    const r0 = Math.floor((N * w) / workers);
    const r1 = Math.floor((N * (w + 1)) / workers);
    // A worker is handed rows r0 - 2 .. r1, exactly as the page slices them.
    const base = Math.max(0, r0 - 2);
    const held = pix.slice(base * P * 4, r1 * P * 4);
    parts.push(engineJs.processRows(engines[w % engines.length], held, base, r0, r1, N, P, s, drops));
  }
  return engineJs.finishFrame(parts, N, P, s);
}

const input = [];
const port = [];
let frameId = 0;
const splits = [1, 7, 3];
const engines = [new engineJs.Engine(), new engineJs.Engine(), new engineJs.Engine()];
let splitDiffers = 0;
for (const sc of SCENARIOS) {
  const inst = new model.Instance();
  const lines = [];
  input.push(`SCENARIO ${sc.W} ${sc.H} ${sc.p.map((v) => v.toPrecision(9)).join(' ')} ${sc.t.length}`);
  let lastP = 0;
  let lastN = 0;
  sc.t.forEach((t, f) => {
    inst.setTime(Number(t.toPrecision(17)));
    const fr = inst.beginFrame(sc.p, sc.W, sc.H);
    const { P, N, settings: s } = fr;
    const pix = picture(sc.kind, P, N, f);
    const inPath = join(dir, `in-${frameId}.f32`);
    const outPath = join(dir, `out-${frameId}.f32`);
    writeFileSync(inPath, Buffer.from(pix.buffer));
    input.push(`${t.toPrecision(17)} ${inPath} ${outPath}`);
    frameId += 1;

    const split = splits[f % splits.length];
    const done = runPort(engines, pix, P, N, s, split);
    if (f === 0) {
      // The same frame in a different split must be the same picture, float for float.
      const other = runPort([new engineJs.Engine()], pix, P, N, s, split === 1 ? 5 : 1);
      for (let i = 0; i < done.lines.length; i += 1) if (Object.is(done.lines[i], other.lines[i]) === false) { splitDiffers += 1; }
    }
    const plan = model.passes(sc.W, sc.H, P, N, fr.mixAmount);
    const ev = [];
    if (P !== lastP || N !== lastN) {
      ev.push(`ENSURE intake ${P} ${N}`);
      ev.push(`ALLOC lines ${P} ${N}`);
    }
    lastP = P;
    lastN = N;
    for (const pass of plan) {
      ev.push(`PASS ${pass.name} -> ${pass.target} textures${pass.textures.map((x) => ` ${x}`).join('')}`);
      const u = [];
      for (const [k, v] of Object.entries(pass.ints)) u.push(`I ${k} ${v}`);
      for (const [k, v] of Object.entries(pass.floats)) u.push(`F ${k} ${bitsOf.f(v)}`);
      u.sort();
      for (const l of u) ev.push(`  ${l}`);
      if (pass.name === 'intake') {
        ev.push(`READBACK intake ${P} ${N}`);
        ev.push(`UPLOAD lines ${P} ${N}`);
      }
    }
    lines.push({
      f, outPath, P, N,
      head: `FRAME ${f} result 0 seconds ${bitsOf.d(fr.seconds)} videoFrame ${s.videoFrame}`,
      settings: `SETTINGS ${s.standard} ${s.recording} ${s.deck} ${s.noise ? 1 : 0} ${bitsOf.d(s.cnrDb)} ${bitsOf.d(s.clogMetres)} ${bitsOf.d(s.dropoutsPerFrame)} ${s.syncAgc ? 1 : 0}`,
      monitor: `MONITOR ${bitsOf.d(done.depth)} ${bitsOf.d(done.gain)} ${bitsOf.d(done.porch)} ${bitsOf.d(engines[0].delay)}`,
      events: ev,
      upload: done.lines,
    });
  });
  port.push({ sc, frames: lines });
}
const stdin = input.join('\n') + '\n';

// The designs, written the reference's way.
function portDesigns() {
  const out = [`DESIGN shelfX ${bitsOf.d(model.kShelfX)}`];
  const cascade = (name, c) => {
    out.push(`DESIGN ${name}${Array.from(c.coeffs, (v) => ` ${bitsOf.f(v)}`).join('')}`);
    for (const x of [0.0, 0.3, -0.21, 5.7e6, 0.5627].map(Math.fround)) {
      const st = new Float32Array(64);
      c.steadyState(x, st);
      out.push(`STEADY ${name} ${bitsOf.f(x)}${Array.from(st.slice(0, c.stateSize()), (v) => ` ${bitsOf.f(v)}`).join('')}`);
    }
    // At DC and at the four blanking carriers Engine::configure evaluates the
    // delay at; then three probes, which also carry libm's cos and sin.
    out.push(`DELAY ${name} ${bitsOf.d(0.0)} ${bitsOf.d(c.groupDelay(0.0))}`);
    for (let f = 0; f < model.kFormatCount; f += 1) {
      for (let st = 0; st < model.kStandardCount; st += 1) {
        const S = model.standardOf(st).syncVolts;
        const Wh = model.standardOf(st).whiteVolts;
        const own = model.formatOf(f);
        const w = 2.0 * model.kPi * (own.syncTipHz + S / (S + Wh) * own.deviationHz) / model.kSampleHz;
        out.push(`DELAY ${name} ${bitsOf.d(w)} ${bitsOf.d(c.groupDelay(w))}`);
      }
    }
    for (const w of [0.1, 0.77, 2.5]) out.push(`PROBE ${name} ${bitsOf.d(w)} ${bitsOf.d(c.groupDelay(w))}`);
  };
  for (let f = 0; f < model.kFormatCount; f += 1) {
    const F = model.formatOf(f);
    cascade(`${F.name}.recordY`, dsp.ButterworthLowPass(model.kRecordYOrder, F.recordYHz, model.kSampleHz));
    cascade(`${F.name}.pre`, dsp.Shelf(F.emphasisTau, F.emphasisX, model.kSampleHz, false));
    cascade(`${F.name}.de`, dsp.Shelf(F.emphasisTau, F.emphasisX, model.kSampleHz, true));
    cascade(`${F.name}.rfHigh`, dsp.ButterworthHighPass(model.kRfHighOrder, F.rfHighPassHz, model.kSampleHz));
    cascade(`${F.name}.rfLow`, dsp.ButterworthLowPass(model.kRfLowOrder, F.rfLowPassHz, model.kSampleHz));
    cascade(`${F.name}.yLow`, dsp.ButterworthLowPass(model.kYLowOrder, F.yLowPassHz, model.kSampleHz));
    cascade(`${F.name}.record`, dsp.ButterworthLowPass(model.kRecordYOrder, F.recordYHz, model.kSampleHz).then(dsp.Shelf(F.emphasisTau, F.emphasisX, model.kSampleHz, false)));
  }
  cascade('chroma', dsp.ButterworthLowPass(2, model.kChromaHalfHz, model.kPixelHz));
  for (let st = 0; st < model.kStandardCount; st += 1) {
    const S = model.standardOf(st);
    out.push(`STANDARD ${st} ${S.lineSamples()} ${S.syncSamples()} ${S.activeStart()} ${S.activePixels()} ${bitsOf.d(S.writingSpeed())} ${bitsOf.d(S.active())} ${bitsOf.d(S.frameRate())}`);
    for (const um of [0.0, 0.01, 0.06, 0.15]) {
      out.push(`CLOG ${st} ${bitsOf.d(um)}${Array.from(model.clogTaps(um * 1e-6, S.writingSpeed(), model.kClogHalfTaps), (v) => ` ${bitsOf.f(v)}`).join('')}`);
    }
  }
  for (const db of [46.0, 30.0, 24.4, 10.0, 27.7]) out.push(`SIGMA ${bitsOf.d(db)} ${bitsOf.d(model.noiseSigma(db))}`);
  for (const frame of [0, 1, 7, 1000, 123456789]) {
    for (const per of [0.0, 0.3, 4.5, 30.0, 100.0]) {
      out.push(`DROPOUTS ${frame} ${bitsOf.d(per)}${model.dropouts(frame, per, 576, model.standardOf(0).active()).map((d) => ` ${d.line}:${bitsOf.d(d.us0)}:${bitsOf.d(d.us1)}:${engineJs.samples(d.us0)}:${engineJs.samples(d.us1)}`).join('')}`);
    }
    out.push(`SEEDS ${frame}${[0, 1, 2, 575, 479].map((l) => ` ${model.lineSeed(frame, l)}`).join('')}`);
  }
  return out;
}

const typeId = model.FF_TYPE;
const declJs = [];
for (const d of model.DECLARATIONS) {
  if (d.type === 'option') {
    declJs.push(`PARAM ${d.index} ${typeId.option} ${bitsOf.f(d.default)} ${d.name}`);
    declJs.push(`  COUNT ${d.elements.length}`);
    d.elements.forEach((e, i) => declJs.push(`  ELEMENT ${d.index} ${i} ${e} ${bitsOf.f(i)}`));
  } else {
    declJs.push(`PARAM ${d.index} ${typeId[d.type]} ${bitsOf.f(d.default)} ${d.name}`);
  }
}
const groupsJs = model.DECLARATIONS.map((d) => `  GROUP ${d.index} ${d.group}`);

// --- compare ------------------------------------------------------------------
function compare(stdout, verbose, gaussPath) {
  const say = (text) => { if (verbose && problems < (process.env.LB_ALL ? 1e9 : 16)) console.log(text); problems += 1; };
  const log = (text) => { if (verbose) console.log(text); };
  let problems = 0;
  const spread = { delay: [0, 0, 0], probe: [0, 0, 0], monitor: [0, 0, 0], dropout: [0, 0, 0] };// count, differing, max ulps
  const ulps = (kind, a, b) => {
    const x = BigInt(`0x${a}`);
    const y = BigInt(`0x${b}`);
    spread[kind][0] += 1;
    if (x !== y) { spread[kind][1] += 1; spread[kind][2] = Math.max(spread[kind][2], Number(x > y ? x - y : y - x)); }
  };
  const refLines = stdout.split('\n');
  let i = 0;

  // The designs.
  const designJs = portDesigns();
  const designRef = [];
  while (i < refLines.length && /^(DESIGN|STEADY|DELAY|PROBE|STANDARD|CLOG|SIGMA|DROPOUTS|SEEDS) /.test(refLines[i])) designRef.push(refLines[i++]);
  // Group delays are doubles from double coefficients that libm's tan, sin
  // and cos made (the probes at arbitrary frequencies add cos and sin of their
  // own). JavaScript's Math functions are not macOS's libm -- neither is
  // correctly rounded -- so these are compared in ulps and reported; the
  // float coefficients the chain RUNS with are compared exactly.
  let designBad = 0;
  let delays = 0;
  for (let k = 0; k < Math.max(designJs.length, designRef.length); k += 1) {
    const r = designRef[k] ?? '<missing>';
    const j = designJs[k] ?? '<missing>';
    const rk = r.split(' ');
    const jk = j.split(' ');
    if (/^(DELAY|PROBE) /.test(r) && rk.slice(0, 3).join(' ') === jk.slice(0, 3).join(' ')) {
      delays += 1;
      ulps(r.startsWith('PROBE') ? 'probe' : 'delay', rk[3], jk[3]);
      continue;
    }
    if (r.startsWith('DROPOUTS ') && rk.length === jk.length && rk.slice(0, 3).join(' ') === jk.slice(0, 3).join(' ')) {
      // A dropout's end is an exp() of a hash (libm's there, JavaScript's
      // here): its double in ulps; its line, its start, and both ends as the
      // samples the chain uses, exactly.
      let same = true;
      for (let q = 3; q < rk.length; q += 1) {
        const a = rk[q].split(':');
        const b = jk[q].split(':');
        if (a[0] !== b[0] || a[1] !== b[1] || a[3] !== b[3] || a[4] !== b[4]) same = false;
        else ulps('dropout', a[2], b[2]);
      }
      if (same) continue;
    }
    if (r === j) continue;
    designBad += 1;
    say(`FAIL  design: plugin "${r.slice(0, 110)}"\n             port   "${j.slice(0, 110)}"`);
  }
  if (designBad === 0) log(`ok    the designs: ${designRef.length - delays} records (every filter's float coefficients and steady states; the rasters; the clog taps; sigma; the dropouts; the line seeds) identical`);

  if (gaussPath) {
    const ref = new Uint32Array(readFileSync(gaussPath).buffer.slice(0));
    const js = new Uint32Array(model.GaussianTable().buffer);
    let bad = 0;
    for (let k = 0; k < js.length; k += 1) if (ref[k] !== js[k]) bad += 1;
    if (bad || ref.length !== js.length) say(`FAIL  the Gaussian table: ${bad} of ${js.length} entries differ from the plugin's (erfc is the page's own)`);
    else log(`ok    the Gaussian table: all ${js.length} entries identical to the plugin's (std::erfc there, the page's own erfc here)`);
  }

  // Declarations: the constructor's record, About dropped.
  const declRef = [];
  const groupsRef = [];
  let inAbout = false;
  while (i < refLines.length && !refLines[i].startsWith('SCENARIO')) {
    const l = refLines[i++];
    if (!l) continue;
    const m = /^(PARAM|  ELEMENT|  GROUP) (\d+)/.exec(l);
    const index = m ? Number(m[2]) : NaN;
    if (l.startsWith('  GROUP')) { if (index < model.kParamCount) groupsRef.push(l); continue; }
    if (l.startsWith('PARAM')) inAbout = index >= model.kParamCount;
    if (!inAbout) declRef.push(l);
  }
  const declSame = declRef.length === declJs.length && declRef.every((l, k) => l === declJs[k]);
  if (!declSame) {
    say('FAIL  DECLARATIONS is not what Lowband::Lowband() declares:');
    for (let k = 0; k < Math.max(declRef.length, declJs.length); k += 1) {
      if (declRef[k] !== declJs[k]) { log(`        plugin: ${declRef[k]}\n        page  : ${declJs[k]}`); break; }
    }
  } else log(`ok    the ${model.DECLARATIONS.length} parameters: names, FFGL types, defaults (as floats), option elements and values`);
  const groupsSame = groupsRef.length === groupsJs.length && groupsRef.every((l, k) => l === groupsJs[k]);
  if (!groupsSame) say("FAIL  the parameter groups differ from the constructor's");
  else log('ok    the groups (Tape, Deck, Monitor) on the same ids');

  let frames = 0;
  let floats = 0;
  let floatsDiffer = 0;
  let worst = 0;
  let monitorDiffer = 0;
  let bigDiffer = 0;
  const perScenario = new Map();
  for (const { sc, frames: fr } of port) {
    while (i < refLines.length && refLines[i] === '') i += 1;
    const head = refLines[i++] ?? '';
    if (head !== `SCENARIO ${sc.W} ${sc.H}`) { say(`FAIL  expected the scenario "${sc.name}", the reference has "${head}"`); break; }
    for (const f of fr) {
      frames += 1;
      const want = [f.head, f.settings, f.monitor, ...f.events];
      for (const w of want) {
        const got = refLines[i++] ?? '';
        if (got === w) continue;
        if (w.startsWith('MONITOR ') && got.startsWith('MONITOR ')) {
          // Depth, gain and porch exactly; the deck's delay (a group delay) in ulps.
          const a = got.split(' ');
          const b = w.split(' ');
          if (a.slice(0, 4).join(' ') === b.slice(0, 4).join(' ')) { ulps('monitor', a[4], b[4]); continue; }
          monitorDiffer += 1;
        }
        say(`FAIL  ${sc.name}, frame ${f.f}:\n        plugin: ${got.slice(0, 120)}\n        port  : ${w.slice(0, 120)}`);
      }
      const ref = new Float32Array(readFileSync(f.outPath).buffer.slice(0));
      const refBits = new Uint32Array(ref.buffer);
      const jsBits = new Uint32Array(f.upload.buffer);
      floats += jsBits.length;
      if (refBits.length !== jsBits.length) { say(`FAIL  ${sc.name}, frame ${f.f}: the upload has ${refBits.length} floats in the plugin, ${jsBits.length} here`); continue; }
      let differ = 0;
      let first = -1;
      for (let k = 0; k < jsBits.length; k += 1) {
        if (refBits[k] === jsBits[k]) continue;
        differ += 1;
        if (first < 0) first = k;
        worst = Math.max(worst, Math.abs(ref[k] - f.upload[k]));
      }
      floatsDiffer += differ;
      if (differ) {
        let big = 0;
        let most = 0;
        for (let k = 0; k < jsBits.length; k += 1) {
          const e = Math.abs(ref[k] - f.upload[k]);
          if (e > 1 / 255) big += 1;
          if (e > most) most = e;
        }
        bigDiffer += big;
        const key = sc.name;
        const row = perScenario.get(key) ?? { floats: 0, differ: 0, big: 0, most: 0 };
        row.floats += jsBits.length; row.differ += differ; row.big += big; row.most = Math.max(row.most, most);
        perScenario.set(key, row);
      }
      if (differ) {
        const px = first >> 2;
        say(`FAIL  ${sc.name}, frame ${f.f}: ${differ} of ${jsBits.length} uploaded floats differ; first at line ${Math.floor(px / f.P)} pixel ${px % f.P} channel ${first & 3}: plugin ${ref[first]}, port ${f.upload[first]}`);
      }
    }
  }
  while (i < refLines.length && refLines[i] === '') i += 1;
  if (i < refLines.length) say(`FAIL  the reference has ${refLines.length - i} more lines than the port wrote`);
  if (verbose) {
    const [n, d, u] = spread.delay;
    const [pn, pd, pu] = spread.probe;
    const [mn, md, mu] = spread.monitor;
    const [dn, dd, du] = spread.dropout;
    log(`note  the dropouts: every line, start and sample identical; ${dd} of ${dn} ends (an exp() of a hash, libm's against JavaScript's) differ as doubles, by at most ${du} ulp, and none as the sample the chain uses`);
    log(`note  group delays, which reach the chain only as the deck's delay compensation (a double, rounded to whole samples for the clamp and added to each pixel's position): ${d} of ${n} filter delays at DC and at the four blanking carriers differ from the plugin's, by at most ${u} ulp, and ${md} of ${mn} frames' compensated delays, by at most ${mu} ulp; at the ${pn} probe frequencies, ${pd} by at most ${pu} ulp. libm's tan, sin and cos are not JavaScript's (and neither is correctly rounded: cos(0.1) and sin(0.77) differ in the last bit), so the double coefficients differ in their last bits while every float the chain runs with is identical`);
  }
  return { problems, frames, floats, floatsDiffer, worst, monitorDiffer, designBad, spread, bigDiffer, perScenario };
}

const run = (exe, tag) => {
  const args = [join(dir, `gauss-${tag}.f32`)];
  const r = spawnSync(exe, args, { input: stdin, encoding: 'utf8', maxBuffer: 1 << 30 });
  if (r.status !== 0) {
    console.log(`FAIL  the reference exited ${r.status}: ${r.stderr}`);
    process.exit(1);
  }
  return r.stdout;
};

// The plugin's arithmetic as written: no fused multiply-add. This is what the
// x86_64 slice of the universal bundle does (no FMA in its baseline).
const strict = compare(run(build('strict', ['-O2', '-ffp-contract=off']), 'strict'), true, join(dir, 'gauss-strict.f32'));
console.log();
if (splitDiffers) {
  console.log(`FAIL  the port gave a different picture for the same frame split another way (${splitDiffers} floats)`);
  process.exit(1);
}
console.log(`ok    the port is the same picture whichever way a frame's lines are split between workers (1, 3, 5 and 7)`);
if (strict.problems) {
  console.log(`${strict.problems} difference(s) between the port and the plugin's C++ as written (-ffp-contract=off); the largest uploaded difference ${strict.worst.toExponential(3)}`);
  process.exit(1);
}
console.log(`ok    ${SCENARIOS.length} scenarios, ${strict.frames} frames: the clock, the video frame, the settings, the sync depth, gain and porch, every buffer, the read-back, both passes' textures and uniforms, and ${strict.floats} uploaded floats, all bit-identical to the plugin's own ProcessOpenGL and Engine compiled with -ffp-contract=off (the deck's delay, a double, in ulps above)`);

// The same C++ as the arm64 slice is built: -O3 and clang's default
// -ffp-contract=on, which fuses a * b + c into one rounding. JavaScript never
// fuses, so the port differs from that slice in the last bits, and a crossing
// that moves by an ulp moves its pulse: said, measured, not failed on.
const arm = compare(run(build('shipped', ['-O3']), 'shipped'), false, join(dir, 'gauss-shipped.f32'));
if (arm.problems === 0) console.log('ok    the same C++ at -O3 with FMA contraction (the arm64 build\'s flags): identical too');
else {
  console.log(`note  the same C++ at -O3 with FMA contraction (the arm64 slice's flags): ${arm.floatsDiffer} of ${arm.floats} uploaded floats differ from the port, ${arm.bigDiffer} of them by more than 1/255 (at most ${arm.worst.toFixed(3)}); ${arm.monitorDiffer} of ${arm.frames} frames' depth/gain/porch differ. Per scenario:`);
  for (const [name, r] of arm.perScenario) console.log(`        ${name}: ${r.differ} of ${r.floats} differ, ${r.big} by more than 1/255, at most ${r.most.toExponential(2)}`);
}
const probe = spawnSync('c++', ['-arch', 'x86_64', '-x', 'c++', '-', '-o', join(dir, 'probe-x86')], { input: 'int main(){return 0;}', encoding: 'utf8' });
const rosetta = probe.status === 0 && spawnSync(join(dir, 'probe-x86')).status === 0;
if (rosetta) {
  const x86 = compare(run(build('x86_64', ['-O3', '-arch', 'x86_64']), 'x86_64'), false, join(dir, 'gauss-x86_64.f32'));
  if (x86.problems === 0) console.log('ok    the same C++ at -O3 for x86_64 (the other slice, under Rosetta): identical to the port on every value');
  else console.log(`note  the same C++ at -O3 for x86_64 (under Rosetta): ${x86.floatsDiffer} of ${x86.floats} uploaded floats differ, by at most ${x86.worst.toExponential(2)}; ${x86.designBad} design records differ (that slice's libm and Gaussian table are its own)`);
} else {
  console.log('skip  no x86_64 run: no Rosetta or no x86_64 toolchain here');
}
console.log('the port matches the plugin on every value recorded');
if (!keep) rmSync(dir, { recursive: true, force: true });
