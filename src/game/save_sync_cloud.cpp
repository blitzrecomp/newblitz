// Cloud Sync's code, keys, sealed bundle, server calls and decisions (save_sync_cloud.h, docs/SAVE_SYNC.md §7).
#include "save_sync_cloud.h"

#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../common/sha256.h"
#include "json/json.hpp"
#include "monocypher.h"

namespace blitz::savesync::cloud {
    namespace {
        using json = nlohmann::json;
        constexpr const char* alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford's base32: no I, L, O or U
        constexpr const char* state_file = "cloud_sync.json";
        constexpr const char* copies_dir = "cloud_sync";
        constexpr uint8_t magic[4] = {'N', 'B', 'C', '1'};

        void random_bytes(uint8_t* out, size_t n) {
            // std::random_device is the system's cryptographic source on every platform we build for (arc4random on
            // Apple's libc++, /dev/urandom or getrandom on Android and Linux, rand_s on Windows, the kernel's on the PS5).
            std::random_device rd;
            for (size_t i = 0; i < n; i += 4) {
                uint32_t v = rd();
                for (size_t j = 0; j < 4 && i + j < n; j++) out[i + j] = (uint8_t)(v >> (8 * j));
            }
        }

        std::string hex(const uint8_t* p, size_t n) { return blitzsha::hex(p, n); }

        void put_le64(uint8_t* p, uint64_t v) {
            for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
        }
        uint64_t get_le64(const uint8_t* p) {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
            return v;
        }

        std::string url_of(const std::string& base, const Keys& k, const std::string& tail = "") {
            std::string b = base;
            if (!b.empty() && b.back() != '/') b += '/';
            return b + "v1/" + k.id + tail;
        }

        std::time_t now() { return std::time(nullptr); }
        int clamp_slot(int s) { return s >= 1 && s <= slot_count ? s : 1; }
    }

    // ---- the code ----------------------------------------------------------------------------------------------------
    std::string new_code() {
        uint8_t b[code_chars];
        random_bytes(b, sizeof b);
        std::string out;
        for (uint8_t v : b) out += alphabet[v & 31];  // 256 is a multiple of 32: every character equally likely
        crypto_wipe(b, sizeof b);
        return out;
    }

    std::string normalize_code(const std::string& typed) {
        std::string out;
        for (char c : typed) {
            if (c == ' ' || c == '-' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c == 'O') c = '0';
            if (c == 'I' || c == 'L') c = '1';
            if (std::strchr(alphabet, c) == nullptr || c == 0) return "";
            out += c;
        }
        return out.size() == code_chars ? out : "";
    }

    std::string group_code(const std::string& code) {
        std::string out;
        for (size_t i = 0; i < code.size(); i++) {
            if (i > 0 && i % 4 == 0) out += '-';
            out += code[i];
        }
        return out;
    }

    std::string Keys::token_hex() const { return hex(token.data(), token.size()); }

    Keys derive(const std::string& code, int slot) {
        // Keyed BLAKE2b with the code as the key, one label per output and slot: each is a separate pseudo-random function
        // of the code, so knowing an id (the server does) tells nothing about the key, the token or another slot's id. The
        // code carries 120 random bits, so there's nothing for a slow, memory-hard function to add here (Sync over Wi-Fi's
        // short code uses one).
        Keys k;
        const std::string s = "new blitz cloud sync v1: slot " + std::to_string(clamp_slot(slot)) + ": ";
        auto prf = [&](const char* what, uint8_t* out) {
            const std::string label = s + what;
            crypto_blake2b_keyed(out, 32, (const uint8_t*)code.data(), code.size(), (const uint8_t*)label.data(), label.size());
        };
        uint8_t id[32];
        prf("id", id);
        prf("key", k.key.data());
        prf("write token", k.token.data());
        k.id = hex(id, sizeof id);
        return k;
    }

    // ---- sealing -----------------------------------------------------------------------------------------------------
    namespace {
        Bytes associated(const Keys& k, const uint8_t* head12) {
            Bytes ad(head12, head12 + 12);
            ad.insert(ad.end(), k.id.begin(), k.id.end());
            return ad;
        }
    }

    Bytes seal(const Keys& k, uint64_t rev, const Bytes& plain) {
        Bytes out(seal_overhead + plain.size());
        std::memcpy(out.data(), magic, 4);
        put_le64(out.data() + 4, rev);
        uint8_t* nonce = out.data() + 12;
        uint8_t* mac = out.data() + 36;
        random_bytes(nonce, 24);
        Bytes ad = associated(k, out.data());
        crypto_aead_lock(out.data() + seal_overhead, mac, k.key.data(), nonce, ad.data(), ad.size(), plain.data(), plain.size());
        return out;
    }

    Opened open(const Keys& k, const Bytes& blob) {
        Opened o;
        if (blob.size() < seal_overhead || std::memcmp(blob.data(), magic, 4) != 0) {
            o.why = "not a sealed bundle";
            return o;
        }
        o.rev = get_le64(blob.data() + 4);
        Bytes ad = associated(k, blob.data());
        o.plain.resize(blob.size() - seal_overhead);
        if (crypto_aead_unlock(o.plain.data(), blob.data() + 36, k.key.data(), blob.data() + 12, ad.data(), ad.size(),
                               blob.data() + seal_overhead, o.plain.size()) != 0) {
            o.plain.clear();
            o.why = "it doesn't open with this code (another code, or changed on the way)";
            return o;
        }
        o.ok = true;
        return o;
    }

    // ---- the server --------------------------------------------------------------------------------------------------
    Head head(const Http& http, const std::string& base, const Keys& k) {
        Head h;
        std::string body;
        bool ok = http.get(url_of(base, k, "/rev"), body, h.status);
        if (!ok || h.status != 200) return h;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object() || !j.contains("rev") || !j["rev"].is_number_unsigned()) return h;
        h.rev = j["rev"].get<uint64_t>();
        h.ok = true;
        return h;
    }

    Bytes fetch(const Http& http, const std::string& base, const Keys& k, int& status) {
        std::string body;
        if (!http.get(url_of(base, k), body, status) || status != 200) return {};
        return Bytes(body.begin(), body.end());
    }

    int put(const Http& http, const std::string& base, const Keys& k, uint64_t if_match, const Bytes& blob) {
        std::string body((const char*)k.token.data(), k.token.size());
        body.append((const char*)blob.data(), blob.size());
        int status = 0;
        http.post(url_of(base, k, "?if_match=" + std::to_string(if_match)), body, status);
        return status;
    }

    int remove(const Http& http, const std::string& base, const Keys& k) {
        int status = 0;
        http.post(url_of(base, k, "/delete"), std::string((const char*)k.token.data(), k.token.size()), status);
        return status;
    }

    // ---- where the code is kept --------------------------------------------------------------------------------------
    Secrets file_secrets(const fs::path& file) {
        Secrets s;
        s.kind = "a file only this user can read";
        s.put = [file](const std::string& value) {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
#ifdef _WIN32
            return write_file(file, Bytes(value.begin(), value.end()));
#else
            // Made 0600 before anything is in it, then renamed into place (write_file's temporary would be 0644 first).
            fs::path tmp = file;
            tmp += ".tmp";
            int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) return false;
            ::fchmod(fd, 0600);
            bool ok = ::write(fd, value.data(), value.size()) == (ssize_t)value.size();
            ok = ::fsync(fd) == 0 && ok;
            ok = ::close(fd) == 0 && ok;
            if (ok) fs::rename(tmp, file, ec);
            if (!ok || ec) fs::remove(tmp, ec);
            return ok && !ec;
#endif
        };
        s.get = [file](std::string& value) {
            bool ok = false;
            Bytes b = read_file(file, &ok);
            value.assign(b.begin(), b.end());
            return ok && !value.empty();
        };
        s.erase = [file] {
            std::error_code ec;
            fs::remove(file, ec);
        };
        return s;
    }

    // ---- state -------------------------------------------------------------------------------------------------------
    namespace {
        void read_slot(const json& j, SlotState& s) {
            if (!j.is_object()) return;
            if (j.contains("rev") && j["rev"].is_number_unsigned()) s.rev = j["rev"].get<uint64_t>();
            s.fp = j.value("fp", "");
            if (j.contains("last_sync") && j["last_sync"].is_number_integer()) s.last_sync = (std::time_t)j["last_sync"].get<int64_t>();
            s.last_device = j.value("last_device", "");
        }
    }

    State load_state(const fs::path& app, const Secrets* secrets) {
        State s;
        bool ok = false;
        Bytes b = read_file(app / state_file, &ok);
        if (!ok) return s;
        json j = json::parse((const char*)b.data(), (const char*)b.data() + b.size(), nullptr, false);
        if (!j.is_object()) return s;
        s.on = j.value("on", false);
        s.slot = clamp_slot(j.value("slot", 1));
        s.last_error = j.value("last_error", "");
        if (j.contains("slots") && j["slots"].is_array()) {
            for (size_t i = 0; i < j["slots"].size() && i < (size_t)slot_count; i++) read_slot(j["slots"][i], s.slots[i]);
        }
        else read_slot(j, s.slots[0]);  // a file from before slots: its one chain is Slot 1's
        if (j.contains("code") && j["code"].is_string()) {
            // A file from before the code moved to secure storage: moved now (and out of the file).
            std::string old = normalize_code(j["code"].get<std::string>());
            if (secrets != nullptr && !old.empty() && secrets->put && secrets->put(old)) {
                store_state(app, s);
                fprintf(stderr, "[cloud] the sync code moved to %s\n", secrets->kind.c_str());
            }
            if (secrets != nullptr) s.code = old;
        }
        if (secrets != nullptr && s.code.empty() && secrets->get) {
            std::string c;
            if (secrets->get(c)) s.code = normalize_code(c);
        }
        if (secrets != nullptr && s.code.empty()) s.on = false;  // no code to sync with
        return s;
    }

    bool store_state(const fs::path& app, const State& s) {
        json slots = json::array();
        for (const SlotState& x : s.slots) {
            slots.push_back({{"rev", x.rev}, {"fp", x.fp}, {"last_sync", (int64_t)x.last_sync}, {"last_device", x.last_device}});
        }
        json j = {{"on", s.on}, {"slot", s.slot}, {"slots", slots}, {"last_error", s.last_error}};
        std::string t = j.dump(1, '\t');
        return write_file(app / state_file, Bytes(t.begin(), t.end()));
    }

    void keep_synced_copy(const fs::path& app, int slot, uint64_t rev, const Bytes& zip) {
        const fs::path d = app / copies_dir;
        const std::string n = "slot" + std::to_string(clamp_slot(slot));
        std::string r = std::to_string(rev);
        if (!write_file(d / (n + ".nbsave"), zip) || !write_file(d / (n + ".rev"), Bytes(r.begin(), r.end())))
            fprintf(stderr, "[cloud] couldn't keep the synced copy of slot %d\n", slot);
    }

    Bytes synced_copy(const fs::path& app, int slot, uint64_t rev) {
        const fs::path d = app / copies_dir;
        const std::string n = "slot" + std::to_string(clamp_slot(slot));
        Bytes r = read_file(d / (n + ".rev"));
        if (std::string(r.begin(), r.end()) != std::to_string(rev)) return {};
        return read_file(d / (n + ".nbsave"));
    }

    // ---- deciding ----------------------------------------------------------------------------------------------------
    Next next_step(uint64_t server_rev, const SlotState& s, const std::string& local_fp) {
        if (server_rev != s.rev) {
            // Nothing on the server any more (expired after 90 days untouched, or turned off elsewhere): send ours again.
            if (server_rev == 0) return local_fp.empty() ? Next::Nothing : Next::Push;
            return Next::Pull;  // another device sent saves since this one last synced
        }
        if (local_fp.empty() || local_fp == s.fp) return Next::Nothing;
        return Next::Push;
    }

    Split split_pull(const Plan& plan, const std::function<bool(const std::string& folder)>& folder_open, bool can_ask, bool ask_replacing) {
        Split s;
        for (size_t i = 0; i < plan.folders.size(); i++) {
            const FolderPlan& f = plan.folders[i];
            const bool open = folder_open && folder_open(f.folder);
            switch (f.verdict) {
                case Verdict::Same: break;
                case Verdict::New: (open ? s.wait : s.take).push_back(i); break;
                case Verdict::Newer:
                    if (ask_replacing) (can_ask ? s.ask : s.wait).push_back(i);
                    else (open ? s.wait : s.take).push_back(i);
                    break;
                case Verdict::Older:
                    // Switching slots: this device's saves carry the slot's on, and sending them would replace what the
                    // slot holds (perhaps kept as a backup). Asked. Otherwise kept, and the cloud gets ours.
                    if (ask_replacing) (can_ask ? s.ask : s.wait).push_back(i);
                    else s.keep.push_back(i);
                    break;
                case Verdict::Conflict: (can_ask ? s.ask : s.wait).push_back(i); break;
            }
        }
        return s;
    }

    std::vector<size_t> push_guard(const Plan& cloud, bool ask_replacing) {
        std::vector<size_t> out;
        for (size_t i = 0; i < cloud.folders.size(); i++) {
            Verdict v = cloud.folders[i].verdict;
            if (v == Verdict::Newer || v == Verdict::Conflict || v == Verdict::New || (ask_replacing && v == Verdict::Older)) out.push_back(i);
        }
        return out;
    }

    const char* result_name(Result r) {
        switch (r) {
            case Result::Off: return "off";
            case Result::Offline: return "offline";
            case Result::UpToDate: return "up to date";
            case Result::Pushed: return "pushed";
            case Result::Pulled: return "pulled";
            case Result::Ask: return "ask";
            case Result::Waiting: return "waiting";
            case Result::Busy: return "busy";
            case Result::Failed: return "failed";
            case Result::Cancelled: return "cancelled";
        }
        return "?";
    }

    // ---- one sync ----------------------------------------------------------------------------------------------------
    namespace {
        std::string slot_name(const State& s) { return "Slot " + std::to_string(s.slot); }

        Outcome done(State& state, const fs::path& app, Result r, const std::string& msg) {
            Outcome o;
            o.result = r;
            o.message = msg;
            if (r == Result::Pushed || r == Result::Pulled || r == Result::UpToDate) {
                state.cur().last_sync = now();
                state.last_error.clear();
            }
            else if (r == Result::Offline || r == Result::Failed) state.last_error = msg;
            store_state(app, state);
            fprintf(stderr, "[cloud] slot %d: %s%s%s\n", state.slot, result_name(r), msg.empty() ? "" : ": ", msg.c_str());
            return o;
        }

        // The player's last chance to cancel, just before a change to the device or the cloud (Hooks::commit).
        bool go_ahead(const Hooks& hooks) { return !hooks.commit || hooks.commit(); }
        Outcome cancelled(const State& state) {
            Outcome o;
            o.result = Result::Cancelled;
            fprintf(stderr, "[cloud] slot %d: cancelled before anything changed\n", state.slot);
            return o;
        }

        Outcome push(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, const Keys& k,
                     uint64_t server_rev, int& retries, bool can_ask);

        // Puts the taken folders of a pulled plan in place, then pushes what the cloud lacks (if nothing waits).
        Outcome apply_pull(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks,
                           const Keys& k, Plan& plan, const std::string& cloud_fp, uint64_t server_rev, size_t waiting, bool can_ask) {
            int taken = 0, later = 0;
            for (const FolderPlan& f : plan.folders) taken += f.take ? 1 : 0;
            if (taken > 0) {
                if (!go_ahead(hooks)) return cancelled(state);
                std::string why;
                // The league settings travel with newer saves (the league they were played in), put in place at the next
                // start (main.cpp save_sync_at_start), as a save file's do. Only with saves taken, never on their own.
                if (!stage(env, plan, !plan.settings.empty(), why)) return done(state, env.app, Result::Failed, why);
                record_import(env, plan);
                for (const fs::path& dir : staged_folders(env, plan)) {
                    bool wait = false;
                    std::string w;
                    if (!hooks.apply(dir, wait, w)) {
                        Outcome o = done(state, env.app, Result::Failed, w.empty() ? "Some saves couldn't be written." : w);
                        o.pulled = taken;  // some may be in: the device changed (the rest go in at the next start)
                        return o;
                    }
                    later += wait ? 1 : 0;
                }
            }
            state.cur().last_device = plan.device;
            const std::string from = plan.device.empty() ? "another device" : plan.device;
            const std::string settings_note = taken > 0 && !plan.settings.empty() ? " (their league settings from the next start)" : "";
            if (waiting > 0 || later > 0) {
                // The cloud's revision isn't marked as had: the next sync pulls again (and nothing is pushed over it meanwhile).
                if (later > 0) state.cur().rev = server_rev;  // put in place at the next start, before the game reads the pak
                Outcome o = done(state, env.app, Result::Waiting,
                                 later > 0 ? "Saves from " + from + " go in when New Blitz starts again"
                                           : "Newer saves from " + from + " come in at the next start");
                o.pulled = taken;
                o.waiting = (int)waiting + later;
                o.device = plan.device;
                o.server_rev = server_rev;
                return o;
            }
            state.cur().rev = server_rev;
            const std::string local = saves_fingerprint(env);
            if (local == cloud_fp || local.empty()) {
                state.cur().fp = local;
                Outcome o = done(state, env.app, taken > 0 ? Result::Pulled : Result::UpToDate,
                                 taken > 0 ? "Saves from " + from + " are here" + settings_note : "");
                o.pulled = taken;
                o.device = plan.device;
                o.server_rev = server_rev;
                return o;
            }
            // This device has saves the cloud doesn't (kept ones, newer ones, other folders): send them up. Decided
            // already (the pull's verdicts, or the player), so no guard.
            int retries = 1;
            Outcome o = push(env, http, base, state, hooks, k, server_rev, retries, can_ask);
            if (taken > 0 && o.result == Result::Pushed) {
                o.result = Result::Pulled;
                o.message = "Saves from " + from + " are here" + settings_note;
            }
            o.pulled = taken;
            o.device = plan.device;
            return o;
        }

        // The cloud's bundle at server_rev: the copy kept at the last sync if it's that revision, else downloaded.
        Bytes cloud_bundle(const Env& env, const Http& http, const std::string& base, const State& state, const Keys& k,
                           uint64_t server_rev, std::string& why, Result& fail) {
            Bytes kept = synced_copy(env.app, state.slot, server_rev);
            if (!kept.empty()) return kept;
            int status = 0;
            Bytes blob = fetch(http, base, k, status);
            if (blob.empty()) {
                why = "The sync server didn't send the saves (" + std::to_string(status) + ")";
                fail = Result::Offline;
                return {};
            }
            Opened o = open(k, blob);
            if (!o.ok) {
                fprintf(stderr, "[cloud] the cloud copy didn't open: %s\n", o.why.c_str());
                why = "The saves in the cloud can't be read with this code.";
                fail = Result::Failed;
                return {};
            }
            if (o.rev != server_rev) fprintf(stderr, "[cloud] the copy says revision %llu, the server %llu\n", (unsigned long long)o.rev, (unsigned long long)server_rev);
            keep_synced_copy(env.app, state.slot, server_rev, o.plain);
            return std::move(o.plain);
        }

        Outcome ask(Plan plan, std::vector<size_t> asked, bool guard, uint64_t server_rev, const std::string& cloud_fp) {
            Outcome out;
            out.result = Result::Ask;
            out.plan = std::move(plan);
            out.asked = std::move(asked);
            out.guard = guard;
            out.server_rev = server_rev;
            out.device = out.plan.device;
            out.cloud_fp = cloud_fp;
            fprintf(stderr, "[cloud] ask: %zu save folder(s) %s\n", out.asked.size(),
                    guard ? "where this device's saves don't carry on from the cloud's" : "changed on both devices");
            return out;
        }

        Outcome pull(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, const Keys& k,
                     uint64_t server_rev, bool can_ask, bool ask_replacing) {
            std::string why;
            Result fail = Result::Failed;
            Bytes zip = cloud_bundle(env, http, base, state, k, server_rev, why, fail);
            if (zip.empty()) return done(state, env.app, fail, why);
            Plan plan = read_bundle(env, zip);
            if (!plan.ok) return done(state, env.app, Result::Failed, plan.why);
            const std::string cloud_fp = bundle_fingerprint(zip);
            Split s = split_pull(plan, [&](const std::string& folder) {
                return hooks.folder_open && hooks.folder_open(folder_dir(env, folder));
            }, can_ask, ask_replacing);
            for (FolderPlan& f : plan.folders) f.take = false;
            for (size_t i : s.take) plan.folders[i].take = true;
            if (!s.ask.empty()) return ask(std::move(plan), s.ask, false, server_rev, cloud_fp);
            bool question_waits = false;
            for (size_t i : s.wait) question_waits = question_waits || plan.folders[i].verdict == Verdict::Conflict || ask_replacing;
            Outcome out = apply_pull(env, http, base, state, hooks, k, plan, cloud_fp, server_rev, s.wait.size(), can_ask);
            if (question_waits && out.result == Result::Waiting) {
                out.message = "Both devices changed a save: choose in Settings \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync";
                state.last_error = out.message;
                store_state(env.app, state);
            }
            return out;
        }

        Outcome push(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, const Keys& k,
                     uint64_t server_rev, int& retries, bool can_ask) {
            if (hooks.busy && hooks.busy()) return done(state, env.app, Result::Busy, "");
            ExportResult r = make_bundle(env);
            if (!r.ok) return done(state, env.app, Result::UpToDate, "");  // no saves yet: nothing to send
            if (r.zip.size() + seal_overhead > max_blob) return done(state, env.app, Result::Failed, "Your saves are too big for Cloud Sync (over 1 MB).");
            if (!go_ahead(hooks)) return cancelled(state);
            Bytes blob = seal(k, server_rev + 1, r.zip);
            int status = put(http, base, k, server_rev, blob);
            switch (status) {
                case 200:
                case 201:
                    state.cur().rev = server_rev + 1;
                    state.cur().fp = bundle_fingerprint(r.zip);
                    keep_synced_copy(env.app, state.slot, server_rev + 1, r.zip);
                    return done(state, env.app, Result::Pushed, "");
                case 412: {
                    // Another device sent saves first: bring those in (and decide), then send again.
                    if (retries-- <= 0) return done(state, env.app, Result::Offline, "Another device is syncing: trying again soon");
                    Head h = head(http, base, k);
                    if (!h.ok) return done(state, env.app, Result::Offline, "The sync server can't be reached");
                    if (h.rev == 0) return push(env, http, base, state, hooks, k, 0, retries, can_ask);
                    return pull(env, http, base, state, hooks, k, h.rev, can_ask, false);
                }
                case 413: return done(state, env.app, Result::Failed, "Your saves are too big for Cloud Sync (over 1 MB).");
                case 429: return done(state, env.app, Result::Offline, "Lots of saves this hour: Cloud Sync sends yours a bit later");
                case 403: return done(state, env.app, Result::Failed, "The sync server turned this code's saves down. Turn Cloud Sync off and on again.");
                case 0: return done(state, env.app, Result::Offline, "The sync server can't be reached");
                default: return done(state, env.app, Result::Offline, "The sync server answered " + std::to_string(status));
            }
        }

        // A push the device decided on by itself (its saves changed since the last sync): first, the cloud's copy must come
        // before this device's saves, folder by folder. If it doesn't, nothing is sent: a question.
        Outcome guarded_push(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks,
                             const Keys& k, uint64_t server_rev, bool can_ask, bool ask_replacing) {
            if (server_rev > 0) {
                std::string why;
                Result fail = Result::Failed;
                Bytes zip = cloud_bundle(env, http, base, state, k, server_rev, why, fail);
                if (zip.empty()) return done(state, env.app, fail, why);
                Plan cloud = read_bundle(env, zip);
                if (!cloud.ok) return done(state, env.app, Result::Failed, cloud.why);
                std::vector<size_t> guard = push_guard(cloud, ask_replacing);
                if (!guard.empty()) {
                    for (FolderPlan& f : cloud.folders) f.take = false;
                    if (can_ask) return ask(std::move(cloud), guard, true, server_rev, bundle_fingerprint(zip));
                    Outcome o = done(state, env.app, Result::Waiting,
                                     "This device's saves don't carry on from " + slot_name(state) +
                                         "'s: choose in Settings \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync");
                    state.last_error = o.message;
                    store_state(env.app, state);
                    return o;
                }
            }
            int retries = 1;
            return push(env, http, base, state, hooks, k, server_rev, retries, can_ask);
        }
    }

    Outcome sync_once(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, bool can_ask,
                      bool ask_replacing) {
        if (!state.on || state.code.empty() || base.empty()) {
            Outcome o;
            o.result = Result::Off;
            return o;
        }
        if (hooks.busy && hooks.busy()) {
            Outcome o;
            o.result = Result::Busy;
            return o;
        }
        // An import from an earlier pull that waited: in now, if the game hasn't opened that pak.
        for (const fs::path& dir : folders_with_staging(env)) {
            if (hooks.folder_open && hooks.folder_open(dir)) continue;
            if (!go_ahead(hooks)) return cancelled(state);
            bool later = false;
            std::string why;
            hooks.apply(dir, later, why);
        }
        if (!folders_with_staging(env).empty()) {
            Outcome o;
            o.result = Result::Waiting;
            o.message = "Saves from the cloud go in when New Blitz starts again";
            return o;
        }
        const Keys k = derive(state.code, state.slot);
        Head h = head(http, base, k);
        if (!h.ok) return done(state, env.app, Result::Offline, h.status ? "The sync server answered " + std::to_string(h.status) : "The sync server can't be reached");
        const std::string local = saves_fingerprint(env);
        switch (next_step(h.rev, state.cur(), local)) {
            case Next::Nothing: return done(state, env.app, Result::UpToDate, "");
            case Next::Push: return guarded_push(env, http, base, state, hooks, k, h.rev, can_ask, ask_replacing);
            case Next::Pull: return pull(env, http, base, state, hooks, k, h.rev, can_ask, ask_replacing);
        }
        return done(state, env.app, Result::Failed, "?");
    }

    // ---- a change the player can still cancel ------------------------------------------------------------------------
    Snapshot snapshot(const fs::path& app, const Secrets& secrets) {
        Snapshot s;
        bool ok = false;
        s.state = read_file(app / state_file, &ok);
        s.had_state = ok;
        std::string c;
        if (secrets.get && secrets.get(c) && !c.empty()) {
            s.had_code = true;
            s.code = c;
        }
        std::error_code ec;
        for (fs::directory_iterator it{app / copies_dir, ec}, end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::u8string n = it->path().filename().u8string();
            s.copies.push_back({std::string(n.begin(), n.end()), read_file(it->path())});
        }
        return s;
    }

    void restore(const fs::path& app, const Secrets& secrets, const Snapshot& snap) {
        std::error_code ec;
        if (snap.had_state) write_file(app / state_file, snap.state);
        else fs::remove(app / state_file, ec);
        std::string now_code;
        const bool has_now = secrets.get && secrets.get(now_code) && !now_code.empty();
        if (snap.had_code) {
            if (!has_now || now_code != snap.code) {
                if (secrets.put) secrets.put(snap.code);
            }
        }
        else if (has_now && secrets.erase) secrets.erase();
        fs::remove_all(app / copies_dir, ec);
        for (const auto& [name, data] : snap.copies) write_file(app / copies_dir / name, data);
    }

    // ---- pairing ------------------------------------------------------------------------------------------------------
    namespace {
        constexpr uint8_t pair_magic[4] = {'N', 'B', 'P', '1'};
        constexpr size_t pair_salt = 16, pair_head = 4 + pair_salt, pair_overhead = pair_head + 24 + 16;
        constexpr const char* pair_label = "new blitz pairing v1";

        std::string pair_url(const std::string& base, const std::string& id, const std::string& tail = "") {
            std::string b = base;
            if (!b.empty() && b.back() != '/') b += '/';
            return b + "v1/pair/" + id + tail;
        }

        // Argon2i of the number with the salt: each offline guess at a sealed pairing costs 8 MiB and three passes. A
        // 6-digit number is still only a million guesses, so this only slows someone who has the sealed blob (the server
        // never hands it to a wrong guess, and keeps it two minutes).
        void pairing_key(const std::string& number, const uint8_t* salt, uint8_t out[32]) {
            constexpr uint32_t blocks = 8192;
            std::vector<uint8_t> work((size_t)blocks * 1024);
            const std::string pass = std::string(pair_label) + ": " + number;
            crypto_argon2_config config{CRYPTO_ARGON2_I, blocks, 3, 1};
            crypto_argon2_inputs inputs{(const uint8_t*)pass.data(), salt, (uint32_t)pass.size(), (uint32_t)pair_salt};
            crypto_argon2(out, 32, work.data(), config, inputs, crypto_argon2_no_extras);
            crypto_wipe(work.data(), work.size());
        }

        std::string reach_message(int status) {
            if (status == 0) return "The sync server can't be reached. Check the connection and try again.";
            if (status == 429) return "Too many tries from this connection. Wait a minute, then try again.";
            if (status == 503) return "This sync server can't pair devices yet. Use the full code instead.";
            return "The sync server answered " + std::to_string(status) + ". Try again in a moment.";
        }
    }

    std::string new_pair_number() {
        // Uniform over 000000-999999: 32 random bits, redrawn above the largest multiple of a million.
        constexpr uint64_t span = 1000000, limit = (1ull << 32) - ((1ull << 32) % span);
        uint64_t v = 0;
        do {
            uint8_t b[4];
            random_bytes(b, 4);
            v = (uint64_t)b[0] | (uint64_t)b[1] << 8 | (uint64_t)b[2] << 16 | (uint64_t)b[3] << 24;
        } while (v >= limit);
        char out[8];
        std::snprintf(out, sizeof out, "%06u", (unsigned)(v % span));
        return out;
    }

    std::string normalize_pair_number(const std::string& typed) {
        std::string out;
        for (char c : typed) {
            if (c >= '0' && c <= '9') out += c;
            else if (c != ' ' && c != '-' && c != '\t' && c != '\n' && c != '\r') return "";
        }
        return out.size() == pair_digits ? out : "";
    }

    std::string group_pair_number(const std::string& number) {
        if (number.size() != pair_digits) return number;
        return number.substr(0, 3) + " " + number.substr(3);
    }

    std::string PairKeys::proof_hex() const { return hex(proof.data(), proof.size()); }

    PairKeys pair_keys(const std::string& number) {
        // Two keyed BLAKE2b of the number with a fixed app label as the key: where the server files it, and what the joiner
        // shows to get it. Anyone can compute both from the number; the point is that the server answers only a lookup
        // that names both, and only a few lookups per connection.
        PairKeys k;
        auto prf = [&](const char* what, uint8_t* out) {
            const std::string msg = std::string(what) + ": " + number;
            crypto_blake2b_keyed(out, 32, (const uint8_t*)pair_label, std::strlen(pair_label), (const uint8_t*)msg.data(), msg.size());
        };
        uint8_t id[32];
        prf("id", id);
        prf("proof", k.proof.data());
        k.id = hex(id, sizeof id);
        return k;
    }

    Bytes seal_pairing(const std::string& number, const std::string& code, int slot) {
        const std::string plain = code + (char)('0' + clamp_slot(slot));
        Bytes out(pair_overhead + plain.size());
        std::memcpy(out.data(), pair_magic, 4);
        random_bytes(out.data() + 4, pair_salt);
        uint8_t* nonce = out.data() + pair_head;
        uint8_t* mac = nonce + 24;
        random_bytes(nonce, 24);
        uint8_t key[32];
        pairing_key(number, out.data() + 4, key);
        const std::string id = pair_keys(number).id;
        Bytes ad(out.data(), out.data() + pair_head);
        ad.insert(ad.end(), id.begin(), id.end());
        crypto_aead_lock(out.data() + pair_overhead, mac, key, nonce, ad.data(), ad.size(), (const uint8_t*)plain.data(), plain.size());
        crypto_wipe(key, sizeof key);
        return out;
    }

    bool open_pairing(const std::string& number, const Bytes& sealed, std::string& code, int& slot) {
        if (sealed.size() != pair_overhead + code_chars + 1 || std::memcmp(sealed.data(), pair_magic, 4) != 0) return false;
        uint8_t key[32];
        pairing_key(number, sealed.data() + 4, key);
        const std::string id = pair_keys(number).id;
        Bytes ad(sealed.data(), sealed.data() + pair_head);
        ad.insert(ad.end(), id.begin(), id.end());
        std::string plain(code_chars + 1, '\0');
        const uint8_t* nonce = sealed.data() + pair_head;
        const bool ok = crypto_aead_unlock((uint8_t*)plain.data(), nonce + 24, key, nonce, ad.data(), ad.size(), sealed.data() + pair_overhead,
                                           plain.size()) == 0;
        crypto_wipe(key, sizeof key);
        if (!ok) return false;
        const std::string c = normalize_code(plain.substr(0, code_chars));
        const int s = plain[code_chars] - '0';
        if (c.empty() || c != plain.substr(0, code_chars) || s < 1 || s > slot_count) return false;
        code = c;
        slot = s;
        return true;
    }

    Offer offer_pairing(const Http& http, const std::string& base, const std::string& code, int slot, const Offer* before) {
        Offer o;
        if (before != nullptr) o.owner = before->owner;
        else random_bytes(o.owner.data(), o.owner.size());
        // A number another device is showing right now (409) is rare (a million numbers, a few live): another one.
        for (int tries = 0; tries < 4; tries++) {
            o.number = new_pair_number();
            if (before != nullptr && o.number == before->number) continue;
            const PairKeys k = pair_keys(o.number);
            o.id = k.id;
            std::string body((const char*)o.owner.data(), o.owner.size());
            body.append((const char*)k.proof.data(), k.proof.size());
            const Bytes sealed = seal_pairing(o.number, code, slot);
            body.append((const char*)sealed.data(), sealed.size());
            std::string url = pair_url(base, k.id);
            if (before != nullptr && before->ok && !before->id.empty()) url += "?replaces=" + before->id;
            http.post(url, body, o.status);
            if (o.status == 409) continue;
            o.ok = o.status == 201 || o.status == 200;
            if (!o.ok) o.message = reach_message(o.status);
            fprintf(stderr, "[cloud] pairing number %s (slot %d)\n", o.ok ? "made" : "not made", slot);
            return o;
        }
        o.message = "The sync server is busy. Try again in a moment.";
        return o;
    }

    bool pairing_live(const Http& http, const std::string& base, const Offer& offer, bool& live) {
        live = false;
        if (!http.get_auth || !offer.ok) return false;
        std::string body;
        int status = 0;
        if (!http.get_auth(pair_url(base, offer.id, "/status"), hex(offer.owner.data(), offer.owner.size()), body, status) || status != 200) return false;
        json j = json::parse(body, nullptr, false);
        if (!j.is_object() || !j.contains("live") || !j["live"].is_boolean()) return false;
        live = j["live"].get<bool>();
        return true;
    }

    void cancel_pairing(const Http& http, const std::string& base, const Offer& offer) {
        if (!offer.ok || offer.id.empty()) return;
        int status = 0;
        http.post(pair_url(base, offer.id, "/cancel"), std::string((const char*)offer.owner.data(), offer.owner.size()), status);
        fprintf(stderr, "[cloud] pairing number cancelled (%d)\n", status);
    }

    Claimed claim_pairing(const Http& http, const std::string& base, const std::string& typed) {
        Claimed c;
        const std::string number = normalize_pair_number(typed);
        if (number.empty()) {
            c.message = "Type the 6-digit number your other device shows.";
            return c;
        }
        if (!http.get_auth) {
            c.message = reach_message(0);
            return c;
        }
        const PairKeys k = pair_keys(number);
        std::string body;
        http.get_auth(pair_url(base, k.id), k.proof_hex(), body, c.status);
        fprintf(stderr, "[cloud] pairing lookup: %d\n", c.status);
        if (c.status == 404) {
            c.message = "That number didn't work. Check it, or choose Add a Device again on your other device for a new one (each "
                        "number lasts 2 minutes and works once).";
            return c;
        }
        if (c.status != 200) {
            c.message = reach_message(c.status);
            return c;
        }
        if (!open_pairing(number, Bytes(body.begin(), body.end()), c.code, c.slot)) {
            c.message = "That number didn't work. Choose Add a Device again on your other device for a new one.";
            return c;
        }
        c.ok = true;
        return c;
    }

    // ---- finishing, joining, turning on, switching ------------------------------------------------------------------
    namespace {
        // The change is kept when it went through: saves came or went, or there was simply nothing to do. Otherwise (a
        // question first, cancelled, offline, failed or busy before anything changed) the snapshot goes back.
        bool went_through(const Outcome& o) {
            switch (o.result) {
                case Result::Pushed:
                case Result::Pulled:
                case Result::UpToDate:
                case Result::Waiting: return true;
                default: return o.pulled > 0;
            }
        }

        // Runs `work` on a copy of the state set up by `setup` (which may keep a code), keeping it only if it went
        // through. The player's cancel is asked once more at the end if nothing asked it on the way.
        Outcome attempt(const Env& env, State& state, const Hooks& hooks, const std::function<bool(State&, Outcome&)>& setup,
                        const std::function<Outcome(State&, const Hooks&)>& work, const Outcome::Pending& pending) {
            const Snapshot snap = snapshot(env.app, hooks.secrets);
            bool passed = false;
            Hooks h = hooks;
            h.commit = [&passed, &hooks] {
                if (passed) return true;
                if (hooks.commit && !hooks.commit()) return false;
                passed = true;
                return true;
            };
            State s = state;
            Outcome o;
            if (!setup(s, o)) {
                restore(env.app, hooks.secrets, snap);
                return o;
            }
            store_state(env.app, s);
            o = work(s, h);
            if (o.result != Result::Ask && went_through(o)) {
                if (passed || go_ahead(h)) {
                    state = s;
                    return o;
                }
                o = cancelled(s);
            }
            restore(env.app, hooks.secrets, snap);
            if (o.result == Result::Ask) o.pending = pending;
            fprintf(stderr, "[cloud] not kept (%s): this device's Cloud Sync is as it was\n", result_name(o.result));
            return o;
        }

        bool keep_code(const Hooks& hooks, const std::string& code, Outcome& o) {
            if (hooks.secrets.put && hooks.secrets.put(code)) return true;
            o.result = Result::Failed;
            o.message = "This device couldn't keep the code safely, so Cloud Sync stays off.";
            return false;
        }
    }

    Outcome finish_pull(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, Outcome& asked) {
        if (asked.pending.slot == 0) {
            const Keys k = derive(state.code, state.slot);
            return apply_pull(env, http, base, state, hooks, k, asked.plan, asked.cloud_fp, asked.server_rev, 0, true);
        }
        // A join's or a switch's questions, answered: now the code and the slot are kept, then the answers applied.
        const Outcome::Pending p = asked.pending;
        return attempt(
            env, state, hooks,
            [&](State& s, Outcome& o) {
                if (!p.code.empty()) {
                    if (!keep_code(hooks, p.code, o)) return false;
                    s = State{};
                    s.on = true;
                    s.code = p.code;
                }
                s.slot = p.slot;
                return true;
            },
            [&](State& s, const Hooks& h) {
                return apply_pull(env, http, base, s, h, derive(s.code, s.slot), asked.plan, asked.cloud_fp, asked.server_rev, 0, true);
            },
            p);
    }

    Outcome join(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, const std::string& typed, int slot) {
        Outcome o;
        const std::string code = normalize_code(typed);
        if (code.empty()) {
            o.result = Result::Failed;
            o.message = "That isn't a sync code: it has 24 letters and numbers.";
            return o;
        }
        int found = slot >= 1 && slot <= slot_count ? slot : 0;
        if (found != 0) {
            // From pairing: the other device's slot, empty or not (the code came from that device, so it's right).
            Head h = head(http, base, derive(code, found));
            if (!h.ok) {
                o.result = Result::Offline;
                o.message = "The sync server can't be reached. Check the connection and try again.";
                return o;
            }
        }
        // A typed code: the first slot with saves in it (Slot 1 usually).
        for (int s = 1; s <= slot_count && found == 0; s++) {
            Head h = head(http, base, derive(code, s));
            if (!h.ok) {
                o.result = Result::Offline;
                o.message = "The sync server can't be reached. Check the connection and try again.";
                return o;
            }
            if (h.rev > 0) found = s;
        }
        if (found == 0) {
            o.result = Result::Failed;
            o.message = "Nothing is saved under that code yet. Check the code, or make a save on the other device first.";
            return o;
        }
        Outcome::Pending p;
        p.code = code;
        p.slot = found;
        o = attempt(
            env, state, hooks,
            [&](State& s, Outcome& out) {
                if (!keep_code(hooks, code, out)) return false;
                s = State{};
                s.on = true;
                s.code = code;
                s.slot = found;
                return true;
            },
            [&](State& s, const Hooks& h) { return sync_once(env, http, base, s, h, true); }, p);
        if (state.on && state.code == code) fprintf(stderr, "[cloud] joined, slot %d\n", found);
        return o;
    }

    Outcome turn_on(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks) {
        const std::string code = new_code();
        Outcome::Pending p;
        p.code = code;
        p.slot = 1;
        return attempt(
            env, state, hooks,
            [&](State& s, Outcome& o) {
                if (!keep_code(hooks, code, o)) {
                    o.message = "This device couldn't keep the code safely.";
                    return false;
                }
                s = State{};
                s.on = true;
                s.code = code;
                s.slot = 1;
                return true;
            },
            [&](State& s, const Hooks& h) { return sync_once(env, http, base, s, h, true); }, p);
    }

    Outcome switch_slot(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, int slot) {
        Outcome::Pending p;
        p.slot = clamp_slot(slot);
        p.switching = true;
        fprintf(stderr, "[cloud] switching from slot %d to slot %d\n", state.slot, p.slot);
        return attempt(
            env, state, hooks,
            [&](State& s, Outcome&) {
                s.slot = p.slot;
                return true;
            },
            [&](State& s, const Hooks& h) { return sync_once(env, http, base, s, h, true, true); }, p);
    }
}
