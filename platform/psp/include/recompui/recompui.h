// recompui-lite (docs/PSP_PORT.md §4.7): the part of RecompFrontend's recompui/recompui.h that has no RmlUi or SDL in it.
// Prompts resolve to their default answer, and say so in the log (src/psp/recompui_lite/lite.cpp).
#pragma once
#include <functional>
#include <string>

namespace recompui {
    enum class ButtonStyle { Primary, Secondary, Tertiary, Success, Warning, Danger, Basic };

    void message_box(const char* msg);
    void open_choice_prompt(const std::string& header_text, const std::string& content_text, const std::string& confirm_label,
                            const std::string& cancel_label, std::function<void()> confirm_action,
                            std::function<void()> cancel_action, ButtonStyle confirm_variant = ButtonStyle::Success,
                            ButtonStyle cancel_variant = ButtonStyle::Danger, bool focus_on_cancel = true);
    void open_info_prompt(const std::string& header_text, const std::string& content_text, const std::string& okay_label,
                          std::function<void()> okay_action, ButtonStyle okay_variant = ButtonStyle::Danger);
    void open_notification(const std::string& header_text, const std::string& content_text);
    void set_prompt_scoreline(const std::string &summary);
    void close_prompt();
    bool is_prompt_open();
    void hide_all_contexts();
    bool is_any_context_shown();
    bool is_context_capturing_input();
    void update_game_mod_id(const std::string &game_mod_id);
    const std::string &get_game_mod_id();
}
