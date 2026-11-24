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
const withText = (s, f) => { const p = text(s); try { return f(p); } finally { X.free(p); } };
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
  'cliffDrop', 'hasPadded16', 'padded16', 'dcLDb', 'dcRDb', 'lowSideToMidDb'];

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

  // JSON an rs_* function writes (out, cap), with a second try when it needs more room
  const answer = (fn) => {
    let cap = 1 << 16, b = X.malloc(cap), k = fn(b, cap);
    if (k < 0) { X.free(b); cap = -k; b = X.malloc(cap); k = fn(b, cap); }
    const v = JSON.parse(cstr(b));
    X.free(b);
    return v;
  };
  // feeds n samples from `at` of both channels into an rs_* object's inputs, then calls run (count)
  const feed = (input, h, at, count, run) => {
    f32(input(h, 0), count).set(sig.l.subarray(at, at + count));
    f32(input(h, 1), count).set((stereo ? sig.r : sig.l).subarray(at, at + count));
    run(count);
  };

  switch (c.op) {
    // ---- restoration of record and tape transfers
    // ---- a pad player
    case 'pad-loop': {
      const h = X.in_sampler_create(fs, block), pads = nums('pads'), hits = nums('hits');
      X.in_sampler_velocity_curve(h, c.velocityCurve ?? 1);
      for (let k = 0; k + 8 < pads.length; k += 9) {
        f32(X.in_sampler_load_left(h, n), n).set(sig.l);
        if (stereo) f32(X.in_sampler_load_right(h, n), n).set(sig.r);
        X.in_sampler_add_pad(h, stereo ? 1 : 0, fs, pads[k], pads[k + 5], pads[k + 6], pads[k + 7], pads[k + 8], pads[k + 1], pads[k + 2], pads[k + 3], pads[k + 4], c.release ?? 0.005);
      }
      const m = hits.length / 3, np = X.malloc(4 * m + 4), bp = X.malloc(8 * m + 8), vp = X.malloc(4 * m + 4);
      for (let k = 0; k < m; k++) { i32(np, m)[k] = hits[3 * k]; f64(bp, m)[k] = hits[3 * k + 1]; f32(vp, m)[k] = hits[3 * k + 2]; }
      const len = X.in_sampler_render_loop(h, np, bp, vp, m, c.bpm ?? 120, c.loopBeats ?? 4, fs, c.tail ?? 3, c.busGain ?? 0.85);
      fingerprintAudio(out, '', [f32(X.in_sampler_loop_left(h), len).slice(), f32(X.in_sampler_loop_right(h), len).slice()], fs);
      X.free(np); X.free(bp); X.free(vp); X.in_sampler_destroy(h);
      return true;
    }
    case 'ensemble':
    case 'ensemble-live': {
      const h = X.in_ensemble_create(fs);
      for (const f of c.synths || []) withText(f, (fp) => X.in_ensemble_add_synth(h, fp));
      f32(X.in_ensemble_recording(h, 0, n), n).set(sig.l);
      withText(c.family, (fp) => withText(c.manifest, (mp) => X.in_ensemble_add_recorded(h, mp, c.trimDb ?? 0, fp)));
      const pv = nums('parts'), np = pv.length / 2, vp = X.malloc(4 * np + 4), pp = X.malloc(4 * np + 4);
      for (let k = 0; k < np; k++) { f32(vp, np)[k] = pv[2 * k]; f32(pp, np)[k] = pv[2 * k + 1]; }
      X.in_ensemble_parts(h, np, vp, pp);
      const nv = nums('notes'), m = nv.length / 11, only = c.only || [];
      const T = X.malloc(8 * m + 8), Dd = X.malloc(8 * m + 8), M = X.malloc(4 * m + 4), V = X.malloc(4 * m + 4), VE = X.malloc(4 * m + 4), P = X.malloc(4 * m + 4),
        I = X.malloc(4 * m + 4), F = X.malloc(4 * m + 4), TE = X.malloc(4 * m + 4), B = X.malloc(4 * m + 4), BA = X.malloc(4 * m + 4), O = X.malloc(4 * only.length + 4);
      for (let k = 0; k < m; k++) {
        const r = nv.slice(11 * k, 11 * k + 11);
        f64(T, m)[k] = r[0]; f64(Dd, m)[k] = r[1]; i32(M, m)[k] = r[2]; f32(V, m)[k] = r[3]; f32(VE, m)[k] = r[4]; i32(P, m)[k] = r[5]; i32(I, m)[k] = r[6];
        i32(F, m)[k] = r[7]; i32(TE, m)[k] = r[8]; i32(B, m)[k] = r[9]; f32(BA, m)[k] = r[10];
      }
      only.forEach((x, k) => { i32(O, only.length)[k] = x; });
      if (c.op === 'ensemble-live') {
        // live: scheduled on the clock, the clicks beside it, a stop part-way, in 128-frame blocks
        X.in_ensemble_live_start(h);
        for (let k = 0; k < m; k++) {
          const r = nv.slice(11 * k, 11 * k + 11);
          X.in_ensemble_schedule(h, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], 1);
        }
        const cv = c.clicks || [];
        for (let k = 0; k + 2 < cv.length; k += 3) X.in_ensemble_click(h, cv[k], cv[k + 1], cv[k + 2]);
        const frames = Math.round((c.seconds ?? 3) * fs), stopAt = Math.round((c.cancel ?? 1e9) * fs), L = new Float32Array(frames), R = new Float32Array(frames);
        let stopped = false;
        for (let at = 0; at < frames; at += 128) {
          if (!stopped && at >= stopAt) { X.in_ensemble_cancel(h, c.cancel ?? 0); stopped = true; }
          const k = Math.min(128, frames - at);
          X.in_ensemble_live_render(h, k, at);
          L.set(f32(X.in_ensemble_left(h), k), at);
          R.set(f32(X.in_ensemble_right(h), k), at);
        }
        fingerprintAudio(out, '', [L, R], fs);
      } else {
        const len = X.in_ensemble_render(h, m, T, Dd, M, V, VE, P, I, F, TE, B, BA, c.normalize ?? 1, only.length, O);
        fingerprintAudio(out, '', [f32(X.in_ensemble_left(h), len).slice(), f32(X.in_ensemble_right(h), len).slice()], fs);
      }
      for (const q of [vp, pp, T, Dd, M, V, VE, P, I, F, TE, B, BA, O]) X.free(q);
      X.in_ensemble_destroy(h);
      return true;
    }
    case 'soft-clip': {
      const ch = channelsOf(sig).map((x) => Float32Array.from(x, (v) => Math.fround(v * Math.fround(c.gain ?? 1)))), buf = alloc(ch);
      X.pr_soft_clip(buf.table, ch.length, n, c.knee ?? 0.7);
      fingerprintAudio(out, '', buf.read(n), fs);
      buf.free();
      return true;
    }
    case 'trim-start': {
      const ch = channelsOf(sig), buf = alloc(ch);
      for (const db of nums('thresholds')) out.push([`at${-db}`, X.pr_trim_start(buf.table, ch.length, n, fs, db, c.lead ?? 0)]);
      buf.free();
      return true;
    }
    case 'loop-timing': {
      const bpm = c.bpm ?? 120, L = c.loopBeats ?? 4, times = nums('times');
      times.forEach((t, i) => {
        out.push([`beatAt${i}`, X.mu_loop_beat_at(t, 0.25, bpm, L)]);
        for (const steps of [2, 3, 4, 6]) out.push([`snap${steps}.${i}`, X.mu_loop_snap(t, steps, L)]);
        out.push([`grid${i}`, X.mu_next_grid_time(t, 0.1, 60 / bpm / 4)]);
      });
      const beats = nums('beats'), bp = X.malloc(8 * beats.length + 8), tp = X.malloc(8 * 256), wp = X.malloc(4 * 256);
      f64(bp, beats.length).set(beats);
      const due = X.mu_loop_hits(bp, 0, beats.length, c.from ?? 0, c.to ?? 4, 0.25, bpm, L, tp, wp, 256);
      out.push(['due', due]);
      for (let i = 0; i < due; i++) out.push([`due${i}.time`, f64(tp, due)[i]], [`due${i}.hit`, i32(wp, due)[i]]);
      const cp = X.malloc(4 * 256), dp = X.malloc(4 * 256);
      const k = X.mu_loop_clicks(c.from ?? 0, c.to ?? 4, 0.25, bpm, 4, tp, cp, dp, 256);
      out.push(['clicks', k]);
      for (let i = 0; i < k; i++) out.push([`click${i}.time`, f64(tp, k)[i]], [`click${i}.down`, i32(dp, k)[i]]);
      [bp, tp, wp, cp, dp].forEach((p) => X.free(p));
      return true;
    }
    // ---- live recordings
    case 'live-set': {
      const h = X.lv_set_create(fs, block);
      blocks(n, block, (s, k) => {
        f32(X.lv_set_input(h, 0), k).set(sig.l.subarray(s, s + k));
        if (stereo) f32(X.lv_set_input(h, 1), k).set(sig.r.subarray(s, s + k));
        X.lv_set_push(h, k, stereo ? 1 : 0);
      });
      const set = answer((b, cap) => X.lv_set_finish(h, c.minSong ?? 40, c.mergeGap ?? 6, b, cap));
      X.lv_set_destroy(h);
      const kinds = ['quiet', 'music', 'applause', 'talk', 'noodling'];
      out.push(['seconds', set.seconds], ['judged', set.judged ? 1 : 0], ['threshold', set.threshold ?? 0], ['peak', set.peak ?? 0], ['songs', set.songs.length]);
      set.songs.forEach((x, i) => out.push([`song${i}.start`, x.start], [`song${i}.end`, x.end], [`song${i}.confidence`, x.confidence], [`song${i}.level`, x.level]));
      out.push(['gaps', set.gaps.length]);
      set.gaps.forEach((g, i) => out.push([`gap${i}.kind`, kinds.indexOf(g.kind)]));
      for (const k of kinds) out.push([`timeline.${k}`, set.timeline.filter((t) => t === k).length]);
      return true;
    }
    case 'live-ride': {
      const h = X.lv_ride_create(fs, c.target ?? -18, c.range ?? 12, block);
      blocks(n, block, (s, k) => {
        f32(X.lv_ride_input(h, 0), k).set(sig.l.subarray(s, s + k));
        if (stereo) f32(X.lv_ride_input(h, 1), k).set(sig.r.subarray(s, s + k));
        X.lv_ride_push(h, k, stereo ? 1 : 0);
      });
      const t = f32(X.lv_ride_track(h), X.lv_ride_count(h));
      let sum = 0, peak = 0;
      for (const v of t) { sum += Math.abs(v); peak = Math.max(peak, Math.abs(v)); }
      out.push(['count', t.length], ['track.sum', sum], ['track.peak', peak], ['levelDb', X.lv_ride_level_db(h)]);
      X.lv_ride_destroy(h);
      return true;
    }
    case 'gain-track': {
      const ch = channelsOf(sig).map((x) => x.slice()), buf = alloc(ch), db = nums('db');
      const dp = X.malloc(4 * Math.max(1, db.length));
      f32(dp, db.length).set(db);
      blocks(n, block, (s, k) => X.pr_apply_gain_track(dp, db.length, c.hop ?? 0.1, c.offset ?? 0, fs, (c.start ?? 0) + s, buf.ptrs[0] + 4 * s, ch.length > 1 ? buf.ptrs[1] + 4 * s : 0, k));
      fingerprintAudio(out, '', buf.read(n), fs);
      buf.free(); X.free(dp);
      return true;
    }
    case 'live-route': {
      const delay = c.delay ?? 0, count = n - delay, pair = channelsOf(sig), r = pair[1] ?? pair[0];
      const buf = alloc([sig.l, r, sig.l, r]), oL = X.malloc(4 * count + 4), oR = X.malloc(4 * count + 4);
      const main = nums('main'), room = nums('room');
      X.lv_route(buf.table, c.inputs ?? 4, count, delay, main[0], main[1], c.mono ? 1 : 0, room.length ? room[0] : -1, room.length ? room[1] : -1, c.roomDb ?? -6, oL, oR);
      fingerprintAudio(out, '', [f32(oL, count).slice(), f32(oR, count).slice()], fs);
      buf.free(); X.free(oL); X.free(oR);
      return true;
    }
    case 'input-survey': {
      const ch = channelsOf(sig), buf = alloc(ch), sv = X.lv_survey_create(ch.length), win = X.lv_survey_window_length(n, fs);
      const table = X.malloc(4 * ch.length);
      for (let k = 0; k < X.lv_survey_spots(); k++) {
        const at = X.lv_survey_window_start(k, n, fs);
        i32(table, ch.length).set(buf.ptrs.map((p) => p + 4 * at));
        X.lv_survey_add(sv, table, Math.min(win, n - at));
      }
      ch.forEach((_, k) => {
        out.push([`input${k}.levelDb`, X.lv_survey_level_db(sv, k)], [`input${k}.corr`, X.lv_survey_next_corr(sv, k)]);
        if (k + 1 < ch.length) out.push([`input${k}.pairLevelDb`, X.lv_survey_pair_level_db(sv, k)]);
      });
      X.lv_survey_destroy(sv); X.free(table); buf.free();
      return true;
    }
    case 'delay-between': {
      const lag = c.lag ?? 0, buf = alloc([sig.l]), a = buf.ptrs[0] + 4 * lag, b = buf.ptrs[0];
      out.push(['lag', X.rh_delay_between(a, a, b, b, n - lag, fs, c.maxSec ?? 0.15)]);
      buf.free();
      return true;
    }
    case 'peaks-overloads': {
      const ch = channelsOf(sig), nc = ch.length, inter = new Float32Array(n * nc);
      for (let i = 0; i < n; i++) for (let k = 0; k < nc; k++) inter[i * nc + k] = ch[k][i];
      const buf = alloc([inter]), pk = X.malloc(4 * nc), ov = X.malloc(4 * nc);
      X.ms_peaks_overloads(buf.ptrs[0], n, nc, pk, ov);
      const peaks = f32(pk, nc).slice(), overloads = i32(ov, nc).slice();
      for (let k = 0; k < nc; k++) out.push([`input${k}.peak`, peaks[k]], [`input${k}.overloads`, overloads[k]]);
      buf.free(); X.free(pk); X.free(ov);
      return true;
    }
    case 'rumble': {
      const ch = channelsOf(sig).map((x) => x.slice()), h = X.pr_rumble_create(fs, c.hz ?? 30, block);
      blocks(n, block, (s, k) => {
        ch.forEach((x, ci) => f32(X.pr_rumble_input(h, ci), k).set(x.subarray(s, s + k)));
        X.pr_rumble_process(h, k, ch.length > 1 ? 1 : 0);
        ch.forEach((x, ci) => x.set(f32(X.pr_rumble_input(h, ci), k), s));
      });
      X.pr_rumble_destroy(h);
      fingerprintAudio(out, '', ch, fs);
      return true;
    }
    case 'restore-chain': {
      // the noise profile learned from 20 frames, half a frame apart, from learnFrom seconds on
      const N = X.rs_denoise_frame(fs), from = Math.round((c.learnFrom ?? 0) * fs);
      const lp = X.rs_profile_create(fs, N, 0, 0);
      for (let k = 0; k < 20 && from + k * (N >> 1) + N <= n; k++) feed(X.rs_profile_input, lp, from + k * (N >> 1), N, () => X.rs_profile_add(lp));
      const bins = X.rs_profile_finish(lp);
      const settings = JSON.parse(c.settings);
      if (bins > 0) settings.denoise = { ...settings.denoise, profile: Array.from(f32(X.rs_profile_data(lp), bins)) };
      X.rs_profile_destroy(lp);
      const q = text(JSON.stringify(settings));
      const h = X.rs_chain_create(fs, q, 0, 0, 0, 0, 0, block);
      X.free(q);
      let length = 0, sumL = 0, sumR = 0, peakL = 0, peakR = 0;
      const take = (got) => {
        const a = f32(X.rs_chain_output(h, 0), got), b = f32(X.rs_chain_output(h, 1), got);
        for (let i = 0; i < got; i++) { sumL += Math.abs(a[i]); sumR += Math.abs(b[i]); peakL = Math.max(peakL, Math.abs(a[i])); peakR = Math.max(peakR, Math.abs(b[i])); }
        length += got;
      };
      blocks(n, block, (at, k) => feed(X.rs_chain_input, h, at, k, (m) => take(X.rs_chain_push(h, m))));
      take(X.rs_chain_flush(h));
      const clicks = answer((b, cap) => X.rs_chain_clicks(h, b, cap));
      X.rs_chain_destroy(h);
      out.push(['length', length], ['events', clicks.length], ['sumL', sumL], ['sumR', sumR], ['peakL', peakL], ['peakR', peakR]);
      return true;
    }
    case 'restore-analysis': {
      const a = X.rs_analyzer_create(fs, n, 3000, block);
      blocks(n, block, (at, k) => feed(X.rs_analyzer_input, a, at, k, (m) => X.rs_analyzer_push(a, m)));
      X.rs_analyzer_finish(a);
      const report = (what, options = {}) => { const w = text(JSON.stringify({ what, ...options })); const v = answer((b, cap) => X.rs_analyzer_report(a, w, b, cap)); X.free(w); return v; };
      const summary = report('summary'), hum = report('hum'), tuning = report('tuning'), wow = report('wow'), spots = report('quietSpots'), wins = report('checkWindows');
      out.push(['summary.peakDb', summary.peakDb], ['summary.musicDb', summary.musicDb ?? -200]);
      out.push(['hum.found', hum.found ? 1 : 0], ['hum.hz', hum.hz], ['hum.harmonics', hum.harmonics.length]);
      out.push(['tuning.found', tuning.found ? 1 : 0], ['tuning.cents', tuning.cents], ['wow.found', wow.found ? 1 : 0]);
      const tracks = report('tracks', { minGap: 1.2, minSong: 3 });   // songs at least 3 s long, as the native run asks
      X.rs_analyzer_destroy(a);
      out.push(['tracks.tracks', tracks.tracks.length], ['tracks.gapStart', tracks.gaps[0]?.start ?? -1], ['tracks.gapEnd', tracks.gaps[0]?.end ?? -1]);
      out.push(['spots.spots', spots.length]);
      const hz = hum.harmonics.map((x) => x.hz), hp = X.malloc(8 * Math.max(1, hz.length));
      f64(hp, hz.length).set(hz);
      const N = summary.N, lp = X.rs_profile_create(fs, N, hp, hz.length);
      X.free(hp);
      let taken = 0;
      for (const at of spots) { if (at + N > n) continue; feed(X.rs_profile_input, lp, at, N, () => X.rs_profile_add(lp)); if (++taken >= 300) break; }
      const bins = X.rs_profile_finish(lp);
      out.push(['noise.frames', X.rs_profile_frames(lp)], ['noise.profileDb', bins > 0 ? X.rs_profile_db_of(X.rs_profile_data(lp), bins, N) : -200]);
      X.rs_profile_destroy(lp);
      const most = Math.max(1, ...wins.map((w) => w.length));
      const cc = X.rs_channels_create(fs, most), counter = X.rs_clicks_create(fs, 50, 40, most);
      for (const w of wins) {
        feed(X.rs_channels_input, cc, w.at, Math.min(w.length, Math.trunc(fs / 2)), (m) => X.rs_channels_add(cc, m));
        feed(X.rs_clicks_input, counter, w.at, w.length, (m) => X.rs_clicks_add(counter, m, w.at));
      }
      const ch = answer((b, cap) => X.rs_channels_result(cc, b, cap)), clicks = answer((b, cap) => X.rs_clicks_result(counter, b, cap));
      X.rs_channels_destroy(cc);
      X.rs_clicks_destroy(counter);
      out.push(['channels.corr', ch.corr], ['channels.balanceDb', ch.balanceDb], ['channels.delay', ch.delay], ['channels.mono', ch.mono ? 1 : 0]);
      out.push(['clicks.perMinute', clicks.perMinute[0]]);
      const env = X.rs_envelope_create(fs, 0.05, n);
      feed(X.rs_envelope_input, env, 0, n, (m) => X.rs_envelope_push(env, m));
      const count = X.rs_envelope_count(env);
      const gaps = answer((b, cap) => X.rs_gap_candidates(X.rs_envelope_levels(env), count, X.rs_envelope_hop(env), b, cap));
      X.rs_envelope_destroy(env);
      out.push(['envelope.levels', count], ['envelope.candidates', gaps.cands.length], ['envelope.candidateAt', gaps.cands[0]?.at ?? -1]);
      return true;
    }

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
      const lr = c.lr && sig.channels > 1, buf = alloc(lr ? [sig.l, sig.r] : [sig.l]), res = X.malloc(8 * 24);
      if (lr) X.ms_tonal_balance_lr(buf.ptrs[0], buf.ptrs[1], n, fs, res);
      else X.ms_tonal_balance(buf.ptrs[0], n, fs, res);
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
      if (c.rms || c.bands) stat('levelDb', 5, columns);
      X.ms_wf_destroy(w);
      return true;
    }
    case 'minmax': {
      const src = channelsOf(sig);
      const m = X.ms_minmax_create(src.length, c.size ?? 256);
      const two = X.malloc(8);
      blocks(n, block, (s, k) => { const b = alloc(src.map((x) => x.subarray(s, s + k))); X.ms_minmax_add(m, b.table, src.length, k); b.free(); });
      const columns = c.columns ?? 50;
      for (let ci = 0; ci < src.length; ci++)
        for (let col = 0; col < columns; col++) {
          X.ms_minmax_range(m, ci, Math.trunc(n * col / columns), Math.trunc(n * (col + 1) / columns), two);
          const r = f32(two, 2);
          out.push([`c${ci}.lo${col}`, r[0]], [`c${ci}.hi${col}`, r[1]]);
        }
      out.push(['peak', X.ms_minmax_peak(m)]);
      X.free(two); X.ms_minmax_destroy(m);
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
    case 'live-resample': {
      const step = c.step ?? 1, outBlock = c.block ?? 512, src = channelsOf(sig);
      const rs = src.map(() => X.pr_live_resampler_create(Math.max(1, step)));
      out.push(['latency', X.pr_live_resampler_latency(rs[0])]);
      const inP = X.malloc(4 * (Math.ceil(outBlock * step) + 8)), outP = X.malloc(4 * outBlock);
      const res = src.map(() => []);
      let at = 0, owed = 0;
      while (true) {
        owed += outBlock * step;
        const want = Math.trunc(owed);
        if (at + want > n) break;
        owed -= want;
        src.forEach((x, ci) => {
          f32(inP, want).set(x.subarray(at, at + want));
          X.pr_live_resampler_process(rs[ci], step, inP, want, outP, outBlock);
          res[ci].push(f32(outP, outBlock).slice());
        });
        at += want;
      }
      X.free(inP); X.free(outP); rs.forEach((r) => X.pr_live_resampler_destroy(r));
      fingerprintAudio(out, '', res.map((parts) => { const all = new Float32Array(parts.length * outBlock); parts.forEach((q, k) => all.set(q, k * outBlock)); return all; }), fs / step);
      return true;
    }
    case 'lpc': {
      const size = c.size ?? 4096, maxOrder = c.order ?? 12;
      const wp = X.malloc(8 * size);
      X.pr_tukey(size, 0.5, wp);
      const w = f64(wp, size);
      for (const t of [0, 0.25, 0.5, 0.9]) out.push([`tukey${Math.trunc(t * 100)}`, w[Math.trunc(t * (size - 1))]]);
      X.free(wp);
      const xp = X.malloc(4 * size), cp = X.malloc(8 * maxOrder * maxOrder), ep = X.malloc(8 * maxOrder);
      let b = 0;
      for (let at = 0; at + size <= n && b < (c.blocks ?? 3); at += size * 7, b++) {
        const x = i32(xp, size);
        for (let i = 0; i < size; i++) { const v = Math.fround(sig.l[at + i] * 32767); x[i] = v < 0 ? -Math.round(-v) : Math.round(v); }   // as lround: halves away from zero
        const count = X.pr_lpc_predictors(xp, size, maxOrder, cp, ep);
        const coefs = f64(cp, maxOrder * maxOrder), errs = f64(ep, maxOrder);
        for (let o = 1; o <= count; o++) {
          const p = `b${b}.o${o}`;
          out.push([p + '.err', errs[o - 1]], [p + '.a1', coefs[(o - 1) * maxOrder]], [p + '.last', coefs[(o - 1) * maxOrder + o - 1]]);
        }
        out.push([`b${b}.orders`, count]);
      }
      X.free(xp); X.free(cp); X.free(ep);
      return true;
    }
    case 'levels': {
      const src = channelsOf(sig);
      const buf = alloc(src);
      out.push(['rms', X.pr_rms(buf.ptrs[0], n)]);
      {
        const inter = new Float32Array(n * src.length);
        for (let i = 0; i < n; i++) for (let c = 0; c < src.length; c++) inter[i * src.length + c] = src[c][i];
        const ib = alloc([inter]);
        out.push(['pairDb', X.pr_rms_db(ib.ptrs[0], n, src.length, Math.min(2, src.length))]);
        ib.free();
      }
      out.push(['silent', X.pr_nearly_silent(buf.ptrs[0], n)]);
      const quiet = alloc([new Float32Array(n).fill(0.0001)]);
      out.push(['quietSilent', X.pr_nearly_silent(quiet.ptrs[0], n)]);
      quiet.free();
      const mono = X.malloc(4 * n + 4);
      X.pr_mono_average(buf.table, src.length, n, mono);
      fingerprintAudio(out, 'mono', [f32(mono, n).slice()], fs);
      X.free(mono);
      let peak = 0;
      for (const x of src) for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(x[i]));
      out.push(['gainDb', X.pr_gain_to_peak_db(peak, -0.3, -60, 24)]);
      // a gain given in dB
      X.pr_apply_gain_db(buf.table, src.length, n, -6.5);
      fingerprintAudio(out, 'gained', buf.ptrs.map((p) => f32(p, n).slice()), fs);
      buf.free();
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

    // ---- the effects: each one over the input in blocks (stereo out)
    case 'effect':
    case 'vocoder': {
      const fx = c.fx;
      const res = [Float32Array.from(sig.l), Float32Array.from(stereo ? sig.r : sig.l)];
      const g = (k, d) => c[k] ?? d;
      let h, left, right, run;
      if (fx === 'chorus') {
        h = X.fx_chorus_create(fs, block); X.fx_chorus_settings(h, g('depthMs', 3), g('speedHz', 1), g('width', 0.5), g('mix', 0.5));
        left = X.fx_chorus_left; right = X.fx_chorus_right; run = (k) => X.fx_chorus_process(h, k, 1);
      } else if (fx === 'phaser') {
        h = X.fx_phaser_create(fs, block); X.fx_phaser_settings(h, g('depth', 5), g('rate', 0.4), g('feedback', 0.7));
        left = X.fx_phaser_left; right = X.fx_phaser_right; run = (k) => X.fx_phaser_process(h, k, 1);
      } else if (fx === 'filter') {
        h = X.fx_filter_create(fs, block); X.fx_filter_settings(h, g('type', 4), g('hz', 1000), g('q', 0.7071), 0, g('slope', 0));
        left = X.fx_filter_left; right = X.fx_filter_right; run = (k) => X.fx_filter_process(h, k, 1);
      } else if (fx === 'pitch') {
        h = X.fx_pitch_create(fs, 2, block); X.fx_pitch_semitones(h, g('semitones', 12));
        out.push(['latency', X.fx_pitch_latency(h)]);
        left = X.fx_pitch_left; right = X.fx_pitch_right; run = (k) => X.fx_pitch_process(h, k, 1);
      } else if (fx === 'comp') {
        h = X.fx_comp_create(fs, block); X.fx_comp_detail(h, g('threshold', -20), g('ratio', 4), g('attackMs', 5), g('releaseMs', 100), g('makeup', 0), g('rms', 0));
        left = X.fx_comp_left; right = X.fx_comp_right; run = (k) => X.fx_comp_process(h, k, 1);
      } else if (fx === 'delay') {
        h = X.fx_delay_create(fs, block); X.fx_delay_settings(h, g('amount', 0.4), X.fx_delay_seconds(g('time', 2), g('bpm', 120)), g('feedback', 0.35));
        left = X.fx_delay_left; right = X.fx_delay_right; run = (k) => X.fx_delay_process(h, k);
      } else if (fx === 'reverb') {
        h = X.fx_reverb_create(fs, block);
        X.fx_reverb_settings(h, g('mode', 3), 20, g('decay', 2), 0.5, 8000, 150, 1, 0.3, 0, 0, g('mix', 0.3));
        left = X.fx_reverb_left; right = X.fx_reverb_right; run = (k) => X.fx_reverb_process(h, k, 1);
      } else return false;
      blocks(n, block, (s, k) => {
        f32(left(h), k).set(res[0].subarray(s, s + k));
        f32(right(h), k).set(res[1].subarray(s, s + k));
        run(k);
        res[0].set(f32(left(h), k), s);
        res[1].set(f32(right(h), k), s);
      });
      X[`fx_${fx}_destroy`](h);
      fingerprintAudio(out, '', res, fs);
      return true;
    }

    // ---- the instruments: a fixed phrase of notes, rendered in blocks
    case 'instrument': {
      const len = Math.trunc((c.seconds ?? 3) * fs), beat = Math.trunc(0.25 * fs);
      const res = [new Float32Array(len), new Float32Array(len)];
      const phrase = [[0, 48, 0.9, 1], [0, 55, 0.7, 1], [beat, 60, 0.8, 1], [2 * beat, 60, 0, 0], [3 * beat, 64, 0.6, 1], [4 * beat, 48, 0, 0],
                      [4 * beat, 55, 0, 0], [6 * beat, 64, 0, 0], [6 * beat, 36, 1, 1], [7 * beat, 36, 0, 0]];
      const synth = c.instrument === 'synth';
      let h;
      if (synth) {
        h = X.in_synth_create(fs, block);
        X.in_synth_osc(h, 0, 1, 2, 0, 0, -6, 0.5, 3, 12, 0.5, 0);
        X.in_synth_osc(h, 1, 1, 4, -12, 0, -10, 0.5, 1, 12, 0.5, 0);
        X.in_synth_filter(h, 1, 0, 1200, 1.5, 0.5, 24, 0);
        X.in_synth_lfo(h, 0, 0, 5, 0.15, 1);
      } else {
        h = X.in_sampler_create(fs, block);
        const m = Math.min(n, Math.trunc(fs));
        f32(X.in_sampler_load_left(h, m), m).set(sig.l.subarray(0, m));
        if (stereo) f32(X.in_sampler_load_right(h, m), m).set(sig.r.subarray(0, m));
        X.in_sampler_add(h, stereo ? 1 : 0, fs, 60, 30, 90, 0, 0.25, 0, 0, 0.05);
      }
      const P = synth ? 'in_synth_' : 'in_sampler_';
      let next = 0;
      blocks(len, block, (s, k) => {
        for (let i = 0; i < k;) {
          while (next < phrase.length && phrase[next][0] <= s + i) {
            const [, note, vel, on] = phrase[next++];
            if (on) X[P + 'note_on'](h, note, vel); else X[P + 'note_off'](h, note);
          }
          const until = next < phrase.length ? Math.min(k, phrase[next][0] - s) : k;
          const m = Math.max(1, until - i);
          X[P + 'render'](h, m);
          res[0].set(f32(X[P + 'left'](h), m), s + i);
          res[1].set(f32(X[P + 'right'](h), m), s + i);
          i += m;
        }
      });
      X[P + 'destroy'](h);
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
      for (let f = -7; f <= 7; f++)
        for (let minor = 0; minor < 2; minor++) {
          const id = `fifths${f}${minor ? 'm' : ''}`;
          X.mu_key_from_fifths(f, minor, intBuf);
          const t = i32(intBuf, 2).slice();
          out.push([id + '.key', mtext((b, cap) => X.mu_key_name(t[0], t[1], minor, b, cap))]);
          out.push([id + '.signature', Array.from({ length: 7 }, (_, l) => X.mu_signature_alter(l, f)).join(' ')]);
          out.push([id + '.written2', X.mu_transpose_fifths(f, 2)]);
          out.push([id + '.written9', X.mu_transpose_fifths(f, 9)]);
          for (let dir = -1; dir <= 1; dir++) {
            const line = [];
            for (let pc = 0; pc < 12; pc++) { X.mu_spell_moving(pc, f, minor, dir, intBuf); line.push(spelledAt(intBuf)); }
            out.push([`${id}.moving${dir}`, line.join(' ')]);
          }
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
    case 'theory': {
      // each request's answer from th_call, named as the native results are
      nums('requests').forEach((req, i) => {
        const q = text(req);
        let cap = 1 << 16, b = X.malloc(cap), n = X.th_call(q, b, cap);
        if (n < 0) { X.free(b); cap = -n; b = X.malloc(cap); n = X.th_call(q, b, cap); }
        out.push([`${JSON.parse(req).fn}.${i}`, cstr(b)]);
        X.free(b);
        X.free(q);
      });
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
    case 'laws': {
      const two = X.malloc(8);
      const laws = ['linear', '3dB', '2.5dB', '4.5dB', '6dB'];
      laws.forEach((name, law) => (c.pans ?? []).forEach((pan, i) => {
        X.pr_pan_gains(0.8, pan, law, two);
        const g = f32(two, 2);
        out.push([`pan.${name}.${i}.l`, g[0]], [`pan.${name}.${i}.r`, g[1]]);
      }));
      (c.positions ?? []).forEach((p, i) => out.push([`fader.pos${i}.db`, X.ms_fader_position_to_db(p)], [`fader.pos${i}.gain`, X.ms_fader_position_to_gain(p)], [`amp.pos${i}.db`, X.ms_amp_db(p)]));
      (c.dbs ?? []).forEach((db, i) => out.push([`fader.db${i}.pos`, X.ms_db_to_fader_position(db)],
                                                [`fader.db${i}.fromGain`, X.ms_gain_to_fader_position(Math.fround(Math.pow(10, db * 0.05)))]));
      for (let curve = 0; curve < 5; curve++)
        (c.ts ?? []).forEach((t, i) => out.push([`fade.${curve}.${i}.gain`, X.pr_fade_gain(curve, t)], [`fade.${curve}.${i}.out`, X.pr_fade_out_gain(curve, t)],
                                                [`fade.${curve}.${i}.float`, X.pr_fade_gain_float(curve, t)]));
      (c.ts ?? []).forEach((t, i) => { X.pr_crossfade_gains(t, two); const g = f32(two, 2); out.push([`xfade.${i}.first`, g[0]], [`xfade.${i}.second`, g[1]]); });
      (c.curves ?? []).forEach((amount, k) => (c.xs ?? []).forEach((x, i) => out.push(
        [`curve.up.${k}.${i}`, X.pr_curve_value(x, 0, 0.2, amount, 1, 0.9)], [`curve.down.${k}.${i}`, X.pr_curve_value(x, 0, 0.9, amount, 1, 0.2)])));
      out.push(['tempo.min', X.rh_tempo_min()], ['tempo.max', X.rh_tempo_max()]);
      (c.bpms ?? []).forEach((bpm, i) => out.push([`tempo.clamp${i}`, X.rh_clamp_tempo(bpm)], [`tempo.is${i}`, X.rh_is_tempo(bpm)]));
      (c.hzs ?? []).forEach((hz, i) => out.push([`axis.hz${i}`, X.ms_axis_log_position(hz, 20, 20000)]));
      (c.positions ?? []).forEach((p, i) => out.push([`axis.at${i}`, X.ms_axis_log_hz(p, 20, 20000)]));
      const pb = c.playback ?? [];
      for (let i = 0; i + 3 < pb.length; i += 4) out.push([`playback${i / 4}`, X.ms_playback_gain(pb[i], pb[i + 1], pb[i + 2], pb[i + 3], -1)]);
      (c.rates ?? []).forEach((rate, i) => out.push([`tuner.stride${i}`, X.tp_window_stride(rate)], [`tuner.n${i}`, X.tp_window_length(rate)]));
      X.free(two);
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
    case 'song-profile': {
      const p = X.ms_prof_create(fs, n, block);
      const st = stereo ? 1 : 0;
      if ((c.stretchLength ?? 0) > 0) X.ms_prof_choose(p, c.stretchFrom ?? 0, c.stretchLength, c.whole ?? -100);
      else blocks(n, block, (s, k) => { fill(X.ms_prof_left(p), X.ms_prof_right(p), s, k); X.ms_prof_song(p, k, st); });
      const from = X.ms_prof_stretch_from(p), length = X.ms_prof_stretch_length(p);
      blocks(length, block, (s, k) => { fill(X.ms_prof_left(p), X.ms_prof_right(p), from + s, k); X.ms_prof_stretch(p, k, st); });
      X.ms_prof_finish(p);
      out.push(['from', from], ['length', length], ['lufs', X.ms_prof_value(p, 0)], ['wholeLufs', X.ms_prof_value(p, 1)], ['plr', X.ms_prof_value(p, 2)]);
      for (let b = 0; b < 10; b++) out.push([`tone${b}`, X.ms_prof_value(p, 3 + b)]);
      for (let b = 0; b < 3; b++) out.push([`width${b}`, X.ms_prof_value(p, 13 + b)]);
      X.ms_prof_destroy(p);
      break;
    }
    case 'master-monitor': {
      const m = X.mm_create(fs, block);
      X.mm_settings(m, c.style ?? 0, 0.5, 0, -14, -1, 0.5, 0, 0, 0, 0, 0, 0, 0);
      X.mm_hear_original(m, c.original ?? 0);
      if (c.level) X.mm_playback_level(m, 1, c.master ?? 0, c.orig ?? 0);
      X.mm_device(m, c.device ?? 0);
      const l = new Float32Array(n), r = new Float32Array(n);
      blocks(n, block, (s, k) => {
        f32(X.mm_left(m), k).set(sig.l.subarray(s, s + k));
        f32(X.mm_right(m), k).set((stereo ? sig.r : sig.l).subarray(s, s + k));
        X.mm_process(m, k);
        l.set(f32(X.mm_left(m), k), s);
        r.set(f32(X.mm_right(m), k), s);
      });
      const lm = X.lm_create(fs, n);
      f32(X.lm_left(lm), n).set(l);
      f32(X.lm_right(lm), n).set(r);
      X.lm_process(lm, n, 1);
      let peak = 0;
      for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(l[i]), Math.abs(r[i]));
      out.push(['lufs', X.lm_integrated(lm)], ['peak', peak], ['matchDb', X.mm_get(m, 14)]);
      X.lm_destroy(lm);
      X.mm_destroy(m);
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
