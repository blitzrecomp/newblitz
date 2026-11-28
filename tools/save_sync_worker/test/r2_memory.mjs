// An R2 bucket in memory, with the parts of the Workers R2 API the sync server uses (get, head, put with
// onlyIf.etagMatches, delete of one key or many, list by prefix with cursors). For the tests and test/local_server.mjs:
// no Cloudflare account needed.
export class MemoryR2 {
  constructor() {
    this.m = new Map();
    this.n = 0;
    this.ops = { a: 0, b: 0 };  // Class A (put, list, delete is free but counted here as A) and Class B (get, head)
  }
  object(key, e, withBody) {
    const bytes = e.bytes;
    const o = {
      key,
      size: bytes.length,
      etag: e.etag,
      uploaded: e.uploaded,
      customMetadata: { ...e.meta },
    };
    if (withBody) {
      o.body = bytes;
      o.arrayBuffer = async () => bytes.slice().buffer;
      o.text = async () => new TextDecoder().decode(bytes);
    }
    return o;
  }
  async get(key) {
    this.ops.b++;
    const e = this.m.get(key);
    return e ? this.object(key, e, true) : null;
  }
  async head(key) {
    this.ops.b++;
    const e = this.m.get(key);
    return e ? this.object(key, e, false) : null;
  }
  async put(key, value, opts = {}) {
    this.ops.a++;
    const cur = this.m.get(key);
    if (opts.onlyIf && opts.onlyIf.etagMatches !== undefined && (!cur || cur.etag !== opts.onlyIf.etagMatches)) return null;
    let bytes;
    if (typeof value === "string") bytes = new TextEncoder().encode(value);
    else if (value instanceof ArrayBuffer) bytes = new Uint8Array(value.slice(0));
    else bytes = new Uint8Array(value);
    const e = { bytes, etag: `e${++this.n}`, uploaded: this.now ? new Date(this.now()) : new Date(), meta: { ...(opts.customMetadata || {}) } };
    this.m.set(key, e);
    return this.object(key, e, false);
  }
  async delete(keys) {
    this.ops.a++;
    for (const k of Array.isArray(keys) ? keys : [keys]) this.m.delete(k);
  }
  async list({ prefix = "", cursor, limit = 1000 } = {}) {
    this.ops.a++;
    const all = [...this.m.keys()].filter((k) => k.startsWith(prefix)).sort();
    const start = cursor ? Number(cursor) : 0;
    const page = all.slice(start, start + limit);
    const truncated = start + limit < all.length;
    return { objects: page.map((k) => this.object(k, this.m.get(k), false)), truncated, cursor: truncated ? String(start + limit) : undefined };
  }
}
