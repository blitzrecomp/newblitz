// Host-side state that belongs with a save state (savestate.cpp). A module whose own variables follow
// the game (the Advanced season's seed, the resume checkpoint's "game in progress") registers them
// here; each snapshot stores a copy and a restore puts it back. `check` can refuse a restore (an empty
// string allows it), e.g. a state saved in another Season Mode.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "blitz_soft_reset.h"

namespace blitz {
    using StateBlob = std::vector<uint8_t>;

    // check, before a load: "" to go ahead, a message to refuse it, or "?" and a question to ask the player first (Load It /
    // Cancel; not for test marks or the game saved on quitting).
    void savestate_register(const std::string& name, std::function<StateBlob()> save, std::function<void(const StateBlob&)> load,
                            std::function<std::string(const StateBlob&)> check = nullptr);

    // A plain struct or value, copied as bytes (a blob of another size is ignored). It follows the game, so a restart in
    // place (blitz_soft_reset.h) puts back the value it had when it registered, before any game ran.
    template <typename T> bool savestate_register_pod(const std::string& name, T* obj) {
        static_assert(std::is_trivially_copyable_v<T>);
        savestate_register(
            name, [obj] { return StateBlob((const uint8_t*)obj, (const uint8_t*)obj + sizeof(T)); },
            [obj](const StateBlob& b) {
                if (b.size() == sizeof(T)) std::memcpy((void*)obj, b.data(), sizeof(T));
            });
        StateBlob fresh((const uint8_t*)obj, (const uint8_t*)obj + sizeof(T));
        on_soft_reset(name.c_str(), [obj, fresh] { std::memcpy((void*)obj, fresh.data(), sizeof(T)); });
        return true;
    }
}
