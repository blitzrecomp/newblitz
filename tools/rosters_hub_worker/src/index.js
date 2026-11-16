// The rosters post office (2026-10-28): the daily roster job's files, served to every New Blitz device over the air.
//
// The job (blitz-rosters' rosters.yml) builds the files on GitHub and uploads them here; each device's app follows
// this as its rosters hub (<app folder>/rosters/hub or the build's Resources/rosters/hub: the base address below),
// which needs no GitHub key (src/game/rosters_live.cpp). Nothing else changes in the app.
//
//   PUT  /upload/<path>          the job: a file (manifest.json last), Authorization: Bearer <UPLOAD_SECRET>
//   GET  /<HUB_KEY>/<path>       a device: manifest.json, players.json, lineups/<name>.json, psp/<name>.txt (the PSP's
//                                pack, docs/PSP_PORT.md §6; the PSP asks over plain http://, workers.dev answers it)
//   GET  /<HUB_KEY>/status       when the last upload landed, and the manifest's "latest" (alerts, tools)
//
// HUB_KEY is a long random path segment: the address isn't a public, findable endpoint (privacy, not security).
// Files live in KV (free, no billing): a few writes a day, well under its limits.
//
// The season server (2026-11-13; docs/design/SEASON_PAK.md): the public New Blitz app downloads the 2026 Season pak
// from here. Its routes are public and need no HUB_KEY (the public app carries this address; it must never carry the
// key), and nothing under them reveals it:
//   PUT  /upload/paks/catalog.json        tools/publish_pak.sh, last: the catalog (blitz-paks/1), same Bearer secret
//   PUT  /upload/paks/<name>.nbpak        tools/publish_pak.sh, first: a pak (a zip, at most 25 MiB, KV's limit)
//   GET  /paks/catalog.json               the app: which pak, its file, size and SHA-256, and the switches every
//                                         installed copy follows ("headshots", "live_rosters"). Removing the pak's
//                                         entry (or "available": false) is the kill switch: no new installs.
//   GET  /paks/<name>.nbpak               the pak (a versioned name: cached for good)
//   GET  /paks/rosters/<path>             the weekly roster files for a pak install that opted in (manifest.json,
//                                         players.json, lineups/*.json: the same files the pak carries, newer), only
//                                         while the catalog's season2026 entry has "live_rosters" on
//
// Crash reports (2026-11-15; docs/design/FIRST_RUN_V2.md §6.2): the app sends one, opted in, at the start after a crash
// (src/main/crash_report.cpp). Public like the paks; kept apart from the private blitz-logs Worker.
//   POST /reports/crash                   the app: the report, plain text that starts "New Blitz crash report", at most
//                                         256 KiB. Kept 30 days. Nothing about the sender is stored: not the address
//                                         (CF-Connecting-IP is never read), not the headers; only the text and when.
//                                         At most 300 a day (KV's free writes), then 429 and the app tries next start.
//   GET  /<HUB_KEY>/reports               the newest 100: id, when, size, first crash line
//   GET  /<HUB_KEY>/reports/<id>          one report's text

const PATH = /^(manifest\.json|players\.json|lineups\/[A-Za-z0-9._-]+\.json|psp\/[A-Za-z0-9._-]+\.txt)$/;
const TEXT = /^psp\//;  // plain text, not JSON
const PAK = /^[a-z0-9_]+(-[A-Za-z0-9.]+)?\.nbpak$/;
const PUBLIC_ROSTERS = /^(manifest\.json|players\.json|lineups\/[A-Za-z0-9._-]+\.json)$/;
const MAX_PAK = 25 * 1024 * 1024;
const MAX_REPORT = 256 * 1024;
const REPORT_DAYS = 30;
const REPORTS_PER_DAY = 300;
const REPORT_ID = /^[0-9TZ:.-]+-[0-9a-f]{8}$/;

// POST /reports/crash: checked, then kept with an expiry. The id is the arrival time and a random tag.
async function report_crash(request, env) {
  const length = Number(request.headers.get("content-length") || "0");
  if (length > MAX_REPORT) return json({ error: "too big" }, 413);
  const body = await request.text();
  if (body.length > MAX_REPORT) return json({ error: "too big" }, 413);
  if (!body.startsWith("New Blitz crash report\n")) return json({ error: "not a crash report" }, 400);
  const day = new Date().toISOString().slice(0, 10);
  const count_key = `reports:count:${day}`;
  const count = Number((await env.ROSTERS.get(count_key)) || "0");
  if (count >= REPORTS_PER_DAY) return json({ error: "enough for today" }, 429);
  await env.ROSTERS.put(count_key, String(count + 1), { expirationTtl: 2 * 86400 });
  const tag = [...crypto.getRandomValues(new Uint8Array(4))].map((b) => b.toString(16).padStart(2, "0")).join("");
  const id = `${new Date().toISOString()}-${tag}`;
  const crash = (body.split("\n").find((l) => l.startsWith("[crash]")) || "").slice(0, 200);
  await env.ROSTERS.put(`report:${id}`, body, {
    expirationTtl: REPORT_DAYS * 86400,
    metadata: { bytes: body.length, crash },
  });
  return json({ ok: true, id }, 201);
}

// GET /<HUB_KEY>/reports[/<id>]
async function read_reports(path, env) {
  if (path === "reports") {
    const list = await env.ROSTERS.list({ prefix: "report:", limit: 1000 });
    const rows = list.keys.map((k) => ({ id: k.name.slice("report:".length), ...(k.metadata || {}) }));
    rows.sort((a, b) => (a.id < b.id ? 1 : -1));
    return json(rows.slice(0, 100));
  }
  const id = path.slice("reports/".length);
  if (!REPORT_ID.test(id)) return new Response("Not found", { status: 404 });
  const body = await env.ROSTERS.get(`report:${id}`);
  if (body === null) return new Response("Not found", { status: 404 });
  return new Response(body, { headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" } });
}

// The catalog's entry for the season pak, or null (withdrawn, or no catalog yet).
async function season_entry(env) {
  try {
    const cat = JSON.parse((await env.ROSTERS.get("pak:catalog.json")) || "null");
    const e = cat && cat.paks && cat.paks.season2026;
    return e && e.available !== false ? e : null;
  } catch {
    return null;
  }
}

// The public season routes (no key): GET /paks/...
async function paks(path, env) {
  if (path === "catalog.json") {
    const body = await env.ROSTERS.get("pak:catalog.json");
    if (body === null) return new Response("Not found", { status: 404 });
    return new Response(body, { headers: { "content-type": "application/json", "cache-control": "max-age=60" } });
  }
  if (PAK.test(path)) {
    const body = await env.ROSTERS.get(`pak:file:${path}`, "arrayBuffer");
    if (body === null) return new Response("Not found", { status: 404 });
    return new Response(body, {
      headers: { "content-type": "application/octet-stream", "cache-control": "public, max-age=31536000, immutable" },
    });
  }
  if (path.startsWith("rosters/")) {
    const file = path.slice("rosters/".length);
    const entry = await season_entry(env);
    if (!PUBLIC_ROSTERS.test(file) || !entry || entry.live_rosters === false) return new Response("Not found", { status: 404 });
    const body = await env.ROSTERS.get(`file:${file}`);
    if (body === null) return new Response("Not found", { status: 404 });
    return new Response(body, {
      headers: { "content-type": "application/json", "cache-control": file === "manifest.json" ? "no-store" : "max-age=300" },
    });
  }
  return new Response("Not found", { status: 404 });
}

// PUT /upload/paks/...: a pak (a zip) or the catalog (checked JSON).
async function upload_pak(path, request, env) {
  if (path === "catalog.json") {
    const body = await request.text();
    let cat;
    try {
      cat = JSON.parse(body);
    } catch {
      return json({ error: "not JSON" }, 400);
    }
    if (!cat || cat.format !== "blitz-paks/1" || typeof cat.paks !== "object") return json({ error: "not a blitz-paks/1 catalog" }, 400);
    for (const [id, e] of Object.entries(cat.paks)) {
      if (!e || !PAK.test(e.file || "") || !/^[0-9a-f]{64}$/.test(e.sha256 || "")) return json({ error: `entry ${id} needs file and sha256` }, 400);
      // The pak it names must be here already (publish_pak.sh uploads it first): a catalog never points at nothing.
      if (e.available !== false && (await env.ROSTERS.get(`pak:file:${e.file}`, "arrayBuffer")) === null) {
        return json({ error: `${e.file} isn't uploaded` }, 400);
      }
    }
    await env.ROSTERS.put("pak:catalog.json", body);
    return json({ ok: true, path, bytes: body.length });
  }
  if (!PAK.test(path)) return json({ error: "not a pak name" }, 400);
  const body = await request.arrayBuffer();
  const head = new Uint8Array(body, 0, Math.min(4, body.byteLength));
  if (body.byteLength > MAX_PAK || head.length < 4 || head[0] !== 0x50 || head[1] !== 0x4b || head[2] !== 3 || head[3] !== 4) {
    return json({ error: "not a zip (or over 25 MiB)" }, 400);
  }
  await env.ROSTERS.put(`pak:file:${path}`, body);
  return json({ ok: true, path, bytes: body.byteLength });
}

// A PSP pack (tools/psp/make_pack.py): its header, then P| and L| lines only.
function is_pack(body) {
  const lines = body.split("\n");
  if (!/^#blitz-psp-pack\/1 \S+ \S+$/.test(lines[0])) return false;
  if (lines[lines.length - 1] === "") lines.pop();
  return lines.length > 1 && lines.slice(1).every((l) => /^[PL]\|[^\r]*$/.test(l));
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body, null, 1), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const parts = url.pathname.replace(/^\/+/, "").split("/");
    const head = parts.shift();
    const path = parts.join("/");

    if (head === "reports") {  // public: the app's crash reports, in only (above)
      if (path !== "crash" || request.method !== "POST") return new Response("Not found", { status: 404 });
      return report_crash(request, env);
    }

    if (head === "paks") {  // public: no key (above)
      if (request.method !== "GET" && request.method !== "HEAD") return new Response("Not found", { status: 404 });
      return paks(path, env);
    }

    if (head === "upload") {
      if (request.method !== "PUT") return json({ error: "PUT only" }, 405);
      if (!env.UPLOAD_SECRET || request.headers.get("authorization") !== `Bearer ${env.UPLOAD_SECRET}`) return json({ error: "no" }, 403);
      if (path.startsWith("paks/")) return upload_pak(path.slice("paks/".length), request, env);
      if (!PATH.test(path)) return json({ error: "not a roster file" }, 400);
      const body = await request.text();
      if (TEXT.test(path)) {
        if (!is_pack(body)) return json({ error: "not a PSP pack" }, 400);  // a broken file never reaches a device
      } else {
        try {
          JSON.parse(body);  // a broken file never reaches a device
        } catch {
          return json({ error: "not JSON" }, 400);
        }
      }
      await env.ROSTERS.put(`file:${path}`, body);
      if (path === "manifest.json") {
        const latest = JSON.parse(body).latest || null;
        await env.ROSTERS.put("status", JSON.stringify({ uploaded: new Date().toISOString(), latest }));
      }
      return json({ ok: true, path, bytes: body.length });
    }

    if (!env.HUB_KEY || head !== env.HUB_KEY || request.method !== "GET") return new Response("Not found", { status: 404 });
    if (path === "reports" || path.startsWith("reports/")) return read_reports(path, env);
    if (path === "status") {  // the last upload, how long ago, and whether that's overdue (a day and a half)
      const st = JSON.parse((await env.ROSTERS.get("status")) || "{}");
      if (st.uploaded) {
        st.hours_ago = Math.round((Date.now() - Date.parse(st.uploaded)) / 36e5 * 10) / 10;
        st.stale = st.hours_ago > 36;
      }
      return json(st);
    }
    if (!PATH.test(path)) return new Response("Not found", { status: 404 });
    const body = await env.ROSTERS.get(`file:${path}`);
    if (body === null) return new Response("Not found", { status: 404 });
    return new Response(body, {
      headers: {
        "content-type": TEXT.test(path) ? "text/plain; charset=utf-8" : "application/json",
        "cache-control": path === "manifest.json" ? "no-store" : "max-age=300",
      },
    });
  },
};
