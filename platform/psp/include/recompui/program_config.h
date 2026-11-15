// recompui-lite (docs/PSP_PORT.md §4.7): RecompFrontend's recompui/program_config.h without recompui.h (RmlUi, SDL).
#pragma once
#include <string>

namespace recompui {
    namespace programconfig {
        void set_program_name(const std::string& name);
        void set_program_id(const std::u8string& id);
        const std::string &get_program_name();
        const std::u8string &get_program_id();
    }
}
