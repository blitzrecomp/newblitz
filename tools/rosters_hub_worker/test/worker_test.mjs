// The post office's routes against an in-memory KV, in Node (no Cloudflare account needed): node test/worker_test.mjs
// (tests/test_season_pak.py runs it). Prints "ok N" and exits 0, or the first failure and exits 1.
import worker from "../src/index.js";

class KV {
  constructor() { this.m = new Map(); this.opts = new Map(); }
  async get(k, type) {
    if (!this.m.has(k)) return null;
    const v = this.m.get(k);
    if (type === "arrayBuffer") return typeof v === "string" ? new TextEncoder().encode(v).buffer : v;
    return typeof v === "string" ? v : new TextDecoder().decode(v);
  }
  async put(k, v, o) { this.m.set(k, v); this.opts.set(k, o || {}); }
  async list({ prefix }) {
    return { keys: [...this.m.keys()].filter((k) => k.startsWith(prefix)).map((name) => ({ name, metadata: (this.opts.get(name) || {}).metadata })) };
  }
}

const env = { ROSTERS: new KV(), HUB_KEY: "secretkey123", UPLOAD_SECRET: "up" };
let n = 0;
async function call(method, path, body, auth, extra = {}) {
  const headers = { ...(auth ? { authorization: `Bearer ${auth}` } : {}), ...extra };
  return worker.fetch(new Request(`https://hub.example${path}`, { method, body, headers }), env);
}
function check(what, ok) {
  n++;
  if (!ok) {
    console.log(`FAIL ${what}`);
    process.exit(1);
  }
}

const zip = new Uint8Array([0x50, 0x4b, 3, 4, 1, 2, 3, 4, 5]);
const sha = "a".repeat(64);
const cat = (extra = {}) => JSON.stringify({ format: "blitz-paks/1", paks: { season2026: { version: "1", file: "season2026-1.nbpak", sha256: sha, ...extra } } });

check("no catalog yet", (await call("GET", "/paks/catalog.json")).status === 404);
check("upload needs the secret", (await call("PUT", "/upload/paks/season2026-1.nbpak", zip)).status === 403);
check("upload refuses a wrong secret", (await call("PUT", "/upload/paks/season2026-1.nbpak", zip, "nope")).status === 403);
check("a catalog naming a pak that isn't there is refused", (await call("PUT", "/upload/paks/catalog.json", cat(), "up")).status === 400);
check("a non-zip pak is refused", (await call("PUT", "/upload/paks/season2026-1.nbpak", "hello", "up")).status === 400);
check("a bad pak name is refused", (await call("PUT", "/upload/paks/../x.nbpak", zip, "up")).status === 400);
check("the pak uploads", (await call("PUT", "/upload/paks/season2026-1.nbpak", zip, "up")).status === 200);
check("a catalog that isn't one is refused", (await call("PUT", "/upload/paks/catalog.json", "{\"format\":\"x\"}", "up")).status === 400);
check("the catalog uploads", (await call("PUT", "/upload/paks/catalog.json", cat(), "up")).status === 200);

let r = await call("GET", "/paks/catalog.json");
check("the catalog is public", r.status === 200 && JSON.parse(await r.text()).paks.season2026.file === "season2026-1.nbpak");
r = await call("GET", "/paks/season2026-1.nbpak");
const got = new Uint8Array(await r.arrayBuffer());
check("the pak is public and byte for byte", r.status === 200 && got.length === zip.length && got.every((b, i) => b === zip[i]));
check("the pak is cached for good", (r.headers.get("cache-control") || "").includes("immutable"));
check("an unknown pak is 404", (await call("GET", "/paks/season2026-2.nbpak")).status === 404);
check("nothing else under /paks", (await call("GET", "/paks/../secretkey123/manifest.json")).status === 404);
check("no writes under /paks", (await call("PUT", "/paks/catalog.json", cat())).status === 404);

// The weekly rosters: the hub's own files, public under /paks/rosters only while the catalog allows it.
check("roster upload", (await call("PUT", "/upload/manifest.json", "{\"latest\":\"2026-w09\"}", "up")).status === 200);
check("the keyed hub still serves", (await call("GET", "/secretkey123/manifest.json")).status === 200);
check("the keyed hub needs its key", (await call("GET", "/wrongkey/manifest.json")).status === 404);
check("public rosters while live_rosters is on", (await call("GET", "/paks/rosters/manifest.json")).status === 200);
check("no PSP files or status publicly", (await call("GET", "/paks/rosters/status")).status === 404);
check("the catalog switches live rosters off", (await call("PUT", "/upload/paks/catalog.json", cat({ live_rosters: false }), "up")).status === 200);
check("then public rosters stop", (await call("GET", "/paks/rosters/manifest.json")).status === 404);
check("withdrawn (the kill switch)", (await call("PUT", "/upload/paks/catalog.json", cat({ available: false }), "up")).status === 200);
check("withdrawn: no public rosters", (await call("GET", "/paks/rosters/manifest.json")).status === 404);
r = await call("GET", "/paks/catalog.json");
check("no response names the hub key", !(await r.text()).includes("secretkey123"));


// Crash reports: in through the public route, out only with the key; nothing about the sender kept.
const report = "New Blitz crash report\napp: 0.9.5\n--- crash ---\n[crash] signal 11 (Segmentation fault: 11). Call stack:\n";
check("a report needs POST", (await call("GET", "/reports/crash")).status === 404);
check("only /reports/crash", (await call("POST", "/reports/other", report)).status === 404);
check("not a report is refused", (await call("POST", "/reports/crash", "hello")).status === 400);
check("a big report is refused", (await call("POST", "/reports/crash", report + "x".repeat(256 * 1024))).status === 413);
r = await call("POST", "/reports/crash", report, null, { "cf-connecting-ip": "203.0.113.9" });
const sent = JSON.parse(await r.text());
check("a report is kept", r.status === 201 && /^[0-9TZ:.-]+-[0-9a-f]{8}$/.test(sent.id));
const stored = [...env.ROSTERS.m.entries()].filter(([k]) => k.startsWith("report:"));
check("kept 30 days", (env.ROSTERS.opts.get(stored[0][0]) || {}).expirationTtl === 30 * 86400);
check("no address kept", !JSON.stringify([...env.ROSTERS.m.values(), ...env.ROSTERS.opts.values()].map(String)).includes("203.0.113.9") &&
      !JSON.stringify([...env.ROSTERS.opts.values()]).includes("203.0.113.9"));
check("the list needs the key", (await call("GET", "/reports")).status === 404);
r = await call("GET", "/secretkey123/reports");
const rows = JSON.parse(await r.text());
check("the list with the key", r.status === 200 && rows.length === 1 && rows[0].crash.startsWith("[crash] signal 11"));
r = await call("GET", `/secretkey123/reports/${sent.id}`);
check("one report with the key", r.status === 200 && (await r.text()) === report);
check("a bad id is 404", (await call("GET", "/secretkey123/reports/not-an-id")).status === 404);
env.ROSTERS.m.set(`reports:count:${new Date().toISOString().slice(0, 10)}`, "300");
check("at most 300 a day", (await call("POST", "/reports/crash", report)).status === 429);

console.log(`ok ${n}`);
