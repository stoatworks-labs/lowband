/**
 * Lowband's numbers, PORTED to JavaScript: Model.cpp, Controls.cpp, Clock.cpp,
 * the constructor's declarations and the CPU half of Lowband::ProcessOpenGL
 * (the settings and the clock).
 *
 *   source/Model.cpp      the two standards and the two formats, the deck's
 *                         playback mode, the noise's sigma for a CNR, the head
 *                         clog's FIR taps (Wallace), the dropouts, the PCG
 *                         hash, the 65,536-entry Gaussian table and the line
 *                         seeds
 *   source/Controls.cpp   every slider's law
 *   source/Clock.cpp      the part that runs with the unit declared (lbtest's
 *                         case and this page's): origin + offset in double, a
 *                         jump of one nominal frame on a backward or
 *                         over-long delta. The unit vote never runs here.
 *   source/Lowband.cpp    the constructor's parameters (DECLARATIONS) and the
 *                         head of ProcessOpenGL: the settings, the raster, the
 *                         clock and the video frame (`Instance.beginFrame`)
 *
 * The signal chain itself is demo/engine.js and the filters demo/dsp.js.
 * `demo/tools/check_port.sh` compiles the plugin's own C++ and compares.
 *
 * Arithmetic is kept in the C++'s order, in doubles; a C++ `float` is
 * `Math.fround`, `std::lround` rounds half away from zero, and the 32-bit
 * hash is `Math.imul` and `>>> 0`. The one function JavaScript does not have
 * is `erfc`, which the Gaussian table's Halley step needs: `erfc` below is
 * this page's own (a series and a continued fraction, not the platform
 * libm's), and check_port.sh compares all 65,536 entries of the table it
 * produces with the plugin's.
 */

const f32 = Math.fround;

/** std::lround / std::round: half away from zero. */
export const roundAway = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
export const clamp = (v, lo, hi) => (v < lo ? lo : hi < v ? hi : v);

//---------------------------------------------------------------------------
// Model.h
//---------------------------------------------------------------------------

export const kPi = 3.14159265358979323846;
export const kSampleHz = 40.5e6;
export const kPixelHz = 13.5e6;
export const kOversample = 3;

export const kPAL = 0;
export const kNTSC = 1;
export const kStandardCount = 2;

export const kVideo8 = 0;
export const kHi8 = 1;
export const kFormatCount = 2;

export const kRecordYOrder = 4;
export const kRfLowOrder = 8;
export const kRfHighOrder = 2;
export const kYLowOrder = 8;

export const kChromaHalfHz = 0.5e6;
export const kClogHalfTaps = 24;
export const kMaxDropouts = 64;
export const kDropoutDepth = 0.0316227766;
export const kDropoutEdgeUs = 0.3;
export const kGaussianBits = 16;

//---------------------------------------------------------------------------
// Model.cpp
//---------------------------------------------------------------------------

const kFhPal = 15625.0;
const kFhNtsc = 15750000.0 / 1001.0;

function standard(name, line, frontPorch, sync, backPorch, frameLines, frameNum, frameDen, syncVolts, whiteVolts, colourUnderHz) {
  return {
    name, line, frontPorch, sync, backPorch, frameLines, frameNum, frameDen, syncVolts, whiteVolts, colourUnderHz,
    active() { return this.line - this.frontPorch - this.sync - this.backPorch; },
    frameRate() { return this.frameNum / this.frameDen; },
    lineSamples() { return roundAway(this.line * kSampleHz * 1e-6); },
    syncSamples() { return roundAway(this.sync * kSampleHz * 1e-6); },
    activeStart() { return Math.floor((this.sync + this.backPorch) * kSampleHz * 1e-6); },
    activePixels() { return roundAway(this.active() * kPixelHz * 1e-6); },
    writingSpeed() { return kPi * 0.040 * this.frameRate(); },
  };
}

const kStandards = [
  standard('625/50 (PAL)', 64.0, 1.65, 4.7, 5.7, 576, 25, 1, 0.3, 0.7, 46.875 * kFhPal),
  standard('525/59.94 (NTSC)', 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 30000, 1001, 40.0 / 140.0, 100.0 / 140.0, 47.25 * kFhNtsc),
];

export const standardOf = (index) => kStandards[clamp(index, 0, kStandardCount - 1)];

/** The shelf's gain, computed from vhs-decode's dB figure, as Model.cpp does. */
export const kShelfX = Math.pow(10.0, 11.5794 / 20.0);

function format(name, syncTipHz, deviationHz, emphasisTau, emphasisX, recordYHz, whiteClip, darkClip, rfHighPassHz, rfLowPassHz, yLowPassHz) {
  return {
    name, syncTipHz, deviationHz, emphasisTau, emphasisX, recordYHz, whiteClip, darkClip, rfHighPassHz, rfLowPassHz, yLowPassHz,
    peakWhiteHz() { return this.syncTipHz + this.deviationHz; },
  };
}

const kFormats = [
  format('Video8', 4.2e6, 1.2e6, 1.30e-6, kShelfX, 3.5e6, 2.2, 0.9, 1.9e6, 7.0e6, 3.5e6),
  format('Hi8', 5.7e6, 2.0e6, 0.47e-6, kShelfX, 5.0e6, 2.2, 0.9, 1.85e6, 10.3e6, 5.0e6),
];

export const formatOf = (index) => kFormats[clamp(index, 0, kFormatCount - 1)];

/** A Hi8 deck plays each tape in its own mode; a Video8 deck has one. */
export const playbackMode = (deck, recording) => (deck === kHi8 ? clamp(recording, 0, kFormatCount - 1) : kVideo8);

export const referenceBandHz = () => kFormats[kVideo8].rfLowPassHz - kFormats[kVideo8].rfHighPassHz;

export function noiseSigma(cnrDb) {
  const cnr = Math.pow(10.0, cnrDb / 10.0);
  return Math.sqrt(0.5 * (kSampleHz / 2.0) / (referenceBandHz() * cnr));
}

export const spacingLossDb = (spacingMetres, fHz, speed) => 20.0 / Math.log(10.0) * 2.0 * kPi * spacingMetres * fHz / speed;

/** Wallace's spacing loss as a zero-phase FIR (Model.cpp's closed form). Empty for d = 0. */
export function clogTaps(spacingMetres, speed, halfLength) {
  if (spacingMetres <= 0.0) return new Float32Array(0);
  const a = 2.0 * kPi * spacingMetres / speed;
  const fs = kSampleHz;
  const eB = Math.exp(-a * fs / 2.0);
  const taps = new Float32Array(2 * halfLength + 1);
  for (let n = -halfLength; n <= halfLength; n += 1) {
    const b = 2.0 * kPi * n / fs;
    const sign = (n & 1) ? -1.0 : 1.0;
    const h = (2.0 / fs) * a * (1.0 - sign * eB) / (a * a + b * b);
    taps[n + halfLength] = h;
  }
  return taps;
}

//--- the fleet's hash -------------------------------------------------------

/** PCG's output permutation, exact in 32 bits. */
export function hash(v) {
  v >>>= 0;
  const state = (Math.imul(v, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

export const hashUnit = (h) => (h >>> 8) * (1.0 / 16777216.0);

/** The two 32-bit halves of an int64 the C++ takes them from (non-negative here). */
const low32 = (v) => v >>> 0;
const high32 = (v) => Math.floor(v / 4294967296) >>> 0;

const kNoiseSeed = 0x4C420001;
const kDropSeed = 0x4C420002;

export function dropouts(videoFrame, perFrame, lines, activeUs) {
  const out = [];
  if (perFrame <= 0.0) return out;
  const seed = hash((low32(videoFrame) ^ hash((high32(videoFrame) + kDropSeed) >>> 0)) >>> 0);
  let count = Math.floor(perFrame);
  if (hashUnit(hash((seed ^ 0x51) >>> 0)) < perFrame - Math.floor(perFrame)) count += 1;
  count = Math.min(count, kMaxDropouts);
  for (let i = 0; i < count; i += 1) {
    const h = hash((seed + Math.imul(0x9E3779B9, i + 1)) >>> 0);
    const line = (hash((h ^ 1) >>> 0) % (lines >>> 0)) | 0;
    const us0 = hashUnit(hash((h ^ 2) >>> 0)) * activeUs;
    const len = Math.exp(Math.log(1.0) + hashUnit(hash((h ^ 3) >>> 0)) * (Math.log(25.0) - Math.log(1.0)));
    const us1 = Math.min(activeUs, us0 + len);
    out.push({ line, us0, us1 });
  }
  return out;
}

export function lineSeed(videoFrame, line) {
  const f = hash((low32(videoFrame) ^ hash((high32(videoFrame) + kNoiseSeed) >>> 0)) >>> 0);
  return hash((f ^ hash((Math.imul(line >>> 0, 0x85EBCA6B) + 0x27D4EB2F) >>> 0)) >>> 0);
}

//--- the Gaussian table ----------------------------------------------------

/**
 * erfc in double. NOT the plugin's: Model.cpp calls the platform's
 * std::erfc, and JavaScript has none. Below 1, 1 - erf by erf's series with
 * every term positive (no cancellation); from 1, the continued fraction,
 * evaluated by Lentz's method. A few units in the last place either way, which
 * moves the table's double quantiles by about 1e-16 -- far below the float
 * each is stored as (check_port.sh compares every entry).
 */
export function erfc(y) {
  if (y < 1.0) {
    // erf(z) = 2/sqrt(pi) e^(-z^2) sum 2^n z^(2n+1) / (1 3 5 ... (2n+1))
    const z = Math.abs(y);
    const z2 = z * z;
    let term = z;
    let sum = z;
    for (let n = 1; n < 200; n += 1) {
      term *= 2.0 * z2 / (2 * n + 1);
      sum += term;
      if (term < sum * 1e-17) break;
    }
    const erf = 2.0 / Math.sqrt(kPi) * Math.exp(-z2) * sum;
    return y < 0 ? 1.0 + erf : 1.0 - erf;
  }
  // erfc(y) = e^(-y^2) / sqrt(pi) * 1 / ( y + (1/2) / ( y + 1 / ( y + (3/2) / ( y + ... ) ) ) )
  const tiny = 1e-300;
  let fr = y;
  let C = fr;
  let D = 0.0;
  for (let n = 1; n < 2000; n += 1) {
    const an = n / 2.0;
    D = y + an * D;
    D = D === 0 ? tiny : D;
    C = y + an / C;
    C = C === 0 ? tiny : C;
    D = 1.0 / D;
    const delta = C * D;
    fr *= delta;
    if (Math.abs(delta - 1.0) < 1e-17) break;
  }
  return Math.exp(-y * y) / Math.sqrt(kPi) / fr;
}

/** Model.cpp's normalQuantile: Acklam's rational approximation, then one Halley step. */
export function normalQuantile(p) {
  const a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00];
  const b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01];
  const c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00];
  const d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00];
  const low = 0.02425;
  let x;
  if (p < low) {
    const q = Math.sqrt(-2.0 * Math.log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  } else if (p > 1.0 - low) {
    const q = Math.sqrt(-2.0 * Math.log(1.0 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  } else {
    const q = p - 0.5;
    const r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  }
  const e = 0.5 * erfc(-x / Math.sqrt(2.0)) - p;
  const u = e * Math.sqrt(2.0 * kPi) * Math.exp(x * x / 2.0);
  return x - u / (1.0 + x * u / 2.0);
}

let gaussianTable = null;

/** 65,536 quantiles of the unit normal at ( i + 1/2 ) / 65536, as floats. Built once. */
export function GaussianTable() {
  if (gaussianTable === null) {
    const t = new Float32Array(1 << kGaussianBits);
    const n = t.length;
    for (let i = 0; i < n; i += 1) t[i] = normalQuantile((i + 0.5) / n);
    gaussianTable = t;
  }
  return gaussianTable;
}

//---------------------------------------------------------------------------
// Controls.cpp
//---------------------------------------------------------------------------

const unit = (value) => clamp(f32(value), 0.0, 1.0);
const kCnrTop = 46.0;
const kCnrBottom = 10.0;
const kClogMax = 0.15e-6;

export const controls = {
  optionIndex: (value, count) => clamp(roundAway(f32(value)), 0, count - 1),
  formatName: (index) => (index === kHi8 ? 'Hi8' : 'Video8'),
  standardName: (index) => (index === kNTSC ? 'NTSC' : 'PAL'),
  noiseOn: (value) => f32(value) > 0.0,
  cnrDb: (value) => kCnrTop - (kCnrTop - kCnrBottom) * unit(value),
  cnrParam: (db) => f32(clamp((kCnrTop - db) / (kCnrTop - kCnrBottom), 0.0, 1.0)),
  dropoutsPerFrame: (value) => { const v = unit(value); return 30.0 * v * v; },
  clogMetres: (value) => kClogMax * unit(value),
  clogParam: (metres) => f32(clamp(metres / kClogMax, 0.0, 1.0)),
  amount: (value) => f32(unit(value)),
};

//---------------------------------------------------------------------------
// Clock.cpp, with the unit declared (seconds): what lbtest does and what this
// page does. The vote on the host's unit and the wall-clock fallback before it
// never run, because the unit is never undecided.
//---------------------------------------------------------------------------

export const kMaxFrameSeconds = 0.5;
export const kNominalFrameSeconds = 1.0 / 60.0;

export class Clock {
  constructor() { this.reset(); }

  reset() {
    this.started = false;
    this.lastScaled = -1.0;
    this.anchor = 0.0;
    this.offset = 0.0;
    this.now = 0.0;
    this.jumped = false;
  }

  update(hostTime) {
    this.jumped = false;
    const scaled = hostTime * 1.0;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0.0;
      this.now = 0.0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0.0 || delta > kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}

//---------------------------------------------------------------------------
// Lowband.cpp: the parameters, and the head of ProcessOpenGL.
//---------------------------------------------------------------------------

/** Lowband::ParamID, in the plugin's enum order. */
export const PT = {
  RECORDING: 0, TAPE_NOISE: 1, DROPOUTS: 2,
  DECK: 3, STANDARD: 4, HEAD_CLOG: 5,
  SYNC_AGC: 6, MIX: 7,
};
export const kParamCount = 8;

/** Lowband::Lowband()'s defaults, as the floats it stores. */
export const DEFAULTS = [
  f32(kHi8), f32(0.6), f32(0.15),
  f32(kVideo8), f32(kPAL), f32(0.4),
  f32(0.0), f32(1.0),
];

/**
 * Every parameter Lowband::Lowband() declares, in its order: name, FFGL type,
 * group, the option elements (each element's value is its index) and the
 * default float. The About block is left out, as on every page in this suite.
 * check_port.sh compares this table with what the plugin's own constructor
 * declares, run under a recorder.
 */
export const FF_TYPE = { boolean: 0, standard: 10, option: 11 };
const FORMATS = [controls.formatName(kVideo8), controls.formatName(kHi8)];
const STANDARDS = [controls.standardName(kPAL), controls.standardName(kNTSC)];
export const DECLARATIONS = [
  { index: PT.RECORDING, name: 'Recording', type: 'option', group: 'Tape', elements: FORMATS, default: DEFAULTS[PT.RECORDING] },
  { index: PT.TAPE_NOISE, name: 'Tape Noise', type: 'standard', group: 'Tape', default: DEFAULTS[PT.TAPE_NOISE] },
  { index: PT.DROPOUTS, name: 'Dropouts', type: 'standard', group: 'Tape', default: DEFAULTS[PT.DROPOUTS] },
  { index: PT.DECK, name: 'Deck', type: 'option', group: 'Deck', elements: FORMATS, default: DEFAULTS[PT.DECK] },
  { index: PT.STANDARD, name: 'Standard', type: 'option', group: 'Deck', elements: STANDARDS, default: DEFAULTS[PT.STANDARD] },
  { index: PT.HEAD_CLOG, name: 'Head Clog', type: 'standard', group: 'Deck', default: DEFAULTS[PT.HEAD_CLOG] },
  { index: PT.SYNC_AGC, name: 'Sync AGC', type: 'boolean', group: 'Monitor', default: DEFAULTS[PT.SYNC_AGC] },
  { index: PT.MIX, name: 'Mix', type: 'standard', group: 'Monitor', default: DEFAULTS[PT.MIX] },
];

/**
 * The plugin instance's CPU state across frames: the clock, the host time
 * SetTime last handed over, the last host size. Lowband's members of the
 * same names; the test hooks (perturb, quiet, the forced dropout) are what
 * the shipped plugin carries: off.
 */
export class Instance {
  constructor() {
    this.clock = new Clock();
    this.hostTimeSeen = false;
    this.hostTime = 0.0;
    this.lastWidth = 0;
    this.lastHeight = 0;
  }

  /** Lowband::SetTime. */
  setTime(time) {
    this.hostTimeSeen = true;
    this.hostTime = time;
  }

  /**
   * The head of Lowband::ProcessOpenGL, up to the buffers: the settings from
   * the parameter floats, the standard's raster, the clock and the video
   * frame. `params` are the plugin's floats in ParamID order.
   */
  beginFrame(params, W, H) {
    const p = params.map(f32);
    const s = {
      standard: controls.optionIndex(p[PT.STANDARD], kStandardCount),
      recording: controls.optionIndex(p[PT.RECORDING], kFormatCount),
      deck: controls.optionIndex(p[PT.DECK], kFormatCount),
      noise: controls.noiseOn(p[PT.TAPE_NOISE]),
      cnrDb: controls.cnrDb(p[PT.TAPE_NOISE]),
      clogMetres: controls.clogMetres(p[PT.HEAD_CLOG]),
      dropoutsPerFrame: controls.dropoutsPerFrame(p[PT.DROPOUTS]),
      syncAgc: p[PT.SYNC_AGC] >= 0.5,
      videoFrame: 0,
      perturb: 0,
    };
    const mixAmount = controls.amount(p[PT.MIX]);
    const st = standardOf(s.standard);
    const P = st.activePixels();
    const N = st.frameLines;

    // The clock. A resize is not a reason to touch it.
    this.lastWidth = W;
    this.lastHeight = H;
    this.clock.update(this.hostTimeSeen ? this.hostTime : -1.0);
    s.videoFrame = Math.floor(this.clock.now * st.frameRate());
    return { settings: s, mixAmount, P, N, seconds: this.clock.now };
  }
}

/**
 * ProcessOpenGL's two passes: which program draws into which buffer, the
 * texture on each unit, and every uniform it sets, by name. plugin.js
 * executes this; check_port.sh compares it with the plugin's own calls.
 */
export function passes(W, H, P, N, mixAmount) {
  return [
    {
      name: 'intake', target: 'intake', textures: ['picture'],
      ints: { Source: 0, HostW: W, HostH: H, Pixels: P, Lines: N }, floats: {},
    },
    {
      name: 'display', target: 'host', textures: ['lines', 'picture'],
      ints: { Lines: 0, Source: 1, HostW: W, HostH: H, LineCount: N, Pixels: P }, floats: { MixAmount: mixAmount },
    },
  ];
}
