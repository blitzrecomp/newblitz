// New Blitz's save sync server (docs/SAVE_SYNC.md §7): Cloud Sync's encrypted saves, one small blob per sync code.
// Kept apart from the rosters hub (tools/rosters_hub_worker): its own Worker, its own R2 bucket, its own address.
//
// The app (src/game/save_sync_cloud.cpp) encrypts the player's saves bundle on the device with a key made from the sync
// code, and files it here under an id also made from the code (one-way). So this server holds only ciphertext under
// random-looking ids: it can't read a save, can't get a code back, and knows nothing about who sent it. No accounts, no
// names, no addresses: nothing about the sender is stored or logged. (Pairing's rate limit, below, is the one place the
// connection's address is read: hashed with a salt, for a count, and dropped.)
//
// Routes (<id> is 64 lower-case hex):
//   GET  /v1/<id>/rev        {"rev": N, "size": bytes}: N is 0 when nothing is filed under <id>
//   GET  /v1/<id>            the blob (ETag "<rev>", X-Revision: <rev>); 404 when none
//   HEAD /v1/<id>            the same headers, no body
//   GET  /v1/<id>/previous   the blob before the current one (kept for a bad write; the app doesn't use it yet)
//   POST /v1/<id>?if_match=N the app's write: body = the 32-byte write token, then the blob
//   PUT  /v1/<id>            the same for other clients: If-Match: "N" (If-None-Match: * for a new id),
//                            Authorization: Bearer <token as 64 hex>, body = the blob
//   POST /v1/<id>/delete     body = the token; DELETE /v1/<id> with the Bearer token: removes both copies
//
// A write is taken only when N is the current revision (0: nothing there yet): compare-and-set, so two devices can't
// overwrite each other unseen (412 and the current revision; the app pulls first, then writes again). The blob must
// be a sealed bundle ("NBC1", then its revision, which must be N + 1) of at most 1 MiB (413). The first write of an id
// sets its token: the server keeps only the token's SHA-256, and later writes and deletes must carry the same token
// (403). At most 30 writes an hour per id (429), and at most MAX_NEW_PER_DAY new ids a day in all (429; default 2000).
// Each write keeps the one before as <id>.prev. Ids untouched for 90 days go: the daily sweep below (and/or an R2
// lifecycle rule, tools/save_sync_worker/DEPLOY.md).
//
// Pairing (added 2026-11-27; docs/SAVE_SYNC.md §7.2): a device with Cloud Sync on shows a 6-digit number P for two
// minutes, and another device types it to join, instead of the 24-character code. The first device seals the code with
// a key it makes from P and a random salt (Argon2i, on the device) and leaves it here for 2 minutes:
//   POST /v1/pair/<pid>[?replaces=<old pid>]   body = owner (32 bytes) + proof (32) + the sealed code ("NBP1", <= 512)
//        <pid> and the proof are two separate one-way derivations of P (the app's pairing_keys). 201 {"expires_in":120};
//        409 when a live record under <pid> belongs to another device (the app picks another number); 429 past the
//        per-connection limit or the global cap. ?replaces= drops that device's earlier number (same owner) at once.
//   GET  /v1/pair/<pid>          Authorization: Bearer <proof as 64 hex>: the sealed code, once: the record is deleted
//        as it is handed over. 404 for no record, an expired one (deleted then) or a wrong proof (also deleted: only
//        someone who didn't derive <pid> from P can send a wrong proof), all alike. 429 past the limit.
//   GET  /v1/pair/<pid>/status   Authorization: Bearer <owner as 64 hex>: {"live": true|false} (the first device's
//        screen: "your other device has it"). false for anyone but the owner, so it tells nobody else anything.
//   POST /v1/pair/<pid>/cancel   body = owner: deletes it now (Cancel on the first device). Always 200, so it can't be
//        used to learn which numbers are live.
// The real protection is that a wrong guess gets nothing: every create and every lookup counts against the caller's
// connection, at most PAIR_TRIES (5) a minute (Cloudflare's Workers Rate Limiting binding PAIR_LIMITER: about 10 tries
// in a number's two minutes), and at most MAX_PAIRS (500) numbers are live at once in all. The limiter is keyed by a
// salted SHA-256 of CF-Connecting-IP: the address is used only for that count, never stored here, never logged, never
// sent anywhere in the clear. Without the binding, pairing answers 503 (never unlimited). R2 keys: pair/<pid>, with
// only the proof's and owner's SHA-256 and the expiry in its metadata.

const ID = /^[0-9a-f]{64}$/;
const MAX_BLOB = 1024 * 1024;
const TOKEN = 32;
const WRITES_PER_HOUR = 30;
const KEEP_DAYS = 90;
const NEW_PER_DAY = 2000;
const MAGIC = [0x4e, 0x42, 0x43, 0x31];  // "NBC1"
const PAIR_MAGIC = [0x4e, 0x42, 0x50, 0x31];  // "NBP1"
const PAIR_TTL = 120e3;        // a number lasts two minutes
const PAIR_BLOB_MAX = 512;     // the sealed code is ~100 bytes
const MAX_PAIRS = 500;         // live numbers at once, all players together
const PAIR_RATE_SALT = "new blitz pairing rate v1|";

function json(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store", ...extra },
  });
}

async function sha256hex(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...d].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function hex_to_bytes(h) {
  if (!/^[0-9a-f]{64}$/i.test(h || "")) return null;
  const out = new Uint8Array(32);
  for (let i = 0; i < 32; i++) out[i] = parseInt(h.slice(2 * i, 2 * i + 2), 16);
  return out;
}

// Constant-time compare of two hex strings of the same length.
function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

function rev_of(obj) {
  const n = Number((obj && obj.customMetadata && obj.customMetadata.rev) || "0");
  return Number.isSafeInteger(n) && n > 0 ? n : 0;
}

// The blob's own revision (bytes 4..11, little-endian), or -1 when it isn't a sealed bundle.
function blob_rev(blob) {
  if (blob.length < 52 || MAGIC.some((b, i) => blob[i] !== b)) return -1;
  let v = 0;
  for (let i = 7; i >= 0; i--) v = v * 256 + blob[4 + i];
  return Number.isSafeInteger(v) ? v : -1;
}

function serve(obj, body) {
  const rev = rev_of(obj);
  return new Response(body ? obj.body : null, {
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(obj.size),
      etag: `"${rev}"`,
      "x-revision": String(rev),
      "cache-control": "no-store",
    },
  });
}

// A new id today: counted in one small object per day, so the bucket can't be filled from nowhere. Not exact under
// a burst of simultaneous first writes (no lock), which only lets a few more through.
async function room_for_new(env) {
  const day = new Date().toISOString().slice(0, 10);
  const key = `meta/new-${day}`;
  const cap = Number(env.MAX_NEW_PER_DAY || NEW_PER_DAY);
  const got = await env.SAVES.get(key);
  const n = got ? Number(await got.text()) || 0 : 0;
  if (n >= cap) return false;
  await env.SAVES.put(key, String(n + 1));
  return true;
}

// The write: the same rules for POST (the app) and PUT (anything else).
async function write(env, id, if_match, token, blob) {
  if (!Number.isSafeInteger(if_match) || if_match < 0) return json({ error: "if_match must be the current revision" }, 400);
  if (!token || token.length !== TOKEN) return json({ error: "no write token" }, 403);
  if (blob.length > MAX_BLOB) return json({ error: "over 1 MiB" }, 413);
  const key = `v1/${id}`;
  const current = await env.SAVES.get(key);
  const rev = rev_of(current);
  const tok = await sha256hex(token);
  if (current && !same(current.customMetadata.tok, tok)) return json({ error: "this id's token is another" }, 403);
  if (rev !== if_match) return json({ error: "revision moved", rev }, 412, { etag: `"${rev}"` });
  if (blob_rev(blob) !== if_match + 1) return json({ error: "not a sealed bundle for the next revision" }, 400);
  const now = Date.now();
  let win = Number((current && current.customMetadata.win) || "0");
  let n = Number((current && current.customMetadata.n) || "0");
  if (now - win >= 3600e3) {
    win = now;
    n = 0;
  }
  if (n >= WRITES_PER_HOUR) return json({ error: "too many writes this hour", retry_after: Math.ceil((win + 3600e3 - now) / 1000) }, 429);
  if (!current && !(await room_for_new(env))) return json({ error: "too many new ids today" }, 429);
  const meta = { rev: String(rev + 1), tok, win: String(win), n: String(n + 1) };
  if (current) {
    // The one before, kept beside it. Then the new one, only if the current is still the one read (another write
    // landing in between makes this fail: 412, and the app pulls).
    const old = new Uint8Array(await current.arrayBuffer());
    await env.SAVES.put(`${key}.prev`, old, { customMetadata: { rev: String(rev), tok } });
    const put = await env.SAVES.put(key, blob, { customMetadata: meta, onlyIf: { etagMatches: current.etag } });
    if (put === null) {
      const now_rev = rev_of(await env.SAVES.head(key));
      return json({ error: "revision moved", rev: now_rev }, 412, { etag: `"${now_rev}"` });
    }
  } else {
    await env.SAVES.put(key, blob, { customMetadata: meta });
  }
  return json({ ok: true, rev: rev + 1 }, 201, { etag: `"${rev + 1}"` });
}

async function remove(env, id, token) {
  const key = `v1/${id}`;
  const current = await env.SAVES.head(key);
  if (!current) return json({ ok: true, rev: 0 });
  if (!token || token.length !== TOKEN || !same(current.customMetadata.tok, await sha256hex(token))) return json({ error: "this id's token is another" }, 403);
  await env.SAVES.delete([key, `${key}.prev`]);
  return json({ ok: true, rev: 0 });
}

// The daily sweep (wrangler.toml's cron): ids whose current copy hasn't been written for 90 days, and their previous
// copies; the day counters after a week.
export async function sweep(env, now = Date.now()) {
  const cutoff = now - KEEP_DAYS * 86400e3;
  let cursor;
  let removed = 0;
  do {
    const page = await env.SAVES.list({ prefix: "v1/", cursor, limit: 1000 });
    const old = page.objects.filter((o) => !o.key.endsWith(".prev") && o.uploaded.getTime() < cutoff).map((o) => o.key);
    const keys = old.flatMap((k) => [k, `${k}.prev`]);
    // Previous copies whose current copy is gone (deleted, or swept on an earlier page).
    for (const o of page.objects) {
      if (o.key.endsWith(".prev") && o.uploaded.getTime() < cutoff && !keys.includes(o.key)) keys.push(o.key);
    }
    for (let i = 0; i < keys.length; i += 1000) await env.SAVES.delete(keys.slice(i, i + 1000));
    removed += old.length;
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
  // Pairing numbers that were never claimed or cancelled (each lasts two minutes; a lookup deletes an expired one too).
  let pcursor;
  do {
    const page = await env.SAVES.list({ prefix: "pair/", cursor: pcursor, limit: 1000 });
    const gone = page.objects.filter((o) => pair_expired(o, now)).map((o) => o.key);
    if (gone.length) await env.SAVES.delete(gone);
    pcursor = page.truncated ? page.cursor : undefined;
  } while (pcursor);
  const meta = await env.SAVES.list({ prefix: "meta/new-" });
  const week = new Date(now - 7 * 86400e3).toISOString().slice(0, 10);
  const stale = meta.objects.filter((o) => o.key.slice("meta/new-".length) < week).map((o) => o.key);
  if (stale.length) await env.SAVES.delete(stale);
  return removed;
}

// ---- pairing ------------------------------------------------------------------------------------------------------
function clock(env) {
  return typeof env.now === "function" ? env.now() : Date.now();  // env.now: the tests' fake clock
}

function pair_expired(obj, now) {
  const exp = Number((obj && obj.customMetadata && obj.customMetadata.exp) || "0");
  // A record without a readable expiry, or older than its two minutes by its upload time, is expired too.
  return !(exp > now) || (obj.uploaded && obj.uploaded.getTime() + PAIR_TTL + 60e3 < now);
}

// One more try for this connection: false past the limit. The key is a salted hash of the address, never the address.
async function pair_try(request, env) {
  const ip = request.headers.get("cf-connecting-ip") || "local";
  const key = (await sha256hex(new TextEncoder().encode(PAIR_RATE_SALT + ip))).slice(0, 32);
  const { success } = await env.PAIR_LIMITER.limit({ key });
  return success;
}

function too_many() {
  return json({ error: "too many tries: wait a minute", retry_after: 60 }, 429, { "retry-after": "60" });
}

async function pair_create(request, env, pid, url) {
  if (!(await pair_try(request, env))) return too_many();
  const body = new Uint8Array(await request.arrayBuffer());
  if (body.length > 64 + PAIR_BLOB_MAX) return json({ error: "too big" }, 413);
  const owner = body.slice(0, 32), proof = body.slice(32, 64), sealed = body.slice(64);
  if (body.length < 64 + 4 + 16 || PAIR_MAGIC.some((b, i) => sealed[i] !== b)) return json({ error: "not a sealed pairing" }, 400);
  const now = clock(env);
  const owner_h = await sha256hex(owner);
  const key = `pair/${pid}`;
  const current = await env.SAVES.head(key);
  const live = current && !pair_expired(current, now);
  if (live && !same(current.customMetadata.owner, owner_h)) return json({ error: "that number is taken" }, 409);
  // The same device's earlier number, dropped at once (Add a Device's New Number).
  const old = url.searchParams.get("replaces");
  if (old && ID.test(old) && old !== pid) {
    const o = await env.SAVES.head(`pair/${old}`);
    if (o && same(o.customMetadata.owner, owner_h)) await env.SAVES.delete(`pair/${old}`);
  }
  if (!live) {
    // The global cap, counting only live numbers (expired ones found on the way are removed).
    const cap = Number(env.MAX_PAIRS || MAX_PAIRS);
    let n = 0, cursor;
    do {
      const page = await env.SAVES.list({ prefix: "pair/", cursor, limit: 1000 });
      const gone = [];
      for (const o of page.objects) {
        if (pair_expired(o, now)) gone.push(o.key);
        else n++;
      }
      if (gone.length) await env.SAVES.delete(gone);
      cursor = page.truncated && n < cap ? page.cursor : undefined;
    } while (cursor);
    if (n >= cap) return json({ error: "lots of devices are pairing: try again in a minute", retry_after: 60 }, 429, { "retry-after": "60" });
  }
  await env.SAVES.put(key, sealed, {
    customMetadata: { proof: await sha256hex(proof), owner: owner_h, exp: String(now + PAIR_TTL) },
  });
  return json({ ok: true, expires_in: PAIR_TTL / 1000 }, 201);
}

async function pair_claim(request, env, pid) {
  if (!(await pair_try(request, env))) return too_many();
  const proof = hex_to_bytes((request.headers.get("authorization") || "").replace(/^Bearer\s+/i, ""));
  const key = `pair/${pid}`;
  const obj = await env.SAVES.get(key);
  const none = () => new Response("Not found", { status: 404, headers: { "cache-control": "no-store" } });
  if (!obj) return none();
  if (pair_expired(obj, clock(env)) || !proof || !same(obj.customMetadata.proof, await sha256hex(proof))) {
    await env.SAVES.delete(key);
    return none();
  }
  const sealed = new Uint8Array(await obj.arrayBuffer());
  await env.SAVES.delete(key);  // one use
  return new Response(sealed, {
    headers: { "content-type": "application/octet-stream", "content-length": String(sealed.length), "cache-control": "no-store" },
  });
}

async function pair_status(request, env, pid) {
  const owner = hex_to_bytes((request.headers.get("authorization") || "").replace(/^Bearer\s+/i, ""));
  const obj = owner ? await env.SAVES.head(`pair/${pid}`) : null;
  const live = !!obj && !pair_expired(obj, clock(env)) && same(obj.customMetadata.owner, await sha256hex(owner));
  return json({ live });
}

async function pair_cancel(request, env, pid) {
  const owner = new Uint8Array(await request.arrayBuffer());
  const key = `pair/${pid}`;
  const obj = owner.length === 32 ? await env.SAVES.head(key) : null;
  if (obj && same(obj.customMetadata.owner, await sha256hex(owner))) await env.SAVES.delete(key);
  return json({ ok: true });
}

async function pairing(request, env, parts, url) {
  const pid = parts[2] || "";
  const tail = parts[3] || "";
  if (!ID.test(pid) || parts.length > 4) return new Response("Not found", { status: 404 });
  const m = request.method;
  if (!env.PAIR_LIMITER && (tail === "" || tail === "status" || tail === "cancel")) {
    // Never unlimited: without the rate limiter a 6-digit number could be guessed.
    return json({ error: "pairing isn't set up on this server" }, 503);
  }
  if (tail === "" && m === "POST") {
    const length = Number(request.headers.get("content-length") || "0");
    if (length > 64 + PAIR_BLOB_MAX) return json({ error: "too big" }, 413);
    return pair_create(request, env, pid, url);
  }
  if (tail === "" && m === "GET") return pair_claim(request, env, pid);
  if (tail === "status" && m === "GET") return pair_status(request, env, pid);
  if (tail === "cancel" && m === "POST") return pair_cancel(request, env, pid);
  return new Response("Not found", { status: 404 });
}

async function handle(request, env) {
  const url = new URL(request.url);
  const parts = url.pathname.replace(/^\/+/, "").split("/");
  if (parts[0] === "v1" && parts[1] === "pair") return pairing(request, env, parts, url);
  if (parts[0] !== "v1" || !ID.test(parts[1] || "") || parts.length > 3) return new Response("Not found", { status: 404 });
  const id = parts[1];
  const tail = parts[2] || "";
  const key = `v1/${id}`;
  const m = request.method;

  if (tail === "rev" && m === "GET") {
    const h = await env.SAVES.head(key);
    return json({ rev: rev_of(h), size: h ? h.size : 0 });
  }
  if (tail === "previous" && m === "GET") {
    const o = await env.SAVES.get(`${key}.prev`);
    return o ? serve(o, true) : new Response("Not found", { status: 404 });
  }
  if (tail === "delete" && m === "POST") {
    return remove(env, id, new Uint8Array(await request.arrayBuffer()));
  }
  if (tail !== "") return new Response("Not found", { status: 404 });

  if (m === "GET") {
    const o = await env.SAVES.get(key);
    return o ? serve(o, true) : new Response("Not found", { status: 404 });
  }
  if (m === "HEAD") {
    const o = await env.SAVES.head(key);
    return o ? serve(o, false) : new Response(null, { status: 404 });
  }
  const length = Number(request.headers.get("content-length") || "0");
  if (length > MAX_BLOB + TOKEN) return json({ error: "over 1 MiB" }, 413);
  if (m === "POST") {
    const q = url.searchParams.get("if_match");
    if (q === null || !/^\d{1,15}$/.test(q)) return json({ error: "if_match must be the current revision" }, 400);
    const body = new Uint8Array(await request.arrayBuffer());
    if (body.length > MAX_BLOB + TOKEN) return json({ error: "over 1 MiB" }, 413);
    return write(env, id, Number(q), body.slice(0, TOKEN), body.slice(TOKEN));
  }
  if (m === "PUT") {
    const auth = (request.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
    const im = request.headers.get("if-match");
    const inm = request.headers.get("if-none-match");
    let if_match = -1;
    if (inm === "*") if_match = 0;
    else if (im && /^"?\d{1,15}"?$/.test(im)) if_match = Number(im.replace(/"/g, ""));
    else return json({ error: "If-Match: \"<rev>\" or If-None-Match: * needed" }, 428);
    const body = new Uint8Array(await request.arrayBuffer());
    return write(env, id, if_match, hex_to_bytes(auth), body);
  }
  if (m === "DELETE") {
    const auth = (request.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
    return remove(env, id, hex_to_bytes(auth));
  }
  return json({ error: "method" }, 405);
}

export default {
  async fetch(request, env) {
    try {
      return await handle(request, env);
    } catch (e) {
      return json({ error: "server" }, 500);
    }
  },
  async scheduled(event, env, ctx) {
    ctx.waitUntil(sweep(env));
  },
};
