// Cloud Sync and Sync over Wi-Fi (docs/SAVE_SYNC.md §7): phase 1's .nbsave bundle (save_sync_core.h), encrypted on the
// device and moved through our sync server (tools/save_sync_worker) or straight from one device to another on the same
// network. No game or runtime needed: tests/save_sync_cloud_test.cpp runs all of it, two "devices" (two app folders)
// against the worker included. src/main/save_sync.cpp is the screens and the background thread.
//
// The sync code is the only secret: 24 Crockford base32 characters (120 bits) made on the device that turns Cloud Sync
// on, typed (or read from its QR code) on the others. Kept in the platform's secure storage (Secrets below; the app's
// is blitz_host.h secure_put/get), never in the app folder's state file. Under one code are three Cloud Save Slots,
// each its own chain of revisions on the server; from the code and a slot number, one-way (keyed BLAKE2b, each output
// independent):
//   id     what the server files that slot's saves under (64 hex): the server can't get the code or the key back;
//   key    XChaCha20-Poly1305 (Monocypher's crypto_aead_lock) for the bundle: the server holds only ciphertext;
//   token  proves a writer knows the code: the server keeps only its SHA-256, and a write or delete without it fails.
//
// The server keeps a revision number per id and takes a write only if it names the revision it replaces
// (compare-and-set), so two devices can't overwrite each other unseen. And a device never replaces saves that don't
// come before its own, either way: a pull takes only what carries on from this device's saves (phase 1's lineage:
// season id and generation, pak hashes in sync.json), and a push first checks that the cloud's copy comes before this
// device's saves. Anything else is a question (Keep This Device's / Use the Cloud's / Cancel), never a silent choice.
#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "save_sync_core.h"

namespace blitz::savesync::cloud {
    namespace fs = std::filesystem;
    using Bytes = std::vector<uint8_t>;

    // ---- the sync code -------------------------------------------------------------------------------------------
    constexpr size_t code_chars = 24;
    constexpr int slot_count = 3;                            // Cloud Save Slots: Slot 1, Slot 2, Slot 3
    std::string new_code();                                  // from the system's random source
    // Upper case, O read as 0 and I or L as 1, spaces and dashes dropped. "" unless exactly code_chars good characters.
    std::string normalize_code(const std::string& typed);
    std::string group_code(const std::string& code);         // "K7QM-3XPA-..." in fours

    struct Keys {
        std::string id;                  // 64 lower-case hex
        std::array<uint8_t, 32> key{};
        std::array<uint8_t, 32> token{};
        std::string token_hex() const;
    };
    Keys derive(const std::string& code, int slot);          // a normalized code; slot 1..3

    // ---- the sealed bundle -----------------------------------------------------------------------------------------
    // "NBC1", the revision (8 bytes, little-endian), a random 24-byte nonce, the 16-byte tag, then the ciphertext. The
    // first 12 bytes and the id are authenticated with it: a blob can't be moved to another id (or slot) or given
    // another revision.
    constexpr size_t seal_overhead = 4 + 8 + 24 + 16;
    Bytes seal(const Keys& k, uint64_t rev, const Bytes& plain);
    struct Opened {
        bool ok = false;
        uint64_t rev = 0;
        Bytes plain;
        std::string why;                 // for the log
    };
    Opened open(const Keys& k, const Bytes& blob);

    // ---- the server (tools/save_sync_worker), through the app's plain GET and POST ------------------------------------
    struct Http {
        // true on 2xx; status set whatever happens (0: no answer). get fills body on 200.
        std::function<bool(const std::string& url, std::string& body, int& status)> get;
        std::function<bool(const std::string& url, const std::string& body, int& status)> post;
        // A GET with "Authorization: Bearer <bearer>" (pairing's proof and owner; blitz_host.h http_get's bearer).
        std::function<bool(const std::string& url, const std::string& bearer, std::string& body, int& status)> get_auth;
    };
    constexpr size_t max_blob = 1 << 20;  // the server's cap

    struct Head {
        bool ok = false;                 // the server answered
        int status = 0;
        uint64_t rev = 0;                // 0: nothing under this id
    };
    Head head(const Http& http, const std::string& base, const Keys& k);
    // The blob, or empty (status says why).
    Bytes fetch(const Http& http, const std::string& base, const Keys& k, int& status);
    // 201 taken (the server's revision is now if_match + 1), 412 someone else wrote first, 413 too big, 429 too many
    // writes this hour, 403 the token doesn't match the one that made this id, 0 no answer.
    int put(const Http& http, const std::string& base, const Keys& k, uint64_t if_match, const Bytes& blob);
    int remove(const Http& http, const std::string& base, const Keys& k);

    // ---- where the code is kept ------------------------------------------------------------------------------------
    // The app's are the platform's secure storage (Keychain, Android Keystore, DPAPI, libsecret; save_sync.cpp).
    struct Secrets {
        std::function<bool(const std::string& value)> put;
        std::function<bool(std::string& value)> get;
        std::function<void()> erase;
        std::string kind;                // "the Keychain", "a file only this user can read", ... (for the docs and log)
    };
    // A file only this user can read (0600 where the system has permissions): the fallback, and the tests'.
    Secrets file_secrets(const fs::path& file);

    // ---- this device's Cloud Sync state: <app>/cloud_sync.json (no code in it) ------------------------------------------
    struct SlotState {
        uint64_t rev = 0;                // the server's revision this device last pushed or pulled in this slot
        std::string fp;                  // saves_fingerprint at that moment
        std::time_t last_sync = 0;       // when the last push or pull went through (or found nothing to do)
        std::string last_device;         // whose saves the last pull brought ("macOS 7KQ3")
    };
    struct State {
        bool on = false;
        std::string code;                // normalized; from Secrets, never written to the state file
        int slot = 1;                    // the slot this device syncs with
        std::array<SlotState, slot_count> slots{};
        std::string last_error;          // the last failure, for the screen ("" when the last try went through)
        SlotState& cur() { return slots[(size_t)(slot - 1)]; }
        const SlotState& cur() const { return slots[(size_t)(slot - 1)]; }
    };
    // secrets null: the state without the code (cheap; for "is it on?"). A state file from before the code moved to
    // secure storage has its code moved there and taken out of the file.
    State load_state(const fs::path& app, const Secrets* secrets);
    bool store_state(const fs::path& app, const State& s);   // never the code
    // The last bundle this device synced in a slot (pulled or pushed), and the server revision it was: the push guard
    // compares against it without a download. <app>/cloud_sync/slot<N>.nbsave (the same saves as on the device).
    void keep_synced_copy(const fs::path& app, int slot, uint64_t rev, const Bytes& zip);
    Bytes synced_copy(const fs::path& app, int slot, uint64_t rev);   // empty unless kept at that revision

    // ---- a change the player can still cancel ---------------------------------------------------------------------------
    // Turning on, joining and switching slots change this device's Cloud Sync state (the code in secure storage,
    // cloud_sync.json, the synced copies) before they know how it ends. A snapshot is taken first and put back exactly if
    // the change doesn't go through (cancelled, offline, a question the player hasn't answered yet), so Cancel never
    // leaves a half state: no code stored, no slot changed, Cloud Sync exactly as on or off as it was.
    struct Snapshot {
        bool had_state = false;
        Bytes state;                                         // cloud_sync.json as it was
        bool had_code = false;
        std::string code;                                    // what secure storage held
        std::vector<std::pair<std::string, Bytes>> copies;  // cloud_sync/<file>
    };
    Snapshot snapshot(const fs::path& app, const Secrets& secrets);
    void restore(const fs::path& app, const Secrets& secrets, const Snapshot& snap);

    // ---- pairing: a 6-digit number in place of the code (Add a Device, Join) -------------------------------------------
    // The device with Cloud Sync on makes a random number P and leaves its code with the server for two minutes, sealed
    // with a key made from P and a random salt (Argon2i, 8 MiB, 3 passes). The record is filed under pid = a keyed hash of
    // P; the joiner sends proof = another keyed hash of P and gets the sealed code only if the proof matches (once: the
    // record goes as it's handed over). The server limits tries per connection, so a wrong guess gets nothing.
    constexpr size_t pair_digits = 6;
    constexpr int pair_seconds = 120;
    std::string new_pair_number();                                 // "482915", from the system's random source
    std::string normalize_pair_number(const std::string& typed);   // digits only; "" unless exactly 6
    std::string group_pair_number(const std::string& number);      // "482 915"
    struct PairKeys {
        std::string id;                        // 64 hex: where the server files it
        std::array<uint8_t, 32> proof{};       // what the joiner shows to get it
        std::string proof_hex() const;
    };
    PairKeys pair_keys(const std::string& number);                 // a normalized number
    // "NBP1", a 16-byte salt, a 24-byte nonce, the tag, then the code and the slot, under Argon2i(number, salt).
    Bytes seal_pairing(const std::string& number, const std::string& code, int slot);
    bool open_pairing(const std::string& number, const Bytes& sealed, std::string& code, int& slot);

    struct Offer {
        bool ok = false;
        int status = 0;                        // the server's answer (0: none)
        std::string number;                    // normalized; group_pair_number for the screen
        std::string id;
        std::array<uint8_t, 32> owner{};       // random: only this device can cancel it or ask whether it was taken
        std::string message;                   // for the screen when !ok
    };
    // A number for this code and slot, left with the server for two minutes. `before` (an earlier offer from this
    // device, or null) is replaced at once: same owner, and the old number stops working.
    Offer offer_pairing(const Http& http, const std::string& base, const std::string& code, int slot, const Offer* before = nullptr);
    // Whether the number is still waiting (false: taken by the other device, expired or cancelled). False if no answer.
    bool pairing_live(const Http& http, const std::string& base, const Offer& offer, bool& live);
    void cancel_pairing(const Http& http, const std::string& base, const Offer& offer);

    struct Claimed {
        bool ok = false;
        int status = 0;
        std::string code;                      // normalized
        int slot = 1;
        std::string message;                   // for the screen when !ok
    };
    Claimed claim_pairing(const Http& http, const std::string& base, const std::string& typed);

    // ---- deciding --------------------------------------------------------------------------------------------------
    enum class Next { Nothing, Push, Pull };
    // server_rev: the server's revision now (0: none); local_fp: saves_fingerprint now.
    Next next_step(uint64_t server_rev, const SlotState& s, const std::string& local_fp);

    // What a pulled bundle's folders get: New comes in (nothing here to replace) and Newer too (it carries on from this
    // device's), unless the game has that pak open: then it waits for the next start. Same needs nothing. Older keeps
    // this device's (this device saved since: the cloud gets ours). Conflict is asked when a player is there to ask,
    // else waits. ask_replacing (switching slots): Newer and Older are asked too, so neither the device's saves nor the
    // slot's change unasked.
    struct Split {
        std::vector<size_t> take, keep, ask, wait;
    };
    Split split_pull(const Plan& plan, const std::function<bool(const std::string& folder)>& folder_open, bool can_ask,
                     bool ask_replacing = false);
    // Before a push: the folders where the cloud's copy (plan: the cloud's bundle read against this device) does NOT come
    // before this device's saves, so sending would replace saves that aren't an ancestor of ours: the cloud's is newer
    // (Newer: this device went back, e.g. a backup put back), unrelated (Conflict), or only the cloud has saves there
    // (New: they'd be dropped). Empty: a push only carries the cloud's saves on. ask_replacing (switching slots): Older
    // too, since sending would replace what the slot holds.
    std::vector<size_t> push_guard(const Plan& cloud, bool ask_replacing = false);

    // ---- one sync ----------------------------------------------------------------------------------------------------
    struct Hooks {
        std::function<bool(const fs::path& folder)> folder_open;  // the game has this folder's pak open now
        // Puts a folder's staged import in place now (controller_pak.cpp pak_sync_apply; tests: apply_staged). False with
        // why on failure; later=true when it waits for the next start.
        std::function<bool(const fs::path& folder, bool& later, std::string& why)> apply;
        std::function<bool()> busy;      // the game is writing the pak right now: don't read it
        Secrets secrets;                 // where join() keeps the code
        // Called before the first change to this device's saves or to the cloud's copy; false: the player cancelled,
        // so stop with Result::Cancelled and change nothing. Null: never cancelled (the background sync).
        std::function<bool()> commit;
    };

    enum class Result {
        Off,         // Cloud Sync isn't on, or this build has no server
        Offline,     // the server couldn't be reached (or answered something we don't read)
        UpToDate,    // nothing to do
        Pushed,      // this device's saves are the cloud's now
        Pulled,      // the cloud's saves are this device's now (some may be waiting for the next start)
        Ask,         // a question: plan holds the cloud's bundle read against this device; the screens ask, then finish
        Waiting,     // something waits for the next start, or for a question no one was there to answer
        Busy,        // the game is saving: try again shortly
        Failed,      // a real problem: message says what
        Cancelled,   // the player cancelled before anything changed (Hooks::commit said no)
    };
    const char* result_name(Result r);

    struct Outcome {
        Result result = Result::Off;
        std::string message;             // short, for a toast or the screen
        Plan plan;                       // Ask: the decisions so far (take set on what comes in unasked)
        std::vector<size_t> asked;       // Ask: the folders to ask about (plan.folders indices)
        bool guard = false;              // Ask: from the push guard (this device's saves don't carry the cloud's on)
        uint64_t server_rev = 0;
        std::string device;              // whose saves were pulled
        std::string cloud_fp;            // Ask: the cloud bundle's fingerprint, for finish_pull
        int pulled = 0, waiting = 0;
        // Ask from join() or a slot switch: nothing is stored until the questions are answered (the snapshot was put
        // back); finish_pull stores this first. slot 0: an ordinary sync's questions.
        struct Pending {
            std::string code;            // join: the code to keep
            int slot = 0;                // the slot to sync with
            bool switching = false;      // a slot switch (the questions name the slot)
        } pending;
    };

    // One round: brings in what's newer, then sends what's newer here. can_ask: a player is looking at the screens
    // (questions come back as Ask); otherwise a question waits and the message says so. ask_replacing: see split_pull.
    Outcome sync_once(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, bool can_ask,
                      bool ask_replacing = false);
    // After the screens have set each asked folder's take (Keep This Device's: false, Use the Cloud's: true): puts the
    // chosen ones in, then sends this device's saves up if the cloud lacks any of them (no guard: the player chose).
    // For a join's or a switch's questions (asked.pending), keeps the code and the slot first; if it doesn't go through
    // (cancelled, offline before anything changed), they're put back.
    Outcome finish_pull(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, Outcome& asked);
    // Join with a code. slot 0 (a typed code): the first slot with saves, and none is "Nothing is saved under that code
    // yet"; 1..3 (from pairing: the other device's slot): that slot, empty or not. Keeps the code, then sync_once. Only a
    // join that goes through stays: cancelled, offline or failed before anything changed, or a question (asked first,
    // kept by finish_pull), and the device is as it was (Snapshot).
    Outcome join(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, const std::string& code,
                 int slot = 0);
    // Turn On: a new code, this device's saves up. As join: only kept if it goes through.
    Outcome turn_on(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks);
    // Switching slots: syncs with `slot` from now on, asking before anything on the device or in the slot is replaced
    // (sync_once's ask_replacing). Only kept if it goes through; questions come back with pending set.
    Outcome switch_slot(const Env& env, const Http& http, const std::string& base, State& state, const Hooks& hooks, int slot);
}
