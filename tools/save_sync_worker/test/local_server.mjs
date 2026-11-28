// The sync server on this computer, for two app copies (or the C++ test's two "devices") to sync through: the Worker's
// own code (../src/index.js) behind Node's HTTP server, with an R2 bucket in memory. No Cloudflare account, no wrangler.
//   node tools/save_sync_worker/test/local_server.mjs [port]      prints "listening <port>", then serves until killed
// Point an app at it with BLITZ_SYNC_SERVER=http://127.0.0.1:<port>/ (save_sync.cpp reads it before the build's file).
import http from "node:http";
import worker from "../src/index.js";
import { MemoryR2 } from "./r2_memory.mjs";
import { MemoryLimiter } from "./rate_memory.mjs";

// The pairing limiter as deployed (5 a minute per connection), unless PAIR_TRIES says otherwise (the C++ test sets a
// high one so its many pairings don't wait; it checks the limit through its own server started with the real one).
const env = { SAVES: new MemoryR2(), PAIR_LIMITER: new MemoryLimiter(Number(process.env.PAIR_TRIES || "5"), 60) };
const port = Number(process.argv[2] || "0");

const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  const body = Buffer.concat(chunks);
  const headers = {};
  for (const [k, v] of Object.entries(req.headers)) headers[k] = Array.isArray(v) ? v.join(", ") : v;
  const request = new Request(`http://127.0.0.1${req.url}`, {
    method: req.method,
    headers,
    body: req.method === "GET" || req.method === "HEAD" ? undefined : body,
  });
  const r = await worker.fetch(request, env);
  const out = r.body ? Buffer.from(await r.arrayBuffer()) : Buffer.alloc(0);
  const h = Object.fromEntries(r.headers.entries());
  if (req.method !== "HEAD") h["content-length"] = String(out.length);
  res.writeHead(r.status, h);
  res.end(req.method === "HEAD" ? undefined : out);
  if (process.env.SYNC_LOG) console.error(`${req.method} ${req.url} ${r.status}`);
});
server.listen(port, "127.0.0.1", () => {
  console.log(`listening ${server.address().port}`);
});
