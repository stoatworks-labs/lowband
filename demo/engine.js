/**
 * Lowband's signal chain, PORTED to JavaScript: source/Engine.cpp.
 *
 * One line of the picture becomes a line of composite luma (sync, blanking,
 * picture), is recorded as an FM carrier by the TAPE's format and played back
 * through the DECK's channel and demodulator, at 40.5 MHz:
 *
 *   record:  Y low-pass -> pre-emphasis -> white/dark clip -> FM (phase in
 *            turns, the folded Taylor cosine)
 *   tape:    head clog (Wallace, a 49-tap FIR) -> dropouts -> head-amplifier
 *            noise (the PCG hash into the Gaussian table)
 *   deck:    RF high-pass -> RF low-pass -> limiter and pulse count (a cubic
 *            B-spline at each Newton-refined crossing) -> Y low-pass ->
 *            de-emphasis -> the deck's map from Hz to volts
 *   monitor: 3-sample box at the deck's own delay, back-porch clamp, and
 *            (over the frame) the sync AGC
 *   chroma:  U, V zero-phase low-pass
 *
 * **The plugin runs eight lines in lock step** (one a SIMD lane, `[ sample ]
 * [ lane ]`) on up to eight threads. Every lane's arithmetic is its own --
 * no lane reads another, and a line's seed is its row -- so this port runs
 * ONE line at a time with the same arithmetic, and the page splits the frame's
 * lines across Web Workers. Which lines share a batch or a worker changes
 * nothing (`lbtest --threads` proves it for the plugin; check_port.sh runs the
 * port in more than one split).
 *
 * Every C++ float operation is one Math.fround, in the C++'s order; doubles
 * stay doubles. check_port.sh compiles the plugin's Engine.cpp and compares
 * the uploaded picture float for float.
 */

import {
  standardOf, formatOf, playbackMode, noiseSigma, clogTaps, GaussianTable, hash, lineSeed, roundAway, clamp,
  kSampleHz, kPixelHz, kPi, kRecordYOrder, kRfHighOrder, kRfLowOrder, kYLowOrder,
  kChromaHalfHz, kClogHalfTaps, kDropoutDepth, kDropoutEdgeUs, kGaussianBits, dropouts,
} from './model.js';
import { ButterworthLowPass, ButterworthHighPass, Shelf, runCascade } from './dsp.js';

const f32 = Math.fround;

/** Engine.h's constants: the warm-up and tail, the clamp's and the tip's windows, us. */
export const kWarmUs = 4.0;
export const kTailUs = 2.0;
export const kPorchFromSyncEndUs = 1.0;
export const kPorchBeforeActiveUs = 0.5;
export const kTipInsetUs = 0.8;

export const samples = (us) => roundAway(us * kSampleHz * 1e-6);

// The float constants the C++ folds at compile time.
const kTwoPi = f32(6.28318530717958647692);
const kInv24 = f32(1.0 / 24.0);
const kInv720 = f32(-1.0 / 720.0);
const kInv40320 = f32(1.0 / 40320.0);
const kInv3628800 = f32(-1.0 / 3628800.0);
const kThird = f32(1.0 / 3.0);
const kSixth = f32(1.0 / 6.0);
const kPiF = f32(3.14159265);
const kUMax = f32(0.99999994);
const kDropFloor = f32(1.0 - f32(kDropoutDepth));

/** cos( 2 pi p ) for p in [ 0, 1 ): folded to a quarter turn, then Taylor to the tenth power. */
export function cosTurns(p) {
  const x = f32(p - 0.5);
  const ax = Math.abs(x);
  const folded = ax > 0.25;
  const a = folded ? f32(0.5 - ax) : ax;
  const t = f32(kTwoPi * a);
  const t2 = f32(t * t);
  const c = f32(1.0 + f32(t2 * f32(-0.5 + f32(t2 * f32(kInv24 + f32(t2 * f32(kInv720 + f32(t2 * f32(kInv40320 + f32(t2 * kInv3628800))))))))));
  return folded ? c : -c;
}

/**
 * The composite luma of one line over its own samples [ from, to ), with
 * k = 0 its sync's leading edge, in volts, into dst[ at + k - from ]: sync,
 * blanking, and the picture linearly between pixel centres at activeStart +
 * 3 i + 1. `pix` is the RGBA rows array and `row` the line's offset into it
 * (in floats), or pix null for a line with no picture.
 */
function composite(pix, row, pixels, syncN, a0, S, Wh, from, to, dst, at) {
  const negS = -S;
  for (let k = from; k < to; k += 1) {
    let v = k < syncN ? negS : 0.0;
    if (pix !== null && k >= a0 && k < a0 + 3 * pixels) {
      const q = k - a0 - 1;
      const i0 = q >= 0 ? Math.trunc(q / 3) : -1;
      const t = f32(f32(q - 3 * i0) * kThird);
      const y0 = f32(Wh * clamp(pix[row + 4 * clamp(i0, 0, pixels - 1)], 0.0, 1.0));
      const y1 = f32(Wh * clamp(pix[row + 4 * clamp(i0 + 1, 0, pixels - 1)], 0.0, 1.0));
      v = f32(y0 + f32(t * f32(y1 - y0)));
    }
    dst[at + k - from] = v;
  }
}

/** Lane `lane` of y, linearly between samples (Engine.cpp's laneAt, one lane). */
function lineAt(y, total, at) {
  const fl = Math.floor(at);
  const i = clamp(fl, 0, total - 2);
  const t = f32(at - fl);
  const a = y[i];
  const b = y[i + 1];
  return f32(a + f32(t * f32(b - a)));
}

export class Engine {
  constructor() {
    this.chain = null;
    this.key = '';
    this.delay = 0.0;
    this.scratchTotal = 0;
  }

  /** Engine::configure: the filters for this standard, tape, deck and clog. */
  configure(s) {
    const clog = s.clogMetres;
    const key = `${s.standard}|${s.recording}|${s.deck}|${s.perturb}|${clog}`;
    if (key === this.key) return;
    this.key = key;

    const st = standardOf(s.standard);
    const rec = formatOf(s.recording);
    const mode = playbackMode(s.deck, s.recording);
    const m = formatOf(mode);
    const fs = kSampleHz;

    const chain = {};
    chain.recordY = ButterworthLowPass(kRecordYOrder, rec.recordYHz, fs);
    chain.preEmphasis = Shelf(rec.emphasisTau, rec.emphasisX, fs, false);
    chain.recordTipHz = rec.syncTipHz;
    chain.recordDevHz = rec.deviationHz;
    const S = st.syncVolts;
    const Wh = st.whiteVolts;
    chain.whiteClipV = -S + rec.whiteClip * (S + Wh);
    chain.darkClipV = -rec.darkClip * Wh;

    // The perturbations are test hooks, off in the shipped plugin and here.
    chain.rfHigh = ButterworthHighPass(kRfHighOrder, m.rfHighPassHz, fs);
    chain.rfLow = ButterworthLowPass(kRfLowOrder, m.rfLowPassHz, fs);
    chain.yLow = ButterworthLowPass(kYLowOrder, m.yLowPassHz, fs);
    chain.deEmphasis = Shelf(m.emphasisTau, m.emphasisX, fs, true);
    chain.deckTipHz = m.syncTipHz;
    chain.record = chain.recordY.then(chain.preEmphasis);
    chain.rf = chain.rfHigh.then(chain.rfLow);
    chain.playback = chain.yLow.then(chain.deEmphasis);
    chain.deckDevHz = m.deviationHz;

    chain.chroma = ButterworthLowPass(2, kChromaHalfHz, kPixelHz);

    const speed = st.writingSpeed();
    chain.clogTaps = clogTaps(clog, speed, kClogHalfTaps);

    // The deck's own luma delay, as its designers would have matched it.
    const own = m;
    const ownY = ButterworthLowPass(kRecordYOrder, own.recordYHz, fs);
    const ownPre = Shelf(own.emphasisTau, own.emphasisX, fs, false);
    const ownDe = Shelf(own.emphasisTau, own.emphasisX, fs, true);
    const ownHigh = ButterworthHighPass(kRfHighOrder, own.rfHighPassHz, fs);
    const ownLow = ButterworthLowPass(kRfLowOrder, own.rfLowPassHz, fs);
    const blankHz = own.syncTipHz + S / (S + Wh) * own.deviationHz;
    const w = 2.0 * kPi * blankHz / fs;
    this.delay = ownY.groupDelay(0.0) + ownPre.groupDelay(0.0) + ownHigh.groupDelay(w) + ownLow.groupDelay(w) - 0.5
      + chain.yLow.groupDelay(0.0) + ownDe.groupDelay(0.0);

    this.chain = chain;
  }

  /** Scratch for one line, at this standard's length. */
  scratch(total) {
    if (this.scratchTotal === total) return;
    this.scratchTotal = total;
    this.a = new Float32Array(total);
    this.b = new Float32Array(total);
    this.w0 = new Float32Array(total);
    this.w1 = new Float32Array(total);
    this.w2 = new Float32Array(total);
    this.w3 = new Float32Array(total);
    this.padded = new Float32Array(total + 2 * kClogHalfTaps);
    this.state = new Float32Array(16);
  }

  /**
   * One frame row through the whole luma chain: Engine::runBatch for its lane
   * and Engine::monitorLane. `pix` holds RGBA rows of the intake (Y' in
   * channel 0); `rowAt(r)` gives frame row r's offset into it in floats, or -1
   * for none. Writes the row's pixels (deck volts, before the clamp) into
   * outY[ outAt .. outAt + pixels ) and returns its porch and tip.
   */
  lumaLine(pix, rowAt, row, lines, pixels, s, drops, outY, outAt) {
    const chain = this.chain;
    const st = standardOf(s.standard);
    const L = st.lineSamples();
    const syncN = st.syncSamples();
    const a0 = st.activeStart();
    const warm = samples(kWarmUs);
    const total = warm + L + samples(kTailUs);
    const S = f32(st.syncVolts);
    const Wh = f32(st.whiteVolts);
    const fs = f32(kSampleHz);
    this.scratch(total);
    const a = this.a;
    const b = this.b;
    const state = this.state;

    //--- record: the composite luma (the line before in the same field, this
    //--- line, the start of the next line's sync).
    const here = row < lines ? rowAt(row) : -1;
    const before = (row >= 2 && row - 2 < lines) ? rowAt(row - 2) : -1;
    composite(before >= 0 ? pix : null, before, pixels, syncN, a0, S, Wh, L - warm, L, a, 0);
    composite(here >= 0 ? pix : null, here, pixels, syncN, a0, S, Wh, 0, L, a, warm);
    composite(null, 0, pixels, syncN, a0, S, Wh, 0, total - warm - L, a, warm + L);

    // Y low-pass, pre-emphasis, from the steady state of the first sample.
    state.fill(0);
    chain.record.steadyState(a[0], state);
    chain.record.run(a, 0, total, state);

    // The clips and the frequency, kept in `a`.
    const whiteClip = f32(chain.whiteClipV);
    const darkClip = f32(chain.darkClipV);
    const tip = f32(chain.recordTipHz);
    const perVolt = f32(f32(chain.recordDevHz) / f32(S + Wh));
    const perSample = f32(1.0 / fs);
    for (let i = 0; i < total; i += 1) {
      let v = a[i];
      v = v < darkClip ? darkClip : v;
      v = whiteClip < v ? whiteClip : v;
      a[i] = f32(tip + f32(f32(v + S) * perVolt));
    }
    const firstHz = a[0];

    // The phase, in turns, and the carrier.
    let phase = 0.0;
    for (let i = 0; i < total; i += 1) {
      let q = f32(phase + f32(a[i] * perSample));
      q = q >= 1.0 ? f32(q - 1.0) : q;
      phase = q;
      b[i] = cosTurns(q);
    }

    //--- tape: the clog's spacing loss, zero phase, the ends held.
    const h = chain.clogTaps;
    if (h.length > 0) {
      const M = kClogHalfTaps;
      const p = this.padded;
      for (let i = -M; i < total + M; i += 1) p[i + M] = b[clamp(i, 0, total - 1)];
      const hM = h[M];
      for (let i = 0; i < total; i += 1) {
        let acc = f32(hM * p[i + M]);
        for (let j = 0; j < M; j += 1) acc = f32(acc + f32(h[j] * f32(p[i + j] + p[i + 2 * M - j])));
        b[i] = acc;
      }
    }

    //--- dropouts on this line: the carrier dipped 30 dB, raised-cosine edges.
    const edge = Math.max(1, samples(kDropoutEdgeUs));
    const edgeF = f32(edge);
    for (const d of drops) {
      if (d.line !== row) continue;
      const s0 = warm + a0 + samples(d.us0);
      const s1 = warm + a0 + samples(d.us1);
      for (let i = Math.max(0, s0 - edge); i < Math.min(total, s1 + edge); i += 1) {
        let wgt = 1.0;
        if (i < s0) wgt = f32(0.5 - f32(0.5 * f32(Math.cos(f32(f32(kPiF * f32(i - (s0 - edge))) / edgeF)))));
        else if (i >= s1) wgt = f32(0.5 + f32(0.5 * f32(Math.cos(f32(f32(kPiF * f32(i - s1)) / edgeF)))));
        b[i] = f32(b[i] * f32(1.0 - f32(kDropFloor * wgt)));
      }
    }

    //--- the head amplifier's noise.
    if (s.noise) {
      const sigma = f32(noiseSigma(s.cnrDb));
      const gauss = GaussianTable();
      const shift = 32 - kGaussianBits;
      const seed = lineSeed(s.videoFrame, row);
      for (let i = 0; i < total; i += 1) b[i] = f32(b[i] + f32(sigma * gauss[hash((seed + i) >>> 0) >>> shift]));
    }

    //--- the deck: RF band, limiter, pulse count.
    state.fill(0);
    chain.rf.run(b, 0, total, state);

    // Each interval ( i - 1, i ]'s crossing: its offset u past sample i - 1
    // (or none), refined by two Newton steps on the cubic through the four
    // samples about it, and its four B-spline weights.
    const w0 = this.w0, w1 = this.w1, w2 = this.w2, w3 = this.w3;
    w0[0] = 0; w1[0] = 0; w2[0] = 0; w3[0] = 0;
    for (let i = 1; i < total; i += 1) {
      const x0 = b[i - 1];
      const x1 = b[i];
      const cross = (x0 < 0.0) !== (x1 < 0.0);
      if (!cross) { w0[i] = 0; w1[i] = 0; w2[i] = 0; w3[i] = 0; continue; }
      let u = f32(x0 / f32(x0 - x1));
      if (i >= 2 && i + 1 < total) {
        const xm = b[i - 2];
        const xp = b[i + 1];
        const c0 = x0;
        const c1 = f32(f32(f32(f32(-xm / 3.0) - f32(x0 / 2.0)) + x1) - f32(xp / 6.0));
        const c2 = f32(f32(f32(xm / 2.0) - x0) + f32(x1 / 2.0));
        const c3 = f32(f32(f32(f32(-xm / 6.0) + f32(x0 / 2.0)) - f32(x1 / 2.0)) + f32(xp / 6.0));
        for (let step = 0; step < 2; step += 1) {
          const p = f32(c0 + f32(u * f32(c1 + f32(u * f32(c2 + f32(u * c3))))));
          const dp = f32(c1 + f32(u * f32(f32(2.0 * c2) + f32(f32(u * 3.0) * c3))));
          u = dp !== 0.0 ? f32(u - f32(p / dp)) : u;
        }
        u = u < 0.0 ? 0.0 : u;
        u = kUMax < u ? kUMax : u;
      }
      const u2 = f32(u * u);
      const u3 = f32(u2 * u);
      const v = f32(1.0 - u);
      w0[i] = f32(f32(f32(v * v) * v) * kSixth);
      w1[i] = f32(f32(f32(f32(3.0 * u3) - f32(6.0 * u2)) + 4.0) * kSixth);
      w2[i] = f32(f32(f32(f32(f32(-3.0 * u3) + f32(3.0 * u2)) + f32(3.0 * u)) + 1.0) * kSixth);
      w3[i] = f32(u3 * kSixth);
    }
    // Sample m takes w0 from interval m + 2, w1 from m + 1, w2 from m, w3 from m - 1.
    const pulses = a;
    for (let mI = 0; mI < total; mI += 1) {
      const q0 = mI + 2 < total ? w0[mI + 2] : 0.0;
      const q1 = mI + 1 < total ? w1[mI + 1] : 0.0;
      const q2 = w2[mI];
      const q3 = mI >= 1 ? w3[mI - 1] : 0.0;
      pulses[mI] = f32(f32(f32(q0 + q1) + q2) + q3);
    }

    //--- the demodulator's low-pass and the de-emphasis, then the deck's map.
    state.fill(0);
    chain.playback.steadyState(f32(f32(2.0 * firstHz) / fs), state);
    chain.playback.run(pulses, 0, total, state);
    const y = pulses;
    const deckTip = f32(chain.deckTipHz);
    const voltsPerHz = f32(f32(S + Wh) / f32(chain.deckDevHz));
    const half = f32(fs * 0.5);
    const negS = -S;
    for (let i = 0; i < total; i += 1) y[i] = f32(negS + f32(f32(f32(y[i] * half) - deckTip) * voltsPerHz));

    //--- the monitor: the porch and the tip where the deck's delay puts them,
    //--- and each pixel the three samples about its centre.
    const D = roundAway(this.delay);
    const zero = warm;
    const p0 = zero + D + syncN + samples(kPorchFromSyncEndUs);
    const p1 = zero + D + a0 - samples(kPorchBeforeActiveUs);
    let porch = 0.0;
    for (let i = p0; i < p1; i += 1) porch += y[i];
    porch /= Math.max(1, p1 - p0);
    const t0 = zero + D + samples(kTipInsetUs);
    const t1 = zero + D + syncN - samples(kTipInsetUs);
    let tipV = 0.0;
    for (let i = t0; i < t1; i += 1) tipV += y[i];
    tipV /= Math.max(1, t1 - t0);

    for (let i = 0; i < pixels; i += 1) {
      const c = (zero + a0 + 3 * i + 1) + this.delay;
      outY[outAt + i] = f32(f32(f32(lineAt(y, total, c - 1.0) + lineAt(y, total, c)) + lineAt(y, total, c + 1.0)) * kThird);
    }
    return { porch, tip: tipV };
  }

  /**
   * Engine::chromaBatch for one row: U and V through the chroma low-pass
   * forward then backward, with the blanking's zero colour either side.
   */
  chromaLine(pix, rowOffset, pixels, outU, outV, outAt) {
    const pad = 32;
    const total = pixels + 2 * pad;
    if (!this.chromaScratch || this.chromaScratch.length !== total) this.chromaScratch = new Float32Array(total);
    const v = this.chromaScratch;
    const st = new Float32Array(2);
    for (let c = 1; c <= 2; c += 1) {
      v.fill(0);
      for (let i = 0; i < pixels; i += 1) v[i + pad] = pix[rowOffset + 4 * i + c];
      st[0] = 0; st[1] = 0;
      this.chain.chroma.run(v, 0, total, st);
      v.reverse();
      st[0] = 0; st[1] = 0;
      this.chain.chroma.run(v, 0, total, st);
      v.reverse();
      const out = c === 1 ? outU : outV;
      for (let i = 0; i < pixels; i += 1) out[outAt + i] = v[i + pad];
    }
  }
}

/**
 * The frame's dropouts, as Engine::Process draws them (the forced dropout is
 * a test hook and is off).
 */
export function frameDropouts(s, lines) {
  const st = standardOf(s.standard);
  return dropouts(s.videoFrame, s.dropoutsPerFrame, lines, st.active());
}

/**
 * Rows [ r0, r1 ) of a frame: what one Web Worker does, and what one or more
 * of the plugin's threads do for the same rows. `pix` holds the intake rows
 * [ base, base + rowsHeld ) (RGBA floats); the rows it needs are r0 - 2 .. r1.
 */
export function processRows(engine, pix, base, r0, r1, lines, pixels, s, drops) {
  engine.configure(s);
  const n = r1 - r0;
  const luma = new Float32Array(n * pixels);
  const u = new Float32Array(n * pixels);
  const v = new Float32Array(n * pixels);
  const porch = new Float64Array(n);
  const tip = new Float64Array(n);
  const rowAt = (r) => (r >= base ? (r - base) * pixels * 4 : -1);
  for (let row = r0; row < r1; row += 1) {
    const k = row - r0;
    const res = engine.lumaLine(pix, rowAt, row, lines, pixels, s, drops, luma, k * pixels);
    porch[k] = res.porch;
    tip[k] = res.tip;
    engine.chromaLine(pix, rowAt(row), pixels, u, v, k * pixels);
  }
  return { r0, r1, luma, u, v, porch, tip };
}

/**
 * The tail of Engine::Process: the clamp per line and the AGC over the frame,
 * summed in line order, into the uploaded ( Y', U, V, 1 ) picture.
 * `parts` are processRows results covering every row once.
 */
export function finishFrame(parts, lines, pixels, s) {
  const st = standardOf(s.standard);
  const porchOf = new Float64Array(lines);
  const tipOf = new Float64Array(lines);
  const out = new Float32Array(lines * pixels * 4);
  for (const part of parts) {
    for (let row = part.r0; row < part.r1; row += 1) {
      const k = row - part.r0;
      porchOf[row] = part.porch[k];
      tipOf[row] = part.tip[k];
      for (let i = 0; i < pixels; i += 1) {
        const o = (row * pixels + i) * 4;
        out[o + 1] = part.u[k * pixels + i];
        out[o + 2] = part.v[k * pixels + i];
      }
    }
  }
  let depth = 0.0;
  let porch = 0.0;
  for (let row = 0; row < lines; row += 1) {
    depth += porchOf[row] - tipOf[row];
    porch += porchOf[row];
  }
  depth /= Math.max(1, lines);
  porch /= Math.max(1, lines);
  const S = st.syncVolts;
  const gain = s.syncAgc ? S / Math.max(depth, 0.05 * S) : 1.0;
  const scale = f32(gain / st.whiteVolts);
  for (const part of parts) {
    for (let row = part.r0; row < part.r1; row += 1) {
      const k = row - part.r0;
      const black = f32(porchOf[row]);
      for (let i = 0; i < pixels; i += 1) {
        const o = (row * pixels + i) * 4;
        out[o] = f32(f32(part.luma[k * pixels + i] - black) * scale);
        out[o + 3] = 1.0;
      }
    }
  }
  return { lines: out, depth, gain, porch };
}

