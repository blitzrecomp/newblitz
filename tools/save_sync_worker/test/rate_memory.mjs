// Cloudflare's Workers Rate Limiting binding in memory ({ limit({ key }) } -> { success }), for the tests and
// test/local_server.mjs: `limit` calls per key in each `period` seconds (a fixed window), on the clock given.
export class MemoryLimiter {
  constructor(limit = 5, period = 60, now = () => Date.now()) {
    this.max = limit;
    this.period = period * 1000;
    this.now = now;
    this.windows = new Map();  // key -> { start, n }
    this.keys = [];            // every key seen, for the tests (they must never be an address)
  }
  async limit({ key }) {
    this.keys.push(key);
    const t = this.now();
    let w = this.windows.get(key);
    if (!w || t - w.start >= this.period) {
      w = { start: t, n: 0 };
      this.windows.set(key, w);
    }
    w.n++;
    return { success: w.n <= this.max };
  }
}
