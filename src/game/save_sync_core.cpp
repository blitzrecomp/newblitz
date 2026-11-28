// Save Sync's bundle: export, checks, decisions and the staged apply (save_sync_core.h, docs/SAVE_SYNC.md).
#include "save_sync_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/stat.h>

#include "../common/sha256.h"
#include "../main/platform_compat.h"
#include "json/json.hpp"
#include "miniz.h"
#include "pak_guard.h"

namespace blitz::savesync {
    namespace {
        using json = nlohmann::json;
        using Bytes = std::vector<uint8_t>;

        constexpr const char* kind_key = "new-blitz-saves";
        constexpr const char* ready_mark = "ready";
        constexpr const char* sync_file = "sync.json";
        constexpr const char* settings_names[] = {"rules.json", "season.json"};
        constexpr size_t seen_cap = 64;

        // adv_save.cpp's season file: magic, version 2, stage, id, gen, seed, schedule, 32 franchises, the record's size,
        // the 0x774-byte record, then the CRC-32 of everything before it.
        constexpr char adv_magic[8] = "BLZADV1";
        constexpr uint32_t adv_version = 2, record_size = 0x774;
        constexpr size_t adv_head = 8 + 4 + 4 + 8 + 4 + 8 + 8 + 32 * 4 + 4;
        constexpr size_t adv_size = adv_head + record_size + 4;

        uint64_t le(const uint8_t* p, int n) {
            uint64_t v = 0;
            for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
            return v;
        }

        uint32_t crc32(const uint8_t* p, size_t n) {
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; i++) {
                c ^= p[i];
                for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            }
            return ~c;
        }

        std::string hex16(uint64_t v) {
            char b[17];
            snprintf(b, sizeof(b), "%016llX", (unsigned long long)v);
            return b;
        }

        std::string str(const fs::path& p) {
            std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }
        fs::path path_of(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

        std::time_t now_of(const Env& env) { return env.now != 0 ? env.now : std::time(nullptr); }

        std::string stamp(std::time_t t, const char* fmt) {
            char b[40];
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            std::strftime(b, sizeof(b), fmt, &tm);
            return b;
        }

        bool is_pak_image(const Bytes& b) { return b.size() == pakimg::pak_size; }
        bool blank(const Bytes& b) { return !is_pak_image(b) || pakimg::notes(b.data()).empty(); }

        // A folder name under saves/ that Save Sync may read or write: "", "original", then "packs/<id>" or
        // "advanced/<id>" (a pack id: letters, digits, . _ -). Anything else (.., absolute, pak_history) is refused.
        bool valid_pack_id(const std::string& id) {
            if (id.empty() || id.size() > 64 || id == "." || id == "..") return false;
            for (char c : id) {
                if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
            }
            return true;
        }

        struct FolderId {
            bool ok = false;
            bool original = false;
            std::string kind;  // "", "packs", "advanced"
            std::string pack;
        };

        FolderId parse_folder(const std::string& folder) {
            FolderId f;
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= folder.size() && !folder.empty()) {
                size_t slash = folder.find('/', start);
                parts.push_back(folder.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            size_t i = 0;
            if (i < parts.size() && parts[i] == "original") {
                f.original = true;
                i++;
            }
            if (i < parts.size()) {
                if ((parts[i] != "packs" && parts[i] != "advanced") || i + 1 >= parts.size() || !valid_pack_id(parts[i + 1])) return f;
                f.kind = parts[i];
                f.pack = parts[i + 1];
                i += 2;
            }
            f.ok = i == parts.size();
            return f;
        }

        std::string folder_label(const FolderId& f) {
            if (f.kind == "advanced") return f.pack == "default" ? "Advanced" : "Advanced (" + f.pack + ")";
            if (f.kind == "packs") return "Standard (" + f.pack + ")";
            return "Original 1999";
        }

        fs::path saves_dir(const Env& env) { return env.app / "saves"; }
        fs::path folder_path(const Env& env, const std::string& folder) {
            return folder.empty() ? saves_dir(env) : saves_dir(env) / path_of(folder);
        }

        // Every pak folder under saves/ (relative names), whether or not it has a pak.
        std::vector<std::string> pak_folders(const Env& env) {
            std::vector<std::string> out;
            std::error_code ec;
            for (std::string base : {"", "original"}) {
                fs::path b = folder_path(env, base);
                if (!fs::is_directory(b, ec)) continue;
                out.push_back(base);
                for (const char* kind : {"packs", "advanced"}) {
                    for (fs::directory_iterator it{b / kind, ec}, end; !ec && it != end; it.increment(ec)) {
                        std::error_code dec;
                        if (!it->is_directory(dec)) continue;
                        std::string id = str(it->path().filename());
                        if (!valid_pack_id(id)) continue;
                        out.push_back((base.empty() ? "" : base + "/") + kind + "/" + id);
                    }
                    ec.clear();
                }
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // Where a pak folder's season files are: one folder per roster pack, saves/advanced/<pack>/season/ (adv_save.cpp
        // season_dir), whichever engine's pak points at them. Modern on the original game keeps its pak in
        // saves/original/advanced/<pack>/ and its season files in saves/advanced/<pack>/season/, so "next to the pak"
        // missed them all (the team's iPhone, 2026-11-27: a pak up without its season).
        fs::path season_of(const fs::path& folder) {
            const fs::path kind = folder.parent_path();
            if (kind.filename() == "advanced" && kind.parent_path().filename() == "original") {
                return kind.parent_path().parent_path() / "advanced" / folder.filename() / "season";
            }
            return folder / "season";
        }

        // The season files in a folder's season/ (only names adv_save.cpp writes), by name.
        std::map<std::string, Bytes> season_files(const fs::path& folder) {
            std::map<std::string, Bytes> out;
            std::error_code ec;
            for (fs::directory_iterator it{season_of(folder), ec}, end; !ec && it != end; it.increment(ec)) {
                std::string n = str(it->path().filename());
                std::error_code fec;
                if (!it->is_regular_file(fec) || it->path().extension() != ".adv") continue;
                bool ok = false;
                Bytes b = read_file(it->path(), &ok);
                AdvInfo a = adv_info(b);
                if (!ok || !a.ok || adv_name(a.id, a.gen) != n) {
                    fprintf(stderr, "[sync] %s: not a season file this version reads; left out\n", str(it->path()).c_str());
                    continue;
                }
                out[n] = std::move(b);
            }
            return out;
        }

        // The pak images a folder keeps besides the pak: .bak, .bak2 and the dated copies in pak_history/.
        std::vector<Bytes> kept_images(const fs::path& folder) {
            std::vector<Bytes> out;
            std::error_code ec;
            for (const char* ext : {".bak", ".bak2"}) {
                bool ok = false;
                Bytes b = read_file(folder / (std::string(pak_name) + ext), &ok);
                if (ok && is_pak_image(b)) out.push_back(std::move(b));
            }
            for (fs::directory_iterator it{folder / "pak_history", ec}, end; !ec && it != end; it.increment(ec)) {
                if (it->path().extension() != ".mpk") continue;
                bool ok = false;
                Bytes b = read_file(it->path(), &ok);
                if (ok && is_pak_image(b)) out.push_back(std::move(b));
            }
            return out;
        }

        // Season states (id, gen, CRC) a folder has had: its season files, the stubs in its pak and kept copies, and
        // season files an import replaced (pak_history/season-*-replaced-*.adv).
        std::set<SeasonRef> lineage(const fs::path& folder, const Bytes& pak) {
            std::set<SeasonRef> out;
            for (const auto& [n, b] : season_files(folder)) {
                AdvInfo a = adv_info(b);
                out.insert({a.id, a.gen, a.crc});
            }
            auto stubs_of = [&](const Bytes& img) {
                if (!is_pak_image(img)) return;
                for (const pakimg::Stub& s : pakimg::stubs(img.data())) out.insert({s.id, s.gen, s.crc});
            };
            stubs_of(pak);
            for (const Bytes& img : kept_images(folder)) stubs_of(img);
            std::error_code ec;
            for (fs::directory_iterator it{folder / "pak_history", ec}, end; !ec && it != end; it.increment(ec)) {
                if (it->path().extension() != ".adv") continue;
                AdvInfo a = adv_info(read_file(it->path()));
                if (a.ok) out.insert({a.id, a.gen, a.crc});
            }
            return out;
        }

        // saves/sync.json: this device's label and, per folder, the pak states (sha256) it has exported or imported.
        json load_sync(const Env& env) {
            bool ok = false;
            Bytes b = read_file(saves_dir(env) / sync_file, &ok);
            if (!ok) return json::object();
            json j = json::parse((const char*)b.data(), (const char*)b.data() + b.size(), nullptr, false);
            return j.is_object() ? j : json::object();
        }

        void store_sync(const Env& env, const json& j) {
            std::string s = j.dump(1, '\t');
            std::error_code ec;
            fs::create_directories(saves_dir(env), ec);
            if (!write_file(saves_dir(env) / sync_file, Bytes(s.begin(), s.end())))
                fprintf(stderr, "[sync] couldn't write saves/%s\n", sync_file);
        }

        std::vector<std::string> seen_of(const json& sync, const std::string& folder) {
            std::vector<std::string> out;
            if (!sync.contains("folders") || !sync["folders"].is_object()) return out;
            const json& f = sync["folders"];
            auto it = f.find(folder.empty() ? "." : folder);
            if (it == f.end() || !it->is_object() || !it->contains("seen") || !(*it)["seen"].is_array()) return out;
            for (const json& h : (*it)["seen"]) {
                if (h.is_string()) out.push_back(h.get<std::string>());
            }
            return out;
        }

        void add_seen(json& sync, const std::string& folder, const std::vector<std::string>& hashes) {
            std::vector<std::string> seen = seen_of(sync, folder);
            for (const std::string& h : hashes) {
                seen.erase(std::remove(seen.begin(), seen.end(), h), seen.end());
                seen.push_back(h);  // newest last
            }
            if (seen.size() > seen_cap) seen.erase(seen.begin(), seen.end() - seen_cap);
            if (!sync.contains("folders") || !sync["folders"].is_object()) sync["folders"] = json::object();
            sync["folders"][folder.empty() ? "." : folder]["seen"] = seen;
        }

        std::string describe_with(const Bytes& img, const std::function<const Bytes*(uint64_t, uint32_t)>& adv_of) {
            if (!is_pak_image(img)) return "No pak";
            return pakimg::describe(img.data(), [&](uint64_t id, uint32_t gen, uint8_t team) -> std::string {
                const Bytes* b = adv_of(id, gen);
                if (b == nullptr || team >= 32) return "";
                AdvInfo a = adv_info(*b);
                if (!a.ok) return "";
                const char* name = (const char*)b->data() + 44 + 4 * team;
                return std::string(name, strnlen(name, 4));
            });
        }

        std::string describe_local(const fs::path& folder, const Bytes& img) {
            std::map<std::string, Bytes> files = season_files(folder);
            return describe_with(img, [&](uint64_t id, uint32_t gen) -> const Bytes* {
                auto it = files.find(adv_name(id, gen));
                return it == files.end() ? nullptr : &it->second;
            });
        }

        // Each season's (gen, crc) a pak's stubs point at, the newest if one season is in two slots.
        std::map<uint64_t, std::pair<uint32_t, uint32_t>> pak_seasons(const Bytes& img) {
            std::map<uint64_t, std::pair<uint32_t, uint32_t>> out;
            if (!is_pak_image(img)) return out;
            for (const pakimg::Stub& s : pakimg::stubs(img.data())) {
                auto it = out.find(s.id);
                if (it == out.end() || s.gen > it->second.first) out[s.id] = {s.gen, s.crc};
            }
            return out;
        }

        bool has_ref(const std::set<SeasonRef>& refs, uint64_t id, uint32_t gen, uint32_t crc) { return refs.count({id, gen, crc}) != 0; }

        // The rules of docs/SAVE_SYNC.md §3 for one folder. Season generations decide Advanced; for what they don't
        // decide (Classic and Modern paks, an Advanced pak whose seasons agree), the pak states each side has synced.
        Verdict decide(const Bytes& mine, const Bytes& theirs, const std::set<SeasonRef>& my_lineage,
                       const std::set<SeasonRef>& their_lineage, const std::set<std::string>& my_known,
                       const std::set<std::string>& their_seen, std::string& why) {
            if (blank(mine)) {
                why = "nothing here";
                return blank(theirs) && mine == theirs ? Verdict::Same : Verdict::New;
            }
            if (mine == theirs) {
                why = "the same pak";
                return Verdict::Same;
            }
            auto m = pak_seasons(mine), t = pak_seasons(theirs);
            bool newer = false, older = false, conflict = false;
            std::set<uint64_t> ids;
            for (const auto& [id, v] : m) ids.insert(id);
            for (const auto& [id, v] : t) ids.insert(id);
            for (uint64_t id : ids) {
                auto mi = m.find(id), ti = t.find(id);
                if (mi != m.end() && ti != t.end()) {
                    auto [mg, mc] = mi->second;
                    auto [tg, tc] = ti->second;
                    if (mg == tg && mc == tc) continue;
                    if (tg == mg) conflict = true;                                    // same gen, other CRC
                    else if (tg > mg) (has_ref(their_lineage, id, mg, mc) ? newer : conflict) = true;
                    else (has_ref(my_lineage, id, tg, tc) ? older : conflict) = true;
                }
                else if (ti != t.end()) {
                    // A season only theirs has: one this device had and dropped, or a new one.
                    (has_ref(my_lineage, id, ti->second.first, ti->second.second) ? older : newer) = true;
                }
                else {
                    // A season only this device has: theirs had it and moved on, or never saw it.
                    (has_ref(their_lineage, id, mi->second.first, mi->second.second) ? newer : conflict) = true;
                }
            }
            const std::string mine_hash = sha256(mine), their_hash = sha256(theirs);
            const bool img_older = my_known.count(their_hash) != 0;   // this device had their pak before
            const bool img_newer = their_seen.count(mine_hash) != 0;  // their device had this pak before
            if (conflict || (newer && older)) {
                why = "seasons both changed";
                return Verdict::Conflict;
            }
            if (newer) {
                why = "their seasons carry on from these";
                return img_older && !img_newer ? Verdict::Conflict : Verdict::Newer;
            }
            if (older) {
                why = "these seasons carry on from theirs";
                return img_newer && !img_older ? Verdict::Conflict : Verdict::Older;
            }
            if (img_newer && !img_older) {
                why = "their device had this pak and saved since";
                return Verdict::Newer;
            }
            if (img_older && !img_newer) {
                why = "this device had their pak and saved since";
                return Verdict::Older;
            }
            why = "both changed";
            return Verdict::Conflict;
        }

        // A league settings file as this device will have it: one an import staged for the next start (sync_incoming/
        // settings/, with its ready mark) over the one in use now. So a bundle made before that start carries the
        // settings this device took, not the ones they replace, and the same settings aren't offered back to it.
        Bytes settings_file(const fs::path& app, const std::string& name, bool* ok) {
            std::error_code ec;
            const fs::path st = app / staging / "settings";
            if (fs::is_regular_file(st / ready_mark, ec)) {
                bool staged = false;
                Bytes b = read_file(st / name, &staged);
                if (staged) {
                    if (ok) *ok = true;
                    return b;
                }
            }
            return read_file(app / name, ok);
        }

        // When a file was last written, "2026-11-27 14:03" in local time; "" when it can't be read.
        std::string saved_at(const fs::path& p) {
            struct stat sb;
            if (::stat(str(p).c_str(), &sb) != 0) return "";
            return stamp((std::time_t)sb.st_mtime, "%Y-%m-%d %H:%M");
        }

        bool zip_add(mz_zip_archive& zip, const std::string& name, const Bytes& data) {
            return mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
        }

        // A plain, safe entry name: relative, '/' separated, no "..", no empty parts.
        bool safe_entry(const std::string& n) {
            if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
            size_t start = 0;
            while (true) {
                size_t slash = n.find('/', start);
                std::string part = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (part.empty() || part == "." || part == "..") return false;
                if (slash == std::string::npos) return true;
                start = slash + 1;
            }
        }

        std::string history_copy_name(const fs::path& dir, const std::string& stem, std::time_t when, const std::string& ext) {
            std::string base = pakimg::history_name(stem, when);  // "<stem>-YYYYMMDD-HHMMSS.mpk"
            base = base.substr(0, base.size() - 4);
            std::string name = base + ext;
            std::error_code ec;
            for (int n = 2; fs::exists(dir / name, ec) && n < 100; n++) name = base + "-" + std::to_string(n) + ext;
            return name;
        }

        // A season file about to be replaced: kept in pak_history/ (once per content), where Controller Pak Backups
        // finds it again (restore_replaced_seasons).
        bool keep_replaced(const fs::path& folder, const std::string& name, const Bytes& old) {
            fs::path dir = folder / "pak_history";
            std::error_code ec;
            std::string stem = name.substr(0, name.size() - 4) + "-replaced";
            for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
                std::string n = str(it->path().filename());
                if (n.rfind(stem, 0) == 0 && read_file(it->path()) == old) return true;
            }
            ec.clear();
            fs::create_directories(dir, ec);
            return write_file(dir / history_copy_name(dir, stem, std::time(nullptr), ".adv"), old);
        }
    }

    // ---- small helpers ---------------------------------------------------------------------------------------------
    std::vector<uint8_t> read_file(const fs::path& p, bool* ok) {
        Bytes out;
        FILE* f = fopen(str(p).c_str(), "rb");
        if (ok) *ok = false;
        if (f == nullptr) return out;
        uint8_t buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        bool good = ferror(f) == 0;
        fclose(f);
        if (ok) *ok = good;
        return good ? out : Bytes{};
    }

    bool write_file(const fs::path& p, const std::vector<uint8_t>& data) {
        std::error_code ec;
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        fs::path tmp = p;
        tmp += ".tmp";
        FILE* f = fopen(str(tmp).c_str(), "wb");
        if (f == nullptr) return false;
        bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            blitz::replace_file(tmp, p, ec);
            ok = !ec;
        }
        if (!ok) fs::remove(tmp, ec);
        return ok;
    }

    std::string sha256(const std::vector<uint8_t>& data) { return blitzsha::sha256_hex(data.data(), data.size()); }

    std::string adv_name(uint64_t id, uint32_t gen) { return "season-" + hex16(id) + "-" + std::to_string(gen) + ".adv"; }

    AdvInfo adv_info(const std::vector<uint8_t>& d) {
        AdvInfo a;
        if (d.size() < 12 || std::memcmp(d.data(), adv_magic, 8) != 0) return a;
        a.version = (uint32_t)le(d.data() + 8, 4);
        if (a.version != adv_version || d.size() != adv_size) return a;
        uint32_t crc = crc32(d.data(), d.size() - 4);
        if (crc != (uint32_t)le(d.data() + d.size() - 4, 4) || le(d.data() + adv_head - 4, 4) != record_size) return a;
        a.id = le(d.data() + 16, 8);
        a.gen = (uint32_t)le(d.data() + 24, 4);
        a.crc = crc;
        a.team = d[adv_head];
        a.week = d[adv_head + 1];
        if (a.team < 32) {
            const char* name = (const char*)d.data() + 44 + 4 * a.team;
            a.team_name = std::string(name, strnlen(name, 4));
        }
        a.ok = true;
        return a;
    }

    const char* verdict_name(Verdict v) {
        switch (v) {
            case Verdict::Same: return "same";
            case Verdict::New: return "new";
            case Verdict::Newer: return "newer";
            case Verdict::Older: return "older";
            case Verdict::Conflict: return "conflict";
        }
        return "?";
    }

    bool pair_ok(const fs::path& folder, std::string* why) {
        bool ok = false;
        Bytes pak = read_file(folder / pak_name, &ok);
        if (!ok || !is_pak_image(pak)) return true;  // no pak: nothing to point anywhere
        for (const pakimg::Stub& s : pakimg::stubs(pak.data())) {
            AdvInfo a = adv_info(read_file(season_of(folder) / adv_name(s.id, s.gen)));
            if (!a.ok || a.id != s.id || a.gen != s.gen || a.crc != s.crc) {
                if (why) *why = "season " + hex16(s.id) + " gen " + std::to_string(s.gen) + (a.ok ? ": its file's CRC differs" : ": no file");
                return false;
            }
        }
        return true;
    }

    std::string device_label(const Env& env) {
        json sync = load_sync(env);
        if (sync.contains("device") && sync["device"].is_string() && !sync["device"].get<std::string>().empty())
            return sync["device"].get<std::string>();
        static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::random_device rd;
        std::string tag;
        for (int i = 0; i < 4; i++) tag += letters[rd() % (sizeof(letters) - 1)];
        std::string label = env.platform + " " + tag;
        sync["device"] = label;
        store_sync(env, sync);
        return label;
    }

    std::string bundle_file_name(std::time_t when) {
        // BRANDING: the saves file's name
        return "New Blitz Saves " + stamp(when, "%Y-%m-%d %H.%M.%S") + extension;
    }

    // ---- export ----------------------------------------------------------------------------------------------------
    ExportResult make_bundle(const Env& env) {
        ExportResult r;
        const std::string device = device_label(env);
        json sync = load_sync(env);
        json manifest = {{"kind", kind_key},
                         {"format", format_version},
                         {"version", env.version},
                         {"build", env.build},
                         {"platform", env.platform},
                         {"device", device},
                         {"made", stamp(now_of(env), "%Y-%m-%d %H:%M")}};
        json files = json::array(), folders = json::array();
        std::map<std::string, Bytes> entries;
        auto add = [&](const std::string& name, Bytes data) {
            files.push_back({{"path", name}, {"sha256", sha256(data)}, {"size", data.size()}});
            entries[name] = std::move(data);
        };
        std::map<std::string, std::string> exported;  // folder -> pak hash
        for (const std::string& folder : pak_folders(env)) {
            fs::path dir = folder_path(env, folder);
            bool ok = false;
            Bytes pak = read_file(dir / pak_name, &ok);
            if (!ok || !is_pak_image(pak)) continue;
            std::map<std::string, Bytes> advs = season_files(dir);
            if (blank(pak) && advs.empty()) continue;  // a blank pak would only wipe the other side's
            FolderId id = parse_folder(folder);
            std::string prefix = "saves/" + (folder.empty() ? "" : folder + "/");
            json f = {{"folder", folder},
                      {"engine", id.original ? "original" : "standard"},
                      {"mode", id.kind == "advanced" ? "advanced" : id.kind == "packs" ? "pack" : "base"},
                      {"pack", id.pack},
                      {"pak", prefix + pak_name},
                      {"what", describe_local(dir, pak)},
                      {"saved", saved_at(dir / pak_name)}};
            json seasons = json::array();
            std::set<std::pair<uint64_t, uint32_t>> in_pak = pakimg::stub_refs(pak.data());
            for (auto& [n, b] : advs) {
                AdvInfo a = adv_info(b);
                char crc[16];
                snprintf(crc, sizeof(crc), "%08X", a.crc);
                seasons.push_back({{"file", prefix + "season/" + n},
                                   {"id", hex16(a.id)},
                                   {"gen", a.gen},
                                   {"crc", crc},
                                   {"team", a.team_name},
                                   {"week", a.week},
                                   {"in_pak", in_pak.count({a.id, a.gen}) != 0}});
                add(prefix + "season/" + n, std::move(b));
                r.seasons++;
            }
            f["seasons"] = seasons;
            json lin = json::array();
            for (const auto& [sid, gen, crc] : lineage(dir, pak)) lin.push_back({hex16(sid), gen, crc});
            f["lineage"] = lin;
            std::string hash = sha256(pak);
            std::vector<std::string> seen = seen_of(sync, folder);
            if (std::find(seen.begin(), seen.end(), hash) == seen.end()) seen.push_back(hash);
            f["seen"] = seen;
            add(prefix + pak_name, std::move(pak));
            folders.push_back(f);
            exported[folder] = hash;
            r.paks++;
        }
        if (r.paks == 0) {
            r.why = "There are no saves to export yet.";
            return r;
        }
        json settings = json::array();
        for (const char* n : settings_names) {
            bool ok = false;
            Bytes b = settings_file(env.app, n, &ok);
            if (!ok || b.empty()) continue;
            settings.push_back(std::string("settings/") + n);
            add(std::string("settings/") + n, std::move(b));
        }
        manifest["folders"] = folders;
        manifest["settings"] = settings;
        manifest["files"] = files;
        std::string m = manifest.dump(1, '\t');
        // BRANDING: the note inside a saves file
        std::string about = "New Blitz saves from " + device + ", " + manifest["made"].get<std::string>() +
                            ".\nControllerPak saves and Advanced seasons. No save states, no ROM.\n"
                            "On the other device: Settings > General > Saves > Import Save File.\n";
        mz_zip_archive zip{};
        if (!mz_zip_writer_init_heap(&zip, 0, 1 << 16)) {
            r.why = "Couldn't make the saves file.";
            return r;
        }
        bool ok = zip_add(zip, "manifest.json", Bytes(m.begin(), m.end())) && zip_add(zip, "README.txt", Bytes(about.begin(), about.end()));
        r.entries = {"manifest.json", "README.txt"};
        for (const auto& [name, data] : entries) {
            ok = ok && zip_add(zip, name, data);
            r.entries.push_back(name);
        }
        void* buf = nullptr;
        size_t size = 0;
        ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
        mz_zip_writer_end(&zip);
        if (ok) r.zip.assign((uint8_t*)buf, (uint8_t*)buf + size);
        if (buf) mz_free(buf);
        if (!ok) {
            r.why = "Couldn't make the saves file.";
            return r;
        }
        for (const auto& [folder, hash] : exported) add_seen(sync, folder, {hash});
        store_sync(env, sync);
        r.ok = true;
        fprintf(stderr, "[sync] export: %d pak(s), %d season file(s), %zu bytes\n", r.paks, r.seasons, r.zip.size());
        return r;
    }

    // ---- import ----------------------------------------------------------------------------------------------------
    Plan read_bundle(const Env& env, const std::vector<uint8_t>& data) {
        Plan p;
        auto refuse = [&](const std::string& why, const std::string& log) {
            p = Plan{};
            p.why = why;
            fprintf(stderr, "[sync] import refused: %s\n", log.c_str());
            return p;
        };
        const std::string not_ours = "This isn't a New Blitz saves file.";
        mz_zip_archive zip{};
        if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return refuse(not_ours, "not a zip");
        std::map<std::string, Bytes> entries;
        bool unsafe = false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
            std::string name = st.m_filename;
            if (!safe_entry(name) || st.m_uncomp_size > (4u << 20)) {
                unsafe = true;
                break;
            }
            size_t size = 0;
            void* b = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);  // checks each entry's CRC-32
            if (b == nullptr) {
                unsafe = true;
                break;
            }
            entries[name] = Bytes((uint8_t*)b, (uint8_t*)b + size);
            mz_free(b);
        }
        mz_zip_reader_end(&zip);
        if (unsafe) return refuse("This saves file is damaged.", "an entry couldn't be read or has an unsafe name");
        auto mit = entries.find("manifest.json");
        if (mit == entries.end()) return refuse(not_ours, "no manifest");
        json m = json::parse((const char*)mit->second.data(), (const char*)mit->second.data() + mit->second.size(), nullptr, false);
        if (!m.is_object() || m.value("kind", "") != kind_key || !m.contains("format") || !m["format"].is_number_integer())
            return refuse(not_ours, "not our manifest");
        if (m["format"].get<int>() > format_version)
            return refuse("This saves file was made by a newer New Blitz. Update this one, then import it again.",
                          "format " + std::to_string(m["format"].get<int>()));
        // Every file listed, with its hash; nothing else but the manifest and the note.
        std::map<std::string, std::string> listed;
        if (m.contains("files") && m["files"].is_array()) {
            for (const json& f : m["files"]) {
                if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("sha256") || !f["sha256"].is_string())
                    return refuse("This saves file is damaged.", "a bad files entry");
                listed[f["path"].get<std::string>()] = f["sha256"].get<std::string>();
            }
        }
        for (const auto& [name, b] : entries) {
            if (name == "manifest.json" || name == "README.txt") continue;
            auto it = listed.find(name);
            if (it == listed.end()) return refuse("This saves file is damaged.", name + " isn't in the manifest");
            if (it->second != sha256(b)) return refuse("This saves file is damaged.", name + ": its hash differs");
        }
        for (const auto& [name, h] : listed) {
            if (!entries.count(name)) return refuse("This saves file is damaged.", name + " is missing");
        }
        p.device = m.value("device", "another device");
        p.made = m.value("made", "");
        p.version = m.value("version", "");
        json sync = load_sync(env);
        // Settings: only the two league files, each a JSON object.
        if (m.contains("settings") && m["settings"].is_array()) {
            for (const json& s : m["settings"]) {
                std::string path = s.is_string() ? s.get<std::string>() : "";
                std::string name = path.rfind("settings/", 0) == 0 ? path.substr(9) : "";
                if (name != "rules.json" && name != "season.json") return refuse("This saves file is damaged.", "settings entry " + path);
                auto it = entries.find(path);
                if (it == entries.end()) return refuse("This saves file is damaged.", path + " is missing");
                json j = json::parse((const char*)it->second.data(), (const char*)it->second.data() + it->second.size(), nullptr, false);
                if (!j.is_object()) return refuse("This saves file is damaged.", path + " isn't a settings file");
                if (settings_file(env.app, name, nullptr) != it->second) p.settings[name] = std::string(it->second.begin(), it->second.end());
            }
        }
        if (!m.contains("folders") || !m["folders"].is_array() || m["folders"].empty()) return refuse("There are no saves in this file.", "no folders");
        std::set<std::string> done;
        for (const json& f : m["folders"]) {
            if (!f.is_object() || !f.contains("folder") || !f["folder"].is_string()) return refuse("This saves file is damaged.", "a bad folder entry");
            FolderPlan fp;
            fp.folder = f["folder"].get<std::string>();
            FolderId id = parse_folder(fp.folder);
            if (!id.ok || done.count(fp.folder)) return refuse("This saves file is damaged.", "folder '" + fp.folder + "'");
            done.insert(fp.folder);
            fp.label = folder_label(id);
            fp.pack = id.pack;
            if (!id.pack.empty() && id.pack != "default" && !(env.pack_installed && env.pack_installed(id.pack))) {
                return refuse("These saves are for the roster pack \"" + id.pack +
                                  "\", which isn't on this device. Add that pack first, then import again.",
                              "pack " + id.pack + " isn't installed");
            }
            const std::string prefix = "saves/" + (fp.folder.empty() ? "" : fp.folder + "/");
            auto pit = entries.find(prefix + pak_name);
            if (pit == entries.end() || !is_pak_image(pit->second))
                return refuse("This saves file is damaged.", fp.folder + ": no 32 KB pak");
            fp.pak = pit->second;
            // Its season files: each one whole (magic, CRC), named for what's inside.
            for (const auto& [name, b] : entries) {
                if (name.rfind(prefix + "season/", 0) != 0) continue;
                std::string file = name.substr(prefix.size() + 7);
                if (file.find('/') != std::string::npos) continue;  // another folder's (deeper path)
                AdvInfo a = adv_info(b);
                if (!a.ok || adv_name(a.id, a.gen) != file) return refuse("This saves file is damaged.", name + " isn't a good season file");
                fp.adv[file] = b;
            }
            // Every season the pak points at has its file, in the bundle or already here.
            const fs::path dir = folder_path(env, fp.folder);
            for (const pakimg::Stub& s : pakimg::stubs(fp.pak.data())) {
                std::string file = adv_name(s.id, s.gen);
                auto ait = fp.adv.find(file);
                AdvInfo a = ait != fp.adv.end() ? adv_info(ait->second) : adv_info(read_file(season_of(dir) / file));
                if (!a.ok || a.crc != s.crc)
                    return refuse("A season in this file is missing its season data, so nothing was imported. Export it again on the other device.",
                                  fp.folder + ": " + file + (a.ok ? " CRC differs from its stub" : " missing"));
            }
            // Theirs: the season states and pak states their device had.
            std::set<SeasonRef> their_lineage;
            for (const auto& [n, b] : fp.adv) {
                AdvInfo a = adv_info(b);
                their_lineage.insert({a.id, a.gen, a.crc});
            }
            for (const pakimg::Stub& s : pakimg::stubs(fp.pak.data())) their_lineage.insert({s.id, s.gen, s.crc});
            if (f.contains("lineage") && f["lineage"].is_array()) {
                for (const json& l : f["lineage"]) {
                    if (!l.is_array() || l.size() != 3 || !l[0].is_string() || !l[1].is_number_unsigned() || !l[2].is_number_unsigned()) continue;
                    their_lineage.insert({std::strtoull(l[0].get<std::string>().c_str(), nullptr, 16), l[1].get<uint32_t>(), l[2].get<uint32_t>()});
                }
            }
            std::set<std::string> their_seen;
            if (f.contains("seen") && f["seen"].is_array()) {
                for (const json& h : f["seen"]) {
                    if (h.is_string()) {
                        their_seen.insert(h.get<std::string>());
                        fp.seen.push_back(h.get<std::string>());
                    }
                }
            }
            // Mine.
            bool ok = false;
            Bytes mine = read_file(dir / pak_name, &ok);
            if (!ok || !is_pak_image(mine)) mine.clear();
            std::set<SeasonRef> my_lineage = lineage(dir, mine);
            std::set<std::string> my_known;
            for (const std::string& h : seen_of(sync, fp.folder)) my_known.insert(h);
            for (const Bytes& img : kept_images(dir)) my_known.insert(sha256(img));
            if (!mine.empty()) my_known.erase(sha256(mine));  // "had before" means before now
            fp.verdict = decide(mine, fp.pak, my_lineage, their_lineage, my_known, their_seen, fp.why);
            fp.take = fp.verdict == Verdict::New || fp.verdict == Verdict::Newer;
            fp.mine = mine.empty() ? "No saves" : describe_local(dir, mine);
            fp.mine_when = mine.empty() ? "" : saved_at(dir / pak_name);
            fp.theirs_when = f.value("saved", p.made);
            fp.theirs = describe_with(fp.pak, [&](uint64_t sid, uint32_t gen) -> const Bytes* {
                auto it = fp.adv.find(adv_name(sid, gen));
                return it == fp.adv.end() ? nullptr : &it->second;
            });
            fprintf(stderr, "[sync] import: %s: %s (%s); theirs %s, mine %s\n", fp.folder.empty() ? "saves" : fp.folder.c_str(),
                    verdict_name(fp.verdict), fp.why.c_str(), fp.theirs.c_str(), fp.mine.c_str());
            p.folders.push_back(std::move(fp));
        }
        p.ok = true;
        return p;
    }

    std::vector<fs::path> staged_folders(const Env& env, const Plan& plan) {
        std::vector<fs::path> out;
        for (const FolderPlan& f : plan.folders) {
            if (f.take) out.push_back(folder_path(env, f.folder));
        }
        return out;
    }

    bool stage(const Env& env, const Plan& plan, bool take_settings, std::string& why) {
        std::error_code ec;
        std::vector<fs::path> written;
        auto fail = [&](const std::string& w) {
            for (const fs::path& d : written) fs::remove_all(d, ec);
            why = w;
            fprintf(stderr, "[sync] stage failed: %s\n", w.c_str());
            return false;
        };
        for (const FolderPlan& f : plan.folders) {
            if (!f.take) continue;
            fs::path st = folder_path(env, f.folder) / staging;
            fs::remove_all(st, ec);
            written.push_back(st);
            bool ok = true;
            for (const auto& [name, b] : f.adv) ok = ok && write_file(st / "season" / name, b);
            ok = ok && write_file(st / pak_name, f.pak);
            ok = ok && write_file(st / ready_mark, Bytes{'1', '\n'});  // last: the staging is whole
            if (!ok) return fail("Couldn't write in the saves folder.");
        }
        if (take_settings && !plan.settings.empty()) {
            fs::path st = env.app / staging / "settings";
            fs::remove_all(st, ec);
            written.push_back(st);
            bool ok = true;
            for (const auto& [name, s] : plan.settings) ok = ok && write_file(st / name, Bytes(s.begin(), s.end()));
            ok = ok && write_file(st / ready_mark, Bytes{'1', '\n'});
            if (!ok) return fail("Couldn't write in New Blitz's folder.");
        }
        return true;
    }

    bool has_staged(const fs::path& folder) {
        std::error_code ec;
        return fs::exists(folder / staging, ec);
    }

    Applied apply_staged(const fs::path& folder, const PakWriter& write_pak, std::string* log) {
        std::error_code ec;
        const fs::path st = folder / staging;
        auto note = [&](const std::string& s) {
            fprintf(stderr, "[sync] %s: %s\n", str(folder).c_str(), s.c_str());
            if (log) *log += s + "\n";
        };
        if (!fs::exists(st, ec)) return Applied::Nothing;
        if (!fs::is_regular_file(st / ready_mark, ec)) {
            // Cut off while staging: nothing of it was put in place yet.
            fs::remove_all(st, ec);
            note("an unfinished import was dropped (nothing had changed)");
            return Applied::Nothing;
        }
        bool ok = false;
        Bytes pak = read_file(st / pak_name, &ok);
        std::map<std::string, Bytes> advs;
        for (fs::directory_iterator it{st / "season", ec}, end; !ec && it != end; it.increment(ec)) {
            Bytes b = read_file(it->path());
            AdvInfo a = adv_info(b);
            std::string name = str(it->path().filename());
            if (!a.ok || adv_name(a.id, a.gen) != name) {
                ok = false;
                break;
            }
            advs[name] = std::move(b);
        }
        if (!ok || !is_pak_image(pak)) {
            fs::remove_all(st, ec);
            note("the staged import couldn't be read; nothing changed");
            return Applied::Failed;
        }
        // Season files first: a crash from here on finishes at the next start (the staging stays until the pak is in).
        int placed = 0, kept = 0;
        for (const auto& [name, b] : advs) {
            fs::path target = season_of(folder) / name;
            bool had = false;
            Bytes old = read_file(target, &had);
            if (had && old == b) continue;
            if (had) {
                if (!keep_replaced(folder, name, old)) {
                    note("couldn't keep " + name + " before replacing it; stopped (nothing else changed)");
                    return Applied::Failed;
                }
                kept++;
            }
            if (!write_file(target, b)) {
                note("couldn't write " + name + "; stopped, tried again at the next start");
                return Applied::Failed;
            }
            placed++;
        }
        // Then the pak.
        bool have = false;
        Bytes now = read_file(folder / pak_name, &have);
        if (!(have && now == pak)) {
            if (!write_pak(folder / pak_name, pak)) {
                note("the pak couldn't be written; tried again at the next start");
                return Applied::Failed;
            }
            now = read_file(folder / pak_name, &have);
            if (!(have && now == pak)) {
                note("the pak isn't the imported one after writing it; tried again at the next start");
                return Applied::Failed;
            }
        }
        fs::remove(st / ready_mark, ec);
        fs::remove_all(st, ec);
        note("import put in place: " + std::to_string(placed) + " season file(s)" +
             (kept ? " (" + std::to_string(kept) + " replaced one(s) kept in pak_history)" : "") + ", then the pak (" +
             pakimg::describe_plain(pak.data()) + ")");
        return Applied::Done;
    }

    bool write_pak_with_history(const fs::path& pak_path, const std::vector<uint8_t>& image) {
        bool ok = false;
        Bytes current = read_file(pak_path, &ok);
        if (ok && is_pak_image(current) && current != image) {
            fs::path dir = pak_path.parent_path() / "pak_history";
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (!write_file(dir / history_copy_name(dir, pak_path.stem().string(), std::time(nullptr), ".mpk"), current)) return false;
        }
        return write_file(pak_path, image);
    }

    int restore_replaced_seasons(const fs::path& folder, const std::vector<uint8_t>& image) {
        if (!is_pak_image(image)) return 0;
        int put_back = 0;
        std::error_code ec;
        for (const pakimg::Stub& s : pakimg::stubs(image.data())) {
            std::string name = adv_name(s.id, s.gen);
            fs::path target = season_of(folder) / name;
            bool had = false;
            Bytes cur = read_file(target, &had);
            AdvInfo a = adv_info(cur);
            if (had && a.ok && a.crc == s.crc) continue;
            std::string stem = name.substr(0, name.size() - 4) + "-replaced";
            for (fs::directory_iterator it{folder / "pak_history", ec}, end; !ec && it != end; it.increment(ec)) {
                if (str(it->path().filename()).rfind(stem, 0) != 0) continue;
                Bytes b = read_file(it->path());
                AdvInfo r = adv_info(b);
                if (!r.ok || r.id != s.id || r.gen != s.gen || r.crc != s.crc) continue;
                if (had && !keep_replaced(folder, name, cur)) break;
                if (write_file(target, b)) {
                    put_back++;
                    fprintf(stderr, "[sync] put back %s (an import had replaced it)\n", name.c_str());
                }
                break;
            }
            ec.clear();
        }
        return put_back;
    }

    int apply_staged_settings(const fs::path& app) {
        std::error_code ec;
        const fs::path st = app / staging / "settings";
        if (!fs::exists(st, ec)) return 0;
        int n = 0;
        if (fs::is_regular_file(st / ready_mark, ec)) {
            for (const char* name : settings_names) {
                bool ok = false;
                Bytes b = read_file(st / name, &ok);
                if (!ok) continue;
                bool had = false;
                Bytes old = read_file(app / name, &had);
                if (had && old == b) continue;
                if (had) write_file(app / (std::string(name) + ".before-sync"), old);
                if (write_file(app / name, b)) {
                    n++;
                    fprintf(stderr, "[sync] %s taken from the imported saves%s\n", name, had ? " (the old one kept as .before-sync)" : "");
                }
            }
        }
        fs::remove_all(st, ec);
        fs::remove(app / staging, ec);  // only when empty
        return n;
    }

    namespace {
        // sha256 over "<path>\t<sha256>\n" lines, sorted by path; "" for none.
        std::string fingerprint_of(std::map<std::string, std::string> files) {
            if (files.empty()) return "";
            std::string all;
            for (const auto& [path, hash] : files) all += path + "\t" + hash + "\n";
            return blitzsha::sha256_hex(all.data(), all.size());
        }
        bool counted(const std::string& path) { return path.rfind("saves/", 0) == 0; }  // not settings/
    }

    std::string saves_fingerprint(const Env& env) {
        std::map<std::string, std::string> files;
        for (const std::string& folder : pak_folders(env)) {
            fs::path dir = folder_path(env, folder);
            bool ok = false;
            Bytes pak = read_file(dir / pak_name, &ok);
            if (!ok || !is_pak_image(pak)) continue;
            std::map<std::string, Bytes> advs = season_files(dir);
            if (blank(pak) && advs.empty()) continue;  // as make_bundle
            std::string prefix = "saves/" + (folder.empty() ? "" : folder + "/");
            for (const auto& [n, b] : advs) files[prefix + "season/" + n] = sha256(b);
            files[prefix + pak_name] = sha256(pak);
        }
        return fingerprint_of(files);
    }

    std::string bundle_fingerprint(const std::vector<uint8_t>& data) {
        mz_zip_archive zip{};
        if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return "";
        size_t size = 0;
        void* b = mz_zip_reader_extract_file_to_heap(&zip, "manifest.json", &size, 0);
        mz_zip_reader_end(&zip);
        if (b == nullptr) return "";
        json m = json::parse((const char*)b, (const char*)b + size, nullptr, false);
        mz_free(b);
        std::map<std::string, std::string> files;
        if (m.is_object() && m.contains("files") && m["files"].is_array()) {
            for (const json& f : m["files"]) {
                if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("sha256") || !f["sha256"].is_string()) continue;
                std::string path = f["path"].get<std::string>();
                if (counted(path)) files[path] = f["sha256"].get<std::string>();
            }
        }
        return fingerprint_of(files);
    }

    Summary summarize(const std::vector<uint8_t>& data) {
        Summary out;
        mz_zip_archive zip{};
        if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return out;
        size_t size = 0;
        void* b = mz_zip_reader_extract_file_to_heap(&zip, "manifest.json", &size, 0);
        mz_zip_reader_end(&zip);
        if (b == nullptr) return out;
        json m = json::parse((const char*)b, (const char*)b + size, nullptr, false);
        mz_free(b);
        if (!m.is_object() || m.value("kind", "") != kind_key) return out;
        out.ok = true;
        out.device = m.value("device", "");
        out.made = m.value("made", "");
        if (m.contains("folders") && m["folders"].is_array()) {
            for (const json& f : m["folders"]) {
                if (!f.is_object() || !f.contains("folder") || !f["folder"].is_string()) continue;
                FolderId id = parse_folder(f["folder"].get<std::string>());
                if (!id.ok) continue;
                out.folders.push_back(folder_label(id) + ": " + f.value("what", "?"));
            }
        }
        return out;
    }

    std::vector<fs::path> folders_with_staging(const Env& env) {
        std::vector<fs::path> out;
        for (const std::string& folder : pak_folders(env)) {
            fs::path dir = folder_path(env, folder);
            if (has_staged(dir)) out.push_back(dir);
        }
        return out;
    }

    fs::path folder_dir(const Env& env, const std::string& folder) { return folder_path(env, folder); }

    void record_import(const Env& env, const Plan& plan) {
        json sync = load_sync(env);
        for (const FolderPlan& f : plan.folders) {
            if (!f.take) continue;
            std::vector<std::string> hashes = f.seen;
            hashes.push_back(sha256(f.pak));
            add_seen(sync, f.folder, hashes);
        }
        store_sync(env, sync);
    }
}
