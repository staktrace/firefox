// Deterministic allocation-heavy microbenchmark for comparing allocators.
//
// A fixed number of steps; each step runs a fixed list of operations in four
// components (JS engine, DOM/style, layout, graphics) driven by a seeded PRNG,
// then waits for the next refresh tick so painting happens between steps.
// Part of what each step creates survives in fixed-size ring buffers and is
// evicted later, so frees do not happen in allocation order.
//
// Query parameters: steps, seed (default 1), scale (default 1), tag (copied
// into the result), only (comma-separated op names to run). The result is
// printed via dump() as one line starting with "ALLOC_MICRO_RESULT " (needs
// browser.dom.window.dump.enabled).
"use strict";

const params = new URLSearchParams(location.search);
const STEPS = Number(params.get("steps") || 900);
const SEED = Number(params.get("seed") || 1);
const SCALE = Number(params.get("scale") || 1);
const TAG = params.get("tag") || "";
const ONLY = params.get("only") ? new Set(params.get("only").split(",")) : null;

function mulberry32(a) {
  return function () {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rng = mulberry32(SEED);
const randInt = k => Math.floor(rng() * k);
const pick = a => a[randInt(a.length)];
const n = k => Math.max(1, Math.round(k * SCALE));

class Ring {
  constructor(cap, onEvict) {
    this.cap = cap;
    this.items = [];
    this.onEvict = onEvict;
  }
  add(x) {
    if (this.items.length < this.cap) {
      this.items.push(x);
      return;
    }
    const i = randInt(this.cap);
    if (this.onEvict) this.onEvict(this.items[i]);
    this.items[i] = x;
  }
  clear() {
    if (this.onEvict) this.items.forEach(this.onEvict);
    this.items = [];
  }
}
const rings = [];
const ring = (cap, onEvict) => {
  const r = new Ring(cap, onEvict);
  rings.push(r);
  return r;
};
const removeEl = el => el.remove();

const WORDS = (
  "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod " +
  "tempor incididunt ut labore et dolore magna aliqua enim ad minim veniam " +
  "quis nostrud exercitation ullamco laboris nisi aliquip ex ea commodo " +
  "consequat duis aute irure in reprehenderit voluptate velit esse cillum " +
  "fugiat nulla pariatur excepteur sint occaecat cupidatat non proident " +
  "sunt culpa qui officia deserunt mollit anim id est laborum ĉiuj ŝtonoj " +
  "Größe naïve façade résumé 東京 世界 مرحبا שלום Ελλάδα"
).split(" ");
const words = k => {
  let s = "";
  for (let i = 0; i < k; i++) s += (i ? " " : "") + pick(WORDS);
  return s;
};
const color = () => `rgb(${randInt(256)}, ${randInt(256)}, ${randInt(256)})`;
const TAGS = ["div", "span", "p", "section", "article", "ul", "li", "a", "em", "b"];

const stage = document.getElementById("stage");
let sink = 0;

// ---- JS engine -----------------------------------------------------------

const jsKeep = ring(300);

function js_objects() {
  const objs = [];
  for (let i = 0; i < n(1000); i++) {
    const o = { id: i, v: rng() };
    const k = randInt(24);
    for (let j = 0; j < k; j++) o["p" + ((j * 7 + i) % 40)] = j;
    if (k > 18) delete o.id;
    objs.push(o);
  }
  const m = new Map();
  for (let i = 0; i < n(1000); i++) m.set("k" + randInt(5000), objs[i]);
  jsKeep.add(randInt(4) ? objs[randInt(objs.length)] : m);
}

function js_strings() {
  // String building (join, replace, encode, JSON) grows StringBuilder buffers
  // by realloc, and flattening ropes allocates the final buffer.
  let rope = "";
  for (let i = 0; i < n(300); i++) rope += pick(WORDS) + i;
  const parts = rope.toUpperCase().split("E");
  const joined = parts.join("-").replace(/[AEIOU]+/g, m => m.toLowerCase() + m.length);
  const enc = encodeURIComponent(joined.slice(0, 1 + randInt(4000)));
  const pieces = [];
  for (let i = 0; i < n(40); i++) {
    pieces.push(words(1 + randInt(20)).padEnd(16 + randInt(200), ".") + i);
  }
  const big = pieces.join(randInt(2) ? "\n" : ", ");
  sink += JSON.stringify({ joined, enc, big }).length;
  jsKeep.add(big.slice(randInt(100)));
}

function js_buffers() {
  for (let i = 0; i < n(40); i++) {
    const size = 64 << randInt(12); // 64 B .. 128 KiB
    const ta = new Uint8Array(size);
    ta[size - 1] = i;
    if (randInt(8) === 0) jsKeep.add(ta);
  }
}

function js_json_clone() {
  const data = [];
  for (let i = 0; i < n(100); i++) {
    data.push({ id: i, name: words(3), tags: [pick(WORDS), pick(WORDS)], v: rng() });
  }
  const json = JSON.stringify(data);
  const back = JSON.parse(json);
  const cloned = structuredClone(back);
  jsKeep.add(cloned[randInt(cloned.length)]);
}

function js_regexp() {
  const text = words(n(200));
  for (let i = 0; i < n(6); i++) {
    const re = new RegExp(
      "(" + pick(WORDS) + "|" + pick(WORDS) + ")\\s+(\\w{" + (1 + randInt(5)) + ",})",
      "g"
    );
    sink += (text.match(re) || []).length;
  }
}

function js_compile() {
  for (let i = 0; i < n(3); i++) {
    let body = `let t = ${randInt(1000)};`;
    const lines = 10 + randInt(30);
    for (let j = 0; j < lines; j++) {
      body +=
        `const v${j} = { a: x[${j} % x.length] * ${rng()}, s: "${pick(WORDS)}" };` +
        `if (v${j}.a > ${rng()}) t += v${j}.s.length; else t -= ${j};`;
    }
    body += "return t;";
    sink += new Function("x", body)([1, 2, 3]);
  }
}

function js_url_text() {
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  for (let i = 0; i < n(30); i++) {
    const u = new URL(`https://ex${randInt(50)}.example/${pick(WORDS)}/${i}?q=${pick(WORDS)}&n=${i}#${pick(WORDS)}`);
    u.searchParams.append(pick(WORDS), words(2));
    const bytes = enc.encode(u.href + words(10));
    sink += dec.decode(bytes).length;
  }
}

// ---- DOM -----------------------------------------------------------------

const domKeep = ring(40, removeEl);

function buildTree(depth, breadth) {
  const el = document.createElement(pick(TAGS));
  el.className = `c${randInt(400)} c${randInt(400)}`;
  el.setAttribute("data-k", randInt(8));
  el.setAttribute("title", words(2));
  if (randInt(3) === 0) {
    el.style.cssText =
      `color: ${color()}; margin: ${randInt(6)}px; border-radius: ${randInt(12)}px`;
  }
  if (depth === 0) {
    el.textContent = words(1 + randInt(6));
    return el;
  }
  for (let i = 0; i < breadth; i++) el.append(buildTree(depth - 1, breadth));
  if (randInt(2)) el.append(document.createTextNode(words(3)));
  return el;
}

function dom_create() {
  const roots = [];
  for (let i = 0; i < n(4); i++) roots.push(buildTree(3, 4));
  domKeep.add(roots[randInt(roots.length)]);
}

function dom_innerhtml() {
  let html = "";
  for (let i = 0; i < n(150); i++) {
    const t = pick(TAGS);
    html += `<${t} class="c${randInt(400)}" data-i="${i}" title="${pick(WORDS)}">${words(1 + randInt(5))}</${t}>`;
  }
  const div = document.createElement("div");
  div.innerHTML = html;
  sink += div.innerHTML.length;
  const clone = div.cloneNode(true);
  domKeep.add(clone);
}

function dom_mutate() {
  const host = document.createElement("div");
  for (let i = 0; i < n(100); i++) {
    const el = document.createElement("span");
    el.id = "id" + randInt(100000);
    el.setAttribute("aria-label", words(2));
    el.dataset.v = String(rng());
    el.textContent = words(2);
    host.append(el);
  }
  for (const el of host.children) {
    el.textContent += pick(WORDS);
    el.removeAttribute("aria-label");
    el.setAttribute("data-w", pick(WORDS) + randInt(1000));
  }
  const range = document.createRange();
  range.selectNodeContents(host);
  const frag = range.cloneContents();
  sink += frag.childNodes.length;
}

function dom_parser() {
  let xml = "<root>";
  for (let i = 0; i < n(80); i++) xml += `<item id="${i}" k="${pick(WORDS)}">${words(3)}</item>`;
  xml += "</root>";
  const doc = new DOMParser().parseFromString(xml, "application/xml");
  sink += doc.getElementsByTagName("item").length;
}


function dom_text() {
  // Strings of widely varying sizes copied into and out of DOM text and
  // attribute storage.
  const host = document.createElement("div");
  for (let i = 0; i < n(60); i++) {
    const t = document.createTextNode(words(1 << randInt(9)));
    host.append(t);
    if (i % 3 === 0) host.setAttribute("data-a" + (i % 7), words(1 << randInt(7)));
  }
  sink += host.textContent.length;
  for (const t of host.childNodes) {
    t.data = t.data.slice(randInt(10)) + pick(WORDS);
  }
  domKeep.add(host);
}

function style_cssom() {
  const sheet = new CSSStyleSheet();
  for (let i = 0; i < n(120); i++) {
    sheet.insertRule(
      `.k${randInt(1000)} > .m${i} { color: ${color()}; margin: ${randInt(9)}px ${randInt(9)}px; ` +
        `transition: opacity ${randInt(500)}ms ease-in, transform ${randInt(500)}ms; }`,
      sheet.cssRules.length
    );
    if (randInt(3) === 0) sheet.deleteRule(randInt(sheet.cssRules.length));
  }
  for (const r of sheet.cssRules) {
    r.style.setProperty("--x", String(randInt(100)));
    sink += r.cssText.length;
  }
}

// ---- Style ---------------------------------------------------------------

const sheetKeep = ring(3, removeEl);
const styleKeep = ring(20, removeEl);
let classSeq = 0;

function makeRules(count) {
  let css = "";
  for (let i = 0; i < count; i++) {
    const c = "c" + ((classSeq + i) % 400);
    const sel = pick([
      `.${c}`,
      `div > .${c}`,
      `.${c} span`,
      `.${c}[data-k="${randInt(8)}"]`,
      `:is(p, li).${c}`,
    ]);
    css +=
      `${sel} { color: ${color()}; --v${i % 16}: ${randInt(100)}px; ` +
      `margin: ${randInt(6)}px; padding: var(--v${randInt(16)}, 2px); ` +
      `border: ${randInt(3)}px solid ${color()}; ` +
      `background: linear-gradient(${randInt(360)}deg, ${color()}, ${color()}); ` +
      `font-weight: ${pick([300, 400, 600, 800])}; }\n`;
  }
  classSeq += 37;
  return css;
}

function style_parse() {
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(makeRules(n(150)));
  sink += sheet.cssRules.length;
  for (let i = 0; i < n(80); i++) {
    const el = document.createElement("div");
    el.style.cssText =
      `transform: rotate(${randInt(360)}deg) scale(${rng().toFixed(2)}); ` +
      `box-shadow: 0 ${randInt(5)}px ${randInt(9)}px ${color()}; ` +
      `grid-template-columns: repeat(${1 + randInt(4)}, ${randInt(100)}px 1fr)`;
    sink += el.style.length;
  }
}

function style_restyle() {
  const style = document.createElement("style");
  style.textContent = makeRules(n(30));
  document.head.append(style);
  sheetKeep.add(style);

  const root = buildTree(2, 5);
  root.className = "tree";
  stage.append(root);
  styleKeep.add(root);
  const all = root.querySelectorAll("*");
  for (let i = 0; i < all.length; i++) {
    all[i].classList.toggle(`c${randInt(400)}`);
    if (i % 4 === 0) sink += parseFloat(getComputedStyle(all[i]).marginLeft) || 0;
  }
}

// ---- Layout --------------------------------------------------------------

const layoutKeep = ring(6, removeEl);

function textBlock() {
  const p = document.createElement("p");
  p.style.cssText =
    `font: ${pick(["italic ", "", "bold "])}${10 + randInt(14)}px ` +
    `${pick(["serif", "sans-serif", "monospace", "Georgia", "Helvetica"])}; ` +
    `letter-spacing: ${randInt(3)}px`;
  for (let i = 0; i < 3; i++) {
    const s = document.createElement(pick(["span", "b", "i", "code"]));
    s.textContent = words(3 + randInt(12)) + " ";
    p.append(s);
  }
  return p;
}

function layout_text() {
  const box = document.createElement("div");
  box.className = "lay";
  const kind = randInt(3);
  if (kind === 0) {
    box.style.cssText = `display: flex; flex-wrap: wrap; gap: ${randInt(8)}px`;
    for (let i = 0; i < n(10); i++) {
      const item = textBlock();
      item.style.flex = `${1 + randInt(3)} 1 ${40 + randInt(200)}px`;
      box.append(item);
    }
  } else if (kind === 1) {
    box.style.cssText = `display: grid; grid-template-columns: repeat(${2 + randInt(5)}, minmax(0, 1fr))`;
    for (let i = 0; i < n(10); i++) box.append(textBlock());
  } else {
    const table = document.createElement("table");
    for (let r = 0; r < n(8); r++) {
      const tr = table.insertRow();
      for (let c = 0; c < 4; c++) tr.insertCell().textContent = words(2 + randInt(6));
    }
    box.append(table);
  }
  stage.append(box);
  box.style.width = 300 + randInt(500) + "px";
  sink += box.offsetHeight;
  layoutKeep.add(box);
}

// ---- Graphics ------------------------------------------------------------

const gfxKeep = ring(10, removeEl);
const SVGNS = "http://www.w3.org/2000/svg";
const spinners = [];

function gfx_canvas() {
  const w = 32 + randInt(160);
  const h = 32 + randInt(160);
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  const ctx = c.getContext("2d", { willReadFrequently: true });
  const g = ctx.createLinearGradient(0, 0, w, h);
  g.addColorStop(0, color());
  g.addColorStop(1, color());
  ctx.fillStyle = g;
  ctx.fillRect(0, 0, w, h);
  for (let i = 0; i < n(20); i++) {
    ctx.beginPath();
    ctx.moveTo(randInt(w), randInt(h));
    for (let j = 0; j < 4; j++) {
      ctx.bezierCurveTo(randInt(w), randInt(h), randInt(w), randInt(h), randInt(w), randInt(h));
    }
    ctx.strokeStyle = color();
    ctx.stroke();
  }
  ctx.font = `${10 + randInt(20)}px sans-serif`;
  for (let i = 0; i < n(5); i++) ctx.fillText(words(3), randInt(w), randInt(h));
  for (let i = 0; i < n(4); i++) {
    const img = ctx.getImageData(randInt(w / 2), randInt(h / 2), 16 + randInt(w / 2), 16 + randInt(h / 2));
    ctx.putImageData(img, randInt(w), randInt(h));
  }
  const box = document.createElement("div");
  box.className = "gfx";
  box.append(c);
  stage.append(box);
  gfxKeep.add(box);
}

function gfx_svg() {
  const svg = document.createElementNS(SVGNS, "svg");
  svg.setAttribute("width", 200);
  svg.setAttribute("height", 200);
  svg.setAttribute("class", "gfx");
  for (let i = 0; i < n(20); i++) {
    const path = document.createElementNS(SVGNS, "path");
    let d = `M${randInt(200)} ${randInt(200)}`;
    for (let j = 0; j < 6; j++) d += ` Q${randInt(200)} ${randInt(200)} ${randInt(200)} ${randInt(200)}`;
    path.setAttribute("d", d);
    path.setAttribute("fill", randInt(2) ? "none" : color());
    path.setAttribute("stroke", color());
    svg.append(path);
  }
  stage.append(svg);
  gfxKeep.add(svg);
}

function gfx_layers(step) {
  // Transformed, translucent layers that move every step, so each refresh
  // tick builds and sends a new display list.
  if (spinners.length < n(40)) {
    const s = document.createElement("div");
    s.className = "spinner";
    s.style.background = color();
    s.textContent = words(2);
    stage.append(s);
    spinners.push(s);
  }
  for (let i = 0; i < spinners.length; i++) {
    const s = spinners[i];
    s.style.transform =
      `translate(${(i * 37 + step * 3) % 900}px, ${(i * 53 + step * 2) % 600}px) ` +
      `rotate(${(step * (i + 1)) % 360}deg)`;
    s.style.opacity = 0.3 + ((i + step) % 7) / 10;
  }
}

// ---- Driver --------------------------------------------------------------

// Each op appears as many times per step as its weight. Ops whose time went
// almost entirely to non-allocating work in profiles have weight 0.
const WEIGHTS = new Map([
  [js_objects, 0], [js_strings, 3], [js_buffers, 0], [js_json_clone, 3],
  [js_regexp, 0], [js_compile, 2], [js_url_text, 0],
  [dom_create, 1], [dom_innerhtml, 2], [dom_mutate, 1], [dom_parser, 0], [dom_text, 0],
  [style_parse, 2], [style_cssom, 2], [style_restyle, 1],
  [layout_text, 1],
  [gfx_canvas, 1], [gfx_svg, 3], [gfx_layers, 1],
]);
const OPS = [];
for (const [f, w] of WEIGHTS) {
  if (ONLY ? ONLY.has(f.name) : w > 0) {
    for (let i = 0; i < Math.max(w, 1); i++) OPS.push(f);
  }
}
const opMs = Object.fromEntries([...new Set(OPS)].map(f => [f.name, 0]));
const nextFrame = () => new Promise(r => requestAnimationFrame(r));

async function main() {
  await nextFrame();
  await nextFrame();
  const t0 = performance.now();
  for (let step = 0; step < STEPS; step++) {
    for (const f of OPS) {
      const s = performance.now();
      f(step);
      opMs[f.name] += performance.now() - s;
    }
    await nextFrame();
  }
  const wallMs = performance.now() - t0;
  rings.forEach(r => r.clear());
  spinners.forEach(removeEl);

  const round = x => Math.round(x * 10) / 10;
  const workMs = Object.values(opMs).reduce((a, b) => a + b, 0);
  const result = {
    tag: TAG,
    steps: STEPS,
    seed: SEED,
    scale: SCALE,
    wallMs: round(wallMs),
    workMs: round(workMs),
    opMs: Object.fromEntries(Object.entries(opMs).map(([k, v]) => [k, round(v)])),
    sink: sink !== 0,
  };
  dump("ALLOC_MICRO_RESULT " + JSON.stringify(result) + "\n");
  document.title = "done";
}

main();
