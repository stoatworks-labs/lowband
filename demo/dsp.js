/**
 * Lowband's filters, PORTED to JavaScript: source/Dsp.cpp.
 *
 * Designed in double and RUN in float, as in the plugin: Butterworth through
 * the bilinear transform with the corner prewarped, the emphasis shelf through
 * the bilinear transform unwarped, every section Direct Form II transposed.
 * A C++ `float` operation is one `Math.fround` per operation, in the C++'s
 * order, so a run here rounds where the plugin's rounds (check_port.sh
 * compares the coefficients, the steady states and the chain's output).
 *
 * The group delay (only used for the deck's delay compensation) runs through
 * std::complex<double> in the plugin. libc++ hands complex * and / to the
 * compiler's _Complex arithmetic: the textbook product, and compiler-rt's
 * __divdc3 for the quotient, with its logb/scalbn scaling. Both are ported
 * below as they are written.
 */

const f32 = Math.fround;
const kPi = 3.14159265358979323846;

//--- std::complex<double>, as libc++ and compiler-rt compute it ------------

const view = new DataView(new ArrayBuffer(8));

/** logb for a finite, non-zero double: its unbiased exponent (subnormals included). */
function logb(x) {
  if (x === 0) return -Infinity;
  if (!Number.isFinite(x)) return Infinity;
  view.setFloat64(0, x);
  const hi = view.getUint32(0);
  const e = (hi >>> 20) & 0x7ff;
  if (e !== 0) return e - 1023;
  // Subnormal: normalise.
  return logb(x * 2 ** 54) - 54;
}

/** scalbn by a power of two, exact for the ranges the filters reach. */
const scalbn = (x, n) => x * 2 ** n;

const cmul = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];

/** A finite double as an exact BigInt mantissa and a power of two. */
function exact(x) {
  view.setFloat64(0, x);
  const hi = view.getUint32(0);
  const lo = view.getUint32(4);
  const e = (hi >>> 20) & 0x7ff;
  let m = (BigInt(hi & 0xfffff) << 32n) | BigInt(lo);
  if (e !== 0) m |= 1n << 52n;
  return { m: hi >>> 31 ? -m : m, e: (e === 0 ? 1 : e) - 1075 };
}

/**
 * fma( a, b, c ): a * b + c with ONE rounding (to nearest, ties to even),
 * exactly, through BigInt. JavaScript has no fused multiply-add, and the
 * platform's __divdc3 uses three (below). Normal results only, which is all
 * the group delays reach.
 */
export function fma(a, b, c) {
  const p = exact(a);
  const q = exact(b);
  const r = exact(c);
  let m = p.m * q.m;
  let e = p.e + q.e;
  if (r.m !== 0n) {
    if (r.e < e) { m <<= BigInt(e - r.e); e = r.e; m += r.m; } else m += r.m << BigInt(r.e - e);
  }
  if (m === 0n) return a * b + c;
  const negative = m < 0n;
  let mag = negative ? -m : m;
  const bits = mag.toString(2).length;
  if (bits > 53) {
    const shift = BigInt(bits - 53);
    const rest = mag & ((1n << shift) - 1n);
    const halfway = 1n << (shift - 1n);
    mag >>= shift;
    e += bits - 53;
    if (rest > halfway || (rest === halfway && (mag & 1n) === 1n)) mag += 1n;
  }
  let value = Number(mag);
  // Apply 2^e in steps, so no intermediate power of two under- or overflows.
  while (e > 0) { const s = Math.min(e, 1000); value *= 2 ** s; e -= s; }
  while (e < 0) { const s = Math.min(-e, 1000); value *= 2 ** -s; e += s; }
  return negative ? -value : value;
}

/**
 * The platform's __divdc3 (compiler-rt, linked into the plugin by clang), for
 * finite operands: logb/scalbn scaling, then -- as the arm64 build of it does
 * (`fmadd` in its disassembly, whatever flags the plugin is built with) --
 * the denominator and both numerators each with one fused multiply-add.
 */
function cdiv(z, w) {
  const a = z[0];
  const b = z[1];
  let c = w[0];
  let d = w[1];
  let ilogbw = 0;
  const logbw = logb(Math.max(Math.abs(c), Math.abs(d)));
  if (Number.isFinite(logbw)) {
    ilogbw = logbw | 0;
    c = scalbn(c, -ilogbw);
    d = scalbn(d, -ilogbw);
  }
  const denom = fma(c, c, d * d);
  return [scalbn(fma(a, c, b * d) / denom, -ilogbw), scalbn(fma(b, c, -(d * a)) / denom, -ilogbw)];
}

const polar1 = (theta) => [1.0 * Math.cos(theta), 1.0 * Math.sin(theta)];
/** double * complex: libc++'s `t *= x`. */
const scale = (x, z) => [z[0] * x, z[1] * x];
/** double + complex: libc++'s `t += x`. */
const addReal = (x, z) => [z[0] + x, z[1]];
const cadd = (a, b) => [a[0] + b[0], a[1] + b[1]];

//---------------------------------------------------------------------------

export class Biquad {
  constructor(b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0) {
    this.b0 = b0; this.b1 = b1; this.b2 = b2; this.a1 = a1; this.a2 = a2;
  }

  /** The group delay at omega, in samples: Re{ sum k b_k z^-k / sum b_k z^-k } - the same for a. */
  groupDelay(omega) {
    const z1 = polar1(-omega);
    const z2 = cmul(z1, z1);
    const B = cadd(addReal(this.b0, scale(this.b1, z1)), scale(this.b2, z2));
    const Br = cadd(scale(this.b1, z1), scale(2.0 * this.b2, z2));
    const A = cadd(addReal(1.0, scale(this.a1, z1)), scale(this.a2, z2));
    const Ar = cadd(scale(this.a1, z1), scale(2.0 * this.a2, z2));
    return cdiv(Br, B)[0] - cdiv(Ar, A)[0];
  }
}

export class Cascade {
  constructor(sections = []) {
    this.sections = sections;
    this.coeffs = new Float32Array(0);
    this.finalise();
  }

  /** The float copies, five a section: b0 b1 b2 a1 a2. */
  finalise() {
    const c = new Float32Array(5 * this.sections.length);
    this.sections.forEach((s, i) => {
      c[5 * i] = s.b0; c[5 * i + 1] = s.b1; c[5 * i + 2] = s.b2; c[5 * i + 3] = s.a1; c[5 * i + 4] = s.a2;
    });
    this.coeffs = c;
  }

  groupDelay(omega) {
    let t = 0.0;
    for (const s of this.sections) t += s.groupDelay(omega);
    return t;
  }

  stateSize() { return 2 * this.sections.length; }

  /**
   * The state that gives a constant output for a constant input x: in double
   * from the float coefficients, stored as floats ([ section ][ 2 ]).
   */
  steadyState(x, state) {
    const c = this.coeffs;
    for (let i = 0; i < this.sections.length; i += 1) {
      const c0 = c[5 * i], c1 = c[5 * i + 1], c2 = c[5 * i + 2], c3 = c[5 * i + 3], c4 = c[5 * i + 4];
      const g = (c0 + c1 + c2) / (1.0 + c3 + c4);
      const y = g * x;
      const s2 = c2 * x - c4 * y;
      const s1 = c1 * x - c3 * y + s2;
      state[2 * i] = s1;
      state[2 * i + 1] = s2;
      x = f32(y);
    }
  }

  /**
   * Run the cascade over data[ from .. from + total ) in place, from `state`
   * ([ section ][ 2 ]), leaving the final state there. Each lane of the
   * plugin's RunLanes, one line at a time: the same arithmetic, in float.
   */
  run(data, from, total, state) {
    runCascade(this.coeffs, this.sections.length, data, from, total, state);
  }

  /** The cascade followed by another, as one. */
  then(next) {
    return new Cascade([...this.sections, ...next.sections]);
  }
}

/**
 * The cascade's inner loop, Step's arithmetic per sample per section:
 *   y  = b0 x + s1;  s1 = b1 x - a1 y + s2;  s2 = b2 x - a2 y
 * every operation rounded to float. Specialised for the section counts the
 * chain uses (1, 3, 5) so the state stays in locals, as the C++'s template does.
 */
export function runCascade(c, n, data, from, total, state) {
  const end = from + total;
  if (n === 1) {
    const b0 = c[0], b1 = c[1], b2 = c[2], a1 = c[3], a2 = c[4];
    let s1 = state[0], s2 = state[1];
    for (let i = from; i < end; i += 1) {
      const x = data[i];
      const y = f32(f32(b0 * x) + s1);
      s1 = f32(f32(f32(b1 * x) - f32(a1 * y)) + s2);
      s2 = f32(f32(b2 * x) - f32(a2 * y));
      data[i] = y;
    }
    state[0] = s1; state[1] = s2;
    return;
  }
  if (n === 3) {
    const p0 = c[0], p1 = c[1], p2 = c[2], p3 = c[3], p4 = c[4];
    const q0 = c[5], q1 = c[6], q2 = c[7], q3 = c[8], q4 = c[9];
    const r0 = c[10], r1 = c[11], r2 = c[12], r3 = c[13], r4 = c[14];
    let ps1 = state[0], ps2 = state[1], qs1 = state[2], qs2 = state[3], rs1 = state[4], rs2 = state[5];
    for (let i = from; i < end; i += 1) {
      let x = data[i];
      let y = f32(f32(p0 * x) + ps1);
      ps1 = f32(f32(f32(p1 * x) - f32(p3 * y)) + ps2);
      ps2 = f32(f32(p2 * x) - f32(p4 * y));
      x = y;
      y = f32(f32(q0 * x) + qs1);
      qs1 = f32(f32(f32(q1 * x) - f32(q3 * y)) + qs2);
      qs2 = f32(f32(q2 * x) - f32(q4 * y));
      x = y;
      y = f32(f32(r0 * x) + rs1);
      rs1 = f32(f32(f32(r1 * x) - f32(r3 * y)) + rs2);
      rs2 = f32(f32(r2 * x) - f32(r4 * y));
      data[i] = y;
    }
    state[0] = ps1; state[1] = ps2; state[2] = qs1; state[3] = qs2; state[4] = rs1; state[5] = rs2;
    return;
  }
  if (n === 5) {
    const p0 = c[0], p1 = c[1], p2 = c[2], p3 = c[3], p4 = c[4];
    const q0 = c[5], q1 = c[6], q2 = c[7], q3 = c[8], q4 = c[9];
    const r0 = c[10], r1 = c[11], r2 = c[12], r3 = c[13], r4 = c[14];
    const s0 = c[15], s1 = c[16], s2 = c[17], s3 = c[18], s4 = c[19];
    const t0 = c[20], t1 = c[21], t2 = c[22], t3 = c[23], t4 = c[24];
    let ps1 = state[0], ps2 = state[1], qs1 = state[2], qs2 = state[3], rs1 = state[4], rs2 = state[5];
    let ss1 = state[6], ss2 = state[7], ts1 = state[8], ts2 = state[9];
    for (let i = from; i < end; i += 1) {
      let x = data[i];
      let y = f32(f32(p0 * x) + ps1);
      ps1 = f32(f32(f32(p1 * x) - f32(p3 * y)) + ps2);
      ps2 = f32(f32(p2 * x) - f32(p4 * y));
      x = y;
      y = f32(f32(q0 * x) + qs1);
      qs1 = f32(f32(f32(q1 * x) - f32(q3 * y)) + qs2);
      qs2 = f32(f32(q2 * x) - f32(q4 * y));
      x = y;
      y = f32(f32(r0 * x) + rs1);
      rs1 = f32(f32(f32(r1 * x) - f32(r3 * y)) + rs2);
      rs2 = f32(f32(r2 * x) - f32(r4 * y));
      x = y;
      y = f32(f32(s0 * x) + ss1);
      ss1 = f32(f32(f32(s1 * x) - f32(s3 * y)) + ss2);
      ss2 = f32(f32(s2 * x) - f32(s4 * y));
      x = y;
      y = f32(f32(t0 * x) + ts1);
      ts1 = f32(f32(f32(t1 * x) - f32(t3 * y)) + ts2);
      ts2 = f32(f32(t2 * x) - f32(t4 * y));
      data[i] = y;
    }
    state[0] = ps1; state[1] = ps2; state[2] = qs1; state[3] = qs2; state[4] = rs1; state[5] = rs2;
    state[6] = ss1; state[7] = ss2; state[8] = ts1; state[9] = ts2;
    return;
  }
  // Any other count: section by section, the same arithmetic.
  for (let i = from; i < end; i += 1) {
    let x = data[i];
    for (let j = 0; j < n; j += 1) {
      const k = 5 * j;
      const y = f32(f32(c[k] * x) + state[2 * j]);
      state[2 * j] = f32(f32(f32(c[k + 1] * x) - f32(c[k + 3] * y)) + state[2 * j + 1]);
      state[2 * j + 1] = f32(f32(c[k + 2] * x) - f32(c[k + 4] * y));
      x = y;
    }
    data[i] = x;
  }
}

/** Q of the k-th pole pair of an order-N Butterworth. */
const butterworthQ = (order, k) => 1.0 / (2.0 * Math.sin((2.0 * k + 1.0) * kPi / (2.0 * order)));

export function ButterworthLowPass(order, fc, fs) {
  const K = Math.tan(kPi * fc / fs);
  const sections = [];
  for (let k = 0; k < Math.trunc(order / 2); k += 1) {
    const Q = butterworthQ(order, k);
    const norm = 1.0 / (1.0 + K / Q + K * K);
    const b0 = K * K * norm;
    sections.push(new Biquad(b0, 2.0 * b0, b0, 2.0 * (K * K - 1.0) * norm, (1.0 - K / Q + K * K) * norm));
  }
  return new Cascade(sections);
}

export function ButterworthHighPass(order, fc, fs) {
  const K = Math.tan(kPi * fc / fs);
  const sections = [];
  for (let k = 0; k < Math.trunc(order / 2); k += 1) {
    const Q = butterworthQ(order, k);
    const norm = 1.0 / (1.0 + K / Q + K * K);
    sections.push(new Biquad(norm, -2.0 * norm, norm, 2.0 * (K * K - 1.0) * norm, (1.0 - K / Q + K * K) * norm));
  }
  return new Cascade(sections);
}

/** The emphasis shelf ( 1 + s tau ) / ( 1 + s tau / x ); inverse is the de-emphasis. */
export function Shelf(tau, x, fs, inverse) {
  const c = 2.0 * fs;
  let n0 = 1.0 + c * tau;
  let n1 = 1.0 - c * tau;
  let d0 = 1.0 + c * tau / x;
  let d1 = 1.0 - c * tau / x;
  if (inverse) {
    [n0, d0] = [d0, n0];
    [n1, d1] = [d1, n1];
  }
  return new Cascade([new Biquad(n0 / d0, n1 / d0, 0.0, d1 / d0, 0.0)]);
}
