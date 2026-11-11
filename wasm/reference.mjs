// The reference set (data/reference.json) through master.wasm: makes each
// case's input with rf_render, runs it through the exported functions the
// browser calls (lm_*, ms_*, tp_*, pi_*, rh_*, mt_*, sd_*, pr_*, mu_*) and checks every
// expected result within its tolerance, as tests/ReferenceTests.cpp does
// natively. Fails when a result is off, when an input is not the one the file
// names (its hash), when the file has a result this run did not produce, or
// when this run produced one the file does not have. Cases marked "native"
// (processors the browser build has no functions for) are skipped, and counted.
//
//   node wasm/reference.mjs [master.wasm] [reference.json]
//
// Plain JavaScript for Node, no dependencies. The defaults are this
// repository's wasm/master.wasm and data/reference.json.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2] ?? path.join(here, 'master.wasm');
const filePath = process.argv[3] ?? path.join(here, '..', 'data', 'reference.json');

// master.wasm is built standalone: the few system calls it imports are never used, so each is a stub
const stub = () => 0;
const imports = new Proxy({}, { get: () => new Proxy({}, { get: () => stub }) });
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), imports);
const X = instance.exports;
if (X._initialize) X._initialize();

const f32 = (p, n) => new Float32Array(X.memory.buffer, p, n);
const f64 = (p, n) => new Float64Array(X.memory.buffer, p, n);
const i32 = (p, n) => new Int32Array(X.memory.buffer, p, n);
const text = (s) => { const b = Buffer.from(s + '\0'); const p = X.malloc(b.length); new Uint8Array(X.memory.buffer, p, b.length).set(b); return p; };
const cstr = (p) => { const u = new Uint8Array(X.memory.buffer); let e = p; while (u[e]) e++; return Buffer.from(u.subarray(p, e)).toString(); };

// ---- inputs (the fields in dsp/ReferenceSignals.h's order)

const FIELDS = ['type', 'from', 'to', 'l', 'r', 'hz', 'mult', 'phase', 'vibHz', 'vibDepth', 'attack', 'decay', 'every', 'seed', 'lowpass', 'highpass'];
const TYPES = { tone: 0, noise: 1, samples: 2, clip: 3 };

function makeInput(input) {
  const rate = input.rate ?? 48000;
  const frames = Math.round((input.seconds ?? 1) * rate);
  const channels = (input.channels ?? 1) > 1 ? 2 : 1;
  const parts = [];
  for (const p of input.parts) {
    for (const name of FIELDS) {
      const fallback = name === 'to' ? -1 : name === 'mult' || name === 'seed' ? 1 : 0;
      parts.push(name === 'type' ? TYPES[p.type] ?? 0 : p[name] ?? fallback);
    }
    const values = p.values ?? [];
    parts.push(values.length, ...values);
  }
  const pp = X.malloc(parts.length * 8 + 8);
  f64(pp, parts.length).set(parts);
  const lp = X.malloc(frames * 4 + 4), rp = X.malloc(frames * 4 + 4);
  X.rf_render(rate, frames, channels, pp, parts.length, lp, rp);
  const l = f32(lp, frames).slice(), r = channels > 1 ? f32(rp, frames).slice() : null;
  X.free(pp); X.free(lp); X.free(rp);
  return { rate, channels, l, r };
}

// FNV-1a (32-bit) over the samples' bytes, first channel then second, as vs::reference::hash
function hash(sig) {
  let h = 0x811c9dc5;
  for (const x of [sig.l, sig.r]) {
    if (!x) continue;
    const b = new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
    for (let i = 0; i < b.length; i++) { h ^= b[i]; h = Math.imul(h, 16777619) >>> 0; }
  }
  return h.toString(16).padStart(8, '0');
}

// ---- running a case

function blocks(n, block, feed) {
  for (let s = 0; s < n; s += block) feed(s, Math.min(block, n - s));
}

function fingerprint(out, name, x) {
  out.push([name + '.length', x.length]);
  if (x.length === 0) return;
  let sum = 0, peak = 0;
  for (let i = 0; i < x.length; i++) { const a = Math.abs(x[i]); sum += a; if (a > peak) peak = a; }
  out.push([name + '.sum', sum], [name + '.peak', peak]);
}

function slice(c, sig) {
  const from = Math.min(Math.round((c.from ?? 0) * sig.rate), sig.l.length);
  const n = c.length ?? sig.l.length;
  return sig.l.subarray(from, Math.min(sig.l.length, from + n));
}

function withBuffer(x, fn) {
  const p = X.malloc(x.length * 4 + 4);
  f32(p, x.length).set(x);
  try { return fn(p); } finally { X.free(p); }
}

const lenP = X.malloc(4);
function takeSound(p) {
  const n = i32(lenP, 1)[0];
  const x = f32(p, n).slice();
  X.free(p);
  return x;
}

// ---- helpers for the newer cases (as tests/ReferenceTests.cpp's)

// Copies channels into malloc'd buffers; returns their pointers and a pointer to the array of them
function alloc(channels) {
  const ptrs = channels.map((x) => { const p = X.malloc(x.length * 4 + 4); f32(p, x.length).set(x); return p; });
  const table = X.malloc(4 * Math.max(1, ptrs.length));
  i32(table, ptrs.length).set(ptrs);
  return { ptrs, table, read: (n) => ptrs.map((p) => f32(p, n).slice()), free: () => { ptrs.forEach((p) => X.free(p)); X.free(table); } };
}

const channelsOf = (sig) => (sig.channels > 1 ? [sig.l, sig.r] : [sig.l]);

function fingerprintAudio(out, name, ch, fs) {
  const p = name ? name + '.' : '';
  const len = ch.length ? ch[0].length : 0;
  out.push([p + 'length', len]);
  if (len === 0) return;
  let sum = 0, peak = 0;
  for (const c of ch) for (let i = 0; i < c.length; i++) { const a = Math.abs(c[i]); sum += a; if (a > peak) peak = a; }
  out.push([p + 'sum', sum], [p + 'peak', peak]);
  const m = X.lm_create(fs, 4096);
  blocks(len, 4096, (s, k) => {
    f32(X.lm_left(m), k).set(ch[0].subarray(s, s + k));
    if (ch.length > 1) f32(X.lm_right(m), k).set(ch[1].subarray(s, s + k));
    X.lm_process(m, k, ch.length > 1 ? 1 : 0);
  });
  out.push([p + 'lufs', X.lm_integrated(m)]);
  X.lm_destroy(m);
}

function arrayStats(out, name, x, picks = []) {
  let sum = 0, best = -1e300, at = -1;
  for (let i = 0; i < x.length; i++) {
    const v = x[i];
    if (!Number.isFinite(v)) continue;
    sum += v;
    if (v > best) { best = v; at = i; }
  }
  out.push([name + '.count', x.length], [name + '.sum', sum], [name + '.max', best], [name + '.maxAt', at]);
  for (const b of picks) if (Math.trunc(b) < x.length) out.push([`${name}.at${Math.trunc(b)}`, x[Math.trunc(b)]]);
}

const ANALYSIS = ['empty', 'duration', 'samplePeakDb', 'samplePeakAt', 'peakLDb', 'peakRDb', 'dcL', 'dcR', 'oversFloat', 'corr',
  'monoLossDb', 'sideToMidDb', 'identical', 'maxDiff', 'silentL', 'silentR', 'silent', 'lufs', 'shortTermMax',
  'shortTermMaxAt', 'lra', 'balanceDb', 'truePeakDb', 'truePeakAt', 'tpOverCount', 'tpOverZeroCount', 'clipRuns',
  'clipEvents', 'clipLongest', 'clipHasLevel', 'clipLevelDb', 'lowCorr', 'lowMonoLossDb', 'lowShareDb', 'leadSec',
  'tailSec', 'firstSampleDb', 'lastSampleDb', 'headRmsDb', 'endRmsDb', 'endDecayDb', 'hasSpectrum', 'cliffHz',
  'cliffDrop', 'hasPadded16', 'padded16', 'dcLDb', 'dcRDb'];

// text written by a mu_* function into a buffer
const textBuf = X.malloc(256), intBuf = X.malloc(64);
const mtext = (fn) => { fn(textBuf, 256); return cstr(textBuf); };
const noteText = (letter, acc) => mtext((b, cap) => X.mu_note_text(letter, acc, b, cap));
const spelledAt = (p) => { const v = i32(p, 2); return noteText(v[0], v[1]); };

// a list of ints at p
const intList = (p, n) => Array.from(i32(p, n));
const ints = (a) => { const p = X.malloc(4 * Math.max(1, a.length)); i32(p, a.length).set(a); return p; };

// A DSD stream from a reference input, as tests/ReferenceTests.cpp's dsdBits: kind "sine", a
// second-order one-bit modulator in integers; kind "sign", each sample's sign
function dsdBits(x, kind) {
  const out = new Uint8Array(Math.trunc(x.length / 8));
  const one = 2 ** 24;
  let i1 = 0, i2 = 0, y = 0;
  for (let k = 0; k < out.length * 8; k++) {
    let bit;
    if (kind === 'sine') {
      const xi = Math.floor(x[k] * one);
      i1 += xi - y;
      i2 += i1 - y;
      bit = i2 >= 0;
      y = bit ? one : -one;
    } else bit = x[k] >= 0;
    if (bit) out[k >> 3] |= 0x80 >> (k & 7);
  }
  return out;
}

function runMore(c, sig, out) {
  const fs = sig ? sig.rate : (c.rate ?? 48000);
  const block = c.block ?? 4096;
  const n = sig ? sig.l.length : 0;
  const stereo = sig !== null && sig.channels > 1;
  const nums = (k) => c[k] ?? [];

  switch (c.op) {
    // ---- measurement
    case 'file-analysis': {
      const a = X.ms_an_create(fs, sig.channels, n, c.bitDepth ?? 0, c.isFloat ?? 0, block);
      do {
        blocks(n, block, (s, k) => {
          f32(X.ms_an_left(a), k).set(sig.l.subarray(s, s + k));
          f32(X.ms_an_right(a), k).set((stereo ? sig.r : sig.l).subarray(s, s + k));
          X.ms_an_process(a, k);
        });
      } while (X.ms_an_next_pass(a));
      X.ms_an_finish(a);
      ANALYSIS.forEach((name, k) => out.push([name, X.ms_an_value(a, k)]));
      for (let b = 0; b < 5; b++) ['corr', 'monoLossDb', 'shareDb'].forEach((f, k) => out.push([`band${b}.${f}`, X.ms_an_value(a, 100 + 3 * b + k)]));
      ['timeline', 'tpOvers', 'tpOverZero', 'clipTimes', 'lowOut'].forEach((name, l) => {
        const size = X.ms_an_list_size(a, l);
        out.push([name + '.count', size]);
        const v = f64(X.ms_an_list(a, l), size);
        for (let i = 0; i < size; i++) out.push([`${name}.${i}`, v[i]]);
      });
      X.ms_an_destroy(a);
      return true;
    }
    case 'spectrum': {
      let o;
      if (c.webAudio) o = [c.size ?? 2048, c.size ?? 2048, 2, c.size ?? 2048, 0.5, 0.15, 0.04, c.smoothing ?? 0.8];
      else o = [c.size ?? 4096, c.history ?? 8192, c.window ?? 0, c.scale ?? 1024, c.attack ?? 0.5, c.release ?? 0.15, c.powerSmoothing ?? 0.04, c.smoothing ?? 0];
      const a = X.ms_sp_create(...o, block);
      const update = c.mode === 'update';
      blocks(n, block, (s, k) => { f32(X.ms_sp_input(a), k).set(sig.l.subarray(s, s + k)); X.ms_sp_push(a, k); if (update) X.ms_sp_update(a); else X.ms_sp_analyse(a); });
      const bins = X.ms_sp_bins(a), picks = nums('bins');
      arrayStats(out, 'magnitude', f32(X.ms_sp_magnitude(a), bins), picks);
      if (update) {
        arrayStats(out, 'levels', f32(X.ms_sp_levels(a), bins), picks);
        arrayStats(out, 'power', f32(X.ms_sp_power(a), bins), picks);
        for (const hz of nums('levelAt')) out.push([`levelAt${Math.trunc(hz)}`, X.ms_sp_level_at(a, hz, fs, 3, 1000)]);
      } else arrayStats(out, 'decibels', f32(X.ms_sp_decibels(a), bins), picks);
      X.ms_sp_destroy(a);
      return true;
    }
    case 'welch': {
      const buf = alloc(channelsOf(sig));
      const size = c.size ?? 4096, res = X.malloc(8 * (size / 2 + 1));
      const r = stereo ? buf.ptrs[1] : 0;
      const starts = nums('starts');
      if (starts.length === 0) X.ms_welch(buf.ptrs[0], r, n, fs, size, c.maxSegments ?? 64, res);
      else {
        const sp = X.malloc(8 * starts.length);
        f64(sp, starts.length).set(starts);
        X.ms_welch_at(buf.ptrs[0], r, fs, size, sp, starts.length, res);
        X.free(sp);
      }
      arrayStats(out, 'power', f64(res, size / 2 + 1), nums('bins'));
      X.free(res); buf.free();
      return true;
    }
    case 'tonal-balance': {
      const buf = alloc([sig.l]), res = X.malloc(8 * 24);
      X.ms_tonal_balance(buf.ptrs[0], n, fs, res);
      const v = f64(res, 24);
      for (let b = 0; b < 10; b++) out.push([`level${b}`, v[b]], [`rel${b}`, v[10 + b]]);
      out.push(['lows', v[20]], ['mud', v[21]], ['harsh', v[22]], ['top', v[23]]);
      X.free(res); buf.free();
      return true;
    }
    case 'waveform': {
      const columns = c.columns ?? 16;
      const w = X.ms_wf_create(fs, n, columns, c.stride ?? 1, c.rms ?? 0, c.bands ?? 0, block);
      blocks(n, block, (s, k) => {
        f32(X.ms_wf_left(w), k).set(sig.l.subarray(s, s + k));
        if (stereo) f32(X.ms_wf_right(w), k).set(sig.r.subarray(s, s + k));
        X.ms_wf_process(w, k, stereo ? 1 : 0);
      });
      const picks = [0, Math.trunc(columns / 2), columns - 1];
      const stat = (name, which, len) => arrayStats(out, name, f32(X.ms_wf_get(w, which), len).slice(), picks);
      stat('peak', 0, columns); stat('peakL', 1, columns); stat('peakR', 2, columns);
      if (c.rms) stat('rms', 3, columns);
      if (c.bands) stat('bands', 4, 3 * columns);
      X.ms_wf_destroy(w);
      return true;
    }
    case 'block-peaks': {
      const b = X.ms_wf_blocks_create(c.blockSize ?? 100, block);
      blocks(n, block, (s, k) => { f32(X.ms_wf_blocks_input(b), k).set(sig.l.subarray(s, s + k)); X.ms_wf_blocks_process(b, k); });
      const count = X.ms_wf_blocks_count(b);
      const peaksP = X.ms_wf_blocks_peaks(b);
      arrayStats(out, 'peaks', f64(peaksP, count).slice());
      const points = c.points ?? 200, shapeP = X.malloc(points + 4);
      const got = X.ms_wf_shape(peaksP, count, points, shapeP);
      arrayStats(out, 'shape', new Uint8Array(X.memory.buffer, shapeP, got).slice(), [0, Math.trunc(got / 2), Math.max(0, got - 1)]);
      X.free(shapeP); X.ms_wf_blocks_destroy(b);
      const width = c.width ?? 64, buf = alloc(channelsOf(sig)), px = X.malloc(4 * width);
      X.ms_wf_pixel_peaks(buf.ptrs[0], stereo ? buf.ptrs[1] : 0, n, width, px);
      arrayStats(out, 'pixels', f32(px, width).slice());
      X.free(px); buf.free();
      return true;
    }
    case 'clip-runs': {
      const buf = alloc(channelsOf(sig)), res = X.malloc(8 * 64);
      const spots = X.ms_clip_runs(buf.ptrs[0], stereo ? buf.ptrs[1] : 0, n, fs, c.bitDepth ?? 0, c.isFloat ?? 0, res, 61);
      const v = f64(res, 64);
      out.push(['runs', v[0]], ['longest', v[1]], ['spots', v[2]]);
      for (let k = 0; k < spots; k++) out.push([`spot${k}`, v[3 + k]]);
      X.free(res); buf.free();
      return true;
    }

    // ---- processing
    case 'resample': {
      const to = c.to ?? 48000, ch = sig.channels;
      const r = X.pr_resampler_create(fs, to, ch, block);
      const res = Array.from({ length: ch }, () => []);
      const take = (got) => { for (let ci = 0; ci < ch; ci++) res[ci].push(f32(X.pr_resampler_output(r, ci), got).slice()); };
      const src = channelsOf(sig);
      blocks(n, block, (s, k) => { src.forEach((x, ci) => f32(X.pr_resampler_input(r, ci), k).set(x.subarray(s, s + k))); take(X.pr_resampler_push(r, k)); });
      take(X.pr_resampler_flush(r));
      X.pr_resampler_destroy(r);
      out.push(['resampledLength', X.pr_resampled_length(n, fs, to)]);
      fingerprintAudio(out, '', res.map((parts) => { const all = new Float32Array(parts.reduce((a, p) => a + p.length, 0)); let o = 0; for (const p of parts) { all.set(p, o); o += p.length; } return all; }), to);
      return true;
    }
    case 'fracdelay': {
      const delays = nums('delays'), ch = channelsOf(sig).map((x) => x.slice());
      const dp = X.malloc(8 * delays.length);
      f64(dp, delays.length).set(delays);
      const d = X.pr_fracdelay_create(dp, ch.length, block);
      ch.forEach((x, ci) => blocks(n, block, (s, k) => {
        const p = X.pr_fracdelay_buffer(d, ci);
        f32(p, k).set(x.subarray(s, s + k));
        X.pr_fracdelay_process(d, ci, k);
        x.set(f32(p, k), s);
      }));
      out.push(['latency', X.pr_fracdelay_latency(d)]);
      X.pr_fracdelay_destroy(d); X.free(dp);
      fingerprintAudio(out, '', ch, fs);
      return true;
    }
    case 'decimate': {
      const factor = c.factor ?? 2, d = X.pr_decimator_create(fs, factor, block), res = [];
      blocks(n, block, (s, k) => { f32(X.pr_decimator_input(d), k).set(sig.l.subarray(s, s + k)); const got = X.pr_decimator_push(d, k); res.push(...f32(X.pr_decimator_output(d), got)); });
      X.pr_decimator_destroy(d);
      fingerprintAudio(out, '', [Float32Array.from(res)], fs / factor);
      return true;
    }
    case 'quantize': {
      const bits = c.bits ?? 16, src = channelsOf(sig);
      const q = X.pr_quantizer_create(bits, c.mode ?? 0, c.seed ?? 1, src.length, block);
      const ints = src.map(() => new Int32Array(n));
      blocks(n, block, (s, k) => {
        src.forEach((x, ci) => f32(X.pr_quantizer_input(q, ci), k).set(x.subarray(s, s + k)));
        X.pr_quantizer_process(q, k);
        ints.forEach((o, ci) => o.set(i32(X.pr_quantizer_ints(q, ci), k), s));
      });
      let sum = 0, peak = 0, h = 0x811c9dc5;
      for (const o of ints) {
        const bytes = new Uint8Array(o.buffer);
        for (let i = 0; i < o.length; i++) { const a = Math.abs(o[i]); sum += a; if (a > peak) peak = a; }
        for (let i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 16777619) >>> 0; }
      }
      const buf = alloc(src);
      out.push(['dithering', X.pr_quantizer_dithering(q)], ['onGrid', X.pr_on_grid(buf.table, src.length, n, bits)],
               ['sum', sum], ['peak', peak], ['hash', h.toString(16).padStart(8, '0')]);
      buf.free(); X.pr_quantizer_destroy(q);
      return true;
    }
    case 'fades': {
      let buf;
      const curve = c.curve ?? 0;
      if (c.kind === 'seconds' || c.kind === 'samples') {
        buf = alloc(channelsOf(sig));
        if (c.kind === 'seconds') X.pr_fades(buf.table, sig.channels, n, fs, c.in ?? 0, c.out ?? 0, curve);
        else X.pr_fade_samples(buf.table, sig.channels, n, c.in ?? 0, c.out ?? 0, curve, c.point ?? 0);
        fingerprintAudio(out, '', buf.read(n), fs);
      } else {
        const len = c.length ?? 1000, src = channelsOf(sig);
        buf = alloc(src.map((x) => x.subarray(0, len)));
        const from = alloc(src.map((x) => x.subarray(len, 2 * len)));
        X.pr_crossfade(buf.table, from.table, src.length, len, curve, c.point ?? 0);
        fingerprintAudio(out, '', buf.read(len), fs);
        from.free();
      }
      buf.free();
      return true;
    }
    case 'channels': {
      const src = alloc(channelsOf(sig));
      ['keep', 'left', 'right', 'mono', 'stereo'].forEach((name, m) => {
        const nOut = X.pr_mapped_channels(m, sig.channels);
        const o = alloc(Array.from({ length: nOut }, () => new Float32Array(n)));
        X.pr_map_channels(m, src.table, sig.channels, n, o.table);
        out.push([name + '.channels', nOut]);
        fingerprintAudio(out, name, o.read(n), fs);
        o.free();
      });
      out.push(['peakOf', X.pr_peak(src.table, sig.channels, n)]);
      src.free();
      const t = nums('trim');
      for (let k = 0; k + 1 < t.length; k += 2) {
        X.pr_trim_range(n, fs, t[k], t[k + 1], intBuf);
        const r = i32(intBuf, 2);
        out.push([`trim${k / 2}.first`, r[0]], [`trim${k / 2}.last`, r[1]]);
      }
      for (const mode of [1, 2]) {
        const x = alloc(channelsOf(sig)), res = X.malloc(32);
        X.pr_normalize(x.table, sig.channels, n, fs, mode, res);
        const v = f64(res, 4), p = mode === 1 ? 'normalizePeak' : 'normalizeLufs';
        out.push([p + '.gainDb', v[0]], [p + '.outcome', v[1]], [p + '.measuredLufs', v[2]], [p + '.reachedLufs', v[3]]);
        fingerprintAudio(out, p, x.read(n), fs);
        x.free(); X.free(res);
      }
      return true;
    }
    case 'devices': {
      for (let d = 0; d < X.pr_device_count(); d++) {
        const id = cstr(X.pr_device_id(d)), h = X.pr_device_create(fs, d, block);
        const ch = channelsOf(sig).map((x) => x.slice());
        blocks(n, block, (s, k) => {
          f32(X.pr_device_left(h), k).set(ch[0].subarray(s, s + k));
          if (stereo) f32(X.pr_device_right(h), k).set(ch[1].subarray(s, s + k));
          X.pr_device_process(h, k, stereo ? 1 : 0);
          ch[0].set(f32(X.pr_device_left(h), k), s);
          if (stereo) ch[1].set(f32(X.pr_device_right(h), k), s);
        });
        fingerprintAudio(out, id, ch, fs);
        for (const f of nums('freqs')) out.push([`${id}.magnitude${Math.trunc(f)}`, X.pr_device_magnitude(h, f)]);
        X.pr_device_destroy(h);
      }
      return true;
    }
    case 'vocal-eq': {
      const h = X.pr_vocal_eq_create(fs, block);
      X.pr_vocal_eq_settings(h, c.lowCutHz ?? 20, c.lowDb ?? 0, c.bodyDb ?? 0, c.presenceDb ?? 0, c.airDb ?? 0);
      const ch = channelsOf(sig).map((x) => x.slice());
      blocks(n, block, (s, k) => {
        f32(X.pr_vocal_eq_left(h), k).set(ch[0].subarray(s, s + k));
        if (stereo) f32(X.pr_vocal_eq_right(h), k).set(ch[1].subarray(s, s + k));
        X.pr_vocal_eq_process(h, k, stereo ? 1 : 0);
        ch[0].set(f32(X.pr_vocal_eq_left(h), k), s);
        if (stereo) ch[1].set(f32(X.pr_vocal_eq_right(h), k), s);
      });
      fingerprintAudio(out, '', ch, fs);
      for (const f of nums('freqs')) out.push([`magnitude${Math.trunc(f)}`, X.pr_vocal_eq_magnitude(h, f)]);
      X.pr_vocal_eq_destroy(h);
      return true;
    }
    case 'ab': {
      const h = X.pr_ab_create(fs, block, c.latency ?? 0);
      X.pr_ab_hear_original(h, 1);
      const res = [new Float32Array(n), new Float32Array(n)], half = Math.trunc(n / 2);
      const r = stereo ? sig.r : sig.l;
      blocks(n, block, (s, k) => {
        if (s >= half) X.pr_ab_hear_original(h, 0);
        f32(X.pr_ab_original_left(h), k).set(sig.l.subarray(s, s + k));
        f32(X.pr_ab_original_right(h), k).set(r.subarray(s, s + k));
        X.pr_ab_take_original(h, k);
        const pl = f32(X.pr_ab_left(h), k), pr = f32(X.pr_ab_right(h), k);
        for (let i = 0; i < k; i++) { pl[i] = sig.l[s + i] * 0.5; pr[i] = r[s + i] * 0.5; }
        X.pr_ab_mix(h, k);
        res[0].set(f32(X.pr_ab_left(h), k), s);
        res[1].set(f32(X.pr_ab_right(h), k), s);
      });
      out.push(['matchDb', X.pr_ab_match_db(h)]);
      X.pr_ab_destroy(h);
      fingerprintAudio(out, '', res, fs);
      return true;
    }

    // ---- music
    case 'music': {
      for (let m = 0; m < 128; m++) out.push([`name${m}`, mtext((b, cap) => X.mu_note_name(m, 1, b, cap))]);
      out.push(['name-1', mtext((b, cap) => X.mu_note_name(-1, 1, b, cap))]);
      for (let letter = 0; letter < 7; letter++)
        for (let acc = -1; acc <= 1; acc++)
          for (let minor = 0; minor < 2; minor++) {
            const id = noteText(letter, acc) + (minor ? 'm' : '');
            out.push([id + '.name', mtext((b, cap) => X.mu_key_name(letter, acc, minor, b, cap))]);
            out.push([id + '.signature', X.mu_signature(letter, acc, minor)]);
            X.mu_scale_notes(letter, acc, minor, intBuf);
            const v = i32(intBuf, 14).slice();
            const scale = [];
            for (let i = 0; i < 7; i++) scale.push(noteText(v[2 * i], v[2 * i + 1]));
            out.push([id + '.scale', scale.join(' ')]);
            out.push([id + '.midi61', mtext((b, cap) => X.mu_note_name_in_key(61, 1, letter, acc, minor, b, cap))]);
          }
      for (let pc = 0; pc < 12; pc++) {
        const p = `pc${pc}`;
        X.mu_plain_note(pc, intBuf); out.push([p + '.plain', spelledAt(intBuf)]);
        const count = X.mu_spellings(pc, intBuf);
        const v = i32(intBuf, 2 * count).slice(), all = [];
        for (let i = 0; i < count; i++) all.push(noteText(v[2 * i], v[2 * i + 1]));
        out.push([p + '.spellings', all.join(' ')]);
        X.mu_spell_note(pc, 0, 0, 0, 1, intBuf); out.push([p + '.sharp', spelledAt(intBuf)]);
        X.mu_spell_note(pc, 0, 0, 0, -1, intBuf); out.push([p + '.flat', spelledAt(intBuf)]);
        X.mu_in_key(pc, 3, 1, intBuf); out.push([p + '.inF#', spelledAt(intBuf)]);
        X.mu_in_key(pc, 1, -1, intBuf); out.push([p + '.inDb', spelledAt(intBuf)]);
      }
      for (const t of nums('parse')) {
        const tp = text(t);
        out.push(['note:' + t, X.mu_parse_note(tp, intBuf) ? spelledAt(intBuf) : '-']);
        if (X.mu_parse_key(tp, intBuf)) { const k = i32(intBuf, 3).slice(); out.push(['key:' + t, mtext((b, cap) => X.mu_key_name(k[0], k[1], k[2], b, cap))]); }
        else out.push(['key:' + t, '-']);
        X.free(tp);
      }
      for (const hz of nums('hz')) out.push([`hzToMidi${Math.trunc(hz)}`, X.mu_hz_to_midi(hz, 440)]);
      for (const m of nums('midi')) out.push([`midiToHz${Math.trunc(m)}`, X.mu_midi_to_hz(m, 442)]);
      for (let d = 27; d <= 87; d++) {
        out.push([`drum${d}`, mtext((b, cap) => X.mu_gm_drum_name(d, b, cap))]);
        out.push([`pad${d}`, X.mu_pad_for_note(d)]);
      }
      for (let letter = 0; letter < 7; letter++) {
        for (let pc = 0; pc < 12; pc++) { X.mu_on_letter(letter, pc, intBuf); out.push([`onLetter.${letter}.${pc}`, spelledAt(intBuf)]); }
        for (let acc = -2; acc <= 2; acc++) out.push([`soundsAs.${letter}.${acc}`, mtext((b, cap) => X.mu_sounds_as(letter, acc, b, cap))]);
      }
      for (const [name, minor] of [['majorKeys', 0], ['minorKeys', 1]])
        out.push([name, Array.from({ length: X.mu_key_count(minor) }, (_, i) => mtext((b, cap) => X.mu_key_tonic(minor, i, b, cap))).join(' ')]);
      return true;
    }
    case 'meter': {
      const cap = 1024, buf = X.malloc(4 * cap), buf2 = X.malloc(4 * cap), dbl = X.malloc(8 * 32);
      for (const top of nums('tops'))
        for (const bottom of nums('bottoms')) {
          const m = `${top}/${bottom}`;
          const count = X.rh_grouping_options(top, bottom, buf, cap);
          const v = intList(buf, cap), options = [];
          for (let i = 0, k = 0; i < count; i++) { options.push(v.slice(k + 1, k + 1 + v[k]).join('+')); k += 1 + v[k]; }
          out.push([`options.${m}`, options.join(' ')]);
          const g = intList(buf, X.rh_default_groups(top, bottom, buf, cap));
          out.push([`groups.${m}`, g.join('+')]);
          const gp = ints(g);
          out.push([`accents.${m}`, intList(buf2, X.rh_default_accents(gp, g.length, buf2, cap)).join(' ')]);
          const places = X.rh_group_map(gp, g.length, buf2, cap), map = intList(buf2, 2 * places), parts = [];
          for (let i = 0; i < places; i++) parts.push(`${map[2 * i]}${map[2 * i + 1] ? 's' : ''}`);
          out.push([`map.${m}`, parts.join(' ')]);
          const units = intList(buf2, X.rh_unit_options(gp, g.length, bottom, buf2));
          out.push([`units.${m}`, units.map((u) => `${u} ${mtext((b, c2) => X.rh_note_value_name(u, bottom, b, c2))}`).join(', ')]);
          X.free(gp);
        }
      (c.parse ?? []).forEach(([t, top], i) => {
        const tp = text(t), n = X.rh_parse_groups(tp, top, buf, cap);
        X.free(tp);
        out.push([`parse${i}`, n > 0 ? intList(buf, n).join('+') : '-']);
      });
      for (let units = 1; units <= 12; units++)
        for (const bottom of [1, 2, 4, 8, 16, 32, 3]) out.push([`noteValue.${units}/${bottom}`, mtext((b, c2) => X.rh_note_value_name(units, bottom, b, c2))]);
      for (const id of ['none', '8', '3', '16', '5', '6', 'x'])
        nums('swings').forEach((swing, k) => {
          const tp = text(id), n = X.rh_sub_fractions(tp, swing, dbl);
          X.free(tp);
          out.push([`sub.${id}.${k}.count`, n]);
          f64(dbl, n).forEach((f, i) => out.push([`sub.${id}.${k}.f${i}`, f]));
        });
      nums('bpms').forEach((bpm, k) => out.push([`tempoName${k}`, mtext((b, c2) => X.rh_tempo_name(bpm, b, c2))]));
      const SUBS = ['none', '8', '3', '16', '5', '6'];
      (c.clean ?? []).forEach((item, i) => {
        const sp = X.malloc(8 * 18);
        f64(sp, 18).set(item.s);
        const gp = ints(item.groups ?? []), ap = ints(item.accents ?? []), subp = text(item.sub ?? 'none');
        X.sd_clean_settings(sp, gp, item.groups ? item.groups.length : -1, ap, item.accents ? item.accents.length : -1, subp, dbl, buf, buf2);
        const o = Array.from(f64(dbl, 21));
        for (let k = 0; k < 18; k++) out.push([`clean${i}.v${k}`, o[k]]);
        out.push([`clean${i}.groups`, intList(buf, o[18]).join('+')], [`clean${i}.accents`, intList(buf2, o[19]).join(' ')], [`clean${i}.sub`, SUBS[o[20]]]);
        [sp, gp, ap, subp].forEach((p) => X.free(p));
      });
      [buf, buf2, dbl].forEach((p) => X.free(p));
      return true;
    }
    case 'downsample': {
      const xp = X.malloc(4 * n + 4), yp = X.malloc(4 * n + 4);
      f32(xp, n).set(sig.l);
      for (const stride of nums('strides')) {
        const m = X.tp_downsample(xp, n, stride, yp), y = f32(yp, m);
        const p = `stride${Math.trunc(stride)}`;
        let sum = 0;
        for (let i = 0; i < m; i++) sum += Math.abs(y[i]);
        out.push([`${p}.length`, m], [`${p}.sum`, sum]);
        for (const at of nums('at')) if (Math.trunc(at) < m) out.push([`${p}.at${Math.trunc(at)}`, y[Math.trunc(at)]]);
      }
      X.free(xp); X.free(yp);
      return true;
    }
    case 'dsd': {
      const bits = [dsdBits(sig.l, c.kind)];
      if (stereo) bits.push(dsdBits(sig.r, c.kind));
      const h = X.pr_dsd_create(fs, bits.length, block);
      out.push(['rate', h ? X.pr_dsd_rate(h) : 0]);
      const pcm = bits.map(() => []);
      const len = bits[0].length;
      blocks(len, block, (s, k) => {
        bits.forEach((b, ch) => new Uint8Array(X.memory.buffer, X.pr_dsd_input(h, ch), k).set(b.subarray(s, s + k)));
        const made = X.pr_dsd_push(h, k);
        bits.forEach((_, ch) => pcm[ch].push(...f32(X.pr_dsd_output(h, ch), made)));
      });
      X.pr_dsd_destroy(h);
      const rate = out[out.length - 1][1] || 88200;
      fingerprintAudio(out, '', pcm.map((x) => Float32Array.from(x)), rate);
      for (const at of nums('at')) if (Math.trunc(at) < pcm[0].length) out.push([`at${Math.trunc(at)}`, pcm[0][Math.trunc(at)]]);
      return true;
    }
    case 'limiter': {
      const h = X.pr_limiter_create(fs, sig.channels, c.ceiling ?? -1);
      if (c.release !== undefined) X.pr_limiter_release(h, c.release);
      out.push(['latency', X.pr_limiter_latency(h)]);
      const ch = channelsOf(sig).map((x) => x.slice());
      const buf = alloc(ch.map((x) => x.subarray(0, block)));
      blocks(n, block, (s, k) => {
        ch.forEach((x, i) => f32(buf.ptrs[i], k).set(x.subarray(s, s + k)));
        X.pr_limiter_process(h, buf.table, k);
        ch.forEach((x, i) => x.set(f32(buf.ptrs[i], k), s));
      });
      buf.free();
      X.pr_limiter_destroy(h);
      fingerprintAudio(out, '', ch, fs);
      const m = X.ms_tp_create(ch.length, 4096);
      blocks(n, 4096, (s, k) => {
        f32(X.ms_tp_left(m), k).set(ch[0].subarray(s, s + k));
        if (ch.length > 1) f32(X.ms_tp_right(m), k).set(ch[1].subarray(s, s + k));
        X.ms_tp_process(m, k);
      });
      X.ms_tp_finish(m);
      out.push(['truePeak', X.ms_tp_true_peak(m, -1)]);
      X.ms_tp_destroy(m);
      return true;
    }
  }
  return false;
}

function run(c, sig) {
  const out = [];
  const fs = sig ? sig.rate : (c.rate ?? 48000);
  const block = c.block ?? 4096;
  const n = sig ? sig.l.length : 0;
  const stereo = sig !== null && sig.channels > 1;
  const fill = (lp, rp, s, k) => {
    f32(lp, k).set(sig.l.subarray(s, s + k));
    if (stereo && rp) f32(rp, k).set(sig.r.subarray(s, s + k));
  };

  switch (c.op) {
    case 'loudness-meter': {
      const m = X.lm_create(fs, block);
      blocks(n, block, (s, k) => { fill(X.lm_left(m), X.lm_right(m), s, k); X.lm_process(m, k, stereo ? 1 : 0); });
      out.push(['integrated', X.lm_integrated(m)]);
      X.lm_destroy(m);
      break;
    }
    case 'loudness-report': {
      const m = X.ms_lr_create(fs, sig.channels, block);
      blocks(n, block, (s, k) => { fill(X.ms_lr_left(m), X.ms_lr_right(m), s, k); X.ms_lr_process(m, k); });
      X.ms_lr_finish(m);
      const v = (i) => X.ms_lr_value(m, i);
      out.push(['integrated', v(0)], ['shortTermMax', v(1)], ['momentaryMax', v(3)], ['range', v(5)],
               ['samplePeak', v(7)], ['truePeak', v(8)], ['rmsLeft', v(12)]);
      if (stereo) out.push(['rmsRight', v(13)], ['balance', v(6)]);
      out.push(['frames', v(14)]);
      X.ms_lr_destroy(m);
      break;
    }
    case 'true-peak': {
      const m = X.ms_tp_create(sig.channels, block);
      blocks(n, block, (s, k) => { fill(X.ms_tp_left(m), X.ms_tp_right(m), s, k); X.ms_tp_process(m, k); });
      X.ms_tp_finish(m);
      out.push(['truePeak', X.ms_tp_true_peak(m, -1)], ['samplePeak', X.ms_tp_sample_peak(m, -1)]);
      if (c.position !== undefined) out.push(['position', X.ms_tp_position(m)]);
      if (stereo) out.push(['truePeakLeft', X.ms_tp_true_peak(m, 0)], ['truePeakRight', X.ms_tp_true_peak(m, 1)]);
      X.ms_tp_destroy(m);
      break;
    }
    case 'tuner': {
      const x = slice(c, sig);
      const hz = withBuffer(x, (p) => X.tp_estimate(p, x.length, fs, c.minHz ?? 25, c.maxHz ?? 2100));
      out.push(['hz', hz]);
      const offsets = X.malloc(48), res = X.malloc(16), strings = X.malloc(4);
      f32(offsets, 12).fill(0);
      X.tp_reading(hz, 440, strings, 0, offsets, 0, res);
      const r = f32(res, 4);
      out.push(['note', r[0]]);
      if (hz > 0) out.push(['cents', r[2]]);
      X.free(offsets); X.free(res); X.free(strings);
      break;
    }
    case 'voice': {
      const x = slice(c, sig);
      const res = X.malloc(24), cand = X.malloc(8 * 2 * 64);
      withBuffer(x, (p) => X.pi_voice(p, x.length, fs, c.minHz ?? 65, c.maxHz ?? 1100, res, cand, 64));
      const r = f64(res, 3);
      out.push(['hz', r[0]], ['clarity', r[1]]);
      X.free(res); X.free(cand);
      break;
    }
    case 'tempo': {
      const t = X.rh_tempo_create(fs, block);
      blocks(n, block, (s, k) => { f32(X.rh_tempo_input(t), k).set(sig.l.subarray(s, s + k)); X.rh_tempo_push(t, k); });
      const res = X.malloc(8 * 8);
      const valid = X.rh_tempo_estimate(t, c.minBpm ?? 60, c.maxBpm ?? 200, res);
      const r = f64(res, 8);
      out.push(['valid', valid], ['bpm', r[0]], ['others', r[3]]);
      for (let i = 0; i < r[3]; i++) out.push(['other' + i, r[4 + i]]);
      X.free(res); X.rh_tempo_destroy(t);
      break;
    }
    case 'onsets': {
      const d = X.rh_onsets_create(fs, c.sensitivity ?? 50, c.refractoryMs ?? 45, block);
      const found = [];
      blocks(n, block, (s, k) => {
        f32(X.rh_onsets_input(d), k).set(sig.l.subarray(s, s + k));
        const m = X.rh_onsets_push(d, k, s);
        found.push(...f64(X.rh_onsets_found(d), m));
      });
      out.push(['count', found.length]);
      found.forEach((f, i) => out.push(['onset' + i, f]));
      X.rh_onsets_destroy(d);
      break;
    }
    case 'mastering': {
      const m = X.mt_create(fs, n, block);
      X.mt_settings(m, c.style ?? 0, c.intensity ?? 0.5, 0, c.target ?? -14, c.ceiling ?? -1, 0.5, 0, 0, 0, 0, 0, 0, 0);
      const st = stereo ? 1 : 0;
      blocks(n, block, (s, k) => { fill(X.mt_left(m), X.mt_right(m), s, k); X.mt_energy(m, k, st); });
      const from = X.mt_window_from(m), length = X.mt_window_length(m);
      blocks(length, block, (s, k) => { fill(X.mt_left(m), X.mt_right(m), from + s, k); X.mt_window(m, k, st); });
      X.mt_learn(m);
      const ol = new Float32Array(n), or = new Float32Array(n);
      do {
        X.mt_begin_render(m);
        let o = 0;
        const keep = (k) => {
          ol.set(f32(X.mt_out_left(m), k), o);
          or.set(f32(X.mt_out_right(m), k), o);
          o += k;
        };
        blocks(n, block, (s, k) => { fill(X.mt_left(m), X.mt_right(m), s, k); keep(X.mt_render(m, k, st)); });
        keep(X.mt_finish_render(m));
      } while (X.mt_next_pass(m));
      out.push(['lufs', X.mt_lufs(m)], ['preGain', X.mt_learned(m, 1)], ['drive', X.mt_learned(m, 2)]);
      X.mt_destroy(m);
      const tp = X.ms_tp_create(2, block);
      blocks(n, block, (s, k) => {
        f32(X.ms_tp_left(tp), k).set(ol.subarray(s, s + k));
        f32(X.ms_tp_right(tp), k).set(or.subarray(s, s + k));
        X.ms_tp_process(tp, k);
      });
      X.ms_tp_finish(tp);
      out.push(['truePeak', X.ms_tp_true_peak(tp, -1)]);
      X.ms_tp_destroy(tp);
      break;
    }
    case 'click-sounds': {
      const roles = ['accent', 'beat', 'sub', 'poly', 'alt'];
      for (let i = 0; i < X.sd_click_set_count(); i++) {
        const id = cstr(X.sd_click_set_id(i));
        roles.forEach((role, k) => {
          const p = text(id);
          fingerprint(out, `${id}.${role}`, takeSound(X.sd_click_render(p, k, fs, lenP)));
          X.free(p);
        });
      }
      break;
    }
    case 'drum-sounds': {
      for (let i = 0; i < X.sd_drum_count(); i++) {
        const id = cstr(X.sd_drum_id(i));
        const p = text(id);
        fingerprint(out, id, takeSound(X.sd_drum_render(p, fs, lenP)));
        X.free(p);
      }
      break;
    }
    case 'drum-kits': {
      const res = X.malloc(12);
      for (let k = 0; k < X.sd_kit_count(); k++) {
        const kit = cstr(X.sd_kit_id(k));
        for (let p = 0; p < X.sd_kit_pad_count(k); p++) {
          const sound = cstr(X.sd_kit_pad(k, p, res));
          const r = i32(res, 3);
          out.push([`${kit}.${p}.sound`, sound], [`${kit}.${p}.slot`, r[0]], [`${kit}.${p}.note`, r[1]], [`${kit}.${p}.choke`, r[2]]);
        }
      }
      X.free(res);
      break;
    }
    default:
      if (!runMore(c, sig, out)) out.push([`unknown op ${c.op}`, '']);
  }
  return out;
}

// ---- checking

const file = JSON.parse(fs.readFileSync(filePath, 'utf8'));
let failures = 0, checks = 0;
const worst = {};
const report = (ok, name, detail) => {
  checks++;
  if (!ok) { failures++; console.log(`FAIL ${name.padEnd(62)} ${detail}`); }
};

if (!file.cases?.length) { console.log(`FAIL ${filePath} has no cases`); process.exit(1); }
let nativeOnly = 0;
for (const c of file.cases) {
  // processors with no functions in the browser build: tests/ReferenceTests.cpp checks them
  if (c.native) { nativeOnly++; continue; }
  const before = failures, checksBefore = checks;
  let sig = null;
  if (c.input) {
    sig = makeInput(c.input);
    const h = hash(sig);
    report(h === c.inputHash, `${c.id} input`, `made ${h}, the file names ${c.inputHash}`);
  }
  const produced = run(c, sig), got = new Map(produced);
  if (got.size !== produced.length) report(false, `${c.id}`, 'a result name is produced twice');
  for (const [name, [expected, tol]] of Object.entries(c.results)) {
    const t = file.tolerances[tol];
    if (!got.has(name)) { report(false, `${c.id} ${name}`, 'not produced by this build'); continue; }
    const value = got.get(name);
    got.delete(name);
    if (!t) { report(false, `${c.id} ${name}`, `unknown tolerance ${tol}`); continue; }
    if (typeof expected === 'string') { report(value === expected, `${c.id} ${name}`, `expected ${expected}, got ${value}`); continue; }
    const allow = (t.abs ?? 0) + (t.rel ?? 0) * Math.abs(expected);
    const diff = value === expected ? 0 : Math.abs(value - expected);
    const w = (worst[tol] ??= { diff: 0, allow, at: '' });
    if (diff > w.diff) Object.assign(w, { diff, allow, at: `${c.id} ${name}` });
    report(allow > 0 ? diff <= allow : diff === 0, `${c.id} ${name}`,
           `expected ${expected}, got ${value} (off by ${diff.toPrecision(3)}, ${tol} allows ${allow.toPrecision(3)})`);
  }
  for (const name of got.keys()) report(false, `${c.id} ${name}`, 'produced but not in the file: regenerate it (tools/reference.py)');
  console.log(`${failures === before ? 'PASS' : 'FAIL'} ${c.id.padEnd(62)} ${checks - checksBefore} checks`);
}
console.log('\nLargest difference from the file, by tolerance:');
for (const [t, w] of Object.entries(worst).sort()) console.log(`  ${t.padEnd(16)} ${w.diff.toPrecision(3)} (allowed ${w.allow.toPrecision(3)})${w.diff > 0 ? '  ' + w.at : ''}`);
console.log(`\n${nativeOnly} cases are for the native builds only (no browser functions): tests/ReferenceTests.cpp checks them`);
console.log(`\n${failures === 0 ? 'ALL PASSED' : 'SOME FAILED'} (${checks} checks, ${failures} failed)`);
process.exit(failures === 0 ? 0 : 1);
