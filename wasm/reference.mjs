// The reference set (data/reference.json) through master.wasm: makes each
// case's input with rf_render, runs it through the exported functions the
// browser calls (lm_*, ms_*, tp_*, pi_*, rh_*, mt_*, sd_*) and checks every
// expected result within its tolerance, as tests/ReferenceTests.cpp does
// natively. Fails when a result is off, when an input is not the one the file
// names (its hash), when the file has a result this run did not produce, or
// when this run produced one the file does not have.
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
const TYPES = { tone: 0, noise: 1, samples: 2 };

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
      out.push([`unknown op ${c.op}`, '']);
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
for (const c of file.cases) {
  const before = failures, checksBefore = checks;
  let sig = null;
  if (c.input) {
    sig = makeInput(c.input);
    const h = hash(sig);
    report(h === c.inputHash, `${c.id} input`, `made ${h}, the file names ${c.inputHash}`);
  }
  const got = new Map(run(c, sig));
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
console.log(`\n${failures === 0 ? 'ALL PASSED' : 'SOME FAILED'} (${checks} checks, ${failures} failed)`);
process.exit(failures === 0 ? 0 : 1);
