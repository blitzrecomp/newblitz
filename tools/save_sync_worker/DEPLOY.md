# Deploying the save sync server

Cloud Sync (Settings → General → Saves → Cloud Sync; docs/SAVE_SYNC.md §7) needs this Worker and one R2 bucket. The
first version was deployed on 2026-11-27 (steps 1-8 below). **Pairing** (Add a Device's 6-digit number, branch
`sync-pairing`) needs one more deploy with a rate-limit binding: the section "Pairing" right below. The steps touch only
the Cloudflare account wrangler is logged in to.

## Pairing: the redeploy for Add a Device (not done yet)

What changes: new routes under `/v1/pair/` (src/index.js header), a `[[ratelimits]]` binding `PAIR_LIMITER` (5 creates or
lookups a minute per connection: about 10 guesses in a number's two minutes), a variable `MAX_PAIRS = "500"`, and
`pair/<pid>` objects in the same bucket (each lives two minutes). Every existing route answers exactly as before (the
Worker test checks it), so devices on 1.0.4-era builds keep syncing. Without the binding the pairing routes answer 503
and the app says "This sync server can't pair devices yet" (the full code still works).

1. **Test locally:** `make test-sync-worker` ("ok 83") and `make test-cloud` ("ok: 437 checks").
2. **The rate limiter's namespace id.** wrangler.toml has `namespace_id = "7301"`: any positive integer that no other
   Worker's rate limiter in the account uses (the rosters hub and the logs hub have none). Change it there if 7301 is
   taken. Nothing has to be created first: the binding comes with the deploy (Workers Rate Limiting, wrangler 4).
3. **Deploy** (same login as step 2 below; `whoami` first):

       cd tools/save_sync_worker
       npx wrangler@4 whoami
       npx wrangler@4 deploy                # the output lists the bindings: SAVES (R2), PAIR_LIMITER (rate limit), vars

   If wrangler refuses `[[ratelimits]]`, update it (`npx wrangler@4` fetches the latest 4.x).
4. **Expiry backstop** (optional: a lookup deletes an expired number, the create sweeps them, and the daily cron removes
   any left): one lifecycle rule for `pair/`, safe because no number lives past two minutes:

       npx wrangler@4 r2 bucket lifecycle add newblitz-saves pair-expire pair/ --expire-days 1

5. **Check it** (nothing is written by these):

       W=https://newblitz-saves.<subdomain>.workers.dev
       curl -s -o /dev/null -w '%{http_code}\n' $W/v1/pair/$(printf '%064d' 0)                          # 404 (503: no binding)
       curl -s -X POST --data-binary @/dev/null $W/v1/pair/$(printf '%064d' 0)/cancel                    # {"ok":true}
       curl -s $W/v1/$(printf '%064d' 0)/rev                                                              # {"rev":0,"size":0}

   Then on two devices: docs/HUMAN_TESTS.md ("Cloud Sync pairing").
6. **Privacy:** the limiter is keyed by a salted SHA-256 of CF-Connecting-IP; the address is never stored or logged by
   the Worker (docs/SAVE_SYNC.md §7.4). Keep Workers Logs / Logpush off for this Worker (they would record request
   metadata, addresses included); `wrangler tail` while testing is fine (nothing kept).

## What gets created

| Thing | Name | Notes |
|---|---|---|
| Worker | `newblitz-saves` | `src/index.js`, from `wrangler.toml` in this folder. Address: `https://newblitz-saves.<subdomain>.workers.dev/` |
| R2 bucket | `newblitz-saves` | bound to the Worker as `SAVES`. Keys: `v1/<id>`, `v1/<id>.prev`, `meta/new-<YYYY-MM-DD>`, `pair/<pid>` (two minutes) |
| Cron trigger | `17 4 * * *` | the daily 90-day sweep (`scheduled` → `sweep`; also old pairing numbers) |
| Variable | `MAX_NEW_PER_DAY = "2000"` | new sync codes a day, all players together |
| Variable | `MAX_PAIRS = "500"` | pairing numbers live at once, all players together |
| Rate limit | `PAIR_LIMITER`, namespace `7301` | 5 a minute per connection (salted hash of the address), pairing only |
| Secrets | **none** | write tokens are made on each device from its code; the server keeps only their SHA-256 |
| Ids per code | up to 3 | one per Cloud Save Slot (derived from the code and the slot number); the caps count per id |
| KV, Durable Objects, D1 | none | |

## The first deploy (done 2026-11-27)

The app learns the address from `build/save_sync_server` at build time (step 7), never from the source.

1. **Test locally** (no account needed), from the repo root:

       make test-sync-worker            # the server's rules: "ok 83" (pairing included)
       make test-cloud                  # the app's side against this Worker's code on this computer: "ok: 437 checks"

2. **Log in** to the account the saves should live on. If it is the public project's account (as for the season
   server), keep its login apart:

       cd tools/save_sync_worker
       export XDG_CONFIG_HOME=<repo>/build/wrangler_public     # only for a separate account's login
       npx wrangler@4 login
       npx wrangler@4 whoami                                    # check it's the right account

   R2 has to be enabled on the account (dashboard → R2, with a payment method, even within the free tier).

3. **Create the bucket:**

       npx wrangler@4 r2 bucket create newblitz-saves

4. **Expiry.** The Worker's daily cron removes ids with no write for 90 days (it reads each object's upload time, which
   every save renews), so nothing else is needed. One lifecycle rule tidies the day counters in case the cron is ever
   off (the counters are never rewritten, so their age is safe to go by):

       npx wrangler@4 r2 bucket lifecycle add newblitz-saves meta-expire meta/ --expire-days 8

   Don't put a lifecycle rule on `v1/` unless R2 is confirmed to count an object's age from its latest overwrite: the
   docs don't say, and counting from the first upload would delete active players' saves after 90 days.

5. **Deploy:**

       npx wrangler@4 deploy

   It prints the address, `https://newblitz-saves.<subdomain>.workers.dev`.

6. **Check it** (any 64-hex id; nothing is written):

       curl -s https://newblitz-saves.<subdomain>.workers.dev/v1/$(printf '%064d' 0)/rev      # {"rev":0,"size":0}

   And a full round trip from the Mac without the app: `make test-cloud` can't point at it (it starts its own local
   server), but two app copies can (step 8).

7. **Give the app the address:** one line in `build/save_sync_server` (never committed: build/ isn't), then rebuild.
   The Makefile copies it into every build's `Resources/sync/server` (`save_sync_server_file`); the app reads it
   there (`src/main/save_sync.cpp sync_server`). Every platform's build that bundles the season server's address
   (`pak_server_file`) bundles this one too.

       echo https://newblitz-saves.<subdomain>.workers.dev/ > build/save_sync_server
       make app-modern                                          # and the device builds: make android-modern, ios-modern ...

   Without the file, Settings → Saves has no Cloud Sync row (Sync over Wi-Fi and the save file still work). For a try
   without rebuilding: start the app with `BLITZ_SYNC_SERVER=https://newblitz-saves.<subdomain>.workers.dev/`.

8. **Try it** on the Mac with two copies before any device (each with its own app folder, build/sync_test/A and B):

       BLITZ_SYNC_SERVER=https://newblitz-saves.<subdomain>.workers.dev/ tools/save_sync_two_apps.sh

   Then the devices: docs/HUMAN_TESTS.md ("Cloud Sync and Sync over Wi-Fi").

## Cloud Sync in public builds (done for 1.0.5, branch `cloudsync-public`)

Cloud Sync is in every build since 1.0.5, public and release ones too:

1. CMakeLists.txt: `BLITZ_SAVE_SYNC_NET` ON by default everywhere.
2. Makefile `save_sync_server_file`: copies `build/save_sync_server` into public builds too; `make release` refuses to
   start without it and checks each staged app (Mac, Windows, Linux) has `sync/server`.
3. tools/release_scan.sh: `save_sync_server` is no longer a secret and `sync/server` may ship; its host is allowed beside
   the season server's (both `*.workers.dev`). The private files (rosters hub, season server's keys, logs mailbox,
   install links, PSP keys) are still searched for.
4. This file's wrangler.toml is in the public tree (no secrets, no account).
5. The privacy page: site/privacy.html ("4. Cloud Sync").

## Watching it

    npx wrangler@4 tail newblitz-saves                         # live requests (no bodies are logged)
    npx wrangler@4 r2 object get newblitz-saves/v1/<id> --pipe | wc -c      # one blob's size (ciphertext)

## Turning it off

Delete the Worker (`npx wrangler@4 delete newblitz-saves`) or deploy one that answers 503: the app says it can't reach
the sync server and every device keeps its own saves; nothing is lost. Emptying the bucket deletes only ciphertext that
no one but the players can read.

## Costs

docs/SAVE_SYNC.md §7.5: $0 up to about a thousand daily players, around $9 a month at 10,000, around $113 at 100,000
(heavy two-device use; real use should be a fraction).
