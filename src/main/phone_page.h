// The phone controller page (phone_pads.cpp serves it). Landscape: a stick on the left (the first
// touch there is the centre), the action buttons on the right with labels that follow the game (menus,
// play select, a play), START at the top, and a menu for save states, Redo Play and taking player 1.
// Messages from the app: "v <version>", "p <player>", "r 1|0" (rumble), "m menu|select|play|offense|defense",
// "t <status>".
// To it: "s <buttons> <x> <y>", "c save|load|redo|slot+|slot-|p1".
#pragma once

// Bump with every change to the page: an open page from an older build reloads itself on connect.
static const int phone_page_version = 8;  // 4: the Blitz '27 look (docs/design/redesign); 5: the wide logo in the menu; 7: buttons fit under a browser's bar; 8: the can't-connect box

// BRANDING: phone page <title>Blitz Controller</title> and the Blitz '27 style below
static const char* phone_page_html = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#020712">
<title>Blitz Controller</title>
<style>
/* Blitz '27 house style: midnight ground, purple swoosh, cyan grid, chrome bezels, yellow italic caps.
   Self-contained: no fonts, images or scripts from anywhere else (the phone may only have the LAN). */
:root {
  --ground:#020712; --navy:#070F22; --deep:#0D1A3A; --glass:rgba(6,18,44,.92); --rim:rgba(143,239,245,.7);
  --rim-soft:rgba(143,239,245,.24); --grid:rgba(47,179,179,.14); --text:#F2F2F2; --dim:#B8C4DA;
  --yellow:#F6D400; --yellow-hi:#FFE14A; --red:#B3101E; --green:#45E04A; --purple:#3F0A5F;
  --me:#B8C4DA;
  --display:"Avenir Next Condensed","AvenirNextCondensed-HeavyItalic","Roboto Condensed",sans-serif-condensed,"Arial Narrow",system-ui,sans-serif;
  --body:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  --chrome:conic-gradient(from 210deg,#F4F7FC,#8C98AE 18%,#E6ECF5 32%,#4C5770 50%,#C9D3E3 68%,#6E7A92 84%,#F4F7FC);
  --bar:60px;
}
* { box-sizing:border-box; -webkit-user-select:none; user-select:none; -webkit-touch-callout:none; touch-action:none;
    -webkit-tap-highlight-color:transparent; }
html,body { margin:0; height:100%; height:100dvh; background:var(--ground); color:var(--text); font:600 16px/1.15 var(--body); overflow:hidden; }
body { background:
  radial-gradient(90% 70% at 100% 0%, rgba(63,10,95,.85), transparent 62%),
  radial-gradient(70% 60% at 0% 100%, color-mix(in srgb, var(--me) 16%, transparent), transparent 70%),
  var(--ground); }
.cap { font-family:var(--display); font-style:italic; font-weight:800; font-stretch:condensed; text-transform:uppercase; letter-spacing:.03em; }

/* Top bar: who you are, which side of the ball, START and the menu. */
#bar { position:fixed; top:0; left:0; right:0; height:calc(env(safe-area-inset-top) + var(--bar)); display:flex; align-items:center; gap:12px;
  padding:env(safe-area-inset-top) calc(env(safe-area-inset-right) + 14px) 0 calc(env(safe-area-inset-left) + 16px);
  background:linear-gradient(180deg, rgba(2,7,18,.92), rgba(2,7,18,.55)); }
#bar::after { content:""; position:absolute; left:0; right:0; bottom:0; height:3px;
  background:linear-gradient(90deg, var(--me), color-mix(in srgb, var(--me) 40%, transparent) 55%, transparent); }
#chip { min-width:40px; height:36px; padding:0 8px; border-radius:4px; display:inline-flex; align-items:center; justify-content:center;
  background:var(--me); color:#020712; font-size:28px; line-height:1; }
#who { font-size:26px; line-height:1; color:var(--text); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
#mode { font:800 13px/1 var(--body); letter-spacing:.16em; text-transform:uppercase; color:#8FEFF5; padding:6px 9px;
  border:1.5px solid var(--rim-soft); border-radius:3px; white-space:nowrap; }
#mode:empty { display:none; }
#spacer { flex:1; }
.top { position:relative; height:48px; display:flex; align-items:center; justify-content:center; border-radius:4px;
  border:2px solid var(--rim-soft); background:var(--deep); color:var(--text); }
.top::before { content:""; position:absolute; inset:-12px -6px; }    /* 72 px tall touch area */
#start { min-width:112px; padding:0 18px; font-size:22px; }
#start.on { background:var(--red); border-color:var(--yellow-hi); color:var(--yellow-hi); }
#more { width:68px; font:900 24px/1 var(--body); letter-spacing:.08em; padding-bottom:6px; }

/* Stick: the first touch on the left is the centre. */
#stick { position:fixed; left:0; top:calc(env(safe-area-inset-top) + var(--bar)); bottom:0; width:46%;
  background-image:linear-gradient(var(--grid) 1px, transparent 1px), linear-gradient(90deg, var(--grid) 1px, transparent 1px);
  background-size:32px 32px; background-position:-1px -1px;
  border-right:1px solid var(--rim-soft); }
#ring { position:absolute; width:156px; height:156px; margin:-78px 0 0 -78px; border-radius:50%; display:none;
  border:3px solid var(--rim); background:radial-gradient(circle, rgba(6,18,44,.2) 55%, rgba(143,239,245,.12)); }
#ring::after { content:""; position:absolute; inset:18px; border-radius:50%; border:2px dashed rgba(143,239,245,.3); }
#knob { position:absolute; width:72px; height:72px; margin:-36px 0 0 -36px; border-radius:50%; display:none;
  background:radial-gradient(circle at 40% 32%, color-mix(in srgb, var(--me) 45%, #fff), var(--me) 55%, color-mix(in srgb, var(--me) 60%, #000));
  box-shadow:0 0 0 4px #0a1022, 0 0 28px color-mix(in srgb, var(--me) 55%, transparent); }
#hint { position:absolute; left:50%; top:50%; transform:translate(-50%,-50%); display:flex; flex-direction:column; align-items:center; gap:10px;
  color:var(--dim); text-align:center; pointer-events:none; }
#hint .dial { width:112px; height:112px; border-radius:50%; border:3px dashed rgba(143,239,245,.4); display:flex; align-items:center; justify-content:center; }
#hint .dial i { width:44px; height:44px; border-radius:50%; background:color-mix(in srgb, var(--me) 70%, transparent); }
#hint b { font-size:28px; color:var(--text); }
#hint span { font:700 12px/1 var(--body); letter-spacing:.16em; text-transform:uppercase; }

/* Buttons: arcade caps in a chrome bezel, sized and placed by the button area's own height (cqh), so they fit whatever
   a browser's bars leave (2026-11-14: Chrome's address bar in landscape put TURBO over the top bar). */
#pad { position:fixed; right:0; top:calc(env(safe-area-inset-top) + var(--bar)); bottom:0; width:54%; container-type:size; }
.b { position:absolute; border-radius:50%; background:var(--chrome); padding:6px; display:flex; flex-direction:column; align-items:center;
  justify-content:center; gap:4px; text-align:center; box-shadow:0 7px 0 rgba(0,0,0,.45), 0 0 0 2px #050a18;
  transition:transform .06s, box-shadow .06s; }
.b::after { content:""; position:absolute; inset:7px; border-radius:50%; background:var(--capfill); box-shadow:inset 0 -6px 0 rgba(0,0,0,.25), inset 0 5px 0 rgba(255,255,255,.18); }
.b > span { position:relative; z-index:1; }
.b .l { font-family:var(--display); font-style:italic; font-weight:800; font-stretch:condensed; font-size:clamp(18px, 8cqh, 30px);
  line-height:1; letter-spacing:.03em; color:var(--captext); }
.b .s { font:800 clamp(10px, 3.6cqh, 13px)/1 var(--body); letter-spacing:.12em; text-transform:uppercase; color:var(--captext); opacity:.8;
  max-width:80%; }
.b .s:empty { display:none; }
.b.on { transform:translateY(5px) scale(.96); box-shadow:0 2px 0 rgba(0,0,0,.45), 0 0 0 2px #050a18, 0 0 26px var(--glow); }
.b.on::after { filter:brightness(1.35); }
/* In the button area (H = its height): PASS bottom right, JUMP to its left, TURBO above between them, none overlapping. */
#pass  { --capfill:radial-gradient(circle at 45% 30%, #F0414F, #B3101E 58%, #6E0812); --captext:#FFE14A; --glow:rgba(255,80,80,.6);
  width:52cqh; height:52cqh; right:calc(env(safe-area-inset-right) + 4cqh); bottom:6cqh; }
#jump  { --capfill:radial-gradient(circle at 45% 30%, #7A2BB0, #3F0A5F 60%, #22053A); --captext:#F2F2F2; --glow:rgba(150,80,220,.6);
  width:40cqh; height:40cqh; right:calc(env(safe-area-inset-right) + 60cqh); bottom:4cqh; }
#turbo { --capfill:radial-gradient(circle at 45% 30%, #FFD84A, #FF9A00 55%, #E05200); --captext:#1A0A00; --glow:rgba(255,170,0,.6);
  width:36cqh; height:36cqh; right:calc(env(safe-area-inset-right) + 38cqh); bottom:58cqh; }

/* Status toast over the stick area (never over a button): eyebrow + title, like the game's yardage banners. */
#toast { position:fixed; left:23%; top:calc(env(safe-area-inset-top) + var(--bar) + 10px); transform:translate(-50%,-10px); opacity:0;
  max-width:calc(46vw - 24px); padding:10px 18px 12px; background:var(--glass); border:2px solid var(--rim);
  clip-path:polygon(8px 0, calc(100% - 8px) 0, 100% 8px, 100% calc(100% - 8px), calc(100% - 8px) 100%, 8px 100%, 0 calc(100% - 8px), 0 8px);
  transition:opacity .15s, transform .15s; pointer-events:none; text-align:center; }
#toast.show { opacity:1; transform:translate(-50%,0); }
#toast .e { display:block; font:800 12px/1 var(--body); letter-spacing:.16em; text-transform:uppercase; color:#8FEFF5; margin-bottom:5px; }
#toast .e:empty { display:none; }
#toast .t { display:block; font-size:24px; line-height:1.05; color:var(--yellow-hi); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
#toast::after { content:""; display:block; height:3px; margin-top:8px; background:linear-gradient(90deg, #FF5000, #FFAA00 45%, rgba(255,208,0,0)); }

/* The ··· sheet: save states, Redo Play, slots, taking player 1. */
#sheet { position:fixed; inset:0; background:rgba(2,7,18,.84); display:none; align-items:center; justify-content:center;
  padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left); }
#sheet.open { display:flex; }
#menu { position:relative; width:min(760px, 92vw); padding:16px; display:grid; gap:10px; grid-template-columns:repeat(4, minmax(0, 1fr));
  background:
    linear-gradient(var(--grid) 1px, transparent 1px) 0 0/32px 32px,
    linear-gradient(90deg, var(--grid) 1px, transparent 1px) 0 0/32px 32px,
    var(--glass);
  border:2px solid var(--rim);
  clip-path:polygon(10px 0, calc(100% - 10px) 0, 100% 10px, 100% calc(100% - 10px), calc(100% - 10px) 100%, 10px 100%, 0 calc(100% - 10px), 0 10px); }
#menu h2 { grid-column:1/-1; margin:0 0 2px; display:flex; align-items:center; gap:10px; font-size:26px; line-height:1; color:var(--yellow); }
#menu h2 img { height:30px; width:auto; display:block; }
#menu h2 small { font:800 12px/1 var(--body); letter-spacing:.16em; color:var(--dim); margin-left:auto; font-style:normal; }
#menu button { min-height:72px; border-radius:4px; border:2px solid var(--rim-soft); background:var(--deep); color:var(--text);
  font-family:var(--display); font-style:italic; font-weight:800; font-stretch:condensed; font-size:22px; line-height:1.05;
  letter-spacing:.03em; text-transform:uppercase; padding:8px; }
#menu button:active { background:var(--red); border-color:var(--yellow-hi); color:var(--yellow-hi); }
#menu button[data-c="p1"] { border-color:var(--yellow); color:var(--yellow); }
#menu .arrow { font-size:28px; }
#slot { display:flex; flex-direction:column; align-items:center; justify-content:center; gap:4px; min-height:72px; border:2px dashed var(--rim-soft); border-radius:4px; }
#slot b { font-size:30px; line-height:1; color:var(--yellow-hi); }
#slot span { font:800 11px/1 var(--body); letter-spacing:.16em; text-transform:uppercase; color:var(--dim); }
#menu .close { background:transparent; color:var(--dim); }
#fsHint { display:none; grid-column:1/-1; margin:0; font:600 15px/1.3 var(--body); color:var(--dim); }
#fsHint b { color:var(--text); }

/* Can't reach the game for 5 s: what to do (dismissable; it goes away by itself once connected). */
#stuck { position:fixed; inset:0; background:rgba(2,7,18,.84); display:none; align-items:center; justify-content:center;
  padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left); }
#stuck.open { display:flex; }
#stuck .box { width:min(560px, 90vw); padding:18px 20px 16px; background:var(--glass); border:2px solid var(--rim);
  clip-path:polygon(10px 0, calc(100% - 10px) 0, 100% 10px, 100% calc(100% - 10px), calc(100% - 10px) 100%, 10px 100%, 0 calc(100% - 10px), 0 10px); }
#stuck h2 { margin:0 0 10px; font-size:28px; line-height:1; color:var(--yellow); }
#stuck ol { margin:0 0 14px; padding-left:22px; font:600 17px/1.35 var(--body); color:var(--text); }
#stuck ol span { color:var(--dim); }
#stuck button { min-height:56px; width:100%; border-radius:4px; border:2px solid var(--yellow-hi); background:var(--red); color:var(--yellow-hi);
  font-family:var(--display); font-style:italic; font-weight:800; font-stretch:condensed; font-size:24px; text-transform:uppercase; }

/* Portrait: ask for landscape. */
#rotate { display:none; position:fixed; inset:0; background:var(--ground); flex-direction:column; align-items:center; justify-content:center; gap:22px;
  text-align:center; padding:calc(env(safe-area-inset-top) + 24px) 28px calc(env(safe-area-inset-bottom) + 24px); }
#rotate svg { width:132px; height:132px; animation:turn 2.4s ease-in-out infinite; }
#rotate .big { font-size:40px; line-height:.95; color:var(--yellow); max-width:12ch; }
#rotate .small { font:600 17px/1.35 var(--body); color:var(--dim); max-width:28ch; }
@keyframes turn { 0%, 25% { transform:rotate(0deg); } 55%, 100% { transform:rotate(-90deg); } }
@media (orientation:portrait) { #rotate { display:flex; } }
@media (prefers-reduced-motion:reduce) {
  *, *::before, *::after { transition:none !important; animation:none !important; }
  #rotate svg { transform:rotate(-90deg); }
}
</style></head><body>
<div id="bar"><span id="chip" class="cap">–</span><span id="who" class="cap">Connecting…</span><span id="mode"></span><span id="spacer"></span>
  <div id="start" class="top cap" role="button" aria-label="Start">START</div><div id="more" class="top" role="button" aria-label="Menu">···</div></div>
<div id="stick"><div id="ring"></div><div id="knob"></div>
  <div id="hint"><div class="dial"><i></i></div><b class="cap">Move</b><span>touch anywhere here</span></div></div>
<div id="pad">
  <div class="b" id="turbo" role="button"><span class="l">TURBO</span><span class="s"></span></div>
  <div class="b" id="jump" role="button"><span class="l">JUMP</span><span class="s"></span></div>
  <div class="b" id="pass" role="button"><span class="l">PASS</span><span class="s"></span></div>
</div>
<div id="toast" class="cap" role="status"><span class="e"></span><span class="t"></span></div>
<div id="sheet"><div id="menu">
  <h2 class="cap">__BRAND__<small id="sheetWho"></small></h2>
  <button data-c="save">Save state</button><button data-c="load">Load state</button><button data-c="redo">Redo play</button><button data-c="p1">Be player 1</button>
  <button data-c="slot-" class="arrow" aria-label="Previous slot">◀</button><div id="slot"><b class="cap">–</b><span>Slot</span></div><button data-c="slot+" class="arrow" aria-label="Next slot">▶</button>
  <p id="fsHint">No browser bar: Share → <b>Add to Home Screen</b>, then open the controller from there.</p>
  <button class="close">Back to the game</button>
</div></div>
<div id="stuck" role="alertdialog" aria-labelledby="stuckTitle"><div class="box">
  <h2 class="cap" id="stuckTitle">Can't reach the game</h2>
  <ol><li>Close this tab.</li><li>In the game, open <b>Settings → Players</b> and scan the QR code again.</li>
    <li><span>The phone must be on the same Wi-Fi as the game, and Phone Controllers on.</span></li></ol>
  <button id="stuckOk">OK</button>
</div></div>
<div id="rotate">
  <svg viewBox="0 0 120 120" fill="none" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    <rect x="38" y="14" width="44" height="92" rx="9" stroke="#F2F2F2" stroke-width="5"/>
    <path d="M54 96h12" stroke="#F2F2F2" stroke-width="5"/>
    <path d="M100 52a40 40 0 0 0-22-34" stroke="#FFE14A" stroke-width="5"/><path d="M96 26l-18-8 2 19" stroke="#FFE14A" stroke-width="5"/>
  </svg>
  <div class="big cap">Turn your phone sideways</div>
  <div class="small">The controller works in landscape: stick on the left, buttons on the right.</div>
</div>
<script>
const BITS = { pass: 0x8000, jump: 0x4000, turbo: 0x2000, start: 0x1000 };
const COLORS = ["#B8C4DA", "#3A7BFF", "#FF4D4D", "#2FD36B", "#FFD23F"];
// What the buttons do on each screen: [label, small print] for pass (A), jump (B), turbo (Z).
const LABELS = __LABELS__;  // game_moment.cpp's table, one for the phones and the touch controls
let ws = null, buttons = 0, sx = 0, sy = 0, last = "", stickTouch = null, origin = null, rumbling = false, toastTimer = 0, lock = null;
const $ = id => document.getElementById(id);
const who = $("who"), chip = $("chip"), ring = $("ring"), knob = $("knob"), hint = $("hint");

// This phone's id, kept so it gets its own player back after reconnecting.
let id = "";
try { id = localStorage.getItem("blitz-id") || ""; } catch (e) {}
if (!/^[A-Za-z0-9]{16}$/.test(id)) {
  const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  id = Array.from(crypto.getRandomValues(new Uint8Array(16)), b => abc[b % abc.length]).join("");
  try { localStorage.setItem("blitz-id", id); } catch (e) {}
}

function setMode(m) {
  const L = LABELS[m] || LABELS.play;
  $("mode").textContent = L.mode;
  for (const k of ["pass", "jump", "turbo"]) {
    const el = $(k);
    el.querySelector(".l").textContent = L[k][0];
    el.querySelector(".s").textContent = L[k][1];
    el.setAttribute("aria-label", (L[k][0] + " " + L[k][1]).trim());
  }
}
function setPlayer(n) {
  who.textContent = n ? "Player " + n : "Waiting for a player";
  chip.textContent = n ? String(n) : "–";
  $("sheetWho").textContent = n ? "Player " + n : "";
  document.documentElement.style.setProperty("--me", COLORS[n] || COLORS[0]);
}
// Statuses arrive as one line; text before ": " is the eyebrow ("Slot 2: state saved").
function toast(text) {
  const t = $("toast"), i = text.indexOf(": ");
  t.querySelector(".e").textContent = i > 0 ? text.slice(0, i) : "";
  t.querySelector(".t").textContent = i > 0 ? text.slice(i + 2) : text;
  const slot = /slot (\d)/i.exec(text);
  if (slot) $("slot").querySelector("b").textContent = slot[1];
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), 2500);
}
function send(force) {
  const msg = "s " + buttons + " " + sx.toFixed(3) + " " + sy.toFixed(3);
  if (ws && ws.readyState === 1 && (force || msg !== last)) { ws.send(msg); last = msg; }
}
// Not hearing from the game for 5 s (stuck on Connecting or Reconnecting): say what to do, once until it connects again.
let heard = false, quietSince = Date.now(), stuckDismissed = false;
const stuck = $("stuck");
const dismissStuck = e => { if (e) e.preventDefault(); stuckDismissed = true; stuck.classList.remove("open"); };
$("stuckOk").addEventListener("touchend", dismissStuck); $("stuckOk").addEventListener("click", dismissStuck);
setInterval(() => {
  if (!heard && !stuckDismissed && Date.now() - quietSince > 5000) stuck.classList.add("open");
}, 500);
function connect() {
  ws = new WebSocket("ws://" + location.host + "/ws?id=" + id);
  ws.onopen = () => send(true);
  ws.onmessage = e => {
    if (!heard) { heard = true; stuckDismissed = false; stuck.classList.remove("open"); }
    const sp = e.data.indexOf(" "), k = e.data.slice(0, sp), v = e.data.slice(sp + 1);
    if (k === "p") {
      setPlayer(+v);
    } else if (k === "r") {
      rumbling = v === "1";
      if (rumbling && navigator.vibrate) navigator.vibrate(120);
    } else if (k === "m") {
      setMode(v);
    } else if (k === "t") {
      toast(v);
    } else if (k === "v" && v !== "__VERSION__") {
      location.reload();  // the app has a newer controller page
    }
  };
  ws.onclose = () => {
    if (heard) { heard = false; quietSince = Date.now(); }
    who.textContent = "Reconnecting…"; setTimeout(connect, 800);
  };
}
setMode("menu");
connect();
setInterval(() => { send(true); if (rumbling && navigator.vibrate) navigator.vibrate(80); }, 100);

// Keep the screen on while playing (where the browser allows it).
async function keepAwake() {
  try { if (navigator.wakeLock && !lock) { lock = await navigator.wakeLock.request("screen"); lock.addEventListener("release", () => lock = null); } } catch (e) {}
}
document.addEventListener("visibilitychange", () => { if (document.visibilityState === "visible") keepAwake(); });
// Full screen, so no browser bar takes the controls' room: Android browsers on the first tap (a touchend counts as the
// user's go-ahead); iPhone browsers can't, so the ··· menu says how (the Home Screen opens it without the bar).
document.addEventListener("touchend", () => {
  const d = document.documentElement;
  if (document.fullscreenEnabled && !document.fullscreenElement && d.requestFullscreen) {
    d.requestFullscreen({ navigationUI: "hide" })
      .then(() => { try { screen.orientation.lock("landscape").catch(() => {}); } catch (e) {} })
      .catch(() => {});
  }
}, { capture: true, passive: true });
if (!document.fullscreenEnabled && !(navigator.standalone || matchMedia("(display-mode: standalone)").matches)) $("fsHint").style.display = "block";
const tick = () => { if (navigator.vibrate) navigator.vibrate(8); };

// Stick: the first touch on the left is the centre; 60 px away is full tilt.
const stick = $("stick");
function moveStick(t) {
  const dx = t.clientX - origin.x, dy = t.clientY - origin.y, r = 60;
  const len = Math.hypot(dx, dy), k = len > r ? r / len : 1;
  sx = (dx * k) / r; sy = -(dy * k) / r;
  const box = stick.getBoundingClientRect();
  knob.style.left = origin.x - box.left + dx * k + "px"; knob.style.top = origin.y - box.top + dy * k + "px";
  send();
}
stick.addEventListener("touchstart", e => {
  e.preventDefault(); keepAwake();
  if (stickTouch !== null) return;
  const t = e.changedTouches[0], box = stick.getBoundingClientRect();
  stickTouch = t.identifier; origin = { x: t.clientX, y: t.clientY };
  ring.style.left = origin.x - box.left + "px"; ring.style.top = origin.y - box.top + "px";
  ring.style.display = knob.style.display = "block"; hint.style.display = "none";
  moveStick(t);
}, { passive: false });
stick.addEventListener("touchmove", e => {
  e.preventDefault();
  for (const t of e.changedTouches) if (t.identifier === stickTouch) moveStick(t);
}, { passive: false });
const endStick = e => {
  for (const t of e.changedTouches) if (t.identifier === stickTouch) {
    stickTouch = null; sx = sy = 0; ring.style.display = knob.style.display = "none"; send();
  }
};
stick.addEventListener("touchend", endStick); stick.addEventListener("touchcancel", endStick);

// Buttons: held while touched (several at once, e.g. turbo + pass).
for (const name of Object.keys(BITS)) {
  const el = $(name);
  const down = e => { e.preventDefault(); keepAwake(); tick(); buttons |= BITS[name]; el.classList.add("on"); send(); };
  const up = e => { e.preventDefault(); buttons &= ~BITS[name]; el.classList.remove("on"); send(); };
  el.addEventListener("touchstart", down, { passive: false });
  el.addEventListener("touchend", up); el.addEventListener("touchcancel", up);
  el.addEventListener("mousedown", down); el.addEventListener("mouseup", up);
}

// The menu: save states, Redo Play, taking player 1.
const sheet = $("sheet");
$("more").addEventListener("touchend", e => { e.preventDefault(); tick(); sheet.classList.add("open"); });
$("more").addEventListener("click", () => sheet.classList.add("open"));
for (const b of document.querySelectorAll("#menu button[data-c]")) {
  const run = e => {
    e.preventDefault(); tick();
    if (ws && ws.readyState === 1) ws.send("c " + b.dataset.c);
    if (b.dataset.c !== "slot+" && b.dataset.c !== "slot-") sheet.classList.remove("open");
  };
  b.addEventListener("touchend", run); b.addEventListener("click", run);
}
const closeSheet = e => { if (e.target === sheet || e.target.classList.contains("close")) { e.preventDefault(); sheet.classList.remove("open"); } };
sheet.addEventListener("touchend", closeSheet); sheet.addEventListener("click", closeSheet);
document.addEventListener("gesturestart", e => e.preventDefault());
</script></body></html>)HTML";
