#include "aida_pro.hpp"
#include <moves.hpp>

namespace ida_utils
{
    static bool get_address_from_line_pos(ea_t* out_ea, const char* line, int x)
    {
        if (line == nullptr)
            return false;

        const char* target_ptr = tag_advance(line, x);

        const char* p_on = nullptr;
        for (const char* p = target_ptr; p >= line; --p)
        {
            if (*p == COLOR_ON && p[1] == COLOR_ADDR)
            {
                p_on = p;
                break;
            }
        }

        if (p_on == nullptr)
            return false;

        const char* p_addr = p_on + 2;

        const char* p_off = strstr(p_addr, SCOLOR_OFF SCOLOR_ADDR);
        if (p_off == nullptr || target_ptr > p_off)
            return false;

        qstring addr_str;
        addr_str.append(p_addr, COLOR_ADDR_SIZE);

        return str2ea(out_ea, addr_str.c_str());
    }
}

static bool idaapi handle_viewer_dblclick(TWidget* viewer, int /*shift*/, void* /*ud*/)
{
    qstring word;
    if (get_highlight(&word, viewer, nullptr))
    {
        ea_t ea = BADADDR;
        if (str2ea(&ea, word.c_str()))
        {
            jumpto(ea);
            return true;
        }
    }

    return false;
}

// this stupid form almost gave me an aneurysm
void SettingsForm::show_and_apply(aida_plugin_t* plugin_instance)
{
    static const char form_str[] =
        "STARTITEM 0\n"
        "BUTTON YES Ok\n"
        "BUTTON CANCEL Cancel\n"
        "AI Assistant Settings\n\n"
        // --- general tab ---
        "<#API Provider Configuration#Provider:b1:0:20::>\n\n"
        "<#Analysis Parameters#XRef Context Count:D2:10:10::>\n"
        "<XRef Analysis Depth:D3:10:10::>\n"
        "<Code Snippet Lines:D4:10:10::>\n"
        "<Bulk Processing Delay (sec):q5:10:10::>\n"
        "<Max Prompt Tokens:D6:10:10::>\n"
        "<Model Temperature:q7:10:10::>\n"
        "<=:General>100>\n" // tab ctrl is 100

        // --- gemini ---
        "<API Key:q11:64:64::>\n"
        "<Model Name:b12:0:40::>\n"
        "<=:Gemini>100>\n"

        // --- openai ---
        "<API Key:q21:64:64::>\n"
        "<Model Name:b22:0:40::>\n"
        "<=:OpenAI>100>\n"

        // --- Anthropic Tab ---
        "<API Key:q31:64:64::>\n"
        "<Model Name:b32:0:40::>\n"
        "<=:Anthropic>100>\n"

        // --- copilot ---
        "<Proxy Address:q41:64:64::>\n"
        "<Model Name:b42:0:40::>\n"
        "<=:Copilot>100>\n";

    static const char* const providers_list_items[] = { "Gemini", "OpenAI", "Anthropic", "Copilot" };
    qstrvec_t providers_qstrvec;
    for (const auto& p : providers_list_items)
        providers_qstrvec.push_back(p);

    qstring provider_setting = g_settings.api_provider.c_str();
    qstrlwr(provider_setting.begin());
    int provider_idx = 0;
    if (provider_setting == "openai") provider_idx = 1;
    else if (provider_setting == "anthropic") provider_idx = 2;
    else if (provider_setting == "copilot") provider_idx = 3;

    auto find_model_index = [](const std::vector<std::string>& models, const std::string& name) -> int {
        auto it = std::find(models.begin(), models.end(), name);
        if (it == models.end()) {
            return 0;
        }
        return static_cast<int>(std::distance(models.begin(), it));
    };

    qstrvec_t gemini_models_qsv;
    for (const auto& m : settings_t::gemini_models) gemini_models_qsv.push_back(m.c_str());
    int gemini_model_idx = find_model_index(settings_t::gemini_models, g_settings.gemini_model_name);

    qstrvec_t openai_models_qsv;
    for (const auto& m : settings_t::openai_models) openai_models_qsv.push_back(m.c_str());
    int openai_model_idx = find_model_index(settings_t::openai_models, g_settings.openai_model_name);

    qstrvec_t anthropic_models_qsv;
    for (const auto& m : settings_t::anthropic_models) anthropic_models_qsv.push_back(m.c_str());
    int anthropic_model_idx = find_model_index(settings_t::anthropic_models, g_settings.anthropic_model_name);

    qstrvec_t copilot_models_qsv;
    for (const auto& m : settings_t::copilot_models) copilot_models_qsv.push_back(m.c_str());
    int copilot_model_idx = find_model_index(settings_t::copilot_models, g_settings.copilot_model_name);

    qstring gemini_key = g_settings.gemini_api_key.c_str();
    qstring openai_key = g_settings.openai_api_key.c_str();
    qstring anthropic_key = g_settings.anthropic_api_key.c_str();
    qstring copilot_proxy_addr = g_settings.copilot_proxy_address.c_str();
    qstring bulk_delay_str;
    bulk_delay_str.sprnt("%.2f", g_settings.bulk_processing_delay);
    qstring temp_str;
    temp_str.sprnt("%.2f", g_settings.temperature);

    sval_t xref_count = g_settings.xref_context_count;
    sval_t xref_depth = g_settings.xref_analysis_depth;
    sval_t snippet_lines = g_settings.xref_code_snippet_lines;
    sval_t max_tokens = g_settings.max_prompt_tokens;

    int selected_tab = 0;

    if (ask_form(form_str,
        // general tab (8 args)
        &providers_qstrvec, &provider_idx,
        &xref_count, &xref_depth, &snippet_lines,
        &bulk_delay_str, &max_tokens, &temp_str,
        // gemini tab (3 args)
        &gemini_key, &gemini_models_qsv, &gemini_model_idx,
        // openai tab (3 args)
        &openai_key, &openai_models_qsv, &openai_model_idx,
        // anthropic tab (3 args)
        &anthropic_key, &anthropic_models_qsv, &anthropic_model_idx,
        // copilot tab (3 args)
        &copilot_proxy_addr, &copilot_models_qsv, &copilot_model_idx,
        // tab control (1 arg)
        &selected_tab
    ) > 0)
    {
        g_settings.api_provider = providers_list_items[provider_idx];

        g_settings.gemini_api_key = gemini_key.c_str();
        if (gemini_model_idx < settings_t::gemini_models.size())
            g_settings.gemini_model_name = settings_t::gemini_models[gemini_model_idx];

        g_settings.openai_api_key = openai_key.c_str();
        if (openai_model_idx < settings_t::openai_models.size())
            g_settings.openai_model_name = settings_t::openai_models[openai_model_idx];

        g_settings.anthropic_api_key = anthropic_key.c_str();
        if (anthropic_model_idx < settings_t::anthropic_models.size())
            g_settings.anthropic_model_name = settings_t::anthropic_models[anthropic_model_idx];

        g_settings.copilot_proxy_address = copilot_proxy_addr.c_str();
        if (copilot_model_idx < settings_t::copilot_models.size())
            g_settings.copilot_model_name = settings_t::copilot_models[copilot_model_idx];

        g_settings.xref_context_count = static_cast<int>(xref_count);
        g_settings.xref_analysis_depth = static_cast<int>(xref_depth);
        g_settings.xref_code_snippet_lines = static_cast<int>(snippet_lines);
        g_settings.max_prompt_tokens = static_cast<int>(max_tokens);

        try { g_settings.bulk_processing_delay = std::stod(bulk_delay_str.c_str()); }
        catch (...) { warning("AI Assistant: Invalid value for bulk processing delay."); }

        try { g_settings.temperature = std::stod(temp_str.c_str()); }
        catch (...) { warning("AI Assistant: Invalid value for temperature."); }

        g_settings.save();

        if (plugin_instance)
        {
            msg("AI Assistant: Settings updated. Re-initializing AI client...\n");
            plugin_instance->reinit_ai_client();
        }
    }
}

void idaapi close_handler(TWidget* /*cv*/, void* ud)
{
    strvec_t* lines_ptr = (strvec_t*)ud;
    delete lines_ptr;
}

void show_text_in_viewer(const char* title, const std::string& text_content)
{
    if (text_content.empty() || text_content.find_first_not_of(" \t\n\r") == std::string::npos)
    {
        warning("AI returned an empty or whitespace-only response. Nothing to display.");
        return;
    }

    TWidget* existing_viewer = find_widget(title);
    if (existing_viewer)
    {
        close_widget(existing_viewer, WCLS_SAVE);
    }

    strvec_t* lines_ptr = new strvec_t();

    std::string marked_up_content = ida_utils::markup_text_with_addresses(text_content);

    std::stringstream ss(marked_up_content);
    std::string line;
    while (std::getline(ss, line, '\n'))
    {
        lines_ptr->push_back(simpleline_t(line.c_str()));
    }

    simpleline_place_t s1;
    simpleline_place_t s2;
    s2.n = lines_ptr->empty() ? 0 : static_cast<uint32>(lines_ptr->size() - 1);

    TWidget* viewer = create_custom_viewer(title, &s1, &s2, &s1, nullptr, lines_ptr, nullptr, nullptr);
    if (viewer == nullptr)
    {
        warning("Could not create viewer '%s'.", title);
        delete lines_ptr;
        return;
    }

    static custom_viewer_handlers_t handlers(
        nullptr, // keydown
        nullptr, // popup
        nullptr, // mouse_moved
        nullptr, // click
        handle_viewer_dblclick, // dblclick
        nullptr, // curpos
        close_handler, // close
        nullptr, // help
        nullptr, // adjust_place
        nullptr, // get_place_xcoord
        nullptr, // location_changed
        nullptr); // can_navigate

    set_custom_viewer_handlers(viewer, &handlers, lines_ptr);

    display_widget(viewer, WOPN_DP_TAB | WOPN_RESTORE);
}

static int idaapi finish_populating_widget_popup(TWidget* widget, TPopupMenu* popup_handle, const action_activation_ctx_t* ctx)
{
    if (ctx == nullptr || (ctx->widget_type != BWN_PSEUDOCODE && ctx->widget_type != BWN_DISASM))
        return 0;

    struct menu_item_t
    {
        const char* action_name;
        const char* path; // nullptr for separator
    };

    static const menu_item_t menu_items[] = {
        { "ai_assistant:analyze",      "Analyze/" },
        { "ai_assistant:rename",       "Analyze/" },
        { "ai_assistant:comment",      "Analyze/" },
        { "ai_assistant:gen_struct",   "Generate/" },
        { "ai_assistant:gen_hook",     "Generate/" },
        { nullptr,                     nullptr }, // Separator
        { "ai_assistant:scan_for_offsets", "" },
        { "ai_assistant:custom_query", "" },
        { nullptr,                     nullptr }, // Separator
        { "ai_assistant:settings",     "" },
    };

    const char* menu_root = "AI Assistant/";
    for (const auto& item : menu_items)
    {
        qstring full_path;
        if (item.path != nullptr)
            full_path.append(menu_root).append(item.path);
        
        attach_action_to_popup(widget, popup_handle, item.action_name, full_path.c_str());
    }

    return 0;
}

ssize_t idaapi ui_callback(void* /*user_data*/, int notification_code, va_list va)
{
    if (notification_code == ui_finish_populating_widget_popup)
    {
        TWidget* widget = va_arg(va, TWidget*);
        TPopupMenu* popup_handle = va_arg(va, TPopupMenu*);
        const action_activation_ctx_t* ctx = va_arg(va, const action_activation_ctx_t*);
        return finish_populating_widget_popup(widget, popup_handle, ctx);
    }
    return 0;
}