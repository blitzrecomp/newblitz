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

namespace blitz {
    using StateBlob = std::vector<uint8_t>;

    void savestate_register(const std::string& name, std::function<StateBlob()> save, std::function<void(const StateBlob&)> load,
                            std::function<std::string(const StateBlob&)> check = nullptr);

    // A plain struct or value, copied as bytes (a blob of another size is ignored).
    template <typename T> bool savestate_register_pod(const std::string& name, T* obj) {
        static_assert(std::is_trivially_copyable_v<T>);
        savestate_register(
            name, [obj] { return StateBlob((const uint8_t*)obj, (const uint8_t*)obj + sizeof(T)); },
            [obj](const StateBlob& b) {
                if (b.size() == sizeof(T)) std::memcpy((void*)obj, b.data(), sizeof(T));
            });
        return true;
    }
}
