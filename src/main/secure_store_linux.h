// blitz_host.h secure_put/get/erase on Linux: the desktop's Secret Service (GNOME Keyring, KWallet's bridge, KeePassXC)
// through libsecret, loaded with dlopen so the app needs neither the library nor its headers to build or run. No
// libsecret, no Secret Service running, or a locked one that won't open: false, and save_sync.cpp keeps a file only
// this user can read (0600) instead. Included by platform_linux.cpp only; header-only so the Mac can compile-check it.
#pragma once
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace blitz::secure_linux {
    // libsecret's types, as its headers declare them (secret-schema.h): only what secret_password_*_sync uses.
    enum { SECRET_SCHEMA_NONE = 0 };
    enum { SECRET_SCHEMA_ATTRIBUTE_STRING = 0 };
    struct SchemaAttribute {
        const char* name;
        int type;
    };
    struct Schema {
        const char* name;
        int flags;
        SchemaAttribute attributes[32];
        int reserved;
        void* reserved1;
        void* reserved2;
        void* reserved3;
        void* reserved4;
        void* reserved5;
        void* reserved6;
        void* reserved7;
    };
    struct GError {
        unsigned domain;
        int code;
        char* message;
    };
    using store_fn = int (*)(const Schema*, const char* collection, const char* label, const char* password, void* cancellable,
                             GError** error, ...);
    using lookup_fn = char* (*)(const Schema*, void* cancellable, GError** error, ...);
    using clear_fn = int (*)(const Schema*, void* cancellable, GError** error, ...);
    using free_fn = void (*)(char*);
    using error_free_fn = void (*)(GError*);

    struct Lib {
        bool ok = false;
        store_fn store = nullptr;
        lookup_fn lookup = nullptr;
        clear_fn clear = nullptr;
        free_fn free_password = nullptr;
        error_free_fn error_free = nullptr;
    };

    inline const Lib& lib() {
        static const Lib l = [] {
            Lib x;
            void* h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
            if (h == nullptr) return x;
            x.store = (store_fn)dlsym(h, "secret_password_store_sync");
            x.lookup = (lookup_fn)dlsym(h, "secret_password_lookup_sync");
            x.clear = (clear_fn)dlsym(h, "secret_password_clear_sync");
            x.free_password = (free_fn)dlsym(h, "secret_password_free");
            x.error_free = (error_free_fn)dlsym(h, "g_error_free");  // glib's, loaded with libsecret
            x.ok = x.store && x.lookup && x.clear && x.free_password;
            return x;
        }();
        return l;
    }

    // BRANDING: the Secret Service schema's name
    inline const Schema* schema() {
        static const Schema s = {"com.blitzrecomp.newblitz.Secret", SECRET_SCHEMA_NONE, {{"name", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, 0}},
                                 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        return &s;
    }

    inline void drop(GError* e, const char* what) {
        if (e == nullptr) return;
        fprintf(stderr, "[secure] Secret Service %s: %s\n", what, e->message ? e->message : "?");
        if (lib().error_free) lib().error_free(e);
    }

    inline bool put(const std::string& name, const std::string& value) {
        if (!lib().ok) return false;
        GError* e = nullptr;
        // BRANDING: the item's label in the desktop's password manager
        int ok = lib().store(schema(), nullptr /* the default collection */, "New Blitz", value.c_str(), nullptr, &e, "name", name.c_str(),
                             (const char*)nullptr);
        drop(e, "store");
        return ok != 0;
    }

    inline bool get(const std::string& name, std::string& value) {
        value.clear();
        if (!lib().ok) return false;
        GError* e = nullptr;
        char* got = lib().lookup(schema(), nullptr, &e, "name", name.c_str(), (const char*)nullptr);
        drop(e, "lookup");
        if (got == nullptr) return false;
        value = got;
        lib().free_password(got);
        return !value.empty();
    }

    inline void erase(const std::string& name) {
        if (!lib().ok) return;
        GError* e = nullptr;
        lib().clear(schema(), nullptr, &e, "name", name.c_str(), (const char*)nullptr);
        drop(e, "clear");
    }
}
