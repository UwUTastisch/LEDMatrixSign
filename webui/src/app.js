/* LED Matrix Sign 2.0 — web UI.
 *
 * Built into a single self-contained index.html by build.py. Put that file at
 * /index.html on the sign's SD card or LittleFS image and the firmware serves
 * it at http://<sign>/ — every API call then goes to the same origin.
 *
 * API reference: ledmatrixsign-api-2.0.yaml / README_2.0.md in the firmware.
 */
'use strict';

// =====================================================================
// Constants
// =====================================================================

const CELL_PX = 20; // overlay drawing-buffer pixels per LED (independent of zoom)
const POLL_MS = { frame: 1200, composition: 2000, status: 2500 };
const DEFAULT_AP_ADDRESS = 'http://4.3.2.1'; // softAP IP from src/config.h

// Fonts the firmware registers in gfx/font.h. 10x14 and 12x16_serif are
// integer-scaled 5x7 in the current firmware, so the preview matches.
const FONTS = {
  '5x7': { scale: 1, spacing: 1 },
  '10x14': { scale: 2, spacing: 1 },
  '12x16_serif': { scale: 2, spacing: 2 },
};

// gfx/font5x7.h — ASCII 0x20..0x7E, 5 column bytes per glyph, bit n = row n.
// Regenerate from the firmware with: python3 build.py --font path/to/font5x7.h
const FONT_TABLE = (() => {
  const hex = ''
  + '000000000000005f00000007000700147f147f14242a7f2a12631328664136494936500004030000001c2241000041221c00'
  + '2a1c3e1c2a08083e080800403000000808080808000060000040201c02013659414d3600427f400042615149462141454b31'
  + '1814127f1027454545393c4a49493001710905033649494936064949291e0000360000004036000008142241001414141414'
  + '004122140802015109063e415d551e7e0909097e7f494949363e414141227f4141221c7f494949417f090909013e4149493a'
  + '7f0808087f00417f41003040413f017f081422417f404040407f0204027f7f0204087f3e4141413e7f090909063e4151215e'
  + '7f09192946264949493201017f01013f4040403f1f2040201f7f2018207f631408146303047804036151494543007f414100'
  + '01061820400041417f000402010204404040404000010204007e0909097e7f494949363e414141227f4141221c7f49494941'
  + '7f090909013e4149493a7f0808087f00417f41003040413f017f081422417f404040407f0204027f7f0204087f3e4141413e'
  + '7f090909063e4151215e7f09192946264949493201017f01013f4040403f1f2040201f7f2018207f63140814630304780403'
  + '61514945430814634100000077000000416314080804080804';
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i += 1) bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
  return bytes;
})();

const PALETTE = ['#ff0000', '#ff8800', '#ffff00', '#00ff00', '#00ffff', '#0000ff', '#ff00ff', '#ffffff', '#000000'];

// =====================================================================
// State
// =====================================================================

const state = {
  width: 32,
  height: 16,
  zoom: 16,
  userSetZoom: false,
  sizeKnown: false,

  activeTool: 'text',
  color: { mode: 'solid', a: '#ff0000', b: '#0000ff', alpha: 255, angle: 90 },

  anims: [], // [{ id, assets: [] }] from /file/ls
  selectedAsset: '', // "<animId>/<file>"
  animMeta: new Map(), // animId -> Promise<{ doc, params:{}, placeholders:[] }>
  // Animations with assets but no anim.json yet — uploading an asset creates
  // the folder, so this is the normal state until the animation is saved.
  noDocAnims: new Set(),

  overlayLayers: [], // /framebuffer/getcomposition .api
  primaryLayers: [], // /framebuffer/getcomposition .primary
  objectStore: {}, // objname -> { key, fields } — values of objects drawn here
  mutationSeq: 0, // bumped on every overlay change; stale polls are dropped

  undoStack: [],
  redoStack: [],
  editingLayer: null,

  drag: null,
  selected: null, // objname of the object picked with the select tool
  hoverObject: null, // objname under the pointer in select mode
  preview: null, // { key, fields, box? } ghost drawn on the editor canvas
  hoverCell: null,
  highlight: null, // { x, y, w, h } bounds of the object being edited

  timeline: [],
  status: { running: false, paused: false, speed: 1, animname: '' },
  conn: { ok: null, failures: 0 },
  deviceInfo: {},
};

const canvasFrame = document.getElementById('canvasFrame');
const canvas = document.getElementById('frameCanvas');
const liveCanvas = document.getElementById('liveCanvas');
const ctx = canvas.getContext('2d');
const liveCtx = liveCanvas.getContext('2d');

// =====================================================================
// Small helpers
// =====================================================================

const $ = (id) => document.getElementById(id);

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function num(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

let nameCounter = 0;
function makeObjname(prefix) {
  nameCounter = (nameCounter + 1) % 1296;
  return `${prefix}_${Date.now().toString(36)}${nameCounter.toString(36)}`;
}

function formatBytes(bytes) {
  const n = Number(bytes) || 0;
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function debounce(fn, ms) {
  let timer = null;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
}

function splitAssetRef(ref) {
  const i = String(ref).indexOf('/');
  return i > 0 ? { anim: ref.slice(0, i), file: ref.slice(i + 1) } : { anim: '', file: ref };
}

// Strip "/anim/" from the id the firmware reports in /anim/status.
function animIdFromStatus(name) {
  return String(name || '').replace(/^\/?anim\//, '');
}

// =====================================================================
// Toasts + connection indicator
// =====================================================================

function toast(message, kind = 'ok', ms = 3600) {
  const host = $('toastHost');
  const el = document.createElement('div');
  el.className = `toast ${kind}`;
  el.textContent = message;
  host.appendChild(el);
  while (host.children.length > 3) host.firstChild.remove();
  setTimeout(() => el.remove(), kind === 'error' ? ms + 2000 : ms);
}

function fail(error) {
  const message = error && error.message ? error.message : String(error);
  toast(message, 'error');
  if (error && error.network) noteConnection(false);
}

function setConnState(ok) {
  const el = $('connState');
  if (ok === null) {
    el.className = 'conn-state';
    el.innerHTML = '<span class="dot"></span>Connecting…';
    return;
  }
  el.className = `conn-state ${ok ? 'ok' : 'error'}`;
  el.innerHTML = `<span class="dot"></span>${ok ? 'Connected' : 'Offline'}`;
}

// Polls fail now and then on a busy ESP32 — only show "Offline" after two
// misses in a row so the indicator doesn't flicker.
function noteConnection(ok) {
  if (ok) {
    state.conn.failures = 0;
    if (state.conn.ok !== true) {
      state.conn.ok = true;
      setConnState(true);
    }
    return;
  }
  state.conn.failures += 1;
  if (state.conn.failures >= 2 && state.conn.ok !== false) {
    state.conn.ok = false;
    setConnState(false);
  }
}

// =====================================================================
// API
// =====================================================================

function defaultApiBase() {
  const fromQuery = new URLSearchParams(location.search).get('api');
  if (fromQuery) return fromQuery;
  if (location.protocol === 'file:') return DEFAULT_AP_ADDRESS;
  return '';
}

const api = { base: defaultApiBase() };

function apiUrl(path) {
  return `${api.base.trim().replace(/\/+$/, '')}${path}`;
}

function isCrossOrigin() {
  if (!api.base.trim()) return false;
  try {
    return new URL(api.base, location.href).origin !== location.origin;
  } catch (e) {
    return true;
  }
}

function describeTarget() {
  return api.base.trim() || location.origin;
}

// All endpoints speak JSON. For cross-origin use (page opened from disk) the
// body goes out as text/plain: that keeps it a "simple" request with no CORS
// preflight, and the firmware parses the body as JSON regardless.
async function request(path, { method = 'GET', body, as = 'json', timeout = 9000 } = {}) {
  const init = { method, headers: {} };
  if (body !== undefined) {
    init.body = typeof body === 'string' ? body : JSON.stringify(body);
    init.headers['Content-Type'] = isCrossOrigin() ? 'text/plain;charset=UTF-8' : 'application/json';
  }
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  init.signal = controller.signal;

  let response;
  try {
    response = await fetch(apiUrl(path), init);
  } catch (e) {
    const error = new Error(e.name === 'AbortError'
      ? `The sign didn't answer within ${Math.round(timeout / 1000)} s.`
      : `Can't reach the sign at ${describeTarget()}. Check the Wi-Fi connection or the address in Settings.`);
    error.network = true;
    throw error;
  } finally {
    clearTimeout(timer);
  }

  if (as === 'buffer') {
    if (!response.ok) throw new Error(`Download failed (HTTP ${response.status})`);
    return response.arrayBuffer();
  }

  const text = await response.text();
  let data = text;
  try {
    data = text ? JSON.parse(text) : {};
  } catch (e) {
    if (as === 'json' && response.ok) {
      const error = new Error(`Unexpected reply from ${path} — is the address in Settings pointing at the sign?`);
      error.network = true;
      throw error;
    }
  }
  if (!response.ok) {
    const reason = data && typeof data === 'object' && data.error ? data.error : response.statusText || 'request failed';
    throw new Error(`${reason} (HTTP ${response.status} from ${path})`);
  }
  return data;
}

const apiGet = (path, options) => request(path, options);
const apiPost = (path, body, options) => request(path, { ...options, method: 'POST', body });
const downloadUrl = (path) => apiUrl(`/file/download?path=${encodeURIComponent(path)}`);

// =====================================================================
// Colors — mirrors gfx/color.h so previews match the panel
// =====================================================================

function parseHex(input) {
  const s = String(input || '').trim();
  if (s.length < 4 || s[0] !== '#') return null;
  const h = s.slice(1);
  if (!/^[0-9a-fA-F]+$/.test(h)) return null;
  if (h.length === 3 || h.length === 4) {
    const n = [...h].map((c) => parseInt(c, 16) * 17);
    return { r: n[0], g: n[1], b: n[2], a: h.length === 4 ? n[3] : 255 };
  }
  if (h.length === 6 || h.length === 8) {
    const n = [0, 2, 4, 6].map((i) => (i < h.length ? parseInt(h.substr(i, 2), 16) : 255));
    return { r: n[0], g: n[1], b: n[2], a: n[3] };
  }
  return null;
}

const WHITE = { r: 255, g: 255, b: 255, a: 255 };

function parseColorSpec(raw) {
  const s = String(raw ?? '').trim();
  if (/^linear-gradient/i.test(s)) {
    const lp = s.indexOf('(');
    const rp = s.lastIndexOf(')');
    if (lp >= 0 && rp > lp) {
      const spec = { gradient: true, angle: 0, stops: [] };
      s.slice(lp + 1, rp).split(',').map((t) => t.trim()).forEach((tok) => {
        if (tok.endsWith('deg')) spec.angle = parseFloat(tok) || 0;
        else if (tok.startsWith('#')) {
          const c = parseHex(tok);
          if (c) spec.stops.push(c);
        }
      });
      if (!spec.stops.length) spec.stops.push(WHITE);
      if (spec.stops.length === 1) spec.gradient = false;
      return spec;
    }
  }
  const c = parseHex(s);
  return { gradient: false, angle: 0, stops: [c || WHITE] };
}

function isValidColorSpec(raw) {
  const s = String(raw ?? '').trim();
  return Boolean(parseHex(s)) || /^linear-gradient\s*\(/i.test(s);
}

function colorAt(spec, u, v) {
  if (!spec.gradient || spec.stops.length < 2) return spec.stops[0] || WHITE;
  const rad = (spec.angle * Math.PI) / 180;
  const dx = Math.sin(rad);
  const dy = -Math.cos(rad);
  const t = clamp((u - 0.5) * dx + (v - 0.5) * dy + 0.5, 0, 1);
  const scaled = t * (spec.stops.length - 1);
  const i = Math.floor(scaled);
  if (i >= spec.stops.length - 1) return spec.stops[spec.stops.length - 1];
  const f = scaled - i;
  const a = spec.stops[i];
  const b = spec.stops[i + 1];
  const lerp = (x, y) => Math.trunc(x + (y - x) * f);
  return { r: lerp(a.r, b.r), g: lerp(a.g, b.g), b: lerp(a.b, b.b), a: lerp(a.a, b.a) };
}

function hexWithAlpha(hex, alpha) {
  const base = String(hex).slice(0, 7).toLowerCase();
  return alpha >= 255 ? base : `${base}${clamp(Math.round(alpha), 0, 255).toString(16).padStart(2, '0')}`;
}

function currentColorSpec() {
  const { mode, a, b, alpha, angle } = state.color;
  if (mode === 'gradient') {
    return `linear-gradient(${Math.round(num(angle, 90))}deg, ${hexWithAlpha(a, alpha)}, ${hexWithAlpha(b, alpha)})`;
  }
  return hexWithAlpha(a, alpha);
}

// Solid color for places that can only take one (asset tint).
function currentSolidColor() {
  return hexWithAlpha(state.color.a, state.color.alpha);
}

// CSS for a swatch. Our gradient angles already follow CSS conventions.
function cssForSpec(raw) {
  const spec = parseColorSpec(raw);
  const css = (c) => `rgba(${c.r}, ${c.g}, ${c.b}, ${(c.a / 255).toFixed(3)})`;
  if (!spec.gradient) return css(spec.stops[0]);
  return `linear-gradient(${spec.angle}deg, ${spec.stops.map(css).join(', ')})`;
}

// =====================================================================
// Raster — a JS twin of gfx/framebuffer.h for ghost previews + thumbnails
// =====================================================================

function fontOf(name) {
  const f = FONTS[name] || FONTS['5x7'];
  const gw = 5 * f.scale;
  const gh = 7 * f.scale;
  return { ...f, gw, gh, advance: gw + f.spacing };
}

function glyphPixel(font, ch, gx, gy) {
  if (gx < 0 || gy < 0) return false;
  const bx = Math.floor(gx / font.scale);
  const by = Math.floor(gy / font.scale);
  if (bx >= 5 || by >= 7) return false;
  let code = ch.charCodeAt(0);
  if (!(code >= 0x20 && code <= 0x7e)) code = 0x3f; // '?'
  return ((FONT_TABLE[(code - 0x20) * 5 + bx] >> by) & 1) === 1;
}

function measureText(font, s) {
  return s.length ? s.length * font.advance - font.spacing : 0;
}

class Raster {
  constructor(w, h) {
    this.w = w;
    this.h = h;
    this.px = new Uint8ClampedArray(w * h * 4);
  }

  blend(x, y, c) {
    if (x < 0 || y < 0 || x >= this.w || y >= this.h || c.a === 0) return;
    const i = (y * this.w + x) * 4;
    const d = this.px;
    if (c.a === 255) {
      d[i] = c.r; d[i + 1] = c.g; d[i + 2] = c.b; d[i + 3] = 255;
      return;
    }
    const sa = c.a;
    const da = 255 - sa;
    d[i] = (c.r * sa + d[i] * da) / 255;
    d[i + 1] = (c.g * sa + d[i + 1] * da) / 255;
    d[i + 2] = (c.b * sa + d[i + 2] * da) / 255;
    d[i + 3] = Math.max(c.a, d[i + 3]);
  }

  line(x0, y0, x1, y1, thickness, spec) {
    const bx = Math.min(x0, x1);
    const by = Math.min(y0, y1);
    const bw = Math.abs(x1 - x0) + 1;
    const bh = Math.abs(y1 - y0) + 1;
    const t = Math.max(1, thickness);
    const dx = Math.abs(x1 - x0);
    const sx = x0 < x1 ? 1 : -1;
    const dy = -Math.abs(y1 - y0);
    const sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    let x = x0;
    let y = y0;
    for (let guard = 0; guard < 10000; guard += 1) {
      for (let oy = 0; oy < t; oy += 1) {
        for (let ox = 0; ox < t; ox += 1) {
          const px = x + ox;
          const py = y + oy;
          const u = bw > 1 ? (px - bx) / (bw - 1) : 0;
          const v = bh > 1 ? (py - by) / (bh - 1) : 0;
          this.blend(px, py, colorAt(spec, u, v));
        }
      }
      if (x === x1 && y === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x += sx; }
      if (e2 <= dx) { err += dx; y += sy; }
    }
  }

  rect(x, y, dx, dy, border, spec) {
    if (dx <= 0 || dy <= 0) return;
    for (let j = 0; j < dy; j += 1) {
      for (let i = 0; i < dx; i += 1) {
        const edge = i < border || j < border || i >= dx - border || j >= dy - border;
        if (border > 0 && !edge) continue;
        const u = dx > 1 ? i / (dx - 1) : 0;
        const v = dy > 1 ? j / (dy - 1) : 0;
        this.blend(x + i, y + j, colorAt(spec, u, v));
      }
    }
  }

  text(fontName, s, x, y, spec, clip) {
    const font = fontOf(fontName);
    const total = measureText(font, s);
    let penX = x;
    for (const ch of s) {
      for (let gy = 0; gy < font.gh; gy += 1) {
        for (let gx = 0; gx < font.gw; gx += 1) {
          if (!glyphPixel(font, ch, gx, gy)) continue;
          const ax = penX + gx;
          const ay = y + gy;
          if (clip && (ax < clip.x || ax >= clip.x + clip.w || ay < clip.y || ay >= clip.y + clip.h)) continue;
          const u = total > 1 ? (ax - x) / (total - 1) : 0;
          const v = font.gh > 1 ? gy / (font.gh - 1) : 0;
          this.blend(ax, ay, colorAt(spec, u, v));
        }
      }
      penX += font.advance;
    }
  }

  blit(asset, x, y, tint) {
    for (let j = 0; j < asset.h; j += 1) {
      for (let i = 0; i < asset.w; i += 1) {
        const p = (j * asset.w + i) * 4;
        const r = asset.rgba[p];
        const g = asset.rgba[p + 1];
        const b = asset.rgba[p + 2];
        const a = asset.rgba[p + 3];
        if (tint) {
          const lum = (r * 77 + g * 150 + b * 29) >> 8;
          this.blend(x + i, y + j, { r: tint.r, g: tint.g, b: tint.b, a: Math.trunc((lum * a) / 255) });
        } else {
          this.blend(x + i, y + j, { r, g, b, a });
        }
      }
    }
  }

  missing(x, y) {
    for (let j = 0; j < 4; j += 1) {
      for (let i = 0; i < 4; i += 1) {
        this.blend(x + i, y + j, (i + j) & 1 ? { r: 0, g: 0, b: 0, a: 255 } : { r: 160, g: 0, b: 200, a: 255 });
      }
    }
  }

  toCanvas(target) {
    const cv = target || document.createElement('canvas');
    cv.width = this.w;
    cv.height = this.h;
    cv.getContext('2d').putImageData(new ImageData(this.px, this.w, this.h), 0, 0);
    return cv;
  }
}

// Rasterise one drawable the way the firmware would. Scrolling text is shown
// with its first character at the left of its window (phase paused).
function rasterizeObject(raster, key, f) {
  const spec = parseColorSpec(f.color);
  const x = num(f.x, 0);
  const y = num(f.y, 0);
  switch (key) {
    case 'text':
      raster.text(f.font || '5x7', String(f.text ?? ''), x, y, spec);
      break;
    case 'scrolling_text': {
      const clip = { x, y, w: num(f.dx, 0), h: num(f.dy, 0) };
      raster.text(f.font || '5x7', String(f.text ?? ''), x, y, spec, clip);
      break;
    }
    case 'line':
      raster.line(x, y, x + num(f.dx, 0), y + num(f.dy, 0), num(f.thickness, 1), spec);
      break;
    case 'rectangle':
      raster.rect(x, y, num(f.dx, 0), num(f.dy, 0), num(f.border, 0), spec);
      break;
    case 'asset': {
      const cached = assetCache.get(f.name);
      if (cached && cached.asset && cached.asset.compatible) {
        const tint = f.color ? (parseColorSpec(f.color).stops[0] || WHITE) : null;
        raster.blit(cached.asset, x, y, tint);
      } else if (cached && cached.asset) {
        raster.missing(x, y);
      } else {
        ensureAsset(f.name).then(() => renderOverlay()).catch(() => {});
      }
      break;
    }
    default:
      break;
  }
}

// Bounding box of a drawable, for highlighting it on the canvas.
function objectBounds(key, f) {
  const x = num(f.x, 0);
  const y = num(f.y, 0);
  switch (key) {
    case 'text': {
      const font = fontOf(f.font);
      return { x, y, w: Math.max(1, measureText(font, String(f.text ?? ''))), h: font.gh };
    }
    case 'scrolling_text':
    case 'rectangle':
      return { x, y, w: Math.max(1, num(f.dx, 0)), h: Math.max(1, num(f.dy, 0)) };
    case 'line': {
      const t = Math.max(1, num(f.thickness, 1));
      const x1 = x + num(f.dx, 0);
      const y1 = y + num(f.dy, 0);
      return { x: Math.min(x, x1), y: Math.min(y, y1), w: Math.abs(x1 - x) + t, h: Math.abs(y1 - y) + t };
    }
    case 'asset': {
      const cached = assetCache.get(f.name);
      const a = cached && cached.asset;
      return { x, y, w: a ? a.w : 4, h: a ? a.h : 4 };
    }
    default:
      return null;
  }
}

// =====================================================================
// BMP — decode anything the sign may hold, encode what it can read
// =====================================================================

// Returns { w, h, rgba (top-down RGBA), bpp, compatible }. `compatible` is
// what gfx/asset.h accepts: 32 bpp, BI_RGB or BI_BITFIELDS.
function decodeBmp(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < 54 || view.getUint16(0, true) !== 0x4d42) throw new Error('Not a BMP file');
  const dataOffset = view.getUint32(10, true);
  const dibSize = view.getUint32(14, true);
  if (dibSize < 40) throw new Error('Unsupported BMP header');
  const w = view.getInt32(18, true);
  const rawH = view.getInt32(22, true);
  const bpp = view.getUint16(28, true);
  const compression = view.getUint32(30, true);
  if (![24, 32].includes(bpp) || ![0, 3].includes(compression)) {
    throw new Error(`Unsupported BMP (${bpp} bpp, compression ${compression})`);
  }
  const h = Math.abs(rawH);
  const bottomUp = rawH > 0;
  const bytesPP = bpp / 8;
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  const rgba = new Uint8ClampedArray(w * h * 4);
  const src = new Uint8Array(buffer);
  for (let sy = 0; sy < h; sy += 1) {
    const dy = bottomUp ? h - 1 - sy : sy;
    const row = dataOffset + sy * rowSize;
    for (let x = 0; x < w; x += 1) {
      const p = row + x * bytesPP;
      const o = (dy * w + x) * 4;
      rgba[o] = src[p + 2];
      rgba[o + 1] = src[p + 1];
      rgba[o + 2] = src[p];
      rgba[o + 3] = bytesPP === 4 ? src[p + 3] : 255;
    }
  }
  return { w, h, rgba, bpp, compatible: bpp === 32 };
}

// 32-bit BGRA8888, BITMAPINFOHEADER, BI_RGB, bottom-up — the exact layout
// tools/bmp_bgra.py writes and gfx/asset.h reads.
function encodeBmp32(w, h, rgba) {
  const pixelBytes = w * h * 4;
  const buffer = new ArrayBuffer(54 + pixelBytes);
  const view = new DataView(buffer);
  view.setUint16(0, 0x4d42, true);
  view.setUint32(2, 54 + pixelBytes, true);
  view.setUint32(10, 54, true);
  view.setUint32(14, 40, true);
  view.setInt32(18, w, true);
  view.setInt32(22, h, true);
  view.setUint16(26, 1, true);
  view.setUint16(28, 32, true);
  view.setUint32(30, 0, true);
  view.setUint32(34, pixelBytes, true);
  view.setInt32(38, 2835, true);
  view.setInt32(42, 2835, true);
  const out = new Uint8Array(buffer);
  let o = 54;
  for (let y = h - 1; y >= 0; y -= 1) {
    for (let x = 0; x < w; x += 1) {
      const i = (y * w + x) * 4;
      out[o] = rgba[i + 2];
      out[o + 1] = rgba[i + 1];
      out[o + 2] = rgba[i];
      out[o + 3] = rgba[i + 3];
      o += 4;
    }
  }
  return out;
}

function bytesToBase64(bytes) {
  let binary = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

function sanitizeFileName(name, fallback = 'asset') {
  const base = String(name || '').trim().replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^[._]+/, '');
  return `${base || fallback}.bmp`;
}

function sanitizeAnimName(name) {
  return String(name || '').trim().replace(/[\s/\\]+/g, '_').replace(/[^A-Za-z0-9._-]/g, '');
}

// Asset cache: "<anim>/<file>" -> { promise, asset }
const assetCache = new Map();

function ensureAsset(ref) {
  if (!ref) return Promise.reject(new Error('No asset'));
  let entry = assetCache.get(ref);
  if (entry) return entry.promise;
  const { anim, file } = splitAssetRef(ref);
  entry = { asset: null, error: null, promise: null };
  entry.promise = request(`/file/download?path=${encodeURIComponent(`/anim/${anim}/assets/${file}`)}`, { as: 'buffer' })
    .then((buffer) => {
      entry.asset = decodeBmp(buffer);
      return entry.asset;
    })
    .catch((error) => {
      entry.error = error;
      throw error;
    });
  assetCache.set(ref, entry);
  return entry.promise;
}

function uploadAssetBytes(animName, fileName, bytes) {
  return apiPost('/file/uploadasset', { animname: animName, filename: fileName, data: bytesToBase64(bytes) })
    .then(() => {
      assetCache.delete(`${animName}/${fileName}`);
    });
}

// =====================================================================
// Canvas geometry
// =====================================================================

// The live canvas holds one pixel per LED and is scaled up by CSS; the editor
// canvas on top has CELL_PX pixels per LED for the grid and previews.
function applyFrameGeometry() {
  canvasFrame.style.aspectRatio = `${state.width} / ${state.height}`;
  canvasFrame.style.width = `${state.width * state.zoom}px`;
  const bw = state.width * CELL_PX;
  const bh = state.height * CELL_PX;
  if (canvas.width !== bw) canvas.width = bw;
  if (canvas.height !== bh) canvas.height = bh;
  if (liveCanvas.width !== state.width) liveCanvas.width = state.width;
  if (liveCanvas.height !== state.height) liveCanvas.height = state.height;
  $('matrixSize').textContent = `${state.width} × ${state.height}`;
}

function computeFitZoom() {
  const stage = $('canvasStage');
  const availW = stage.clientWidth - 22;
  const availH = stage.clientHeight - 22;
  if (availW <= 0 || availH <= 0) return state.zoom;
  const fit = Math.floor(Math.min(availW / state.width, availH / state.height));
  return clamp(fit || 16, 4, 48);
}

function setZoom(value, fromUser) {
  state.zoom = clamp(Math.round(value), 4, 48);
  if (fromUser) state.userSetZoom = true;
  $('zoomRange').value = String(state.zoom);
  $('zoomValue').textContent = `${state.zoom}px`;
  applyFrameGeometry();
}

function autoFitZoom() {
  if (state.userSetZoom) return;
  setZoom(computeFitZoom(), false);
}

function setMatrixSize(width, height) {
  const w = num(width, 0);
  const h = num(height, 0);
  if (w <= 0 || h <= 0) return;
  const changed = w !== state.width || h !== state.height;
  state.width = w;
  state.height = h;
  state.sizeKnown = true;
  if (changed) {
    applyFrameGeometry();
    autoFitZoom();
    renderOverlay();
    syncAssetEditorLimits();
  }
}

// =====================================================================
// Editor overlay: grid, ghost preview, highlight
// =====================================================================

const ghostCanvas = document.createElement('canvas');

function renderOverlay() {
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  // Ghost of what the current tool would draw.
  const ghost = currentGhost();
  if (ghost) {
    const raster = new Raster(state.width, state.height);
    rasterizeObject(raster, ghost.key, ghost.fields);
    raster.toCanvas(ghostCanvas);
    ctx.save();
    ctx.imageSmoothingEnabled = false;
    ctx.globalAlpha = state.drag ? 0.9 : 0.6;
    ctx.drawImage(ghostCanvas, 0, 0, w, h);
    ctx.restore();
  }

  // Grid, faint enough to leave the live image readable.
  ctx.strokeStyle = 'rgba(150, 184, 223, 0.14)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let x = 0; x <= state.width; x += 1) {
    const px = x * CELL_PX + 0.5;
    ctx.moveTo(px, 0);
    ctx.lineTo(px, h);
  }
  for (let y = 0; y <= state.height; y += 1) {
    const py = y * CELL_PX + 0.5;
    ctx.moveTo(0, py);
    ctx.lineTo(w, py);
  }
  ctx.stroke();

  if (state.activeTool === 'select') renderHitboxes();

  const boxes = [];
  if (ghost && ghost.box) boxes.push({ ...ghost.box, color: 'rgba(119, 241, 208, 0.9)' });
  if (state.highlight) boxes.push({ ...state.highlight, color: 'rgba(101, 204, 255, 0.95)' });
  boxes.forEach((b) => {
    ctx.save();
    ctx.setLineDash([CELL_PX / 3, CELL_PX / 4]);
    ctx.lineWidth = 2;
    ctx.strokeStyle = b.color;
    ctx.strokeRect(b.x * CELL_PX + 1, b.y * CELL_PX + 1, b.w * CELL_PX - 2, b.h * CELL_PX - 2);
    ctx.restore();
  });
}

// One box per movable object: faint normally, brighter under the pointer, and
// solid with corner handles when selected.
function renderHitboxes() {
  const dragRef = state.drag && state.drag.mode === 'moveObj' ? state.drag.ref : null;
  hitboxes().forEach(({ ref, bounds }) => {
    const selected = ref === state.selected;
    const hovered = ref === state.hoverObject;
    const x = bounds.x * CELL_PX;
    const y = bounds.y * CELL_PX;
    const w = bounds.w * CELL_PX;
    const h = bounds.h * CELL_PX;
    ctx.save();
    if (selected) {
      ctx.strokeStyle = 'rgba(119, 241, 208, 0.95)';
      ctx.lineWidth = 2;
      if (ref === dragRef) ctx.setLineDash([CELL_PX / 3, CELL_PX / 4]); // where it started
    } else {
      ctx.strokeStyle = hovered ? 'rgba(101, 204, 255, 0.95)' : 'rgba(101, 204, 255, 0.55)';
      ctx.lineWidth = hovered ? 2 : 1.5;
    }
    if (hovered && !selected) {
      ctx.fillStyle = 'rgba(101, 204, 255, 0.1)';
      ctx.fillRect(x, y, w, h);
    }
    ctx.strokeRect(x + 1, y + 1, Math.max(2, w - 2), Math.max(2, h - 2));
    if (selected && ref !== dragRef) {
      const s = Math.max(3, CELL_PX / 3);
      ctx.fillStyle = 'rgba(119, 241, 208, 0.95)';
      [[x, y], [x + w - s, y], [x, y + h - s], [x + w - s, y + h - s]]
        .forEach(([hx, hy]) => ctx.fillRect(hx, hy, s, s));
    }
    ctx.restore();
  });
}

// What would be drawn right now: the drag in progress, or a hover preview for
// click-to-place tools.
function currentGhost() {
  if (state.preview) return state.preview;
  const cell = state.hoverCell;
  if (!cell || state.drag) return null;
  if (state.activeTool === 'text') {
    return { key: 'text', fields: textFields(cell) };
  }
  if (state.activeTool === 'asset' && state.selectedAsset) {
    return { key: 'asset', fields: assetFields(cell) };
  }
  return null;
}

function getCellFromPointer(event) {
  const rect = canvas.getBoundingClientRect();
  const px = ((event.clientX - rect.left) / rect.width) * state.width;
  const py = ((event.clientY - rect.top) / rect.height) * state.height;
  return {
    x: clamp(Math.floor(px), 0, state.width - 1),
    y: clamp(Math.floor(py), 0, state.height - 1),
  };
}

// Rectangles and scroll windows include both corner cells, so a drag from
// column 2 to column 5 is 4 wide and a single click is 1×1. (The firmware
// draws nothing for a 0-wide rectangle.)
function boxExtent(a, b) {
  return {
    x: Math.min(a.x, b.x),
    y: Math.min(a.y, b.y),
    dx: Math.abs(b.x - a.x) + 1,
    dy: Math.abs(b.y - a.y) + 1,
  };
}

// ---------- Field builders for each tool ----------

function textFields(cell) {
  return {
    x: cell.x,
    y: cell.y,
    text: $('textInput').value || 'HELLO',
    font: $('textFont').value || '5x7',
    color: currentColorSpec(),
  };
}

function scrollFields(extent) {
  return {
    x: extent.x,
    y: extent.y,
    dx: extent.dx,
    dy: extent.dy,
    text: $('scrollTextInput').value || 'Hello',
    font: $('scrollFont').value || '5x7',
    color: currentColorSpec(),
    scroll_speed: num($('scrollSpeed').value, 12),
    scroll_direction: $('scrollDirection').value === 'vertical' ? 'vertical' : 'horizontal',
  };
}

// A single click with the scroll tool makes a window from the click to the
// right edge (horizontal) or bottom edge (vertical), one line of text tall/wide.
function scrollClickExtent(cell) {
  const font = fontOf($('scrollFont').value);
  if ($('scrollDirection').value === 'vertical') {
    return { x: cell.x, y: cell.y, dx: Math.min(font.gw, state.width - cell.x), dy: state.height - cell.y };
  }
  return { x: cell.x, y: cell.y, dx: state.width - cell.x, dy: Math.min(font.gh, state.height - cell.y) };
}

function lineFields(a, b) {
  return {
    x: a.x,
    y: a.y,
    dx: b.x - a.x,
    dy: b.y - a.y,
    thickness: clamp(num($('lineThickness').value, 1), 1, 16),
    color: currentColorSpec(),
  };
}

function rectFields(extent) {
  return { ...extent, border: clamp(num($('rectBorder').value, 1), 0, 64), color: currentColorSpec() };
}

function assetFields(cell) {
  const fields = { x: cell.x, y: cell.y, name: state.selectedAsset };
  if ($('assetTint').checked) fields.color = currentSolidColor();
  return fields;
}

// ---------- Hitboxes and selection (the select tool) ----------
// Only objects drawn in this page can have a hitbox: /framebuffer/getcomposition
// reports each object's type, id and objname but not its geometry, so anything
// drawn by another client, by a running animation, or before a reload has no
// known position to hit-test against.

function hitboxes() {
  return orderedTrackedObjects().map((o) => {
    const bounds = objectBounds(o.key, o.fields);
    return bounds ? { ref: o.fields.objname, key: o.key, fields: o.fields, bounds } : null;
  }).filter(Boolean);
}

// Topmost object containing the cell. Later objects are drawn on top, so the
// list is searched backwards.
function hitTest(cell) {
  const boxes = hitboxes();
  for (let i = boxes.length - 1; i >= 0; i -= 1) {
    const b = boxes[i].bounds;
    if (cell.x >= b.x && cell.x < b.x + b.w && cell.y >= b.y && cell.y < b.y + b.h) return boxes[i];
  }
  return null;
}

function selectedObject() {
  return state.selected ? state.objectStore[state.selected] || null : null;
}

function setSelection(ref) {
  state.selected = ref && state.objectStore[ref] ? ref : null;
  renderSelectionPanel();
  renderOverlay();
}

function renderSelectionPanel() {
  const stored = selectedObject();
  const info = $('selectionInfo');
  const fields = ['selX', 'selY'].map($);
  const buttons = ['selEditButton', 'selDuplicateButton', 'selRemoveButton'].map($);

  if (!stored) {
    const count = hitboxes().length;
    info.textContent = count
      ? `Nothing selected — click one of the ${count} object${count === 1 ? '' : 's'} on the canvas.`
      : 'Nothing to select yet — draw something first.';
    fields.forEach((el) => { el.value = ''; el.disabled = true; });
    buttons.forEach((el) => { el.disabled = true; });
  } else {
    const label = OBJECT_TYPE_LABELS[stored.key] || stored.key;
    const detail = stored.fields.text || stored.fields.name || '';
    info.innerHTML = `<span class="type-badge">${esc(label)}</span> <strong>${esc(stored.fields.objname)}</strong>${detail ? ` — ${esc(detail)}` : ''}`;
    $('selX').value = String(num(stored.fields.x, 0));
    $('selY').value = String(num(stored.fields.y, 0));
    fields.forEach((el) => { el.disabled = false; });
    buttons.forEach((el) => { el.disabled = false; });
  }

  const untracked = untrackedOverlayCount();
  const note = $('selectUntracked');
  note.hidden = untracked === 0;
  if (untracked) {
    note.textContent = `${untracked} object${untracked === 1 ? '' : 's'} on the overlay ${untracked === 1 ? 'was' : 'were'} drawn outside this page, so ${untracked === 1 ? 'its position is' : 'their positions are'} unknown and ${untracked === 1 ? 'it has' : 'they have'} no hitbox. Redraw ${untracked === 1 ? 'it' : 'them'} here to move ${untracked === 1 ? 'it' : 'them'}, or edit ${untracked === 1 ? 'it' : 'them'} in the Objects tab.`;
  }
}

// Keep an object at least partly on the matrix.
function clampPosition(bounds, x, y) {
  return {
    x: clamp(x, 1 - bounds.w, state.width - 1),
    y: clamp(y, 1 - bounds.h, state.height - 1),
  };
}

// Moving redraws the object under the same name (the API has no partial
// update). Consecutive nudges of one object collapse into a single undo step.
function moveSelectedTo(nx, ny, coalesce) {
  const stored = selectedObject();
  if (!stored) return Promise.resolve();
  const ref = stored.fields.objname;
  const bounds = objectBounds(stored.key, stored.fields) || { w: 1, h: 1 };
  const pos = clampPosition(bounds, Math.round(nx), Math.round(ny));
  if (pos.x === num(stored.fields.x, 0) && pos.y === num(stored.fields.y, 0)) {
    renderSelectionPanel();
    return Promise.resolve();
  }
  const previous = { key: stored.key, fields: { ...stored.fields } };
  const fields = { ...stored.fields, x: pos.x, y: pos.y };

  const top = state.undoStack[state.undoStack.length - 1];
  const merge = coalesce && top && top.moveOf === ref && Date.now() - top.moveAt < 1200;
  if (merge) state.undoStack.pop();
  const action = editAction(ref, stored.key, fields, merge ? top.movePrev : previous);
  action.moveOf = ref;
  action.moveAt = Date.now();
  action.movePrev = merge ? top.movePrev : previous;

  return runAction(action).then(() => {
    setSelection(ref);
  }).catch((error) => {
    if (merge) state.undoStack.push(top);
    fail(error);
  });
}

function nudgeSelection(dx, dy) {
  const stored = selectedObject();
  if (!stored) return;
  moveSelectedTo(num(stored.fields.x, 0) + dx, num(stored.fields.y, 0) + dy, true);
}

function cycleSelection(step) {
  const boxes = hitboxes();
  if (!boxes.length) return;
  const at = boxes.findIndex((b) => b.ref === state.selected);
  const next = at < 0 ? (step > 0 ? 0 : boxes.length - 1) : (at + step + boxes.length) % boxes.length;
  setSelection(boxes[next].ref);
}

function removeSelection() {
  const stored = selectedObject();
  if (!stored) return;
  const previous = { key: stored.key, fields: { ...stored.fields } };
  runAction(removeAction(stored.fields.objname, previous))
    .then(() => {
      toast(`Removed ${previous.fields.objname}`);
      setSelection(null);
    })
    .catch(fail);
}

function duplicateSelection() {
  const stored = selectedObject();
  if (!stored) return;
  const bounds = objectBounds(stored.key, stored.fields) || { w: 1, h: 1 };
  const pos = clampPosition(bounds, num(stored.fields.x, 0) + 1, num(stored.fields.y, 0) + 1);
  const objname = makeObjname(stored.key === 'rectangle' ? 'rect' : stored.key);
  const fields = { ...stored.fields, ...pos, objname };
  runAction(drawAction(stored.key, fields))
    .then(() => {
      setSelection(objname);
      toast(`Duplicated as ${objname}`);
    })
    .catch(fail);
}

// ---------- Pointer handling ----------

function onPointerDown(event) {
  if (event.button !== undefined && event.button !== 0) return;
  canvas.setPointerCapture?.(event.pointerId);

  if (state.activeTool === 'move') {
    state.drag = { mode: 'pan', x: event.clientX, y: event.clientY };
    canvas.classList.add('panning');
    return;
  }

  const cell = getCellFromPointer(event);

  if (state.activeTool === 'select') {
    const hit = hitTest(cell);
    setSelection(hit ? hit.ref : null);
    if (hit) {
      state.drag = {
        mode: 'moveObj',
        ref: hit.ref,
        start: cell,
        origin: { x: num(hit.fields.x, 0), y: num(hit.fields.y, 0) },
        bounds: hit.bounds,
        moved: false,
      };
      canvas.classList.add('dragging-object');
    }
    return;
  }

  if (state.activeTool === 'text') {
    performDraw('text', { ...textFields(cell), objname: makeObjname('text') });
    return;
  }

  if (state.activeTool === 'asset') {
    if (!state.selectedAsset) {
      toast('Pick an asset first — or draw one with ＋.', 'error');
      return;
    }
    performDraw('asset', { ...assetFields(cell), objname: makeObjname('asset') });
    return;
  }

  state.drag = { mode: 'shape', start: cell, moved: false };
  updateDragPreview(cell);
}

function onPointerMove(event) {
  if (state.drag && state.drag.mode === 'pan') {
    const stage = $('canvasStage');
    stage.scrollLeft -= event.clientX - state.drag.x;
    stage.scrollTop -= event.clientY - state.drag.y;
    state.drag.x = event.clientX;
    state.drag.y = event.clientY;
    return;
  }

  const cell = getCellFromPointer(event);
  $('cursorPos').textContent = `x ${cell.x}, y ${cell.y}`;

  if (state.drag && state.drag.mode === 'shape') {
    if (cell.x !== state.drag.start.x || cell.y !== state.drag.start.y) state.drag.moved = true;
    updateDragPreview(cell);
    return;
  }

  if (state.drag && state.drag.mode === 'moveObj') {
    updateMovePreview(cell);
    return;
  }

  const movedCell = !state.hoverCell || state.hoverCell.x !== cell.x || state.hoverCell.y !== cell.y;
  if (movedCell) state.hoverCell = cell;

  if (state.activeTool === 'select') {
    // Only re-render when the object under the pointer changes.
    const hit = hitTest(cell);
    const ref = hit ? hit.ref : null;
    canvas.classList.toggle('over-object', Boolean(ref));
    if (ref !== state.hoverObject) {
      state.hoverObject = ref;
      renderOverlay();
    }
    return;
  }

  if (movedCell && (state.activeTool === 'text' || state.activeTool === 'asset')) renderOverlay();
}

// Ghost of the dragged object at its new position, plus a marker where it
// started. The live view still shows it in the old spot until the move lands.
function updateMovePreview(cell) {
  const drag = state.drag;
  const stored = state.objectStore[drag.ref];
  if (!stored) return;
  if (cell.x !== drag.start.x || cell.y !== drag.start.y) drag.moved = true;
  const pos = clampPosition(
    drag.bounds,
    drag.origin.x + (cell.x - drag.start.x),
    drag.origin.y + (cell.y - drag.start.y),
  );
  drag.target = pos;
  state.preview = {
    key: stored.key,
    fields: { ...stored.fields, ...pos },
    box: { x: pos.x, y: pos.y, w: drag.bounds.w, h: drag.bounds.h },
  };
  $('cursorPos').textContent = `x ${pos.x}, y ${pos.y}`;
  renderOverlay();
}

function updateDragPreview(cell) {
  const start = state.drag.start;
  if (state.activeTool === 'line') {
    state.preview = { key: 'line', fields: lineFields(start, cell) };
  } else if (state.activeTool === 'rectangle') {
    state.preview = { key: 'rectangle', fields: rectFields(boxExtent(start, cell)) };
  } else if (state.activeTool === 'scroll') {
    const extent = state.drag.moved ? boxExtent(start, cell) : scrollClickExtent(start);
    state.preview = { key: 'scrolling_text', fields: scrollFields(extent), box: { x: extent.x, y: extent.y, w: extent.dx, h: extent.dy } };
  }
  renderOverlay();
}

function onPointerUp(event) {
  const drag = state.drag;
  state.drag = null;
  canvas.classList.remove('panning', 'dragging-object');

  if (drag && drag.mode === 'moveObj') {
    state.preview = null;
    if (drag.moved && drag.target) moveSelectedTo(drag.target.x, drag.target.y, false);
    else renderOverlay();
    return;
  }

  if (!drag || drag.mode !== 'shape') return;

  const cell = getCellFromPointer(event);
  if (state.activeTool === 'line') {
    performDraw('line', { ...lineFields(drag.start, cell), objname: makeObjname('line') });
  } else if (state.activeTool === 'rectangle') {
    performDraw('rectangle', { ...rectFields(boxExtent(drag.start, cell)), objname: makeObjname('rect') });
  } else if (state.activeTool === 'scroll') {
    const moved = drag.moved || cell.x !== drag.start.x || cell.y !== drag.start.y;
    const extent = moved ? boxExtent(drag.start, cell) : scrollClickExtent(drag.start);
    performDraw('scrolling_text', { ...scrollFields(extent), objname: makeObjname('scroll') });
  }
  state.preview = null;
  renderOverlay();
}

function onPointerCancel() {
  state.drag = null;
  state.preview = null;
  canvas.classList.remove('panning', 'dragging-object');
  renderOverlay();
}

function onPointerLeave() {
  if (state.drag) return; // pointer capture keeps the drag alive
  state.hoverCell = null;
  state.hoverObject = null;
  canvas.classList.remove('over-object');
  $('cursorPos').textContent = 'x –, y –';
  renderOverlay();
}

// =====================================================================
// Overlay actions with undo / redo
// =====================================================================
// Each action is a { do, undo } pair of promise-returning functions. `do`
// also runs on redo, so it must be safe to call more than once.

function markMutation() {
  state.mutationSeq += 1;
}

function postFrame(frame) {
  markMutation();
  return apiPost('/framebuffer/draw', frame);
}

function clearRefs(refs) {
  return postFrame({ clear: { obj: refs } });
}

function drawObj(key, fields) {
  return postFrame({ [key]: fields });
}

// Several objects in one request, in order (keeps their stacking order).
function drawMany(objects) {
  if (!objects.length) return Promise.resolve();
  return postFrame({ drawables: objects.map((o) => ({ [o.key]: o.fields })) });
}

function clearOverlay() {
  return postFrame({ clear: {} });
}

function drawAction(key, fields) {
  const name = fields.objname;
  const snapshot = { ...fields };
  return {
    do: () => drawObj(key, snapshot).then(() => { state.objectStore[name] = { key, fields: snapshot }; }),
    undo: () => clearRefs([name]).then(() => { delete state.objectStore[name]; }),
  };
}

function drawManyAction(objects) {
  const snapshot = objects.map((o) => ({ key: o.key, fields: { ...o.fields } }));
  return {
    do: () => drawMany(snapshot).then(() => { snapshot.forEach((o) => { state.objectStore[o.fields.objname] = o; }); }),
    undo: () => clearRefs(snapshot.map((o) => o.fields.objname)).then(() => {
      snapshot.forEach((o) => { delete state.objectStore[o.fields.objname]; });
    }),
  };
}

// `ref` is what clears the old object: its objname, or its "f<id>" handle.
function editAction(ref, key, fields, previous) {
  const name = fields.objname;
  return {
    do: () => clearRefs([ref]).then(() => drawObj(key, fields)).then(() => {
      if (ref !== name) delete state.objectStore[ref];
      state.objectStore[name] = { key, fields };
    }),
    undo: () => clearRefs([name]).then(() => (previous ? drawObj(previous.key, previous.fields) : null)).then(() => {
      delete state.objectStore[name];
      if (previous) state.objectStore[previous.fields.objname] = previous;
    }),
  };
}

function removeAction(ref, previous) {
  return {
    do: () => clearRefs([ref]).then(() => { delete state.objectStore[ref]; }),
    undo: () => (previous
      ? drawObj(previous.key, previous.fields).then(() => { state.objectStore[previous.fields.objname] = previous; })
      : Promise.reject(new Error("This object was drawn outside this page, so its values aren't known and it can't be restored."))),
  };
}

// Replace the whole overlay with `objects` (null = just clear it).
function replaceOverlayAction(objects) {
  const previous = orderedTrackedObjects().map((o) => ({ key: o.key, fields: { ...o.fields } }));
  const next = (objects || []).map((o) => ({ key: o.key, fields: { ...o.fields } }));
  const restore = (list) => clearOverlay().then(() => drawMany(list)).then(() => {
    state.objectStore = {};
    list.forEach((o) => { state.objectStore[o.fields.objname] = o; });
  });
  return { do: () => restore(next), undo: () => restore(previous) };
}

function updateHistoryButtons() {
  $('undoButton').disabled = state.undoStack.length === 0;
  $('redoButton').disabled = state.redoStack.length === 0;
}

function afterMutation() {
  updateHistoryButtons();
  if (state.activeTool === 'select') renderSelectionPanel();
  loadLiveView();
  refreshComposition();
}

function runAction(action) {
  return action.do().then(() => {
    state.undoStack.push(action);
    if (state.undoStack.length > 200) state.undoStack.shift();
    state.redoStack = [];
    afterMutation();
  });
}

function undo() {
  const action = state.undoStack.pop();
  if (!action) return;
  action.undo().then(() => {
    state.redoStack.push(action);
    afterMutation();
  }).catch((error) => {
    state.undoStack.push(action);
    fail(error);
  });
}

function redo() {
  const action = state.redoStack.pop();
  if (!action) return;
  action.do().then(() => {
    state.undoStack.push(action);
    afterMutation();
  }).catch((error) => {
    state.redoStack.push(action);
    fail(error);
  });
}

function performDraw(key, fields) {
  return runAction(drawAction(key, fields)).catch(fail);
}

function clearAllObjects() {
  runAction(replaceOverlayAction(null))
    .then(() => toast('Overlay cleared'))
    .catch(fail);
}

// Raw frame JSON from the Draw tab. Drawables get objnames so they show up
// as editable, undoable objects like everything else.
const DRAWABLE_KEYS = ['text', 'scrolling_text', 'line', 'rectangle', 'asset'];

function sendRawFrame() {
  let frame;
  try {
    frame = JSON.parse($('rawFrameInput').value);
  } catch (e) {
    toast(`That isn't valid JSON: ${e.message}`, 'error');
    return;
  }
  if (!frame || typeof frame !== 'object' || Array.isArray(frame)) {
    toast('Send one frame object, e.g. { "text": { … } }.', 'error');
    return;
  }

  const objects = [];
  const collect = (key, value) => {
    const list = Array.isArray(value) ? value : [value];
    list.forEach((fields) => {
      if (fields && typeof fields === 'object') {
        objects.push({ key, fields: { ...fields, objname: fields.objname || makeObjname(key === 'rectangle' ? 'rect' : key) } });
      }
    });
  };
  if (Array.isArray(frame.drawables)) {
    frame.drawables.forEach((item) => {
      const key = item && Object.keys(item)[0];
      if (DRAWABLE_KEYS.includes(key)) collect(key, item[key]);
    });
  } else {
    DRAWABLE_KEYS.forEach((key) => { if (frame[key] !== undefined) collect(key, frame[key]); });
  }

  const hasClear = frame.clear !== undefined;
  if (frame.load_anim !== undefined) toast('load_anim only works inside animations; it was ignored here.', 'error');
  if (!objects.length && !hasClear) {
    toast('No drawable objects found in that frame.', 'error');
    return;
  }

  const clearPart = hasClear ? { clear: frame.clear } : null;
  const before = { ...state.objectStore };
  const action = {
    do: () => (clearPart ? postFrame(clearPart) : Promise.resolve()).then(() => drawMany(objects)).then(() => {
      if (clearPart) pruneStoreAfterClear(clearPart.clear);
      objects.forEach((o) => { state.objectStore[o.fields.objname] = o; });
    }),
    undo: () => clearRefs(objects.map((o) => o.fields.objname)).then(() => {
      objects.forEach((o) => { delete state.objectStore[o.fields.objname]; });
      if (!clearPart) return null;
      const lost = Object.values(before).filter((o) => !state.objectStore[o.fields.objname]);
      return drawMany(lost).then(() => { lost.forEach((o) => { state.objectStore[o.fields.objname] = o; }); });
    }),
  };
  runAction(action)
    .then(() => toast(objects.length ? `Sent ${objects.length} object${objects.length === 1 ? '' : 's'}` : 'Sent clear'))
    .catch(fail);
}

function pruneStoreAfterClear(clear) {
  if (!clear || !Array.isArray(clear.obj)) {
    state.objectStore = {};
    return;
  }
  clear.obj.forEach((ref) => { delete state.objectStore[ref]; });
}

// =====================================================================
// Live view
// =====================================================================

let liveBusy = false;

function loadLiveView() {
  if (liveBusy) return Promise.resolve();
  liveBusy = true;
  return apiGet('/framebuffer/get')
    .then((data) => {
      if (data && data.data) {
        setMatrixSize(data.width || state.width, data.height || state.height);
        renderLiveFrame(data);
      }
      noteConnection(true);
    })
    .catch((error) => { if (error.network) noteConnection(false); })
    .finally(() => { liveBusy = false; });
}

function renderLiveFrame(data) {
  const w = Number(data.width || state.width);
  const h = Number(data.height || state.height);
  const bin = atob(data.data);
  const n = Math.min(bin.length, w * h * 4);
  const img = liveCtx.createImageData(w, h);
  const out = img.data;
  // BGRA → RGBA. Pixels with nothing drawn (alpha 0) stay transparent and
  // show the dark stage behind, like an unlit LED.
  for (let i = 0; i + 3 < n; i += 4) {
    out[i] = bin.charCodeAt(i + 2);
    out[i + 1] = bin.charCodeAt(i + 1);
    out[i + 2] = bin.charCodeAt(i);
    out[i + 3] = bin.charCodeAt(i + 3);
  }
  if (liveCanvas.width !== w) liveCanvas.width = w;
  if (liveCanvas.height !== h) liveCanvas.height = h;
  liveCtx.putImageData(img, 0, 0);
}

// =====================================================================
// Composition (Objects tab)
// =====================================================================

const OBJECT_TYPE_LABELS = {
  text: 'Text',
  scrolling_text: 'Scroll',
  line: 'Line',
  rectangle: 'Rect',
  asset: 'Asset',
};

function layerRef(layer) {
  return layer.objname || (layer.id ? `f${layer.id}` : '');
}

function refreshComposition() {
  const seq = state.mutationSeq;
  return apiGet('/framebuffer/getcomposition')
    .then((data) => {
      if (seq !== state.mutationSeq) return; // overlay changed mid-request
      if (data && data.width && data.height) setMatrixSize(data.width, data.height);
      state.overlayLayers = Array.isArray(data?.api) ? data.api : [];
      state.primaryLayers = Array.isArray(data?.primary) ? data.primary : [];

      // Forget values for objects that are gone (cleared elsewhere).
      const present = new Set(state.overlayLayers.map((l) => l.objname).filter(Boolean));
      Object.keys(state.objectStore).forEach((name) => { if (!present.has(name)) delete state.objectStore[name]; });
      if (state.selected && !state.objectStore[state.selected]) state.selected = null;
      if (state.activeTool === 'select') renderSelectionPanel();

      renderComposition();
    })
    .catch(() => {
      // Best effort — a failing poll shouldn't spam toasts.
    });
}

function renderComposition() {
  const tree = $('compositionTree');
  $('compositionCount').textContent = String(state.overlayLayers.length);
  if (!state.overlayLayers.length) {
    tree.innerHTML = '<li class="empty">Nothing on the overlay yet — draw something on the canvas.</li>';
  } else {
    tree.innerHTML = state.overlayLayers.map((layer, index) => {
      const type = String(layer.type || '');
      const label = OBJECT_TYPE_LABELS[type] || type || 'Object';
      const stored = layer.objname && state.objectStore[layer.objname];
      const detail = stored && (stored.fields.text || stored.fields.name) ? ` — ${stored.fields.text || stored.fields.name}` : '';
      const name = layer.objname || `unnamed (f${layer.id})`;
      return `<li><button type="button" class="tree-item" data-index="${index}"><span class="type-badge">${esc(label)}</span><span class="obj-name">${esc(name + detail)}</span><span class="edit-hint">✎</span></button></li>`;
    }).join('');
    tree.querySelectorAll('.tree-item').forEach((button) => {
      const layer = state.overlayLayers[Number(button.dataset.index)];
      button.addEventListener('click', () => {
        if (!layer) return;
        if (layer.objname && state.objectStore[layer.objname]) setSelection(layer.objname);
        openObjectEditor(layer);
      });
      button.addEventListener('mouseenter', () => setHighlightFor(layer));
      button.addEventListener('mouseleave', () => setHighlightFor(null));
    });
  }

  const primary = $('primaryTree');
  $('primaryCount').textContent = String(state.primaryLayers.length);
  if (!state.primaryLayers.length) {
    primary.innerHTML = `<li class="empty">${state.status.running ? 'The animation has nothing on screen right now.' : 'No animation is playing.'}</li>`;
  } else {
    primary.innerHTML = state.primaryLayers.map((layer) => {
      const type = String(layer.type || '');
      const label = OBJECT_TYPE_LABELS[type] || type || 'Object';
      return `<li><div class="tree-item readonly"><span class="type-badge">${esc(label)}</span><span class="obj-name">${esc(layer.objname || 'unnamed')}</span><span class="obj-id">frame ${esc(String(layer.id || '').replace(':', ', #'))}</span></div></li>`;
    }).join('');
  }
}

function setHighlightFor(layer) {
  const stored = layer && layer.objname ? state.objectStore[layer.objname] : null;
  state.highlight = stored ? objectBounds(stored.key, stored.fields) : null;
  renderOverlay();
}

// Tracked overlay objects in their on-screen stacking order.
function orderedTrackedObjects() {
  const seen = new Set();
  const out = [];
  state.overlayLayers.forEach((layer) => {
    const stored = layer.objname && state.objectStore[layer.objname];
    if (stored && !seen.has(layer.objname)) {
      seen.add(layer.objname);
      out.push(stored);
    }
  });
  Object.entries(state.objectStore).forEach(([name, stored]) => {
    if (!seen.has(name)) out.push(stored);
  });
  return out;
}

function untrackedOverlayCount() {
  return state.overlayLayers.filter((layer) => !(layer.objname && state.objectStore[layer.objname])).length;
}

// =====================================================================
// Object editor
// =====================================================================
// The API has no partial update: editing clears the object and draws a new
// one under the same name.

const FONT_FIELD = { key: 'font', label: 'Font', type: 'font', value: '5x7' };
const COLOR_FIELD = { key: 'color', label: 'Color', type: 'color', value: '#ff0000' };

const FIELD_DEFS = {
  text: [
    { key: 'x', label: 'X', type: 'number', value: 0 },
    { key: 'y', label: 'Y', type: 'number', value: 0 },
    { key: 'text', label: 'Text', type: 'text', value: 'HELLO' },
    FONT_FIELD,
    COLOR_FIELD,
  ],
  scrolling_text: [
    { key: 'x', label: 'Window X', type: 'number', value: 0 },
    { key: 'y', label: 'Window Y', type: 'number', value: 0 },
    { key: 'dx', label: 'Window width', type: 'number', value: 32, min: 1 },
    { key: 'dy', label: 'Window height', type: 'number', value: 7, min: 1 },
    { key: 'text', label: 'Text', type: 'text', value: 'Hello' },
    FONT_FIELD,
    { key: 'scroll_speed', label: 'Speed (px/s)', type: 'number', value: 12, min: 0, step: 'any' },
    { key: 'scroll_direction', label: 'Direction', type: 'select', value: 'horizontal', options: ['horizontal', 'vertical'] },
    COLOR_FIELD,
  ],
  line: [
    { key: 'x', label: 'Start X', type: 'number', value: 0 },
    { key: 'y', label: 'Start Y', type: 'number', value: 0 },
    { key: 'dx', label: 'Δx to end', type: 'number', value: 8 },
    { key: 'dy', label: 'Δy to end', type: 'number', value: 0 },
    { key: 'thickness', label: 'Thickness', type: 'number', value: 1, min: 1, max: 16 },
    COLOR_FIELD,
  ],
  rectangle: [
    { key: 'x', label: 'X', type: 'number', value: 0 },
    { key: 'y', label: 'Y', type: 'number', value: 0 },
    { key: 'dx', label: 'Width', type: 'number', value: 8, min: 1 },
    { key: 'dy', label: 'Height', type: 'number', value: 8, min: 1 },
    { key: 'border', label: 'Border (0 = filled)', type: 'number', value: 1, min: 0 },
    COLOR_FIELD,
  ],
  asset: [
    { key: 'x', label: 'X', type: 'number', value: 0 },
    { key: 'y', label: 'Y', type: 'number', value: 0 },
    { key: 'name', label: 'Asset', type: 'asset', value: '' },
    { key: 'color', label: 'Recolor (empty = original colors)', type: 'color', value: '', optional: true },
  ],
};

function fontOptions(selected) {
  return Object.keys(FONTS).map((f) => `<option value="${f}"${f === selected ? ' selected' : ''}>${f}</option>`).join('');
}

function assetOptions(selected) {
  const groups = state.anims.filter((a) => (a.assets || []).length).map((a) => {
    const opts = a.assets.map((file) => {
      const ref = `${a.id}/${file}`;
      return `<option value="${esc(ref)}"${ref === selected ? ' selected' : ''}>${esc(file)}</option>`;
    }).join('');
    return `<optgroup label="${esc(a.id)}">${opts}</optgroup>`;
  }).join('');
  const known = state.anims.some((a) => (a.assets || []).some((f) => `${a.id}/${f}` === selected));
  const extra = selected && !known ? `<option value="${esc(selected)}" selected>${esc(selected)}</option>` : '';
  return extra + groups || '<option value="">No assets yet</option>';
}

function fieldHtml(field, current) {
  const id = `objField_${field.key}`;
  const has = current && current[field.key] !== undefined;
  const val = has ? current[field.key] : field.value;
  const attrs = ['min', 'max', 'step'].map((a) => (field[a] !== undefined ? ` ${a}="${field[a]}"` : '')).join('');
  const wrap = (inner) => `<label class="tab-field"><span>${esc(field.label)}</span>${inner}</label>`;

  switch (field.type) {
    case 'font':
      return wrap(`<select id="${id}">${fontOptions(val)}</select>`);
    case 'asset':
      return wrap(`<select id="${id}">${assetOptions(val)}</select>`);
    case 'select':
      return wrap(`<select id="${id}">${field.options.map((o) => `<option value="${o}"${o === val ? ' selected' : ''}>${o}</option>`).join('')}</select>`);
    case 'color': {
      const hex = parseHex(val) ? String(val).slice(0, 7) : '#ff0000';
      return wrap(`<div class="asset-settings-row"><input id="${id}" type="text" value="${esc(val)}" placeholder="${field.optional ? 'none' : '#rrggbb or linear-gradient(…)'}" spellcheck="false" /><input id="${id}_picker" type="color" value="${esc(hex)}" title="Pick a color" /></div>`);
    }
    case 'number':
      return wrap(`<input id="${id}" type="number"${attrs} value="${esc(val)}" />`);
    default:
      return wrap(`<input id="${id}" type="text" value="${esc(val)}" />`);
  }
}

function openObjectEditor(layer) {
  state.editingLayer = layer;
  const key = OBJECT_TYPE_LABELS[layer.type] ? layer.type : 'text';
  const defs = FIELD_DEFS[key];
  const stored = layer.objname ? state.objectStore[layer.objname] : null;

  $('objectEditTitle').textContent = `Edit ${OBJECT_TYPE_LABELS[key]} — ${layer.objname || `f${layer.id}`}`;
  let hint = stored
    ? 'Change what you need and apply — the object is redrawn with the new values.'
    : "This object was drawn outside this page, so its current values aren't known. The fields show defaults; apply to redraw it with them.";
  if (!layer.objname) hint += ` It has no name, so it's addressed by its frame handle f${layer.id} — other unnamed objects sent in the same request share that handle and are affected too.`;
  $('objectEditHint').textContent = hint;
  $('objectEditFields').innerHTML = defs.map((f) => fieldHtml(f, stored ? stored.fields : null)).join('');

  defs.filter((f) => f.type === 'color').forEach((f) => {
    const text = $(`objField_${f.key}`);
    const picker = $(`objField_${f.key}_picker`);
    picker.addEventListener('input', () => {
      const alpha = parseHex(text.value)?.a ?? 255;
      text.value = hexWithAlpha(picker.value, alpha);
    });
  });

  state.highlight = stored ? objectBounds(stored.key, stored.fields) : null;
  renderOverlay();
  openModal('objectEditModal');
}

function closeObjectEditor() {
  state.editingLayer = null;
  state.highlight = null;
  renderOverlay();
  closeModal('objectEditModal');
}

function readObjectFields(key) {
  const payload = {};
  FIELD_DEFS[key].forEach((field) => {
    const input = $(`objField_${field.key}`);
    if (!input) return;
    const raw = input.value.trim();
    if (field.type === 'number') {
      payload[field.key] = raw === '' ? field.value : Number(raw);
    } else if (field.type === 'color') {
      if (raw) payload[field.key] = raw;
      else if (!field.optional) payload[field.key] = field.value;
    } else if (raw !== '' || field.type === 'text') {
      payload[field.key] = field.type === 'text' ? input.value : raw;
    }
  });
  return payload;
}

function applyObjectEdit() {
  const layer = state.editingLayer;
  if (!layer) return;
  const key = OBJECT_TYPE_LABELS[layer.type] ? layer.type : 'text';
  const payload = readObjectFields(key);
  if (payload.color && !isValidColorSpec(payload.color)) {
    toast(`"${payload.color}" isn't a color the sign understands — it would show up white. Use #rgb, #rrggbb, #rrggbbaa or linear-gradient(…).`, 'error');
    return;
  }
  if (key === 'asset' && !payload.name) {
    toast('Choose an asset.', 'error');
    return;
  }
  const ref = layerRef(layer);
  payload.objname = layer.objname || makeObjname(key === 'rectangle' ? 'rect' : key);
  const stored = layer.objname ? state.objectStore[layer.objname] : null;
  const previous = stored ? { key: stored.key, fields: { ...stored.fields } } : null;

  runAction(editAction(ref, key, payload, previous))
    .then(() => {
      toast(`Updated ${payload.objname}`);
      closeObjectEditor();
    })
    .catch(fail);
}

function removeObject() {
  const layer = state.editingLayer;
  if (!layer) return;
  const ref = layerRef(layer);
  if (!ref) {
    toast("This object has no name or handle the sign accepts, so it can't be removed on its own. Use Clear overlay.", 'error');
    return;
  }
  const stored = layer.objname ? state.objectStore[layer.objname] : null;
  const previous = stored ? { key: stored.key, fields: { ...stored.fields } } : null;
  runAction(removeAction(ref, previous))
    .then(() => {
      toast(`Removed ${layer.objname || ref}`);
      closeObjectEditor();
    })
    .catch(fail);
}

// =====================================================================
// Files: animation list + storage status
// =====================================================================

function animIds() {
  return state.anims.map((a) => a.id);
}

function refreshFiles() {
  return Promise.all([
    apiGet('/file/ls'),
    apiGet('/file/fsstatus').catch(() => null),
  ]).then(([files, fs]) => {
    state.anims = (Array.isArray(files?.anims) ? files.anims : [])
      .map((a) => ({ id: String(a.id || a.name || ''), assets: Array.isArray(a.assets) ? a.assets.map(String) : [] }))
      .filter((a) => a.id)
      .sort((a, b) => a.id.localeCompare(b.id));
    state.animMeta.clear();
    // Keep only the ids still present, so a folder that has since gained an
    // anim.json is re-checked.
    state.noDocAnims = new Set([...state.noDocAnims].filter((id) => animIds().includes(id)));
    if (fs) state.deviceInfo.fs = fs;
    renderFileList(fs);
    renderAnimSelectors();
    renderAssets();
    renderDeviceInfo();
    noteConnection(true);
  }).catch(fail);
}

function renderFileList(fs) {
  const fsEl = $('fsStatus');
  if (fs && fs.fs) {
    const pct = fs.total ? Math.min(100, (fs.used / fs.total) * 100) : 0;
    const label = { sd: 'SD card', littlefs: 'Internal flash' }[fs.fs] || fs.fs;
    fsEl.innerHTML = `<span><strong>${esc(label)}</strong></span><span>${formatBytes(fs.used)} of ${formatBytes(fs.total)} used</span><span>${formatBytes(fs.free)} free</span><div class="fs-bar"><i style="width:${pct.toFixed(1)}%"></i></div>`;
  }

  const list = $('fileList');
  const playing = animIdFromStatus(state.status.animname);
  const rows = state.anims.map((anim) => {
    const noDoc = state.noDocAnims.has(anim.id);
    const assets = `${anim.assets.length} asset${anim.assets.length === 1 ? '' : 's'}`;
    const note = noDoc
      ? `${assets} — no animation saved yet`
      : `${assets}${state.status.running && anim.id === playing ? ' — playing' : ''}`;
    const actions = noDoc
      ? `<button type="button" data-act="edit" data-name="${esc(anim.id)}" title="Start an animation here">New</button>`
      : `<button type="button" data-act="play" data-name="${esc(anim.id)}" title="Play">▶</button>
         <button type="button" data-act="edit" data-name="${esc(anim.id)}">Edit</button>
         <a class="link-button" href="${esc(downloadUrl(`/anim/${anim.id}/anim.json`))}" download="${esc(anim.id)}.json" title="Download anim.json">⤓</a>`;
    return `
    <div class="file-row${state.status.running && anim.id === playing ? ' playing' : ''}">
      <div class="file-name">
        <strong>${esc(anim.id)}</strong><br />
        <small>${note}</small>
      </div>
      <div class="file-actions">${actions}</div>
    </div>`;
  }).join('');
  const config = `
    <div class="file-row">
      <div class="file-name"><strong>config.json</strong><br /><small>Panel layout and Wi-Fi settings</small></div>
      <div class="file-actions"><a class="link-button" href="${esc(downloadUrl('/config.json'))}" download="config.json" title="Download config.json">⤓</a></div>
    </div>`;
  list.innerHTML = (rows || '<div class="empty">No animations on the sign yet. Save the overlay, build one in Animate, or start from New animation.</div>') + config;

  list.querySelectorAll('[data-act="play"]').forEach((b) => b.addEventListener('click', () => startAnimation(b.dataset.name, 0, {})));
  list.querySelectorAll('[data-act="edit"]').forEach((b) => b.addEventListener('click', () => {
    const id = b.dataset.name;
    if (state.noDocAnims.has(id)) openJsonEditor({ name: id, doc: ANIM_TEMPLATE });
    else openJsonEditorFor(id);
  }));
}

let syncingSelectors = false;

function renderAnimSelectors() {
  if (syncingSelectors) return;
  syncingSelectors = true;
  const ids = animIds().filter((id) => !state.noDocAnims.has(id));
  const fill = (select, emptyLabel) => {
    const prev = select.value;
    select.innerHTML = ids.length
      ? ids.map((id) => `<option value="${esc(id)}">${esc(id)}</option>`).join('')
      : `<option value="">${emptyLabel}</option>`;
    if (ids.includes(prev)) select.value = prev;
  };
  fill($('animationSelector'), 'No animations yet');
  fill($('stepAnimSelect'), 'No animations yet');
  // The datalist keeps every folder, since an asset can be uploaded into one
  // that has no animation yet.
  $('animIdList').innerHTML = animIds().map((id) => `<option value="${esc(id)}"></option>`).join('');
  syncingSelectors = false;
  loadParamsForSelected();
}

// =====================================================================
// Assets
// =====================================================================

function renderAssets() {
  // Asset tool dropdown: one optgroup per animation.
  const select = $('assetSelect');
  const all = state.anims.flatMap((a) => a.assets.map((f) => `${a.id}/${f}`));
  if (state.selectedAsset && !all.includes(state.selectedAsset)) state.selectedAsset = '';
  if (!state.selectedAsset && all.length) state.selectedAsset = all[0];
  select.innerHTML = all.length ? assetOptions(state.selectedAsset) : '<option value="">No assets yet</option>';
  select.value = state.selectedAsset;

  const host = $('assetList');
  const groups = state.anims.filter((a) => a.assets.length);
  if (!groups.length) {
    host.innerHTML = '<div class="empty">No assets yet. Upload an image or draw one.</div>';
    return;
  }
  host.innerHTML = groups.map((anim) => `
    <div class="asset-group">
      <h4>${esc(anim.id)}</h4>
      <div class="asset-grid">
        ${anim.assets.map((file) => {
          const ref = `${anim.id}/${file}`;
          return `<div class="asset-card${ref === state.selectedAsset ? ' selected' : ''}" data-ref="${esc(ref)}">
            <button type="button" class="thumb" data-act="use" title="Draw with this asset"><canvas></canvas></button>
            <div class="asset-name" title="${esc(file)}">${esc(file)}</div>
            <div class="asset-meta">…</div>
            <div class="asset-actions">
              <button type="button" data-act="edit" title="Open in the asset editor">Edit</button>
              <a class="link-button" href="${esc(downloadUrl(`/anim/${anim.id}/assets/${file}`))}" download="${esc(file)}" title="Download">⤓</a>
            </div>
          </div>`;
        }).join('')}
      </div>
    </div>`).join('');

  host.querySelectorAll('.asset-card').forEach((card) => {
    const ref = card.dataset.ref;
    card.querySelector('[data-act="use"]').addEventListener('click', () => selectAsset(ref, true));
    card.querySelector('[data-act="edit"]').addEventListener('click', () => openAssetEditor(ref));
    loadAssetThumb(card, ref);
  });
}

function loadAssetThumb(card, ref) {
  const meta = card.querySelector('.asset-meta');
  ensureAsset(ref).then((asset) => {
    const raster = new Raster(asset.w, asset.h);
    raster.px.set(asset.rgba);
    raster.toCanvas(card.querySelector('canvas'));
    if (asset.compatible) {
      meta.textContent = `${asset.w} × ${asset.h}`;
    } else {
      meta.className = 'asset-meta warn';
      meta.innerHTML = `${asset.bpp}-bit BMP, the sign can't read it.<button type="button" data-act="convert">Convert</button>`;
      meta.querySelector('[data-act="convert"]').addEventListener('click', () => convertAsset(ref, asset));
    }
  }).catch((error) => {
    meta.className = 'asset-meta warn';
    meta.textContent = /Unsupported|Not a BMP/.test(error.message) ? 'Not a readable BMP' : 'Preview unavailable';
  });
}

function selectAsset(ref, switchTool) {
  state.selectedAsset = ref;
  $('assetSelect').value = ref;
  document.querySelectorAll('.asset-card').forEach((c) => c.classList.toggle('selected', c.dataset.ref === ref));
  if (switchTool) {
    setTool('asset');
    switchTab('draw');
  }
  ensureAsset(ref).then(() => renderOverlay()).catch(() => {});
}

// Re-encode a 24-bit (1.0-era) BMP as 32-bit and overwrite it in place.
function convertAsset(ref, asset) {
  const { anim, file } = splitAssetRef(ref);
  const rgba = new Uint8ClampedArray(asset.rgba);
  uploadAssetBytes(anim, file, encodeBmp32(asset.w, asset.h, rgba))
    .then(() => {
      toast(`Converted ${file} to 32-bit`);
      return refreshFiles();
    })
    .catch(fail);
}

// Decode any browser-readable image (or BMP) into RGBA, optionally shrunk to
// fit the matrix.
// The firmware buffers an upload body in RAM (64 KB cap) and refuses to
// decode a BMP over 512 KB, so an image is shrunk to fit both before it goes
// out. Sending a full-resolution photo used to reboot the sign.
const MAX_ASSET_BYTES = 44000; // 44 KB of BMP ≈ 59 KB of base64 body
const MAX_ASSET_DIM = 4096;

function assetByteLimitSize(w, h) {
  const budget = Math.floor((MAX_ASSET_BYTES - 54) / 4);
  if (w * h <= budget && w <= MAX_ASSET_DIM && h <= MAX_ASSET_DIM) return null;
  const scale = Math.min(Math.sqrt(budget / (w * h)), MAX_ASSET_DIM / w, MAX_ASSET_DIM / h);
  return { w: Math.max(1, Math.floor(w * scale)), h: Math.max(1, Math.floor(h * scale)) };
}

async function imageFileToRgba(file, fitToMatrix) {
  let w;
  let h;
  let draw;
  if (/\.bmp$/i.test(file.name) || file.type === 'image/bmp') {
    try {
      const decoded = decodeBmp(await file.arrayBuffer());
      w = decoded.w;
      h = decoded.h;
      const tmp = new Raster(w, h);
      tmp.px.set(decoded.rgba);
      const src = tmp.toCanvas();
      draw = (c, dw, dh) => c.drawImage(src, 0, 0, dw, dh);
    } catch (e) {
      draw = null; // fall back to the browser decoder below
    }
  }
  if (!draw) {
    const bitmap = await createImageBitmap(file);
    w = bitmap.width;
    h = bitmap.height;
    draw = (c, dw, dh) => c.drawImage(bitmap, 0, 0, dw, dh);
  }

  let tw = w;
  let th = h;
  if (fitToMatrix && (w > state.width || h > state.height)) {
    const scale = Math.min(state.width / w, state.height / h);
    tw = Math.max(1, Math.round(w * scale));
    th = Math.max(1, Math.round(h * scale));
  }
  // Hard ceiling even with "shrink" off, so the sign is never sent a body it
  // has to reject (or, on older firmware, chokes on).
  const capped = assetByteLimitSize(tw, th);
  let cappedBySize = false;
  if (capped) {
    tw = capped.w;
    th = capped.h;
    cappedBySize = true;
  }
  const cv = document.createElement('canvas');
  cv.width = tw;
  cv.height = th;
  const c = cv.getContext('2d');
  c.imageSmoothingEnabled = tw !== w;
  c.imageSmoothingQuality = 'high';
  draw(c, tw, th);
  return {
    w: tw,
    h: th,
    rgba: c.getImageData(0, 0, tw, th).data,
    resized: tw !== w || th !== h,
    cappedBySize,
    origW: w,
    origH: h,
  };
}

async function uploadAssetFiles(files) {
  const anim = sanitizeAnimName($('assetTargetAnim').value);
  if (!anim) {
    toast('Enter the animation the asset belongs to.', 'error');
    return;
  }
  $('assetTargetAnim').value = anim;
  let last = '';
  for (const file of files) {
    try {
      const img = await imageFileToRgba(file, $('assetFitMatrix').checked);
      const name = sanitizeFileName(file.name);
      await uploadAssetBytes(anim, name, encodeBmp32(img.w, img.h, img.rgba));
      last = `${anim}/${name}`;
      if (img.cappedBySize) {
        toast(`Uploaded ${name}, shrunk from ${img.origW}×${img.origH} to ${img.w}×${img.h} — the sign can't hold an asset that large.`);
      } else {
        toast(img.resized ? `Uploaded ${name} (shrunk from ${img.origW}×${img.origH} to ${img.w}×${img.h})` : `Uploaded ${name}`);
      }
    } catch (error) {
      fail(new Error(`${file.name}: ${error.message}`));
    }
  }
  await refreshFiles();
  if (last) selectAsset(last, false);
}

// =====================================================================
// Asset editor
// =====================================================================

const assetEditor = { w: 16, h: 16, px: new Uint8ClampedArray(16 * 16 * 4), erase: false, painting: false, cell: 12 };

function syncAssetEditorLimits() {
  const max = Math.max(128, state.width, state.height);
  $('aeWidth').max = String(max);
  $('aeHeight').max = String(max);
}

function resizeAssetEditor(w, h) {
  const nw = clamp(Math.round(num(w, 16)), 1, 256);
  const nh = clamp(Math.round(num(h, 16)), 1, 256);
  const next = new Uint8ClampedArray(nw * nh * 4);
  for (let y = 0; y < Math.min(nh, assetEditor.h); y += 1) {
    for (let x = 0; x < Math.min(nw, assetEditor.w); x += 1) {
      const s = (y * assetEditor.w + x) * 4;
      next.set(assetEditor.px.subarray(s, s + 4), (y * nw + x) * 4);
    }
  }
  assetEditor.w = nw;
  assetEditor.h = nh;
  assetEditor.px = next;
  $('aeWidth').value = String(nw);
  $('aeHeight').value = String(nh);
  renderAssetEditor();
}

function renderAssetEditor() {
  const cv = $('assetEditorCanvas');
  const { w, h } = assetEditor;
  const cell = clamp(Math.floor(480 / Math.max(w, h)), 4, 24);
  assetEditor.cell = cell;
  cv.width = w * cell;
  cv.height = h * cell;
  cv.style.width = `${w * cell}px`;
  const c = cv.getContext('2d');
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      c.fillStyle = (x + y) & 1 ? '#0d1a2a' : '#13233a';
      c.fillRect(x * cell, y * cell, cell, cell);
      const i = (y * w + x) * 4;
      const a = assetEditor.px[i + 3];
      if (a) {
        c.fillStyle = `rgba(${assetEditor.px[i]}, ${assetEditor.px[i + 1]}, ${assetEditor.px[i + 2]}, ${(a / 255).toFixed(3)})`;
        c.fillRect(x * cell, y * cell, cell, cell);
      }
    }
  }
  c.strokeStyle = 'rgba(255,255,255,0.07)';
  c.beginPath();
  for (let x = 0; x <= w; x += 1) { c.moveTo(x * cell + 0.5, 0); c.lineTo(x * cell + 0.5, h * cell); }
  for (let y = 0; y <= h; y += 1) { c.moveTo(0, y * cell + 0.5); c.lineTo(w * cell, y * cell + 0.5); }
  c.stroke();
}

function assetCellAt(event) {
  const rect = $('assetEditorCanvas').getBoundingClientRect();
  return {
    x: clamp(Math.floor(((event.clientX - rect.left) / rect.width) * assetEditor.w), 0, assetEditor.w - 1),
    y: clamp(Math.floor(((event.clientY - rect.top) / rect.height) * assetEditor.h), 0, assetEditor.h - 1),
  };
}

function paintAssetCell(x, y) {
  const i = (y * assetEditor.w + x) * 4;
  if (assetEditor.erase) {
    assetEditor.px.fill(0, i, i + 4);
    return;
  }
  const c = parseHex(state.color.a) || WHITE;
  assetEditor.px[i] = c.r;
  assetEditor.px[i + 1] = c.g;
  assetEditor.px[i + 2] = c.b;
  assetEditor.px[i + 3] = state.color.alpha;
}

// Fill every cell between the previous and current pointer position so fast
// strokes don't leave gaps.
function paintAssetAt(event) {
  const to = assetCellAt(event);
  const from = assetEditor.last || to;
  let { x, y } = from;
  const dx = Math.abs(to.x - x);
  const dy = -Math.abs(to.y - y);
  const sx = x < to.x ? 1 : -1;
  const sy = y < to.y ? 1 : -1;
  let err = dx + dy;
  for (;;) {
    paintAssetCell(x, y);
    if (x === to.x && y === to.y) break;
    const e2 = 2 * err;
    if (e2 >= dy) { err += dy; x += sx; }
    if (e2 <= dx) { err += dx; y += sy; }
  }
  assetEditor.last = to;
  renderAssetEditor();
}

function setAssetBrush(erase) {
  assetEditor.erase = erase;
  $('aePen').classList.toggle('active', !erase);
  $('aeErase').classList.toggle('active', erase);
}

function openAssetEditor(ref) {
  setAssetBrush(false);
  const defaultAnim = sanitizeAnimName($('assetTargetAnim').value) || sanitizeAnimName($('animNameInput').value) || 'demo_anim';
  if (!ref) {
    assetEditor.w = 16;
    assetEditor.h = 16;
    assetEditor.px = new Uint8ClampedArray(16 * 16 * 4);
    $('aeWidth').value = '16';
    $('aeHeight').value = '16';
    $('aeAnim').value = defaultAnim;
    $('assetNameInput').value = 'custom_asset';
    $('assetEditorTitle').textContent = 'New asset';
    renderAssetEditor();
    openModal('assetEditorModal');
    return;
  }
  const { anim, file } = splitAssetRef(ref);
  ensureAsset(ref).then((asset) => {
    assetEditor.w = asset.w;
    assetEditor.h = asset.h;
    assetEditor.px = new Uint8ClampedArray(asset.rgba);
    $('aeWidth').value = String(asset.w);
    $('aeHeight').value = String(asset.h);
    $('aeAnim').value = anim;
    $('assetNameInput').value = file.replace(/\.bmp$/i, '');
    $('assetEditorTitle').textContent = `Edit ${file}`;
    renderAssetEditor();
    openModal('assetEditorModal');
  }).catch(fail);
}

function saveAssetFromEditor() {
  const anim = sanitizeAnimName($('aeAnim').value);
  if (!anim) {
    toast('Enter the animation this asset belongs to.', 'error');
    return;
  }
  const file = sanitizeFileName($('assetNameInput').value, 'custom_asset');
  uploadAssetBytes(anim, file, encodeBmp32(assetEditor.w, assetEditor.h, assetEditor.px))
    .then(() => {
      toast(`Saved ${anim}/${file}`);
      closeModal('assetEditorModal');
      return refreshFiles();
    })
    .then(() => selectAsset(`${anim}/${file}`, false))
    .catch(fail);
}

function initAssetEditor() {
  const cv = $('assetEditorCanvas');
  cv.addEventListener('pointerdown', (event) => {
    assetEditor.painting = true;
    assetEditor.last = null;
    cv.setPointerCapture?.(event.pointerId);
    paintAssetAt(event);
  });
  cv.addEventListener('pointermove', (event) => { if (assetEditor.painting) paintAssetAt(event); });
  const stop = () => { assetEditor.painting = false; assetEditor.last = null; };
  cv.addEventListener('pointerup', stop);
  cv.addEventListener('pointercancel', stop);
  $('aePen').addEventListener('click', () => setAssetBrush(false));
  $('aeErase').addEventListener('click', () => setAssetBrush(true));
  $('aeClear').addEventListener('click', () => { assetEditor.px.fill(0); renderAssetEditor(); });
  $('aeWidth').addEventListener('change', () => resizeAssetEditor($('aeWidth').value, assetEditor.h));
  $('aeHeight').addEventListener('change', () => resizeAssetEditor(assetEditor.w, $('aeHeight').value));
  $('saveAssetButton').addEventListener('click', saveAssetFromEditor);
  syncAssetEditorLimits();
  renderAssetEditor();
}

// =====================================================================
// Animations: metadata, parameters, player
// =====================================================================

function fetchAnimDoc(id) {
  return apiGet(`/file/download?path=${encodeURIComponent(`/anim/${id}/anim.json`)}`)
    .then((doc) => {
      if (state.noDocAnims.delete(id)) renderFileList(state.deviceInfo.fs);
      return doc;
    })
    .catch((error) => {
      // /file/ls lists every folder under /anim/, including ones that only
      // hold assets, so a 404 here is expected rather than a fault.
      if (/HTTP 404/.test(error.message) && !state.noDocAnims.has(id)) {
        state.noDocAnims.add(id);
        renderFileList(state.deviceInfo.fs);
        renderAnimSelectors();
      }
      throw error;
    });
}

// Walk every drawable in an anim.json, in either frame format.
function forEachDrawable(doc, fn) {
  (Array.isArray(doc?.frames) ? doc.frames : []).forEach((frame, fi) => {
    if (!frame || typeof frame !== 'object') return;
    if (Array.isArray(frame.drawables)) {
      frame.drawables.forEach((item) => {
        const key = item && typeof item === 'object' ? Object.keys(item)[0] : null;
        if (key) fn(key, item[key], fi);
      });
      return;
    }
    Object.entries(frame).forEach(([key, value]) => {
      if (!DRAWABLE_KEYS.includes(key)) return;
      (Array.isArray(value) ? value : [value]).forEach((obj) => fn(key, obj, fi));
    });
  });
}

function loadAnimMeta(id) {
  if (state.noDocAnims.has(id)) {
    return Promise.reject(new Error(`${id} has no anim.json yet`));
  }
  if (!state.animMeta.has(id)) {
    const promise = fetchAnimDoc(id).then((doc) => {
      const params = doc && typeof doc.default_params === 'object' && doc.default_params ? { ...doc.default_params } : {};
      const placeholders = new Set(Object.keys(params));
      forEachDrawable(doc, (key, obj) => {
        if ((key === 'text' || key === 'scrolling_text') && obj && typeof obj.text === 'string') {
          for (const m of obj.text.matchAll(/\{([^{}]+)\}/g)) placeholders.add(m[1]);
        }
      });
      return { doc, params, placeholders: [...placeholders] };
    });
    promise.catch(() => state.animMeta.delete(id));
    state.animMeta.set(id, promise);
  }
  return state.animMeta.get(id);
}

function loadParamsForSelected() {
  const id = $('animationSelector').value;
  const block = $('paramsBlock');
  const list = $('paramsList');
  if (!id) {
    block.hidden = true;
    return;
  }
  loadAnimMeta(id).then((meta) => {
    if ($('animationSelector').value !== id) return;
    if (!meta.placeholders.length) {
      block.hidden = true;
      list.innerHTML = '';
      return;
    }
    list.innerHTML = meta.placeholders.map((key) => {
      const def = meta.params[key];
      return `<label class="param-row"><code title="{${esc(key)}}">{${esc(key)}}</code><input type="text" data-param="${esc(key)}" data-default="${esc(def ?? '')}" data-has-default="${def !== undefined}" value="${esc(def ?? '')}" placeholder="${def === undefined ? 'no default — shown literally' : ''}" /></label>`;
    }).join('');
    block.hidden = false;
  }).catch(() => { block.hidden = true; });
}

function collectParams() {
  const params = {};
  $('paramsList').querySelectorAll('[data-param]').forEach((input) => {
    const hasDefault = input.dataset.hasDefault === 'true';
    if (input.value !== '' || hasDefault) {
      if (!hasDefault || input.value !== input.dataset.default) params[input.dataset.param] = input.value;
    }
  });
  return params;
}

function startAnimation(name, cycles, params) {
  if (!name) {
    toast('Choose an animation first.', 'error');
    return Promise.resolve();
  }
  const body = { animname: name, cycles: Math.max(0, Math.round(num(cycles, 0))) };
  if (params && Object.keys(params).length) body.params = params;
  return apiPost('/anim/start', body)
    .catch((error) => {
      if (/HTTP 404/.test(error.message)) {
        state.noDocAnims.add(name);
        renderFileList(state.deviceInfo.fs);
        throw new Error(`"${name}" has no anim.json on the sign yet — save an animation under that name first.`);
      }
      throw error;
    })
    .then(() => {
      toast(`Playing ${name}${body.cycles ? ` × ${body.cycles}` : ''}`);
      refreshStatus();
      refreshComposition();
    })
    .catch(fail);
}

function stopAnimation() {
  apiPost('/anim/stop')
    .then(() => {
      toast('Stopped');
      refreshStatus();
      refreshComposition();
    })
    .catch(fail);
}

let speedTouchedAt = 0;
const sendSpeed = debounce((value) => {
  apiPost('/anim/setspeed', { speed: value }).catch(fail);
}, 150);

function refreshStatus() {
  return apiGet('/anim/status').then((s) => {
    const prevKey = `${state.status.running}|${state.status.animname}`;
    state.status = {
      running: Boolean(s && s.running),
      paused: Boolean(s && s.paused),
      speed: num(s && s.speed, 1),
      animname: String((s && s.animname) || ''),
    };
    renderStatus();
    if (prevKey !== `${state.status.running}|${state.status.animname}`) renderFileList(state.deviceInfo.fs);
  }).catch(() => {});
}

function renderStatus() {
  const { running, paused, speed, animname } = state.status;
  const id = animIdFromStatus(animname);
  const badge = $('statusBadge');
  badge.textContent = running ? `${paused ? 'Paused' : 'Playing'}${id ? ` · ${id}` : ''}` : 'Idle';
  badge.classList.toggle('running', running);
  const info = $('playingInfo');
  info.textContent = running ? `${paused ? 'Paused' : 'Playing'} ${id || 'animation'}${speed !== 1 ? ` at ${speed.toFixed(1)}×` : ''}` : 'Nothing playing';
  info.classList.toggle('running', running);
  if (Date.now() - speedTouchedAt > 3000) {
    $('speedRange').value = String(speed);
    $('speedValue').textContent = `${speed.toFixed(1)}×`;
  }
}

// =====================================================================
// Animation builder (timeline)
// =====================================================================

function cloneObjects(objects) {
  return objects.map((o) => ({ key: o.key, fields: JSON.parse(JSON.stringify(o.fields)) }));
}

function captureFrame(replaceIndex) {
  const objects = orderedTrackedObjects();
  const skipped = untrackedOverlayCount();
  if (!objects.length) {
    toast(skipped
      ? "The overlay's objects were drawn outside this page, so their values aren't known. Redraw them here to capture them."
      : 'Draw something on the overlay first, then capture it.', 'error');
    return;
  }
  const duration = Math.max(0, Math.round(num($('captureDuration').value, 1000)));
  if (typeof replaceIndex === 'number') {
    state.timeline[replaceIndex].objects = cloneObjects(objects);
  } else {
    state.timeline.push({ kind: 'frame', objects: cloneObjects(objects), duration });
  }
  renderTimeline();
  const which = typeof replaceIndex === 'number' ? `Updated step ${replaceIndex + 1}` : `Captured frame as step ${state.timeline.length}`;
  toast(skipped ? `${which}. ${skipped} object${skipped === 1 ? '' : 's'} drawn outside this page ${skipped === 1 ? 'was' : 'were'} skipped.` : which);
}

function addAnimStep() {
  const name = $('stepAnimSelect').value;
  if (!name) {
    toast('There are no animations on the sign to add yet.', 'error');
    return;
  }
  state.timeline.push({
    kind: 'anim',
    name,
    cycles: Math.max(1, Math.round(num($('stepCycles').value, 1))),
    speed: Math.max(0.1, num($('stepSpeed').value, 1)),
  });
  renderTimeline();
}

function renderTimeline() {
  const list = $('timelineList');
  $('timelineCount').textContent = String(state.timeline.length);
  list.innerHTML = state.timeline.map((step, i) => {
    const body = step.kind === 'frame'
      ? `<div class="step-title">Frame — ${step.objects.length} object${step.objects.length === 1 ? '' : 's'}</div>
         <div class="step-sub"><input type="number" min="0" step="100" value="${step.duration}" data-duration="${i}" aria-label="Frame length in ms" /> ms</div>
         <div class="step-sub"><button type="button" data-act="show" data-i="${i}" title="Put this frame on the overlay to change it">Show</button>
           <button type="button" data-act="replace" data-i="${i}" title="Replace with what's on the overlay now">Replace</button></div>`
      : `<div class="step-title">Play ${esc(step.name)}</div>
         <div class="step-sub">${step.cycles}× at ${Number(step.speed).toFixed(1)}× speed</div>`;
    return `<li class="timeline-step">
      <div class="step-thumb">${step.kind === 'frame' ? '<canvas></canvas>' : '▶'}</div>
      <div class="step-body">${body}</div>
      <div class="step-actions">
        <button type="button" data-act="up" data-i="${i}" title="Move up" ${i === 0 ? 'disabled' : ''}>↑</button>
        <button type="button" data-act="down" data-i="${i}" title="Move down" ${i === state.timeline.length - 1 ? 'disabled' : ''}>↓</button>
        <button type="button" data-act="remove" data-i="${i}" title="Remove step">✕</button>
      </div>
    </li>`;
  }).join('');

  list.querySelectorAll('.timeline-step').forEach((li, i) => {
    const step = state.timeline[i];
    if (step.kind !== 'frame') return;
    const raster = new Raster(state.width, state.height);
    step.objects.forEach((o) => rasterizeObject(raster, o.key, o.fields));
    raster.toCanvas(li.querySelector('canvas'));
  });
  list.querySelectorAll('[data-duration]').forEach((input) => {
    input.addEventListener('change', () => {
      state.timeline[Number(input.dataset.duration)].duration = Math.max(0, Math.round(num(input.value, 0)));
    });
  });
  list.querySelectorAll('button[data-act]').forEach((button) => {
    button.addEventListener('click', () => {
      const i = Number(button.dataset.i);
      const t = state.timeline;
      switch (button.dataset.act) {
        case 'up': [t[i - 1], t[i]] = [t[i], t[i - 1]]; break;
        case 'down': [t[i + 1], t[i]] = [t[i], t[i + 1]]; break;
        case 'remove': t.splice(i, 1); break;
        case 'replace': captureFrame(i); return;
        case 'show':
          runAction(replaceOverlayAction(t[i].objects))
            .then(() => toast(`Step ${i + 1} is on the overlay — change it, then Replace to update the step.`))
            .catch(fail);
          return;
        default: return;
      }
      renderTimeline();
    });
  });
}

// Captured frames use the `drawables` array form (the same one
// /framebuffer/savetostorage writes) so objects keep their stacking order. A
// separate `clear` frame in front blanks the screen — the firmware ignores
// clear/load_anim inside a `drawables` frame.
function buildTimelineAnim() {
  const blank = $('blankEachFrame').checked;
  const frames = [];
  state.timeline.forEach((step) => {
    if (step.kind === 'frame') {
      if (blank) frames.push({ clear: {} });
      frames.push({ drawables: step.objects.map((o) => ({ [o.key]: o.fields })), duration: step.duration });
    } else {
      frames.push({ load_anim: { name: step.name, cycles: step.cycles, speed: step.speed } });
    }
  });
  return { default_params: {}, frames };
}

function confirmReplace(name, exceptId) {
  if (name === exceptId || !animIds().includes(name)) return true;
  if (state.noDocAnims.has(name)) return true; // assets only, no document yet
  return window.confirm(`An animation called "${name}" is already on the sign. Replace it?`);
}

function uploadAnimation(name, doc) {
  return apiPost('/file/uploadanim', { animname: name, anim: doc })
    .then(() => {
      state.animMeta.delete(name);
      state.noDocAnims.delete(name); // it definitely has a document now
      return refreshFiles();
    })
    .then(() => {
      $('animationSelector').value = name;
      loadParamsForSelected();
    });
}

function uploadTimeline() {
  if (!state.timeline.length) {
    toast('Add at least one step first.', 'error');
    return;
  }
  const name = sanitizeAnimName($('timelineName').value);
  if (!name) {
    toast('Give the animation a name.', 'error');
    return;
  }
  $('timelineName').value = name;
  if (!confirmReplace(name)) return;
  uploadAnimation(name, buildTimelineAnim())
    .then(() => toast(`Saved ${name} — pick it under Play to run it.`))
    .catch(fail);
}

// "Save overlay": the firmware serialises the overlay into a one-frame
// /anim/<name>/anim.json.
function saveOverlayAsAnimation() {
  const name = sanitizeAnimName($('animNameInput').value);
  if (!name) {
    toast('Give the animation a name.', 'error');
    return;
  }
  $('animNameInput').value = name;
  if (!state.overlayLayers.length) {
    toast('The overlay is empty — draw something first.', 'error');
    return;
  }
  apiGet('/file/ls')
    .then((files) => {
      state.anims = (files?.anims || []).map((a) => ({ id: String(a.id), assets: a.assets || [] }));
      if (!confirmReplace(name)) return null;
      return apiPost('/framebuffer/savetostorage', { animname: name, filename: 'anim.json' })
        .then(() => {
          state.animMeta.delete(name);
          state.noDocAnims.delete(name);
          toast(`Saved the overlay as ${name}`);
        });
    })
    .then(() => refreshFiles())
    .catch(fail);
}

// =====================================================================
// Animation JSON editor + validation (mirrors tools/validate_anim.py)
// =====================================================================

const jsonEditor = { editingId: null };

const ANIM_TEMPLATE = {
  default_params: { greeting: 'hello' },
  frames: [
    { clear: {} },
    { text: { x: 0, y: 0, text: '{greeting}', font: '5x7', color: '#ffb000', objname: 'greeting' }, duration: 2000 },
    { clear: { obj: ['greeting'] } },
    {
      scrolling_text: {
        x: 0, y: 0, dx: 32, dy: 7, text: 'Scrolling…', font: '5x7',
        color: 'linear-gradient(90deg, #4a90e2, #a0d0ff)', scroll_speed: 12, scroll_direction: 'horizontal',
      },
      duration: 4000,
    },
  ],
};

function openJsonEditor({ name = '', doc = ANIM_TEMPLATE, editingId = null } = {}) {
  jsonEditor.editingId = editingId;
  $('jsonEditorTitle').textContent = editingId ? `Edit ${editingId}` : 'New animation';
  $('jsonAnimName').value = name;
  $('jsonEditorText').value = JSON.stringify(doc, null, 2);
  $('jsonProblems').innerHTML = '';
  openModal('jsonEditorModal');
}

function openJsonEditorFor(id) {
  fetchAnimDoc(id)
    .then((doc) => openJsonEditor({ name: id, doc, editingId: id }))
    .catch(fail);
}

function validateAnim(doc, animName) {
  const errors = [];
  const warns = [];
  if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return { errors: ['The top level must be an object with a "frames" list.'], warns };

  if (doc.default_params !== undefined) {
    if (!doc.default_params || typeof doc.default_params !== 'object' || Array.isArray(doc.default_params)) errors.push('default_params must be an object.');
    else Object.entries(doc.default_params).forEach(([k, v]) => { if (!['string', 'number'].includes(typeof v)) warns.push(`default_params.${k} should be a string.`); });
  }
  if (!Array.isArray(doc.frames)) {
    errors.push('"frames" must be a list.');
    return { errors, warns };
  }
  if (!doc.frames.length) warns.push('"frames" is empty — nothing will show.');

  const known = new Set([...DRAWABLE_KEYS, 'clear', 'load_anim', 'duration', 'drawables']);
  const assetExists = (name) => {
    const ref = name.includes('/') ? name : `${animName}/${name}`;
    const { anim, file } = splitAssetRef(ref);
    const entry = state.anims.find((a) => a.id === anim);
    return entry ? entry.assets.includes(file) : null;
  };

  doc.frames.forEach((frame, i) => {
    const where = `frame ${i + 1}`;
    if (!frame || typeof frame !== 'object' || Array.isArray(frame)) {
      errors.push(`${where} must be an object.`);
      return;
    }
    Object.keys(frame).forEach((k) => { if (!known.has(k)) warns.push(`${where}: unknown key "${k}" (ignored by the sign).`); });

    const drawables = [];
    if (Array.isArray(frame.drawables)) {
      if (frame.clear !== undefined || frame.load_anim !== undefined) warns.push(`${where}: in a frame with "drawables", clear and load_anim are ignored — put them in their own frame.`);
      DRAWABLE_KEYS.forEach((k) => { if (frame[k] !== undefined) warns.push(`${where}: "${k}" next to "drawables" is ignored.`); });
      frame.drawables.forEach((item, j) => {
        const key = item && typeof item === 'object' ? Object.keys(item)[0] : null;
        if (!DRAWABLE_KEYS.includes(key)) errors.push(`${where}.drawables[${j}]: expected one of ${DRAWABLE_KEYS.join(', ')}.`);
        else drawables.push([key, item[key]]);
      });
    } else if (frame.drawables !== undefined) {
      errors.push(`${where}: "drawables" must be a list.`);
    } else {
      DRAWABLE_KEYS.forEach((key) => {
        if (frame[key] === undefined) return;
        (Array.isArray(frame[key]) ? frame[key] : [frame[key]]).forEach((obj) => drawables.push([key, obj]));
      });
    }

    const controlOnly = (frame.clear !== undefined || frame.load_anim !== undefined) && !drawables.length;
    if (frame.duration !== undefined && typeof frame.duration !== 'number') errors.push(`${where}: duration must be a number (milliseconds).`);
    else if (frame.duration === undefined && !controlOnly && drawables.length) warns.push(`${where}: no duration — it flashes by in 0 ms.`);

    drawables.forEach(([key, obj]) => {
      const w = `${where}.${key}`;
      if (!obj || typeof obj !== 'object' || Array.isArray(obj)) {
        errors.push(`${w} must be an object.`);
        return;
      }
      if (obj.color !== undefined && !isValidColorSpec(obj.color)) warns.push(`${w}: color "${obj.color}" isn't #rgb/#rrggbb/#rrggbbaa or linear-gradient(…) — it will show white.`);
      if ((key === 'text' || key === 'scrolling_text') && obj.text === undefined) warns.push(`${w}: no "text" — renders empty.`);
      if (obj.font !== undefined && !FONTS[obj.font] && obj.font !== '12x16') warns.push(`${w}: unknown font "${obj.font}" — falls back to 5x7.`);
      if (key === 'scrolling_text') {
        if (obj.scroll_direction !== undefined && !['horizontal', 'vertical'].includes(obj.scroll_direction)) warns.push(`${w}: scroll_direction "${obj.scroll_direction}" is treated as horizontal.`);
        if (!num(obj.dx, 0) || !num(obj.dy, 0)) warns.push(`${w}: dx/dy set the scroll window; with 0 nothing is visible.`);
      }
      if (key === 'rectangle' && (num(obj.dx, 0) <= 0 || num(obj.dy, 0) <= 0)) warns.push(`${w}: dx and dy are width and height — 0 draws nothing.`);
      if (key === 'asset') {
        if (!obj.name) errors.push(`${w}: missing "name".`);
        else if (animName && assetExists(String(obj.name)) === false) warns.push(`${w}: asset "${obj.name}" isn't on the sign — a purple/black marker shows instead.`);
      }
    });

    if (frame.load_anim !== undefined) {
      const lo = frame.load_anim;
      if (!lo || typeof lo !== 'object' || !lo.name) errors.push(`${where}.load_anim: missing "name".`);
      else {
        if (!animIds().includes(lo.name)) warns.push(`${where}.load_anim: "${lo.name}" isn't on the sign.`);
        if (lo.name === animName) warns.push(`${where}.load_anim: plays itself — the sign stops after 8 levels.`);
        if (lo.speed !== undefined && !(num(lo.speed, 0) > 0)) warns.push(`${where}.load_anim: speed should be above 0 (falls back to 1).`);
      }
    }
    if (frame.clear !== undefined) {
      const co = frame.clear;
      if (co && typeof co === 'object' && co.obj !== undefined && !Array.isArray(co.obj)) errors.push(`${where}.clear.obj must be a list of objnames or "f<frame>:<index>" handles.`);
    }
  });
  return { errors, warns };
}

function showProblems({ errors, warns }, okMessage) {
  const items = [
    ...errors.map((e) => `<li class="error">${esc(e)}</li>`),
    ...warns.map((w) => `<li class="warn">${esc(w)}</li>`),
  ];
  if (!items.length && okMessage) items.push(`<li class="ok">${esc(okMessage)}</li>`);
  $('jsonProblems').innerHTML = items.join('');
}

function readJsonEditor() {
  try {
    return { doc: JSON.parse($('jsonEditorText').value) };
  } catch (e) {
    return { error: `Not valid JSON: ${e.message}` };
  }
}

function checkJsonEditor() {
  const { doc, error } = readJsonEditor();
  if (error) {
    showProblems({ errors: [error], warns: [] });
    return null;
  }
  const result = validateAnim(doc, sanitizeAnimName($('jsonAnimName').value));
  showProblems(result, 'Looks good.');
  return { doc, result };
}

function uploadJsonEditor() {
  const checked = checkJsonEditor();
  if (!checked) return;
  if (checked.result.errors.length) {
    toast('Fix the errors listed below the editor first.', 'error');
    return;
  }
  const name = sanitizeAnimName($('jsonAnimName').value);
  if (!name) {
    toast('Give the animation a name.', 'error');
    return;
  }
  $('jsonAnimName').value = name;
  if (!confirmReplace(name, jsonEditor.editingId)) return;
  uploadAnimation(name, checked.doc)
    .then(() => {
      toast(`Saved ${name}`);
      jsonEditor.editingId = name;
      $('jsonEditorTitle').textContent = `Edit ${name}`;
    })
    .catch(fail);
}

// =====================================================================
// Settings + device info
// =====================================================================

const sendBrightness = debounce((value) => {
  apiPost('/display/brightness', { brightness: value }).catch(fail);
}, 150);

function loadDeviceSettings() {
  apiGet('/display/brightness').then((b) => {
    const value = clamp(num(b && b.brightness, 128), 0, 255);
    $('brightnessRange').value = String(value);
    $('brightnessValue').textContent = String(value);
  }).catch(() => {});
  apiGet('/display/gpiopins').then((pins) => {
    state.deviceInfo.pins = pins;
    renderDeviceInfo();
  }).catch(() => {});
  apiGet('/file/fsstatus').then((fs) => {
    state.deviceInfo.fs = fs;
    renderDeviceInfo();
  }).catch(() => {});
}

function renderDeviceInfo() {
  const { pins, fs } = state.deviceInfo;
  const rows = [['Address', esc(describeTarget())]];
  if (state.sizeKnown) rows.push(['Matrix', `${state.width} × ${state.height} LEDs`]);
  if (pins) {
    rows.push(['LED data pin', `GPIO ${esc(pins.data_pin)}`]);
    if (pins.sd) rows.push(['SD card pins', `CS ${esc(pins.sd.cs)}, MOSI ${esc(pins.sd.mosi)}, MISO ${esc(pins.sd.miso)}, SCK ${esc(pins.sd.sck)}`]);
  }
  if (fs && fs.fs) rows.push(['Storage', `${esc({ sd: 'SD card', littlefs: 'Internal flash' }[fs.fs] || fs.fs)}, ${formatBytes(fs.free)} free of ${formatBytes(fs.total)}`]);
  $('deviceInfo').innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
}

function connect() {
  api.base = $('apiBase').value.trim();
  state.conn = { ok: null, failures: 0 };
  setConnState(null);
  assetCache.clear();
  state.animMeta.clear();
  apiGet('/framebuffer/size').then((s) => setMatrixSize(s.width, s.height)).catch(() => {});
  loadLiveView();
  refreshFiles();
  refreshComposition();
  refreshStatus();
  loadDeviceSettings();
}

// =====================================================================
// Modals, tabs, tools, color UI
// =====================================================================

function openModal(id) {
  const modal = $(id);
  modal.classList.remove('hidden');
  modal.setAttribute('aria-hidden', 'false');
  const focusable = modal.querySelector('input, select, textarea, button:not([data-close-modal])');
  if (focusable) setTimeout(() => focusable.focus(), 0);
}

function closeModal(id) {
  const modal = $(id);
  modal.classList.add('hidden');
  modal.setAttribute('aria-hidden', 'true');
  if (id === 'objectEditModal' && state.editingLayer) {
    state.editingLayer = null;
    state.highlight = null;
    renderOverlay();
  }
}

function openModalIds() {
  return [...document.querySelectorAll('.modal:not(.hidden)')].map((m) => m.id);
}

function switchTab(target) {
  document.querySelectorAll('.tab-btn').forEach((b) => {
    const active = b.dataset.tab === target;
    b.classList.toggle('active', active);
    b.setAttribute('aria-selected', String(active));
  });
  document.querySelectorAll('.tab-panel').forEach((panel) => {
    panel.classList.toggle('active', panel.dataset.tabPanel === target);
  });
  if (target === 'composition') refreshComposition();
  if (target === 'anim') renderTimeline();
}

function setTool(tool) {
  state.activeTool = tool;
  document.querySelectorAll('.tool').forEach((button) => button.classList.toggle('active', button.dataset.tool === tool));
  canvas.classList.toggle('pan-mode', tool === 'move');
  const settings = {
    select: 'selectSettings', text: 'textSettings', scroll: 'scrollSettings',
    line: 'lineSettings', rectangle: 'rectSettings', asset: 'assetSettings',
    move: 'moveSettings',
  };
  Object.entries(settings).forEach(([name, id]) => { $(id).hidden = name !== tool; });
  if (tool === 'asset' && state.selectedAsset) ensureAsset(state.selectedAsset).then(() => renderOverlay()).catch(() => {});
  if (tool === 'select') {
    // Asset bounds need the bitmap's size, so make sure they're loaded.
    orderedTrackedObjects().forEach((o) => {
      if (o.key === 'asset' && o.fields.name && !assetCache.has(o.fields.name)) {
        ensureAsset(o.fields.name).then(() => { renderOverlay(); renderSelectionPanel(); }).catch(() => {});
      }
    });
    renderSelectionPanel();
  } else {
    canvas.classList.remove('over-object');
    state.hoverObject = null;
  }
  renderOverlay();
}

function buildColorSwatches() {
  const wrap = $('colorPalette');
  wrap.innerHTML = '';
  PALETTE.forEach((color) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'swatch';
    button.style.background = color;
    button.title = color;
    button.dataset.color = color;
    button.addEventListener('click', () => {
      state.color.a = color;
      $('customColor').value = color;
      updateColorUi();
    });
    wrap.appendChild(button);
  });
}

function buildFontSelects() {
  document.querySelectorAll('.font-select').forEach((select) => { select.innerHTML = fontOptions('5x7'); });
}

function updateColorUi() {
  const { mode, a, alpha } = state.color;
  document.querySelectorAll('.swatch').forEach((s) => s.classList.toggle('selected', s.dataset.color === a));
  $('fillSolidButton').classList.toggle('active', mode === 'solid');
  $('fillGradientButton').classList.toggle('active', mode === 'gradient');
  $('gradientSettings').hidden = mode !== 'gradient';
  $('opacityValue').textContent = `${Math.round((alpha / 255) * 100)}%`;
  $('colorPreview').style.setProperty('--swatch', cssForSpec(currentColorSpec()));
  $('colorPreview').title = currentColorSpec();
  renderOverlay();
}

// =====================================================================
// Events
// =====================================================================

function isTyping(target) {
  const tag = (target && target.tagName ? target.tagName : '').toLowerCase();
  return tag === 'input' || tag === 'select' || tag === 'textarea' || (target && target.isContentEditable);
}

function bindEvents() {
  $('toggleConfigButton').addEventListener('click', () => {
    renderDeviceInfo();
    openModal('settingsModal');
    loadDeviceSettings();
  });
  $('connectButton').addEventListener('click', () => {
    connect();
    closeModal('settingsModal');
  });
  $('apiBase').addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      connect();
      closeModal('settingsModal');
    }
  });

  document.querySelectorAll('.modal').forEach((modal) => {
    modal.addEventListener('click', (event) => { if (event.target === modal) closeModal(modal.id); });
    modal.querySelectorAll('[data-close-modal]').forEach((b) => b.addEventListener('click', () => closeModal(modal.id)));
  });

  // Color
  $('customColor').addEventListener('input', (event) => { state.color.a = event.target.value; updateColorUi(); });
  $('fillSolidButton').addEventListener('click', () => { state.color.mode = 'solid'; updateColorUi(); });
  $('fillGradientButton').addEventListener('click', () => { state.color.mode = 'gradient'; updateColorUi(); });
  $('gradientColor').addEventListener('input', (event) => { state.color.b = event.target.value; updateColorUi(); });
  $('gradientAngle').addEventListener('input', (event) => { state.color.angle = num(event.target.value, 90); updateColorUi(); });
  $('swapGradientButton').addEventListener('click', () => {
    [state.color.a, state.color.b] = [state.color.b, state.color.a];
    $('customColor').value = state.color.a;
    $('gradientColor').value = state.color.b;
    updateColorUi();
  });
  $('opacityRange').addEventListener('input', (event) => { state.color.alpha = num(event.target.value, 255); updateColorUi(); });

  // Tool settings re-render the hover ghost
  ['textInput', 'textFont', 'scrollTextInput', 'scrollFont', 'scrollDirection', 'assetTint'].forEach((id) => {
    $(id).addEventListener('input', () => renderOverlay());
    $(id).addEventListener('change', () => renderOverlay());
  });

  $('brightnessRange').addEventListener('input', (event) => {
    const value = num(event.target.value, 128);
    $('brightnessValue').textContent = String(value);
    sendBrightness(value);
  });

  $('zoomRange').addEventListener('input', (event) => setZoom(num(event.target.value, 16), true));
  $('fitZoomButton').addEventListener('click', () => {
    state.userSetZoom = false;
    autoFitZoom();
  });

  // Draw tab
  $('saveFrameButton').addEventListener('click', saveOverlayAsAnimation);
  $('clearAllButton').addEventListener('click', clearAllObjects);
  $('undoButton').addEventListener('click', undo);
  $('redoButton').addEventListener('click', redo);
  $('sendRawFrameButton').addEventListener('click', sendRawFrame);
  $('assetSelect').addEventListener('change', (event) => selectAsset(event.target.value, false));
  $('openAssetEditorButton').addEventListener('click', () => openAssetEditor(null));
  document.querySelectorAll('.tool').forEach((button) => button.addEventListener('click', () => setTool(button.dataset.tool)));

  // Selection panel
  const commitSelXY = () => {
    const stored = selectedObject();
    if (!stored) return;
    moveSelectedTo(num($('selX').value, stored.fields.x), num($('selY').value, stored.fields.y), false);
  };
  ['selX', 'selY'].forEach((id) => {
    $(id).addEventListener('change', commitSelXY);
    // Enter commits and hands focus back, so the arrow keys nudge again
    // instead of stepping the number field.
    $(id).addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        commitSelXY();
        event.target.blur();
      }
    });
  });
  $('selRemoveButton').addEventListener('click', removeSelection);
  $('selDuplicateButton').addEventListener('click', duplicateSelection);
  $('selEditButton').addEventListener('click', () => {
    const layer = state.overlayLayers.find((l) => l.objname === state.selected);
    if (layer) openObjectEditor(layer);
  });

  // Objects
  $('applyObjectButton').addEventListener('click', applyObjectEdit);
  $('removeObjectButton').addEventListener('click', removeObject);

  // Assets
  $('newAssetButton').addEventListener('click', () => openAssetEditor(null));
  $('assetUpload').addEventListener('change', (event) => {
    const files = [...event.target.files];
    event.target.value = '';
    if (files.length) uploadAssetFiles(files);
  });

  // Animate
  $('animationSelector').addEventListener('change', loadParamsForSelected);
  $('startAnimationButton').addEventListener('click', () => startAnimation($('animationSelector').value, $('cyclesInput').value, collectParams()));
  $('stopAnimationButton').addEventListener('click', stopAnimation);
  $('speedRange').addEventListener('input', (event) => {
    speedTouchedAt = Date.now();
    const value = num(event.target.value, 1);
    $('speedValue').textContent = `${value.toFixed(1)}×`;
    sendSpeed(value);
  });
  $('captureFrameButton').addEventListener('click', () => captureFrame());
  $('addAnimStepButton').addEventListener('click', addAnimStep);
  $('uploadTimelineButton').addEventListener('click', uploadTimeline);
  $('editTimelineJsonButton').addEventListener('click', () => {
    if (!state.timeline.length) {
      toast('Add at least one step first.', 'error');
      return;
    }
    openJsonEditor({ name: sanitizeAnimName($('timelineName').value), doc: buildTimelineAnim() });
  });
  $('clearTimelineButton').addEventListener('click', () => {
    if (state.timeline.length && !window.confirm('Remove all steps?')) return;
    state.timeline = [];
    renderTimeline();
  });

  // Files
  $('refreshFilesButton').addEventListener('click', () => {
    assetCache.clear();
    refreshFiles();
    refreshComposition();
  });
  $('newAnimJsonButton').addEventListener('click', () => openJsonEditor({ name: '', doc: ANIM_TEMPLATE }));
  $('jsonFormatButton').addEventListener('click', () => {
    const { doc, error } = readJsonEditor();
    if (error) showProblems({ errors: [error], warns: [] });
    else $('jsonEditorText').value = JSON.stringify(doc, null, 2);
  });
  $('jsonCheckButton').addEventListener('click', checkJsonEditor);
  $('jsonUploadButton').addEventListener('click', uploadJsonEditor);
  $('jsonEditorText').addEventListener('keydown', (event) => {
    if (event.key === 'Tab') {
      event.preventDefault();
      const el = event.target;
      const s = el.selectionStart;
      el.setRangeText('  ', s, el.selectionEnd, 'end');
    }
  });

  // Canvas
  canvas.addEventListener('pointerdown', onPointerDown);
  canvas.addEventListener('pointermove', onPointerMove);
  canvas.addEventListener('pointerup', onPointerUp);
  canvas.addEventListener('pointercancel', onPointerCancel);
  canvas.addEventListener('pointerleave', onPointerLeave);
  $('canvasStage').addEventListener('wheel', (event) => {
    if (!(event.ctrlKey || event.metaKey)) return;
    event.preventDefault();
    setZoom(state.zoom + (event.deltaY < 0 ? 2 : -2), true);
  }, { passive: false });

  // Keyboard
  const TOOL_KEYS = { v: 'select', t: 'text', s: 'scroll', l: 'line', r: 'rectangle', a: 'asset', m: 'move' };
  const ARROWS = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') {
      const open = openModalIds();
      if (open.length) {
        closeModal(open[open.length - 1]);
        return;
      }
      if (state.drag) onPointerCancel();
      else if (state.selected) setSelection(null);
      return;
    }
    if (isTyping(event.target) || openModalIds().length) return;
    const key = event.key.toLowerCase();
    if (event.ctrlKey || event.metaKey) {
      if (key === 'z' && !event.shiftKey) { event.preventDefault(); undo(); }
      else if ((key === 'z' && event.shiftKey) || key === 'y') { event.preventDefault(); redo(); }
      return;
    }

    if (state.activeTool === 'select') {
      if (ARROWS[event.key]) {
        event.preventDefault();
        const [dx, dy] = ARROWS[event.key];
        const step = event.shiftKey ? 5 : 1;
        nudgeSelection(dx * step, dy * step);
        return;
      }
      if (event.key === 'Tab') {
        event.preventDefault();
        cycleSelection(event.shiftKey ? -1 : 1);
        return;
      }
      if ((event.key === 'Delete' || event.key === 'Backspace') && state.selected) {
        event.preventDefault();
        removeSelection();
        return;
      }
      if (event.key === 'Enter' && state.selected) {
        event.preventDefault();
        $('selEditButton').click();
        return;
      }
    }

    if (!event.altKey && TOOL_KEYS[key]) setTool(TOOL_KEYS[key]);
  });

  window.addEventListener('resize', debounce(() => {
    autoFitZoom();
    renderOverlay();
  }, 120));

  // Refresh right away when the tab comes back into view.
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) {
      loadLiveView();
      refreshComposition();
      refreshStatus();
    }
  });
}

function bindTabs() {
  document.querySelectorAll('.tab-btn').forEach((button) => {
    button.addEventListener('click', () => switchTab(button.dataset.tab));
  });
}

// Each poller waits for its previous request, so a slow sign never gets a
// pile-up of overlapping requests. Nothing is polled while the page is hidden.
function startPolling() {
  const loop = (fn, ms) => {
    const tick = () => {
      const run = document.hidden ? Promise.resolve() : Promise.resolve(fn()).catch(() => {});
      run.finally(() => setTimeout(tick, ms));
    };
    setTimeout(tick, ms);
  };
  loop(loadLiveView, POLL_MS.frame);
  loop(refreshComposition, POLL_MS.composition);
  loop(refreshStatus, POLL_MS.status);
}

function init() {
  $('apiBase').value = api.base;
  buildColorSwatches();
  buildFontSelects();
  initAssetEditor();
  bindEvents();
  bindTabs();
  setTool('text');
  updateColorUi();
  applyFrameGeometry();
  autoFitZoom();
  renderTimeline();
  updateHistoryButtons();
  connect();
  startPolling();
}

document.addEventListener('DOMContentLoaded', init);
