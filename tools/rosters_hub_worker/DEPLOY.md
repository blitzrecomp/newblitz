# Deploying the crash-report route (deployed 2026-11-15 to the public season server)

The route `POST /reports/crash` (src/index.js, 2026-11-15) takes the crash reports New Blitz 0.9.5 sends when a player
ticks Send Crash Reports (src/main/crash_report.cpp; docs/design/FIRST_RUN_V2.md §6.2). It's part of the same Worker
as the season server, so the app needs no new address: it posts to the season server's base (the build's
`Resources/paks/server`, without `paks/`) plus `reports/crash`. Until it's deployed, the app's reports get a 404 and
are tried again at the next start, three times at most, then dropped.

Nothing new to set up: the route uses the Worker's existing KV namespace (`ROSTERS`) and `HUB_KEY`.

## Steps

1. Test locally (no account needed):

       cd tools/rosters_hub_worker
       node test/worker_test.mjs        # prints "ok N"

2. Deploy the public season server (`newblitz`), logged in to the public project's account only, as the comment in
   `wrangler.public.toml` says:

       cd tools/rosters_hub_worker
       XDG_CONFIG_HOME=<repo>/build/wrangler_public npx wrangler@4 deploy -c wrangler.public.toml

   The private rosters post office (`wrangler.toml`) runs the same code; deploying it there too is harmless (the app
   never posts there) but not needed.

3. Check it from a computer (any text that starts with the report's first line):

       printf 'New Blitz crash report\napp: test\n' | curl -s -X POST --data-binary @- <season server base>/reports/crash
       # {"ok": true, "id": "..."}

4. Read reports (the hub key is the Worker's `HUB_KEY` secret; never put it in the app or the public tree):

       curl -s <season server base>/<HUB_KEY>/reports              # the newest 100: id, when, size, first crash line
       curl -s <season server base>/<HUB_KEY>/reports/<id>         # one report

## What it keeps

The report's text and when it arrived, for 30 days (KV expiry). Not the sender's address (`CF-Connecting-IP` is never
read) or any header. At most 300 a day (KV's free tier allows 1,000 writes a day; each report is two), then `429`.
The app removes the player's home folder, account name, ROM and pak file names and network addresses before sending
(crash_report.cpp `scrub`).

## Turning it off

Remove the `reports` branch in `fetch` and deploy again: the app sees 404s and gives up after three starts.
