/**
 * The engine, off the page's main thread.
 *
 * Not the plugin's arrangement: the plugin runs eight lines in lock step on
 * up to eight std::threads inside ProcessOpenGL, and the frame waits for it
 * (2.6 ms at the defaults on eight, 14 ms on one, in C++). The port is the
 * same arithmetic in JavaScript, about a quarter of a second of one core a
 * frame, so the page splits each frame's lines across several of these
 * workers and shows the last frame they finished (plugin.js says so under the
 * picture).
 *
 * The maths is demo/engine.js, which check_port.sh holds to the plugin's
 * Engine.cpp. Lines are independent -- each starts from steady state after a
 * warm-up from the line two rows up, and its noise is seeded by its row -- so
 * how the lines are split does not change a float.
 */
import { Engine, processRows } from './engine.js';

const engine = new Engine();

onmessage = (event) => {
  const { id, settings, drops, P, N, r0, r1, base, data } = event.data;
  const t0 = performance.now();
  const part = processRows(engine, new Float32Array(data), base, r0, r1, N, P, settings, drops);
  const ms = performance.now() - t0;
  postMessage(
    { id, r0, r1, luma: part.luma, u: part.u, v: part.v, porch: part.porch, tip: part.tip, ms },
    [part.luma.buffer, part.u.buffer, part.v.buffer, part.porch.buffer, part.tip.buffer],
  );
};
