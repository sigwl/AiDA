#include "aida_pro.hpp"
using json = nlohmann::json;


static int idaapi timer_cb(void* ud);

struct AIClient::ai_request_t : public exec_request_t
{
    std::string result;
    bool was_cancelled;
    AIClient::callback_t callback;
    qtimer_t timer;
    qstring request_type;
    std::weak_ptr<void> client_validity_token;

    ai_request_t(
        AIClient::callback_t cb,
        qtimer_t t,
        qstring req_type,
        std::shared_ptr<void> validity_token)
        : was_cancelled(false),
        callback(std::move(cb)),
        timer(t),
        request_type(std::move(req_type)),
        client_validity_token(validity_token) {}

    ~ai_request_t() override = default;

    ssize_t idaapi execute() override
    {
        std::shared_ptr<void> client_validity_sp = client_validity_token.lock();
        if (!client_validity_sp)
        {
            delete this;
            return 0;
        }

        try
        {
            if (timer != nullptr)
            {
                unregister_timer(timer);
                timer = nullptr;
            }

            if (was_cancelled)
            {
                msg("AiDA: Request for %s was cancelled.\n", request_type.c_str());
            }
            else if (callback)
            {
                callback(result);
            }
        }
        catch (const std::exception& e)
        {
            warning("AI Assistant: Exception caught during AI request callback execution: %s", e.what());
        }
        catch (...)
        {
            warning("AI Assistant: Unknown exception caught during AI request callback execution.");
        }

        delete this;
        return 0;
    }
};

static int idaapi timer_cb(void* ud)
{
    auto* client = static_cast<AIClient*>(ud);

    if (client->_task_done.load())
    {
        return -1;
    }

    if (user_cancelled())
    {
        client->cancel_current_request();
        return -1;
    }

    if (!client->_is_request_active.load())
    {
        client->_is_request_active = true;
        msg("AiDA: Request for %s is in progress, please wait...\n", client->_current_request_type.c_str());
    }
    else
    {
        int elapsed = client->_elapsed_secs.load();
        msg("AiDA: Request for %s is in progress... elapsed time: %d second%s.\n",
            client->_current_request_type.c_str(),
            elapsed,
            elapsed == 1 ? "" : "s");
    }

    client->_elapsed_secs++;
    return 1000; // Reschedule for 1 second later
}

AIClient::AIClient(const settings_t& settings)
    : _settings(settings), _validity_token(std::make_shared<char>()) {}

AIClient::~AIClient()
{
    _validity_token.reset();
    cancel_current_request();
    if (_worker_thread.joinable())
    {
        _worker_thread.join();
    }
}

void AIClient::cancel_current_request()
{
    _cancelled = true;
    std::shared_ptr<httplib::Client> client_to_stop;
    {
        std::lock_guard<std::mutex> lock(_http_client_mutex);
        client_to_stop = _http_client;
    }

    if (client_to_stop)
    {
        client_to_stop->stop();
    }
}

void AIClient::_generate(const std::string& prompt_text, callback_t callback, double temperature, const qstring& request_type)
{
    std::lock_guard<std::mutex> lock(_worker_thread_mutex);
    if (_worker_thread.joinable())
    {
        _worker_thread.join();
    }

    _cancelled = false;
    _task_done = false;
    _is_request_active = false;
    _current_request_type = request_type;
    _elapsed_secs = 0;

    qtimer_t timer = register_timer(1000, timer_cb, this);

    auto req = new ai_request_t(callback, timer, request_type, _validity_token);

    auto worker_func = [this, prompt_text, temperature, req, validity_token = this->_validity_token]() {
        std::string result;
        try
        {
            result = this->_blocking_generate(prompt_text, temperature);
        }
        catch (const std::exception& e)
        {
            result = "Error: Exception in worker thread: ";
            result += e.what();
            warning("AiDA: %s", result.c_str());
        }
        catch (...)
        {
            result = "Error: Unknown exception in worker thread.";
            warning("AiDA: %s", result.c_str());
        }

        _task_done = true;

        req->was_cancelled = _cancelled.load();
        if (!req->was_cancelled)
        {
            req->result = std::move(result);
        }

        execute_sync(*req, MFF_NOWAIT);
    };

    _worker_thread = std::thread(worker_func);
}

std::string AIClient::_http_post_request(
    const std::string& host,
    const std::string& path,
    const httplib::Headers& headers,
    const std::string& body,
    std::function<std::string(const json&)> response_parser)
{
    std::shared_ptr<httplib::Client> current_client;
    try
    {
        {
            std::lock_guard<std::mutex> lock(_http_client_mutex);
            _http_client = std::make_shared<httplib::Client>(host.c_str());
            current_client = _http_client;
        }

        current_client->set_default_headers(headers);
        current_client->set_read_timeout(600); // 10 minutes
        current_client->set_connection_timeout(10);

        auto res = current_client->Post(
            path.c_str(),
            body.c_str(),
            body.length(),
            "application/json",
            [this](uint64_t, uint64_t) {
                return !_cancelled.load();
            });

        {
            std::lock_guard<std::mutex> lock(_http_client_mutex);
            _http_client.reset();
        }

        if (_cancelled)
            return "Error: Operation cancelled.";

        if (!res)
        {
            auto err = res.error();
            if (err == httplib::Error::Canceled) {
                return "Error: Operation cancelled.";
            }
            return "Error: HTTP request failed: " + httplib::to_string(err);
        }
        if (res->status != 200)
        {
            qstring error_details = "No details in response body.";
            if (!res->body.empty())
            {
                try
                {
                    error_details = json::parse(res->body).dump(2).c_str();
                }
                catch (const json::parse_error&)
                {
                    error_details = res->body.c_str();
                }
            }
            msg("AiDA: API Error. Host: %s, Status: %d\nResponse body: %s\n", host.c_str(), res->status, error_details.c_str());
            return "Error: API returned status " + std::to_string(res->status);
        }
        json jres = json::parse(res->body);
        return response_parser(jres);
    }
    catch (const std::exception& e)
    {
        {
            std::lock_guard<std::mutex> lock(_http_client_mutex);
            _http_client.reset();
        }
        warning("AI Assistant: API call to %s failed: %s\n", host.c_str(), e.what());
        return std::string("Error: API call failed. Details: ") + e.what();
    }
}

std::string AIClient::_blocking_generate(const std::string& prompt_text, double temperature)
{
    if (!is_available())
        return "Error: AI client is not initialized. Check API key.";

    auto payload = _get_api_payload(prompt_text, temperature);
    auto headers = _get_api_headers();
    auto host = _get_api_host();
    auto path = _get_api_path(_model_name);
    auto parser = [this](const json& jres) { return _parse_api_response(jres); };

    return _http_post_request(host, path, headers, payload.dump(), parser);
}

void AIClient::analyze_function(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    std::string prompt = ida_utils::format_prompt(ANALYZE_FUNCTION_PROMPT, context);

    _generate(prompt, callback, _settings.temperature, "function analysis");
}

void AIClient::suggest_name(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    std::string prompt = ida_utils::format_prompt(SUGGEST_NAME_PROMPT, context);
    _generate(prompt, callback, 0.0, "name suggestion");
}

void AIClient::generate_struct(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea, true);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    std::string prompt = ida_utils::format_prompt(GENERATE_STRUCT_PROMPT, context);
    _generate(prompt, callback, 0.0, "struct generation");
}

void AIClient::generate_hook(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    qstring q_func_name;
    get_func_name(&q_func_name, ea);
    std::string func_name = q_func_name.c_str();
    
    static const std::regex non_alnum_re("[^a-zA-Z0-9_]");
    std::string clean_func_name = std::regex_replace(func_name, non_alnum_re, "_");
    
    context["func_name"] = clean_func_name;

    std::string prompt = ida_utils::format_prompt(GENERATE_HOOK_PROMPT, context);
    _generate(prompt, callback, 0.0, "hook generation");
}

void AIClient::generate_comments(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    std::string prompt = ida_utils::format_prompt(GENERATE_COMMENTS_PROMPT, context);
    _generate(prompt, callback, 0.0, "comment generation");
}

void AIClient::custom_query(ea_t ea, const std::string& question, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    context["user_question"] = question;
    std::string prompt = ida_utils::format_prompt(CUSTOM_QUERY_PROMPT, context);
    _generate(prompt, callback, _settings.temperature, "custom query");
}

void AIClient::locate_global_pointer(ea_t ea, const std::string& target_name, addr_callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea, false, 16000);
    if (!context["ok"].get<bool>())
    {
        callback(BADADDR);
        return;
    }
    context["target_name"] = target_name;
    std::string prompt = ida_utils::format_prompt(LOCATE_GLOBAL_POINTER_PROMPT, context);

    auto on_result = [callback, target_name](const std::string& result) {
        if (!result.empty() && result.find("Error:") == std::string::npos && result.find("None") == std::string::npos)
        {
            try
            {
                static const std::regex backtick_re("`");
                std::string clean_result = std::regex_replace(result, backtick_re, "");
                clean_result.erase(0, clean_result.find_first_not_of(" \t\n\r"));
                clean_result.erase(clean_result.find_last_not_of(" \t\n\r") + 1);
                ea_t addr = std::stoull(clean_result, nullptr, 16);
                callback(addr);
            }
            catch (const std::exception&)
            {
                msg("AI Assistant: AI returned a non-address value for %s: %s\n", target_name.c_str(), result.c_str());
                callback(BADADDR);
            }
        }
        else
        {
            callback(BADADDR);
        }
    };
    _generate(prompt, on_result, 0.0, "global pointer location");
}

void AIClient::rename_all(ea_t ea, callback_t callback)
{
    json context = ida_utils::get_context_for_prompt(ea, true);
    if (!context["ok"].get<bool>())
    {
        callback(context["message"].get<std::string>());
        return;
    }
    std::string prompt = ida_utils::format_prompt(RENAME_ALL_PROMPT, context);
    _generate(prompt, callback, 0.0, "renaming");
}

GeminiClient::GeminiClient(const settings_t& settings) : AIClient(settings)
{
    _model_name = _settings.gemini_model_name;
}

bool GeminiClient::is_available() const
{
    return !_settings.gemini_api_key.empty();
}


std::string GeminiClient::_get_api_host() const
{
    if (!_settings.gemini_base_url.empty())
        return _settings.gemini_base_url;
    return "https://generativelanguage.googleapis.com";
}

std::string GeminiClient::_get_api_path(const std::string& model_name) const { return "/v1beta/models/" + model_name + ":generateContent?key=" + _settings.gemini_api_key; }
httplib::Headers GeminiClient::_get_api_headers() const { return {}; }
json GeminiClient::_get_api_payload(const std::string& prompt_text, double temperature) const
{
    return {
        {"contents", {{{"role", "user"}, {"parts", {{{"text", prompt_text}}}}}}},
        {"generationConfig", {{"temperature", temperature}}}
    };
}

std::string GeminiClient::_parse_api_response(const json& jres) const
{
    if (jres.contains("error"))
    {
        std::string error_msg = "Gemini API Error: ";
        if (jres["error"].is_object() && jres["error"].contains("message"))
        {
            error_msg += jres["error"]["message"].get<std::string>();
        }
        else
        {
            error_msg += jres.dump(2);
        }
        msg("AiDA: %s\n", error_msg.c_str());
        return "Error: " + error_msg;
    }

    const auto candidates = jres.value("candidates", json::array());
    if (candidates.empty() || !candidates[0].is_object())
    {
        if (jres.contains("promptFeedback") && jres["promptFeedback"].contains("blockReason")) {
            std::string reason = jres["promptFeedback"]["blockReason"].get<std::string>();
            msg("AiDA: Gemini API blocked the prompt. Reason: %s\n", reason.c_str());
            return "Error: Prompt was blocked by API for reason: " + reason;
        }
        msg("AiDA: Invalid Gemini API response: 'candidates' array is missing or empty.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'candidates' array from API.";
    }

    const auto& first_candidate = candidates[0];
    std::string finish_reason = first_candidate.value("finishReason", "UNKNOWN");

    if (finish_reason != "STOP")
    {
        msg("AiDA: Gemini API returned a non-STOP finish reason: %s\n", finish_reason.c_str());
        return "Error: API request finished unexpectedly. Reason: " + finish_reason;
    }

    const auto content = first_candidate.value("content", json::object());
    if (!content.is_object())
    {
        msg("AiDA: Invalid Gemini API response: 'content' object is missing or invalid.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'content' object from API.";
    }

    const auto parts = content.value("parts", json::array());
    if (parts.empty() || !parts[0].is_object())
    {
        msg("AiDA: Invalid Gemini API response: 'parts' array is missing, empty, or invalid.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'parts' array from API.";
    }

    return parts[0].value("text", "Error: 'text' field not found in API response.");
}

OpenAIClient::OpenAIClient(const settings_t& settings) : AIClient(settings)
{
    _model_name = _settings.openai_model_name;
}

bool OpenAIClient::is_available() const
{
    return !_settings.openai_api_key.empty();
}

std::string OpenAIClient::_get_api_host() const
{
    if (!_settings.openai_base_url.empty())
        return _settings.openai_base_url;
    return "https://api.openai.com";
}

std::string OpenAIClient::_get_api_path(const std::string&) const { return "/v1/chat/completions"; }
httplib::Headers OpenAIClient::_get_api_headers() const
{
    return {
        {"Authorization", "Bearer " + _settings.openai_api_key},
        {"Content-Type", "application/json"}
    };
}
json OpenAIClient::_get_api_payload(const std::string& prompt_text, double temperature) const
{
    std::string model = _model_name;
    json payload = {        
        {"messages", {
            {{"role", "system"}, {"content", BASE_PROMPT}},
            {{"role", "user"}, {"content", prompt_text}}
        }}
    };

    if (model == "gpt-5")
    {
        payload["model"] = "gpt-5";
        payload["reasoning_effort"] = "minimal";
    }
    else if (model == "gpt-5.1 Instant")
    {

        payload["model"] = "gpt-5.1";
        payload["reasoning_effort"] = "none";
    }
    else if (model == "gpt-5.1 Thinking")
    {
        payload["model"] = "gpt-5.1";
        payload["reasoning_effort"] = "high";
    }
    else
    {
        payload["model"] = _model_name;
        payload["temperature"] = temperature;
    }
    return payload;
}

std::string OpenAIClient::_parse_api_response(const json& jres) const
{
    if (jres.contains("error"))
    {
        std::string error_msg = "OpenAI API Error: ";
        if (jres["error"].is_object() && jres["error"].contains("message"))
        {
            error_msg += jres["error"]["message"].get<std::string>();
        }
        else
        {
            error_msg += jres.dump(2);
        }
        msg("AiDA: %s\n", error_msg.c_str());
        return "Error: " + error_msg;
    }

    const auto choices = jres.value("choices", json::array());
    if (choices.empty() || !choices[0].is_object())
    {
        if (jres.contains("promptFeedback") && jres["promptFeedback"].contains("blockReason")) {
            std::string reason = jres["promptFeedback"]["blockReason"].get<std::string>();
            msg("AiDA: OpenAI API blocked the prompt. Reason: %s\n", reason.c_str());
            return "Error: Prompt was blocked by API for reason: " + reason;
        }
        msg("AiDA: Invalid OpenAI API response: 'choices' array is missing or empty.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'choices' array from API.";
    }

    const auto& first_choice = choices[0];
    std::string finish_reason = first_choice.value("finish_reason", "UNKNOWN");

    if (finish_reason != "stop" && finish_reason != "STOP")
    {
        msg("AiDA: OpenAI API returned a non-STOP finish reason: %s\n", finish_reason.c_str());
        return "Error: API request finished unexpectedly. Reason: " + finish_reason;
    }

    const auto message = first_choice.value("message", json::object());
    if (!message.is_object())
    {
        msg("AiDA: Invalid OpenAI API response: 'message' object is missing or invalid.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'message' object from API.";
    }

    return message.value("content", "Error: 'content' field not found in API response.");
}

OpenRouterClient::OpenRouterClient(const settings_t& settings) : OpenAIClient(settings)
{
    _model_name = _settings.openrouter_model_name;
}

bool OpenRouterClient::is_available() const
{
    return !_settings.openrouter_api_key.empty();
}

std::string OpenRouterClient::_get_api_host() const { return "https://openrouter.ai"; }
std::string OpenRouterClient::_get_api_path(const std::string&) const { return "/api/v1/chat/completions"; }
httplib::Headers OpenRouterClient::_get_api_headers() const
{
    std::string auth = _settings.openrouter_api_key;
    if (auth.find("Bearer ") != 0) {
        auth = "Bearer " + auth;
    }
    return {
        {"Authorization", auth},
        {"Content-Type", "application/json"}
    };
}

AnthropicClient::AnthropicClient(const settings_t& settings) : AIClient(settings)
{
    _model_name = _settings.anthropic_model_name;
}

bool AnthropicClient::is_available() const
{
    return !_settings.anthropic_api_key.empty();
}

std::string AnthropicClient::_get_api_host() const
{
    if (!_settings.anthropic_base_url.empty())
        return _settings.anthropic_base_url;
    return "https://api.anthropic.com";
}

std::string AnthropicClient::_get_api_path(const std::string&) const { return "/v1/messages"; }
httplib::Headers AnthropicClient::_get_api_headers() const
{
    httplib::Headers headers = {
        {"x-api-key", _settings.anthropic_api_key},
        {"anthropic-version", "2023-06-01"},
        {"Content-Type", "application/json"}
    };

    if (_model_name.find("claude-opus-4-5") != std::string::npos)
    {
        headers.emplace("anthropic-beta", "effort-2025-11-24");
    }
    else if (_model_name.find("claude-3-7-sonnet") != std::string::npos)
    {
        headers.emplace("anthropic-beta", "output-128k-2025-02-19");
    }

    return headers;
}
json AnthropicClient::_get_api_payload(const std::string& prompt_text, double temperature) const
{
    std::string model_id = _model_name;
    std::string effort = "";
    bool use_thinking = false;

    if (model_id == "claude-opus-4-5 (High Effort)")
    {
        model_id = "claude-opus-4-5";
        effort = "high";
    }
    else if (model_id == "claude-opus-4-5 (Medium Effort)")
    {
        model_id = "claude-opus-4-5";
        effort = "medium";
    }
    else if (model_id == "claude-opus-4-5 (Low Effort)")
    {
        model_id = "claude-opus-4-5";
        effort = "low";
    }
    else if (model_id == "claude-3-7-sonnet-thought")
    {
        model_id = "claude-3-7-sonnet";
        use_thinking = true;
    }

    json payload = {
        {"model", model_id},
        {"system", BASE_PROMPT},
        {"messages", {{{"role", "user"}, {"content", prompt_text}}}},
        {"max_tokens", 4096}
    };

    if (!effort.empty())
    {
        payload["output_config"] = { {"effort", effort} };
    }
    else if (use_thinking)
    {
        payload["thinking"] = { {"type", "enabled"}, {"budget_tokens", 4096} };
        payload["max_tokens"] = 8192; // Increase limit for thoughts
    }
    else
    {
        payload["temperature"] = temperature;
    }

    return payload;
}

std::string AnthropicClient::_parse_api_response(const json& jres) const
{
    if (jres.contains("error"))
    {
        std::string error_msg = "Anthropic API Error: ";
        if (jres["error"].is_object() && jres["error"].contains("message"))
        {
            error_msg += jres["error"]["message"].get<std::string>();
        }
        else
        {
            error_msg += jres.dump(2);
        }
        msg("AiDA: %s\n", error_msg.c_str());
        return "Error: " + error_msg;
    }

    const auto content = jres.value("content", json::array());
    if (content.empty())
    {
        if (jres.contains("promptFeedback") && jres["promptFeedback"].contains("blockReason")) {
            std::string reason = jres["promptFeedback"]["blockReason"].get<std::string>();
            msg("AiDA: Anthropic API blocked the prompt. Reason: %s\n", reason.c_str());
            return "Error: Prompt was blocked by API for reason: " + reason;
        }
        msg("AiDA: Invalid Anthropic API response: 'content' array is missing or empty.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'content' array from API.";
    }

    std::string stop_reason = jres.value("stop_reason", "UNKNOWN");
    if (stop_reason != "end_turn" && stop_reason != "max_tokens")
    {
        msg("AiDA: Anthropic API returned a non-success stop reason: %s\n", stop_reason.c_str());
        return "Error: API request finished unexpectedly. Reason: " + stop_reason;
    }

    std::string result_text;
    for (const auto& block : content)
    {
        if (block.is_object() && block.value("type", "") == "text")
        {
            result_text += block.value("text", "");
        }
    }

    if (result_text.empty())
    {
        msg("AiDA: No text content found in Anthropic API response.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: No text content found in API response.";
    }

    return result_text;
}

CopilotClient::CopilotClient(const settings_t& settings) : AIClient(settings)
{
    _model_name = _settings.copilot_model_name;
}

bool CopilotClient::is_available() const
{
    return !_settings.copilot_proxy_address.empty();
}

std::string CopilotClient::_get_api_host() const { return _settings.copilot_proxy_address; }
std::string CopilotClient::_get_api_path(const std::string&) const { return "/v1/chat/completions"; }
httplib::Headers CopilotClient::_get_api_headers() const { return {{"Content-Type", "application/json"}}; }
json CopilotClient::_get_api_payload(const std::string& prompt_text, double temperature) const
{
    return {
        {"model", _model_name},
        {"messages", {
            {{"role", "system"}, {"content", BASE_PROMPT}},
            {{"role", "user"}, {"content", prompt_text}}
        }},
        {"temperature", temperature}
    };
}
std::string CopilotClient::_parse_api_response(const json& jres) const
{
    if (jres.contains("error"))
    {
        std::string error_msg = "Copilot API Error: ";
        if (jres["error"].is_object() && jres["error"].contains("message"))
        {
            error_msg += jres["error"]["message"].get<std::string>();
        }
        else
        {
            error_msg += jres.dump(2);
        }
        msg("AiDA: %s\n", error_msg.c_str());
        return "Error: " + error_msg;
    }

    const auto choices = jres.value("choices", json::array());
    if (choices.empty() || !choices[0].is_object())
    {
        if (jres.contains("promptFeedback") && jres["promptFeedback"].contains("blockReason")) {
            std::string reason = jres["promptFeedback"]["blockReason"].get<std::string>();
            msg("AiDA: Copilot API blocked the prompt. Reason: %s\n", reason.c_str());
            return "Error: Prompt was blocked by API for reason: " + reason;
        }
        msg("AiDA: Invalid Copilot API response: 'choices' array is missing or empty.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'choices' array from API.";
    }

    const auto& first_choice = choices[0];
    std::string finish_reason = first_choice.value("finish_reason", "UNKNOWN");

    if (finish_reason != "stop" && finish_reason != "STOP")
    {
        msg("AiDA: Copilot API returned a non-STOP finish reason: %s\n", finish_reason.c_str());
        return "Error: API request finished unexpectedly. Reason: " + finish_reason;
    }

    const auto message = first_choice.value("message", json::object());
    if (!message.is_object())
    {
        msg("AiDA: Invalid Copilot API response: 'message' object is missing or invalid.\nResponse body: %s\n", jres.dump(2).c_str());
        return "Error: Received invalid 'message' object from API.";
    }

    return message.value("content", "Error: 'content' field not found in API response.");
}

std::unique_ptr<AIClient> get_ai_client(const settings_t& settings)
{
    qstring provider = ida_utils::qstring_tolower(settings.api_provider.c_str());

    msg("AI Assistant: Initializing AI provider: %s\n", provider.c_str());

    if (provider == "gemini")
    {
        return std::make_unique<GeminiClient>(settings);
    }
    else if (provider == "openai")
    {
        return std::make_unique<OpenAIClient>(settings);
    }
    else if (provider == "openrouter")
    {
        return std::make_unique<OpenRouterClient>(settings);
    }
    else if (provider == "anthropic")
    {
        return std::make_unique<AnthropicClient>(settings);
    }
    else if (provider == "copilot")
    {
        return std::make_unique<CopilotClient>(settings);
    }
    else
    {
        warning("AI Assistant: Unknown AI provider '%s' in settings. No AI features will be available.", provider.c_str());
        return nullptr;
    }
}