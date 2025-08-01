#pragma once

#include <string>
#include <utility>

#include <ida.hpp>
#include <typeinf.hpp>
#include <nlohmann/json.hpp>

struct settings_t;

namespace ida_utils
{
    std::string markup_text_with_addresses(const std::string& text);

    std::pair<std::string, std::string> get_function_code(ea_t ea, size_t max_len = 0, bool force_assembly = false);
    std::string get_code_xrefs_to(ea_t ea, const settings_t& settings);
    std::string get_code_xrefs_from(ea_t ea, const settings_t& settings);
    std::string get_struct_usage_context(ea_t ea);
    std::string get_data_xrefs_for_struct(const tinfo_t& struct_tif, const settings_t& settings);
    nlohmann::json get_context_for_prompt(ea_t ea, bool include_struct_context = false, size_t max_len = 0);
    void apply_struct_from_cpp(const std::string& cpp_code, ea_t ea);
    std::string format_prompt(const char* prompt_template, const nlohmann::json& context);
}