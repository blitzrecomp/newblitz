// The sync server's rules against an R2 bucket in memory, in Node (no Cloudflare account needed):
//   node tools/save_sync_worker/test/worker_test.mjs        (make test-sync-worker; part of make test)
// Prints "ok N" and exits 0, or the first failure and exits 1.
import worker, { sweep } from "../src/index.js";
import { MemoryR2 } from "./r2_memory.mjs";
import { MemoryLimiter } from "./rate_memory.mjs";

const env = { SAVES: new MemoryR2(), MAX_NEW_PER_DAY: "5" };
let n = 0;
function check(what, ok) {
  n++;
  if (!ok) {
    console.log(`FAIL ${what}`);
    process.exit(1);
  }
}
async function call(method, path, body, headers = {}) {
  return worker.fetch(new Request(`https://sync.example${path}`, { method, body, headers }), env);
}

const id = "a".repeat(64), other = "b".repeat(64);
const token = new Uint8Array(32).fill(7), wrong = new Uint8Array(32).fill(9);
const hex = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
// A sealed bundle as the app makes it: "NBC1", its revision (8 bytes LE), nonce, tag, ciphertext.
function blob(rev, size = 100) {
  const b = new Uint8Array(52 + size);
  b.set([0x4e, 0x42, 0x43, 0x31]);
  let v = rev;
  for (let i = 0; i < 8; i++) { b[4 + i] = v % 256; v = Math.floor(v / 256); }
  for (let i = 52; i < b.length; i++) b[i] = (i * 31 + rev) & 255;
  return b;
}
const cat = (a, b) => { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; };
const post = (path, rev, b, t = token) => call("POST", `${path}?if_match=${rev}`, cat(t, b));
const revOf = async (path) => (await (await call("GET", `${path}/rev`)).json()).rev;

// ---- reading what isn't there ----
check("an unknown id has revision 0", (await revOf(`/v1/${id}`)) === 0);
check("its blob is 404", (await call("GET", `/v1/${id}`)).status === 404);
check("HEAD of nothing is 404", (await call("HEAD", `/v1/${id}`)).status === 404);
check("a bad id is 404", (await call("GET", "/v1/xyz/rev")).status === 404);
check("an upper-case id is 404", (await call("GET", `/v1/${"A".repeat(64)}/rev`)).status === 404);
check("anything else is 404", (await call("GET", "/")).status === 404);

// ---- the first write, then compare-and-set ----
let r = await post(`/v1/${id}`, 0, blob(1));
check("the first write takes revision 1", r.status === 201 && (await r.json()).rev === 1);
check("revision 1 now", (await revOf(`/v1/${id}`)) === 1);
r = await call("GET", `/v1/${id}`);
const got = new Uint8Array(await r.arrayBuffer());
check("the blob comes back byte for byte", r.status === 200 && got.length === blob(1).length && got.every((x, i) => x === blob(1)[i]));
check("with its revision as the ETag", r.headers.get("etag") === '"1"' && r.headers.get("x-revision") === "1");
check("and never cached", r.headers.get("cache-control") === "no-store");
r = await call("HEAD", `/v1/${id}`);
check("HEAD has the revision", r.status === 200 && r.headers.get("etag") === '"1"');
r = await post(`/v1/${id}`, 0, blob(1));
check("a write naming an old revision is 412", r.status === 412 && (await r.json()).rev === 1);
check("and changed nothing", (await revOf(`/v1/${id}`)) === 1);
r = await post(`/v1/${id}`, 5, blob(6));
check("a write naming a future revision is 412", r.status === 412);
check("a blob whose own revision isn't the next is refused", (await post(`/v1/${id}`, 1, blob(1))).status === 400);
check("junk isn't a sealed bundle", (await post(`/v1/${id}`, 1, new Uint8Array(200).fill(1))).status === 400);
check("no if_match is refused", (await call("POST", `/v1/${id}`, cat(token, blob(2)))).status === 400);
check("a bad if_match is refused", (await call("POST", `/v1/${id}?if_match=-1`, cat(token, blob(2)))).status === 400);
check("the wrong token can't write", (await post(`/v1/${id}`, 1, blob(2), wrong)).status === 403);
r = await post(`/v1/${id}`, 1, blob(2));
check("the right revision and token write revision 2", r.status === 201 && (await r.json()).rev === 2);

// ---- latest and previous ----
r = await call("GET", `/v1/${id}/previous`);
const prev = new Uint8Array(await r.arrayBuffer());
check("the copy before is kept", r.status === 200 && r.headers.get("etag") === '"1"' && prev.every((x, i) => x === blob(1)[i]));
check("only the latest and the previous", [...env.SAVES.m.keys()].filter((k) => k.startsWith(`v1/${id}`)).length === 2);

// ---- size ----
check("over 1 MiB is 413", (await post(`/v1/${id}`, 2, blob(3, 1024 * 1024))).status === 413);
check("just under 1 MiB is taken", (await post(`/v1/${id}`, 2, blob(3, 1024 * 1024 - 52))).status === 201);

// ---- PUT with If-Match, for other clients ----
const auth = { authorization: `Bearer ${hex(token)}` };
check("PUT needs If-Match", (await call("PUT", `/v1/${id}`, blob(4), auth)).status === 428);
check("PUT with a stale If-Match is 412", (await call("PUT", `/v1/${id}`, blob(4), { ...auth, "if-match": '"2"' })).status === 412);
check("PUT with no token is 403", (await call("PUT", `/v1/${id}`, blob(4), { "if-match": '"3"' })).status === 403);
r = await call("PUT", `/v1/${id}`, blob(4), { ...auth, "if-match": '"3"' });
check("PUT with the current If-Match writes", r.status === 201 && r.headers.get("etag") === '"4"');
check("If-None-Match: * on a taken id is 412", (await call("PUT", `/v1/${id}`, blob(1), { ...auth, "if-none-match": "*" })).status === 412);

// ---- the rate limit: 30 writes an hour per id ----
let rev = await revOf(`/v1/${id}`);
let last = 201;
for (let i = 0; i < 40 && last === 201; i++) {
  last = (await post(`/v1/${id}`, rev, blob(rev + 1))).status;
  if (last === 201) rev++;
}
check("the 31st write in an hour is 429", last === 429 && rev === 30);
check("another id isn't held back", (await post(`/v1/${other}`, 0, blob(1))).status === 201);
// An hour later the window starts again.
const meta = env.SAVES.m.get(`v1/${id}`).meta;
meta.win = String(Date.now() - 3601e3);
check("an hour later it writes again", (await post(`/v1/${id}`, rev, blob(rev + 1))).status === 201);

// ---- new ids a day ----
let created = 2;  // id and other
for (let i = 0; i < 5; i++) {
  const s = (await post(`/v1/${String(i).repeat(64)}`, 0, blob(1))).status;
  if (s === 201) created++;
  else check("the cap answers 429", s === 429);
}
check("at most MAX_NEW_PER_DAY new ids a day", created === 5);
check("an existing id still writes past the cap", (await post(`/v1/${other}`, 1, blob(2))).status === 201);

// ---- delete ----
check("delete needs the token", (await call("POST", `/v1/${other}/delete`, wrong)).status === 403);
check("DELETE needs the token too", (await call("DELETE", `/v1/${other}`)).status === 403);
r = await call("POST", `/v1/${other}/delete`, token);
check("delete with the token", r.status === 200 && (await revOf(`/v1/${other}`)) === 0);
check("both copies are gone", ![...env.SAVES.m.keys()].some((k) => k.startsWith(`v1/${other}`)));
check("deleting nothing is fine", (await call("POST", `/v1/${other}/delete`, token)).status === 200);

// ---- the 90-day sweep ----
const sweepEnv = { SAVES: new MemoryR2() };
const day = 86400e3, now = Date.now();
sweepEnv.SAVES.now = () => now - 91 * day;
await sweepEnv.SAVES.put(`v1/${id}`, blob(1), { customMetadata: { rev: "1" } });
await sweepEnv.SAVES.put(`v1/${id}.prev`, blob(1), { customMetadata: { rev: "1" } });
await sweepEnv.SAVES.put("meta/new-2000-01-01", "3");
sweepEnv.SAVES.now = () => now - 10 * day;
await sweepEnv.SAVES.put(`v1/${other}`, blob(1), { customMetadata: { rev: "1" } });
const removed = await sweep(sweepEnv, now);
check("the sweep removes ids untouched for 90 days", removed === 1 && !sweepEnv.SAVES.m.has(`v1/${id}`) && !sweepEnv.SAVES.m.has(`v1/${id}.prev`));
check("and keeps the rest", sweepEnv.SAVES.m.has(`v1/${other}`));
check("and old day counters", !sweepEnv.SAVES.m.has("meta/new-2000-01-01"));

// ---- nothing about the sender is stored ----
const stored = JSON.stringify([...env.SAVES.m.values()].map((e) => e.meta));
check("only rev, token hash, window and count in metadata", [...env.SAVES.m.values()].every((e) => Object.keys(e.meta).every((k) => ["rev", "tok", "win", "n"].includes(k))));
check("the token itself is never stored", !stored.includes(hex(token)));

// ---- pairing: a 6-digit number for two minutes, one use, a limit on guesses ----
{
  let t = 1_800_000_000_000;
  const clock = () => t;
  const R2 = new MemoryR2();
  R2.now = clock;
  const limiter = new MemoryLimiter(5, 60, clock);
  const penv = { SAVES: R2, PAIR_LIMITER: limiter, MAX_PAIRS: "3", now: clock };
  const ip = (a) => ({ "cf-connecting-ip": a });
  const pcall = (method, path, body, headers = {}) => worker.fetch(new Request(`https://sync.example${path}`, { method, body, headers }), penv);
  const pid = (c) => c.repeat(64);
  const owner = new Uint8Array(32).fill(1), owner2 = new Uint8Array(32).fill(2);
  const proof = new Uint8Array(32).fill(3), badproof = new Uint8Array(32).fill(4);
  const sealedCode = (() => { const b = new Uint8Array(100); b.set([0x4e, 0x42, 0x50, 0x31]); for (let i = 4; i < 100; i++) b[i] = i; return b; })();
  const create = (p, o = owner, pr = proof, from = "10.0.0.1", q = "") => pcall("POST", `/v1/pair/${p}${q}`, cat(cat(o, pr), sealedCode), ip(from));
  const claim = (p, pr = proof, from = "10.0.0.2") => pcall("GET", `/v1/pair/${p}`, undefined, { authorization: `Bearer ${hex(pr)}`, ...ip(from) });
  const status = (p, o = owner) => pcall("GET", `/v1/pair/${p}/status`, undefined, { authorization: `Bearer ${hex(o)}` });
  const cancel = (p, o = owner) => pcall("POST", `/v1/pair/${p}/cancel`, o);
  const live = async (p, o = owner) => (await (await status(p, o)).json()).live;

  r = await create(pid("1"));
  check("pairing: a number is made", r.status === 201 && (await r.json()).expires_in === 120);
  check("pairing: its owner sees it live", (await live(pid("1"))) === true);
  check("pairing: anyone else sees nothing", (await live(pid("1"), owner2)) === false);
  check("pairing: only hashes and the expiry are stored", Object.keys(R2.m.get(`pair/${pid("1")}`).meta).sort().join() === "exp,owner,proof" &&
    !JSON.stringify(R2.m.get(`pair/${pid("1")}`).meta).includes(hex(proof)) && !JSON.stringify(R2.m.get(`pair/${pid("1")}`).meta).includes(hex(owner)));
  check("pairing: another device can't take a live number", (await create(pid("1"), owner2)).status === 409);
  check("pairing: a wrong number is 404", (await claim(pid("2"))).status === 404);
  r = await claim(pid("1"), badproof, "10.0.0.3");
  check("pairing: the right id with a wrong proof is 404 too", r.status === 404);
  check("pairing: and burns that number (only a forger sends one)", !R2.m.has(`pair/${pid("1")}`));
  await create(pid("1"));
  r = await claim(pid("1"));
  const gotSealed = new Uint8Array(await r.arrayBuffer());
  check("pairing: the right proof gets the sealed code", r.status === 200 && gotSealed.length === 100 && gotSealed.every((x, i) => x === sealedCode[i]));
  check("pairing: never cached", r.headers.get("cache-control") === "no-store");
  check("pairing: once: then it's gone", (await claim(pid("1"), proof, "10.0.0.4")).status === 404 && !R2.m.has(`pair/${pid("1")}`));
  check("pairing: its owner sees it taken", (await live(pid("1"))) === false);

  // Expiry after two minutes (the fake clock), and the record is deleted when found expired.
  await create(pid("5"), owner, proof, "10.0.0.5");
  t += 119e3;
  check("pairing: still there just before two minutes", (await live(pid("5"))) === true);
  t += 2e3;
  check("pairing: expired after two minutes", (await live(pid("5"))) === false);
  check("pairing: an expired number is 404", (await claim(pid("5"), proof, "10.0.0.5")).status === 404 && !R2.m.has(`pair/${pid("5")}`));

  // Cancel: the owner removes it at once; anyone else's cancel does nothing and says the same.
  await create(pid("6"), owner, proof, "10.0.0.6");
  r = await cancel(pid("6"), owner2);
  check("pairing: someone else's cancel answers 200 and changes nothing", r.status === 200 && R2.m.has(`pair/${pid("6")}`));
  r = await cancel(pid("6"));
  check("pairing: the owner's cancel removes it", r.status === 200 && !R2.m.has(`pair/${pid("6")}`));
  check("pairing: cancelling nothing is fine", (await cancel(pid("6"))).status === 200);

  // A new number replaces the device's old one.
  await create(pid("7"), owner, proof, "10.0.0.7");
  r = await create(pid("8"), owner, proof, "10.0.0.7", `?replaces=${pid("7")}`);
  check("pairing: a new number drops the old one", r.status === 201 && !R2.m.has(`pair/${pid("7")}`) && R2.m.has(`pair/${pid("8")}`));
  await create(pid("9"), owner2, proof, "10.0.0.8");
  await create(pid("a"), owner, proof, "10.0.0.7", `?replaces=${pid("9")}`);
  check("pairing: but never another device's", R2.m.has(`pair/${pid("9")}`));

  // The global cap (3 here): live ones count, expired ones don't.
  check("pairing: past the cap is 429", (await create(pid("b"), owner, proof, "10.0.0.9")).status === 429);
  t += 121e3;
  check("pairing: expired numbers make room", (await create(pid("b"), owner, proof, "10.0.0.9")).status === 201);
  check("pairing: and are removed on the way", !R2.m.has(`pair/${pid("8")}`) && !R2.m.has(`pair/${pid("9")}`));

  // The guess limit: 5 a minute per connection, so 10 in a number's two minutes; another connection isn't held back.
  t += 61e3;
  await create(pid("c"), owner, proof, "10.0.1.1");
  let ok = 0, limited = 0;
  for (let i = 0; i < 12; i++) {
    const s = (await claim(pid("d"), proof, "10.0.2.2")).status;
    if (s === 404) ok++;
    else if (s === 429) limited++;
  }
  check("pairing: the 6th guess in a minute is 429", ok === 5 && limited === 7);
  check("pairing: another connection still guesses", (await claim(pid("d"), proof, "10.0.2.3")).status === 404);
  t += 60e3;
  ok = 0;
  for (let i = 0; i < 6; i++) if ((await claim(pid("d"), proof, "10.0.2.2")).status === 404) ok++;
  check("pairing: 5 more the next minute: 10 in two", ok === 5);
  check("pairing: creates count too", (await create(pid("e"), owner, proof, "10.0.2.2")).status === 429);
  check("pairing: the limiter never sees an address", limiter.keys.every((k) => /^[0-9a-f]{32}$/.test(k) && !k.includes("10.0")));
  check("pairing: nothing stored names an address", ![...R2.m.entries()].some(([k, e]) => k.includes("10.0") || JSON.stringify(e.meta).includes("10.0")));

  // Bad requests.
  check("pairing: a bad id is 404", (await pcall("GET", "/v1/pair/xyz", undefined, { authorization: `Bearer ${hex(proof)}` })).status === 404);
  check("pairing: junk isn't a sealed pairing", (await pcall("POST", `/v1/pair/${pid("f")}`, new Uint8Array(120).fill(1), ip("10.3.0.1"))).status === 400);
  check("pairing: too big is 413", (await pcall("POST", `/v1/pair/${pid("f")}`, cat(cat(owner, proof), new Uint8Array(600)), ip("10.3.0.2"))).status === 413);
  check("pairing: no proof is 404", (await pcall("GET", `/v1/pair/${pid("c")}`, undefined, ip("10.3.0.3"))).status === 404);

  // Without the rate limiter, pairing is off (never unlimited).
  const bare = { SAVES: new MemoryR2() };
  const b = await worker.fetch(new Request(`https://sync.example/v1/pair/${pid("1")}`, { method: "POST", body: cat(cat(owner, proof), sealedCode) }), bare);
  check("pairing: no limiter, no pairing (503)", b.status === 503 && bare.SAVES.m.size === 0);

  // The sweep removes numbers nobody claimed.
  await create(pid("1"), owner, proof, "10.4.0.1");
  t += 3600e3;
  await sweep(penv, t);
  check("pairing: the sweep removes old numbers", ![...R2.m.keys()].some((k) => k.startsWith("pair/")));

  // The saves routes are as they were beside pairing.
  const sid = "c".repeat(64);
  check("pairing: the saves routes still work", (await pcall("POST", `/v1/${sid}?if_match=0`, cat(token, blob(1)))).status === 201 &&
    (await (await pcall("GET", `/v1/${sid}/rev`)).json()).rev === 1);
}

console.log(`ok ${n}`);
