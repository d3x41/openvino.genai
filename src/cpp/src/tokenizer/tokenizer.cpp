// Copyright (C) 2023-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <memory>
#include <jinja2cpp/template.h>
#include <jinja2cpp/template_env.h>
#include <jinja2cpp/user_callable.h>
#include <jinja2cpp/generic_list.h>
#include <jinja2cpp/generic_list_iterator.h>

#include "openvino/pass/manager.hpp"
#include "openvino/runtime/core.hpp"
#include "openvino/genai/tokenizer.hpp"

#include "gguf_utils/gguf_tokenizer.hpp"
#include "tokenizer/chat_template_fallback_map.hpp"
#include "tokenizer/make_tokenizer_stateful.hpp"
#include "tokenizer/tokenizers_path.hpp"
#include "circular_buffer_queue.hpp"
#include "json_utils.hpp"
#include "utils.hpp"

namespace {

void check_arguments(const ov::AnyMap& parameters, std::set<std::string> allowed_argnames) {
    for (const auto& [key, value] : parameters) {
        if (allowed_argnames.find(key) == allowed_argnames.end()) {
            OPENVINO_THROW("unacceptable parameter key: " + key);
        }
    }
}

constexpr char bos_token_key_name[] = "bos_token";
constexpr char eos_token_key_name[] = "eos_token";
constexpr char pad_token_key_name[] = "pad_token";

ov::Core core_with_extension() {
    ov::Core core;

#ifdef _WIN32
    const wchar_t* ov_tokenizer_path_w = _wgetenv(ScopedVar::ENVIRONMENT_VARIABLE_NAME_W);
    OPENVINO_ASSERT(ov_tokenizer_path_w, "openvino_tokenizers path is not set");
    core.add_extension(std::wstring(ov_tokenizer_path_w));
#else
    const char* ov_tokenizer_path = getenv(ScopedVar::ENVIRONMENT_VARIABLE_NAME);
    OPENVINO_ASSERT(ov_tokenizer_path, "openvino_tokenizers path is not set");
    core.add_extension(ov_tokenizer_path);
#endif
    
    return core;
}

ov::Core get_core_singleton() {
    static ov::Core core = core_with_extension();
    return core;
}

std::optional<std::string> remap_template(const std::string& chat_template) {
    for (const auto& [known, fallback] : chat_template_fallback_map) {
        if (chat_template == known) {
            return fallback;
        }
    }
    return std::nullopt;
}

void parse_chat_template_from_file(const std::filesystem::path& path, std::string& value) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto json_data = nlohmann::json::parse(std::ifstream{path});
    if (!json_data.contains("chat_template")) {
        return;
    }
    auto chat_template_field = json_data["chat_template"];

    if (chat_template_field.is_string()) {
        value = chat_template_field.get<std::string>();
        return;
    }
    // Handle chat template format: [{ "name": "default", "template": "..." }]
    // e.g. for CohereLabs/aya-23-8B & CohereLabs/c4ai-command-r-v01 models
    if (chat_template_field.is_array()) {
        for (const auto& item : chat_template_field) {
            if (
                item.is_object() && item.contains("name") && item["name"] == "default" &&
                item.contains("template") && item["template"].is_string()
            ) {
                value = item["template"].get<std::string>();
                return;
            }
        }
    }
    std::cerr << "[ WARNING ] Unsupported chat_template format in file: " << path.string() << "\n";
    std::cerr << "Supported formats: string or array of objects with 'name' and 'template' fields.\n";
    std::cerr << "To avoid this warning, check \"chat_template\" field in the file and update it accordingly.\n";
}

void parse_chat_template_from_tokenizer(std::shared_ptr<ov::Model> ov_tokenizer, std::string& value) {
    if (!ov_tokenizer->has_rt_info("chat_template")) {
        return;
    }
    ov::Any chat_template_value = ov_tokenizer->get_rt_info<ov::Any>("chat_template");
    if (chat_template_value.is<std::string>()) {
        value = chat_template_value.as<std::string>();
        return;
    }
    // Handle rt_info chat template format: <chat_template><default value="..." /></chat_template>
    if (chat_template_value.is<ov::AnyMap>()) {
        ov::AnyMap chat_template_map = chat_template_value.as<ov::AnyMap>();
        auto default_iter = chat_template_map.find("default");
        if (default_iter != chat_template_map.end() && default_iter->second.is<std::string>()) {
            value = default_iter->second.as<std::string>();
            return;
        }
    }
    std::cerr << "[ WARNING ] Unsupported type for 'chat_template' in ov_tokenizer model: " << chat_template_value.type_info().name() << std::endl;
}

template <typename T>
const T& find_or_fallback(const ov::AnyMap& rt_info, const char name[], const T& fallback) {
    auto iter = rt_info.find(name);
    if (rt_info.end() == iter) {
        return fallback;
    }
    return iter->second.as<T>();
}

std::string patch_template(std::string&& chat_template) {
    // Replace what jinja2cpp doesn't support
    std::pair<std::string, std::string> replace_str_map[] = {
        {"'}", "' }"},
        {"{'", "{ '"},
        {".strip()", ""},
        {"is not none", "is defined"},
        {"is none", "is undefined"},
        {"= none", "= undefined"},
        // Jinja2Cpp does not support Python-style slicing, e.g. [1:].
        // If chat template contains such slicing, we replace it with
        // a placeholder at the moment.
        {"messages[1:]", "slice(messages, 1)"},
    };

    for (const auto& [from, to] : replace_str_map) {
        size_t pos = 0;
        while ((pos = chat_template.find(from, pos)) != std::string::npos) {
            chat_template.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return chat_template;
}

std::string remap_and_patch(const std::string& chat_template) {
    return patch_template(
        remap_template(chat_template).value_or(chat_template)
    );
}

std::vector<std::string> read_vocab_from_detokenizer_model(const std::shared_ptr<ov::Model>& model) {
    std::shared_ptr<ov::Node> vocab_decoder_node;
    for (auto node : model->get_ordered_ops()) {
        if (node->get_friendly_name().find("VocabDecoder") != std::string::npos) {
            vocab_decoder_node = node;
        }
    }
    if (!vocab_decoder_node) {
        return {};
    }

    auto begins_node = ov::as_type_ptr<ov::op::v0::Constant>(vocab_decoder_node->get_input_node_shared_ptr(1));
    auto ends_node = ov::as_type_ptr<ov::op::v0::Constant>(vocab_decoder_node->get_input_node_shared_ptr(2));
    auto chars_node = ov::as_type_ptr<ov::op::v0::Constant>(vocab_decoder_node->get_input_node_shared_ptr(3));
    if (!begins_node || !ends_node || !chars_node) {
        return {};
    }

    auto begins = begins_node->cast_vector<int32_t>();
    auto ends = ends_node->cast_vector<int32_t>();
    auto chars = chars_node->cast_vector<uint8_t>();

    std::vector<std::string> vocab_vector;
    vocab_vector.resize(begins.size());
    for (size_t i = 0; i < begins.size(); ++i) {
        const auto begin = begins[i];
        const auto end = ends[i];
        vocab_vector[i] = std::string(chars.begin() + begin, chars.begin() + end);
    };
    return vocab_vector;
}

}  // namespace

namespace ov {
namespace genai {

class Tokenizer::TokenizerImpl {
public:
    std::unique_ptr<CircularBufferQueue<ov::InferRequest>> m_ireq_queue_tokenizer;
    std::unique_ptr<CircularBufferQueue<ov::InferRequest>> m_ireq_queue_detokenizer;

    // To change the adding special tokens mode we use a statefull subgraph,
    // this flag holds the current state value of the CompiledModel.
    std::unordered_map<ov::InferRequest*, ov::AnyMap> m_request_to_state_flags;

    // handle to shared_object with openvino tokenizers
    std::shared_ptr<void> m_shared_object_ov_tokenizers = nullptr;

    bool m_older_than_24_5 = false;

    int64_t m_pad_token_id = -1;
    int64_t m_bos_token_id = -1;
    int64_t m_eos_token_id = -1;

    std::string m_pad_token = {};
    std::string m_bos_token = {};
    std::string m_eos_token = {};

    std::string m_chat_template = {};

    std::vector<std::string> m_vocab = {};

    template <typename T>
    void set_state_value(ov::VariableState& state, std::optional<T> value, ov::AnyMap& state_flags) {
        // better to store which value is in the state locally so that get_state is not called every infer request
        std::optional<T> last_value;
        ov::genai::utils::read_anymap_param(state_flags, state.get_name(), last_value);

        // If requested add[skip]_special_tokens, max_length, pading mode, etc.
        // is different from the stored state, need to set state variable.
        // Or if we run for the first time and don't know the latest state we need to set it.
        if (value.has_value() && (!last_value.has_value() || *value != *last_value)) {
            ov::Tensor value_tensor = ov::Tensor(ov::element::from<T>(), state.get_state().get_shape());
            OPENVINO_ASSERT(value_tensor.get_size() == 1, "Only flags or single elements values are supported");

            *value_tensor.data<T>() = *value;
            state.set_state(value_tensor);
            state_flags[state.get_name()] = *value;
        } else if (!value.has_value()) {
            // If user called with params, e.g. tokenizer.encode(prompt, add_special_tokens|max_length=...)
            // After that called without params, e.g. tokenizer.encode(prompt) we should reset to the default state.
            state.reset();
            state_flags.erase(state.get_name());
        }
    }

    void set_state_if_necessary(CircularBufferQueueElementGuard<ov::InferRequest>& infer_request_guard, const ov::AnyMap& params) {
        if (m_older_than_24_5) {
            // Changing add_special_tokens at runtime was introduced in
            // 24.5. Older tokenizers still allow manipulating their
            // state but the effect is incorrect.
            return;
        }

        // These values should be equal to default values in py_tokenizer.cpp
        // in order to get the same behavior in C++ when arguments are not specified.
        std::optional<bool> add_special_tokens_flag = true;
        std::optional<bool> skip_special_tokens_flag = true;
        std::optional<int32_t> max_length_val;
        std::optional<bool> pad_to_max_length_val = false;

        ov::genai::utils::read_anymap_param(params, add_special_tokens.name(), add_special_tokens_flag);
        ov::genai::utils::read_anymap_param(params, skip_special_tokens.name(), skip_special_tokens_flag);
        ov::genai::utils::read_anymap_param(params, pad_to_max_length.name(), pad_to_max_length_val);
        ov::genai::utils::read_anymap_param(params, max_length.name(), max_length_val);
        std::optional<bool> is_max_length_set_val = max_length_val.has_value();

        ov::AnyMap& state_flags = m_request_to_state_flags[&infer_request_guard.get()];

        for (auto& state : infer_request_guard.get().query_state()) {
            auto name = state.get_name();

            if (name == add_special_tokens.name()) {
                set_state_value(state, add_special_tokens_flag, state_flags);
            } else if (name == skip_special_tokens.name()) {
                set_state_value(state, skip_special_tokens_flag, state_flags);
            } else if (name == MAX_LENGTH_VAR_ID) {
                set_state_value(state, max_length_val, state_flags);
            } else if (name == PAD_TO_MAX_LENGTH_VAR_ID) {
                set_state_value(state, pad_to_max_length_val, state_flags);
            } else if (name == IS_MAX_LENGTH_SET) {
                set_state_value(state, is_max_length_set_val, state_flags);
            }
        }
    }

    TokenizerImpl(const std::filesystem::path& models_path, const ov::AnyMap& properties) {
        setup_tokenizer(models_path, properties);
    }

    TokenizerImpl(const std::pair<std::shared_ptr<ov::Model>, std::shared_ptr<ov::Model>>& models, const ov::AnyMap& properties) {
        setup_tokenizer(models, properties);
    }

    void setup_tokenizer(const std::filesystem::path& models_path, const ov::AnyMap& properties) {
        ScopedVar env_manager(tokenizers_relative_to_genai());
        auto core = get_core_singleton();

        OPENVINO_ASSERT(models_path.extension() != ".xml", "'models_path' parameter should be a path to a dir not a xml file");

        std::shared_ptr<ov::Model> ov_tokenizer = nullptr;
        std::shared_ptr<ov::Model> ov_detokenizer = nullptr;
        auto [filtered_properties, enable_save_ov_model] = utils::extract_gguf_properties(properties);
        // Pass no addtional properties to tokenizer/detokenizer models since it was not used by default
        filtered_properties = {};
        if (is_gguf_model(models_path)) {
            std::map<std::string, GGUFMetaData> tokenizer_config{};
            std::filesystem::path ov_tokenizer_filesystem_path;
#ifdef _WIN32
            const wchar_t* ov_tokenizer_path_w = _wgetenv(ScopedVar::ENVIRONMENT_VARIABLE_NAME_W);
            ov_tokenizer_filesystem_path = std::filesystem::path(std::wstring(ov_tokenizer_path_w));
#else
            const char* ov_tokenizer_path = getenv(ScopedVar::ENVIRONMENT_VARIABLE_NAME);
            ov_tokenizer_filesystem_path = std::filesystem::path(ov_tokenizer_path);
#endif
            m_shared_object_ov_tokenizers = load_shared_object(ov_tokenizer_filesystem_path);
            std::tie(ov_tokenizer, ov_detokenizer, tokenizer_config) =
                create_tokenizer_from_config(m_shared_object_ov_tokenizers, models_path);

            if (auto val = get_if_exist<ov::Tensor>(tokenizer_config, "padding_token_id")) {
                m_pad_token_id = static_cast<int64_t>((*val).data<uint32_t>()[0]);
            }
            if (auto val = get_if_exist<ov::Tensor>(tokenizer_config, "bos_token_id")) {
                m_bos_token_id = static_cast<int64_t>((*val).data<uint32_t>()[0]);
            }
            if (auto val = get_if_exist<ov::Tensor>(tokenizer_config, "eos_token_id")) {
                m_eos_token_id = static_cast<int64_t>((*val).data<uint32_t>()[0]);
            }
            if (auto val = get_if_exist<std::string>(tokenizer_config, "chat_template")) {
                m_chat_template = *val;
            }            
            if (!m_chat_template.empty()) {
                m_chat_template = patch_gguf_chat_template(m_chat_template);
            }

            if (enable_save_ov_model){
                std::filesystem::path gguf_model_path(models_path);
                std::filesystem::path save_ov_tokenizer_path = gguf_model_path.parent_path() / "openvino_tokenizer.xml";
                std::filesystem::path save_ov_detokenizer_path = gguf_model_path.parent_path() / "openvino_detokenizer.xml";
                ov_tokenizer->set_rt_info(m_pad_token_id, "pad_token_id");
                ov_tokenizer->set_rt_info(m_bos_token_id, "bos_token_id");
                ov_tokenizer->set_rt_info(m_eos_token_id, "eos_token_id");
                ov_tokenizer->set_rt_info(m_chat_template, "chat_template");

                ov_detokenizer->set_rt_info(m_pad_token_id, "pad_token_id");
                ov_detokenizer->set_rt_info(m_bos_token_id, "bos_token_id");
                ov_detokenizer->set_rt_info(m_eos_token_id, "eos_token_id");
                ov_detokenizer->set_rt_info(m_chat_template, "chat_template");

                ov::genai::utils::save_openvino_model(ov_tokenizer, save_ov_tokenizer_path.string(), false);
                ov::genai::utils::save_openvino_model(ov_detokenizer, save_ov_detokenizer_path.string(), false);
            }

            setup_tokenizer(std::make_pair(ov_tokenizer, ov_detokenizer), filtered_properties);
            return;
        }
        if (std::filesystem::exists(models_path / "openvino_tokenizer.xml")) {
            ov_tokenizer = core.read_model(models_path / "openvino_tokenizer.xml", {}, filtered_properties);
        }

        if (std::filesystem::exists(models_path / "openvino_detokenizer.xml")) {
            ov_detokenizer = core.read_model(models_path / "openvino_detokenizer.xml", {}, filtered_properties);
        }

        read_config(models_path);
        read_special_tokens_map(models_path);
        // Try to read tokenizer_config if some token ids or token str are not defined.
        read_tokenizer_config_if_necessary(models_path);
        parse_chat_template_from_file(models_path / "tokenizer_config.json", m_chat_template);
        parse_chat_template_from_file(models_path / "processor_config.json", m_chat_template);
        parse_chat_template_from_file(models_path / "chat_template.json", m_chat_template);
        setup_tokenizer(std::make_pair(ov_tokenizer, ov_detokenizer), filtered_properties);
    }

    void setup_tokenizer(const std::pair<std::shared_ptr<ov::Model>, std::shared_ptr<ov::Model>>& models, const ov::AnyMap& properties) {
        auto [ov_tokenizer, ov_detokenizer] = models;

        // temporary allow absense both tokenizer and detokenizer for GGUF support
        // TODO: remove this code once Tokenizers can be created from GGUF file
        if (!ov_tokenizer && !ov_detokenizer) {
            return;
        }

        OPENVINO_ASSERT(ov_tokenizer || ov_detokenizer, "Neither tokenizer nor detokenzier models were provided");

        auto core = get_core_singleton();
        std::string device = "CPU";  // only CPU is supported for now

        // Saving IR version was added only in 24.5, so if it's missing, then it's older than 24.5
        m_older_than_24_5 = !(ov_tokenizer ? ov_tokenizer : ov_detokenizer)->has_rt_info("openvino_tokenizers_version");

        if (ov_tokenizer) {
            ov::pass::Manager manager;
            manager.register_pass<MakeAddSpecialTokensSatateful>();
            manager.register_pass<MakePaddingSatateful>();
            manager.run_passes(ov_tokenizer);
            ov::CompiledModel tokenizer = core.compile_model(ov_tokenizer, device, properties);
            ov::genai::utils::print_compiled_model_properties(tokenizer, "OV Tokenizer");

            m_ireq_queue_tokenizer = std::make_unique<CircularBufferQueue<ov::InferRequest>>(
                tokenizer.get_property(ov::optimal_number_of_infer_requests),
                [&tokenizer]() -> ov::InferRequest {
                    return tokenizer.create_infer_request();
                });

            const ov::AnyMap& rt_info = ov_tokenizer->get_rt_info();
            m_pad_token_id = find_or_fallback(rt_info, "pad_token_id", m_pad_token_id);
            m_bos_token_id = find_or_fallback(rt_info, "bos_token_id", m_bos_token_id);
            m_eos_token_id = find_or_fallback(rt_info, "eos_token_id", m_eos_token_id);

            parse_chat_template_from_tokenizer(ov_tokenizer, m_chat_template);

            std::optional<std::string> fallback = remap_template(m_chat_template);
            if (fallback.has_value()) {
                m_chat_template = std::move(fallback).value();
            } else {
                m_chat_template = find_or_fallback(rt_info, "simplified_chat_template", m_chat_template);
            }
            m_chat_template = patch_template(std::move(m_chat_template));
            // Initialize tokenizer's cache to save time later.
            // TODO CVS-150630: Empty strings sporadically can fail, therefore use nonempty string for warmup.
            encode("non empty string");
        }

        if (ov_detokenizer) {
            ov::pass::Manager manager_detok;
            manager_detok.register_pass<MakeVocabDecoderSatateful>();
            manager_detok.run_passes(ov_detokenizer);
            ov::CompiledModel detokenizer = core.compile_model(ov_detokenizer, device, properties);
            ov::genai::utils::print_compiled_model_properties(detokenizer, "OV Detokenizer");

            m_ireq_queue_detokenizer = std::make_unique<CircularBufferQueue<ov::InferRequest>>(
                detokenizer.get_property(ov::optimal_number_of_infer_requests),
                [&detokenizer]() -> ov::InferRequest {
                    return detokenizer.create_infer_request();
                });

            // Unset/-1 token causes exception in SentencePiece detokenization.
            if (m_pad_token_id != -1 && m_pad_token.empty())
                m_pad_token = decode(std::vector{m_pad_token_id}, {ov::genai::skip_special_tokens(false)});
            if (m_bos_token_id != -1 && m_bos_token.empty())
                m_bos_token = decode(std::vector{m_bos_token_id}, {ov::genai::skip_special_tokens(false)});
            if (m_eos_token_id != -1 && m_eos_token.empty())
                m_eos_token = decode(std::vector{m_eos_token_id}, {ov::genai::skip_special_tokens(false)});
            // Initialize detokenizer's cache to save time later.
            decode({1, 33, 199, 42, 42});

            m_vocab = read_vocab_from_detokenizer_model(ov_detokenizer);
        }
    }

    // load special tokens ids from config.json
    void read_config(const std::filesystem::path& tokenizer_path) {
        auto config_file_path = tokenizer_path / "config.json";
        if (!std::filesystem::exists(config_file_path))
            return;
        std::ifstream file(config_file_path);

        nlohmann::json data = nlohmann::json::parse(file);
        using ov::genai::utils::read_json_param;

        read_json_param(data, "pad_token_id", m_pad_token_id);
        read_json_param(data, "bos_token_id", m_bos_token_id);
        read_json_param(data, "eos_token_id", m_eos_token_id);
    }

    // Reads the string representation of special tokens if they exist.
    void read_special_tokens_map(const std::filesystem::path& tokenizer_path) {
        auto special_tokens_file_path = tokenizer_path / "special_tokens_map.json";
        if (!std::filesystem::exists(special_tokens_file_path))
            return;
        std::ifstream f(special_tokens_file_path);

        nlohmann::json data = nlohmann::json::parse(f);

        // they are in the format {"bos_token": { "content": "<s>",... }}
        auto read_token_content_str = [&data](const std::string& key_name, std::string& val) {
            if (val.empty() && data.contains(key_name)) {
                utils::read_json_param(data[key_name], "content", val);
            }
        };
        read_token_content_str(pad_token_key_name, m_pad_token);
        read_token_content_str(bos_token_key_name, m_bos_token);
        read_token_content_str(eos_token_key_name, m_eos_token);
    }

    // Read string representation of special tokens if they exist.
    // Also tries to load special token ids from added_tokens_decoder if they exist.
    // Will not override special token strings or ids if they already exist.
    void read_tokenizer_config_if_necessary(const std::filesystem::path& tokenizer_path) {
        if (m_pad_token_id != -1 && m_bos_token_id != -1 && m_eos_token_id != -1 &&
            !m_pad_token.empty() && !m_bos_token.empty() && !m_eos_token.empty()) {
            return ;
        }

        auto tokenizer_config_file_path = tokenizer_path / "tokenizer_config.json";
        if (!std::filesystem::exists(tokenizer_config_file_path))
            return;
        std::ifstream f(tokenizer_config_file_path);

        nlohmann::json data = nlohmann::json::parse(f);

        // read special tokens string representation
        // if they are presented directly {"bos_token": "<bos>"}
        using ov::genai::utils::read_json_param;
        auto read_token_str = [&data](std::string key_name, std::string& val) {
            if (val.empty()) { read_json_param(data, key_name, val); }
        };
        read_token_str(pad_token_key_name, m_pad_token);
        read_token_str(bos_token_key_name, m_bos_token);
        read_token_str(eos_token_key_name, m_eos_token);

        // if special tokens are not loaded directly, try to read
        // if they are in the format {"bos_token": { "content": "<s>",... }}
        auto read_token_content_str = [&data](std::string key_name, std::string& val) {
            if (val.empty() && data.contains(key_name)) { read_json_param(data[key_name], "content", val); }
        };
        read_token_content_str(pad_token_key_name, m_pad_token);
        read_token_content_str(bos_token_key_name, m_bos_token);
        read_token_content_str(eos_token_key_name, m_eos_token);

        // if pad_token not found use eos_token as pad_token
        if (m_pad_token.empty() && !m_eos_token.empty()) {
            m_pad_token = m_eos_token;
        }

        // special token ids integer representation are already defined
        if (m_pad_token_id != -1 && m_bos_token_id != -1 && m_eos_token_id != -1)
            return;

        // values are stored as {"added_tokens_decoder": {"0": {"content": "<pad>"}}}
        // token id is a key in the form of a string, need to do std::stoi
        std::string spec_tokens_key_name = "added_tokens_decoder";
        if (!data.contains(spec_tokens_key_name))
            return;

        // if added_tokens_decoder has different format items() will not fail
        for (auto& [key, value] : data[spec_tokens_key_name].items()) {
            if (!value.contains("content"))
                continue;
            auto content = value["content"];
            if (m_pad_token_id == -1 && content == m_pad_token)
                m_pad_token_id = std::stoi(key);
            if (m_bos_token_id == -1 && content == m_bos_token)
                m_bos_token_id = std::stoi(key);
            if (m_eos_token_id == -1 && content == m_eos_token)
                m_eos_token_id = std::stoi(key);
        }

        // if pad_token_id not found use eos_token_id as pad_token_id
        // todo: read m_pad_token_id from tokenizer rt_info once implemented in tokenizers (CVS-144174)
        if (m_pad_token_id == -1 && m_eos_token_id != -1) {
            m_pad_token_id = m_eos_token_id;
        }
    }

    // tokenize str representation to get special tokens integer values
    void infer_special_tokens_if_necessary() {
        auto get_id_from_str = [this](std::string token_str, int64_t& token_val) {
            if (token_val != -1 || token_str.empty())
                return;
            auto token_ids_tensor = this->encode(token_str).input_ids;
            auto data = token_ids_tensor.data<int64_t>();
            auto data_len = token_ids_tensor.get_shape()[1];
            token_val = data[data_len - 1];
        };
        get_id_from_str(m_pad_token, m_pad_token_id);
        get_id_from_str(m_bos_token, m_bos_token_id);
        get_id_from_str(m_eos_token, m_eos_token_id);
    }

    TokenizedInputs encode(const std::string& prompt, const ov::AnyMap& tokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_tokenizer, "Either openvino_tokenizer.xml was not provided or it was not loaded correctly. "
                                                "Tokenizer::encode is not available");

        CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(m_ireq_queue_tokenizer.get());
        set_state_if_necessary(infer_request_guard, tokenization_params);
        size_t batch_size = 1;
        // When the model has more than one input, calling set_input_tensor without specifying an index may fail.
        // If the model has two inputs, explicitly set the first input while leaving the second input tensor empty.
        // The subgraph within the ov::Model will handle this scenario, ensuring the output remains correct.
       infer_request_guard.get().set_input_tensor(0, ov::Tensor{ov::element::string, {batch_size}, const_cast<std::string*>(&prompt)});

        if (infer_request_guard.get().get_compiled_model().inputs().size() > 1) {
            // Set the second input tensor to an empty tensor to avoid errors.
            // The subgraph within the ov::Model will handle this scenario, ensuring the output remains correct.
            infer_request_guard.get().set_input_tensor(1, ov::Tensor{ov::element::string, {0}});
        }

        infer_request_guard.get().infer();

        return get_copied_results(
            infer_request_guard.get().get_tensor("input_ids"),
            infer_request_guard.get().get_tensor("attention_mask")
        );
    }

    TokenizedInputs encode(const std::vector<std::pair<std::string, std::string>>& prompts_pairs, const ov::AnyMap& tokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_tokenizer, "Either openvino_tokenizer.xml was not provided or it was not loaded correctly. "
                                                "Tokenizer::encode is not available");
        size_t batch_size = prompts_pairs.size();
        std::vector<std::string> prompts_1(batch_size);
        std::vector<std::string> prompts_2(batch_size);

        for (size_t i = 0; i < batch_size; ++i) {
            prompts_1[i] = prompts_pairs[i].first;
            prompts_2[i] = prompts_pairs[i].second;
        }
        return encode(prompts_1, prompts_2, tokenization_params);
    }

    TokenizedInputs encode(const std::vector<std::string>& prompts_1, const std::vector<std::string>& prompts_2, const ov::AnyMap& tokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_tokenizer, "Either openvino_tokenizer.xml was not provided or it was not loaded correctly. "
                                                "Tokenizer::encode is not available");
        OPENVINO_ASSERT(prompts_1.size() == prompts_2.size() || prompts_1.size() == 1 || prompts_2.size() == 1,
                        "prompts_1 and prompts_2 should be of the same size or one of them should be of size 1");

        TokenizedInputs result;
        {
            CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(this->m_ireq_queue_tokenizer.get());
            set_state_if_necessary(infer_request_guard, tokenization_params);
            infer_request_guard.get().set_input_tensor(0, ov::Tensor{ov::element::string, {prompts_1.size()}, const_cast<std::string*>(prompts_1.data())});
            infer_request_guard.get().set_input_tensor(1, ov::Tensor{ov::element::string, {prompts_2.size()}, const_cast<std::string*>(prompts_2.data())});

            infer_request_guard.get().infer();

            result = get_copied_results(
                infer_request_guard.get().get_tensor("input_ids"),
                infer_request_guard.get().get_tensor("attention_mask")
            );

            // If the model has a token_type_ids output, copy it to the result
            for (const auto& output : infer_request_guard.get().get_compiled_model().outputs()) {
                if (output.get_any_name() != "token_type_ids") {
                    continue;
                }
                ov::Tensor token_type_ids = infer_request_guard.get().get_tensor(output);
                ov::Tensor token_type_ids_copy = ov::Tensor(token_type_ids.get_element_type(), token_type_ids.get_shape());
                token_type_ids.copy_to(token_type_ids_copy);
                result.token_type_ids = token_type_ids_copy;
            }
        }
        return {result.input_ids, result.attention_mask, result.token_type_ids};
    }

    TokenizedInputs encode(const std::vector<std::string>& prompts, const ov::AnyMap& tokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_tokenizer, "Either openvino_tokenizer.xml was not provided or it was not loaded correctly. "
                                                "Tokenizer::encode is not available");

        TokenizedInputs unpadded;
        {
            CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(this->m_ireq_queue_tokenizer.get());
            set_state_if_necessary(infer_request_guard, tokenization_params);
            // When the model has more than one input, calling set_input_tensor without specifying an index may fail.
            // If the model has two inputs, explicitly set the first input while leaving the second input tensor empty.
            // The subgraph within the ov::Model will handle this scenario, ensuring the output remains correct.
            // The use of const_cast here is necessary because the ov::Tensor API does not accept const data pointers.
            // The prompts vector is not declared as const, and the underlying data is not modified by this operation.
            infer_request_guard.get().set_input_tensor(0, ov::Tensor{ov::element::string, {prompts.size()}, const_cast<std::string*>(prompts.data())});
            if (infer_request_guard.get().get_compiled_model().inputs().size() > 1) {
                // Set the second input tensor to an empty tensor to avoid errors.
                // The subgraph within the ov::Model will handle this scenario, ensuring the output remains correct.
                infer_request_guard.get().set_input_tensor(1, ov::Tensor{ov::element::string, {0}});
            }
            infer_request_guard.get().infer();

            unpadded = get_copied_results(
                infer_request_guard.get().get_tensor("input_ids"),
                infer_request_guard.get().get_tensor("attention_mask")
            );
        }

        return {unpadded.input_ids, unpadded.attention_mask};
    }

    TokenizedInputs get_copied_results(ov::Tensor input_ids, ov::Tensor attention_mask) {
        ov::Tensor input_ids_ = ov::Tensor(input_ids.get_element_type(), input_ids.get_shape());
        ov::Tensor attention_mask_ = ov::Tensor(attention_mask.get_element_type(), attention_mask.get_shape());
        input_ids.copy_to(input_ids_);
        attention_mask.copy_to(attention_mask_);

        return {input_ids_, attention_mask_};
    }

    std::string decode(const std::vector<int64_t>& tokens, const ov::AnyMap& detokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_detokenizer, "Detokenizer model has not been provided. Tokenizer::decode is not available");

        CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(this->m_ireq_queue_detokenizer.get());
        set_state_if_necessary(infer_request_guard, detokenization_params);
        size_t batch_size = 1;
        infer_request_guard.get().set_input_tensor(ov::Tensor{ov::element::i64, {batch_size, tokens.size()}, const_cast<int64_t*>(tokens.data())});
        infer_request_guard.get().infer();
        return infer_request_guard.get().get_output_tensor().data<std::string>()[0];
    }

    std::vector<std::string> decode(const ov::Tensor& tokens, const ov::AnyMap& detokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_detokenizer, "Detokenizer model has not been provided. Tokenizer::decode is not available");
        OPENVINO_ASSERT(tokens.get_element_type() == ov::element::i64, "tokens tensor element type should be an i64");
        OPENVINO_ASSERT(tokens.get_shape().size() == 2, "tokens tensor should of rank 2 with shape [batch_size, seq_len]");

        CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(this->m_ireq_queue_detokenizer.get());
        set_state_if_necessary(infer_request_guard, detokenization_params);
        infer_request_guard.get().set_input_tensor(tokens);
        infer_request_guard.get().infer();

        auto res = infer_request_guard.get().get_output_tensor();
        auto res_data = res.data<std::string>();
        return std::vector<std::string>(res_data, res_data + res.get_shape()[0]);
    }

    std::vector<std::string> decode(const std::vector<std::vector<int64_t>>& lines, const ov::AnyMap& detokenization_params = {}) {
        OPENVINO_ASSERT(m_ireq_queue_detokenizer, "Detokenizer model has not been provided. Tokenizer::decode is not available");

        auto compare_lengths = [](const std::vector<int64_t>& a, const std::vector<int64_t>& b) {
            return a.size() < b.size();
        };
        size_t max_len = std::max_element(lines.begin(), lines.end(), compare_lengths)->size();

        ov::Tensor tokens = ov::Tensor{ov::element::i64, {lines.size(), max_len}};
        auto tokens_data = tokens.data<int64_t>();

        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            size_t line_len = line.size();
            std::copy(line.begin(), line.end(), tokens_data + i * max_len);
            std::fill(tokens_data + i * max_len + line_len, tokens_data + (i + 1) * max_len, m_pad_token_id);
        }

        CircularBufferQueueElementGuard<ov::InferRequest> infer_request_guard(this->m_ireq_queue_detokenizer.get());
        set_state_if_necessary(infer_request_guard, detokenization_params);
        infer_request_guard.get().set_input_tensor(tokens);
        infer_request_guard.get().infer();
        auto res = infer_request_guard.get().get_output_tensor();
        auto res_data = res.data<std::string>();
        return std::vector<std::string>(res_data, res_data + res.get_shape()[0]);
    }

    std::string apply_chat_template(ChatHistory history,
                                    bool add_generation_prompt,
                                    const std::string& chat_template) const {
        std::string chat_tpl = chat_template.empty() ? m_chat_template : remap_and_patch(chat_template);
        OPENVINO_ASSERT(!chat_tpl.empty(),
                        "Chat template wasn't found. This may indicate that the model wasn't trained for chat scenario."
                        " Please add 'chat_template' to tokenizer_config.json to use the model in chat scenario."
                        " For more information see the section Troubleshooting in README.md");
        jinja2::TemplateEnv env;
        env.GetSettings().lstripBlocks = true;
        env.GetSettings().trimBlocks = true;
        jinja2::Template tpl(&env);
        tpl.Load(chat_tpl);

        jinja2::UserCallable slice_callable = jinja2::MakeCallable(
            [](const jinja2::GenericList& messages, const size_t& start) {
                jinja2::ValuesList result;

                size_t iter_num = 0;
                for (auto message = messages.begin(); message != messages.end(); message++, iter_num++) {
                    if (iter_num < start)
                        continue;
                    result.emplace_back(*message);
                }

                return result;
            },
            jinja2::ArgInfo{"messages"}, jinja2::ArgInfo{"start"}
        );

        jinja2::ValuesList jinja_messages;
        jinja2::ValuesMap jinja_message;
        for (const auto& message : history) {
            jinja_message = {{"role", message.at("role")}, {"content", message.at("content")}};
            jinja_messages.emplace_back(jinja_message);
        }

        jinja2::ValuesMap params = {
            {"messages", jinja_messages},
            {"bos_token", m_bos_token},
            {"eos_token", m_eos_token},
            {"pad_token", m_pad_token},
            {"add_generation_prompt", add_generation_prompt},
            {"slice", slice_callable},
        };

        std::string result;
        try {
            result = tpl.RenderAsString(params).value();
        } catch (const std::exception& error) {
            OPENVINO_THROW("Jinja2Cpp failed to apply chat template. Possible solutions are\n"
                           "* Provide a simplified chat template with set_chat_template().\n"
                           "* Set apply_chat_template to false in GenerationConfig. "
                           "It's possible to apply the template manually to your prompt before calling generate. "
                           "For example: <|user|>\\n{prompt}</s>\\n<|assistant|>\\n\n"
                           "Jinja2Cpp's error: ", error.what());
        }
        OPENVINO_ASSERT(!result.empty(), "Applied chat template resulted in an empty string. "
                                         "Please check the chat template or apply template manually to your prompt before calling generate."
                                         "For example: <start_of_turn>user{user_prompt}<end_of_turn><start_of_turn>model");
        return result;
    }

    void set_chat_template(const std::string& chat_template) {
        m_chat_template = remap_and_patch(chat_template);
    }

    std::string get_chat_template() {
        return m_chat_template;
    }
};

Tokenizer::Tokenizer(const std::filesystem::path& tokenizer_path, const ov::AnyMap& properties) {
    m_pimpl = std::make_shared<TokenizerImpl>(tokenizer_path, properties);
}

Tokenizer::Tokenizer(
    const std::string& tokenizer_model_str,
    const ov::Tensor& tokenizer_weights_tensor,
    const std::string& detokenizer_model_str,
    const ov::Tensor&  detokenizer_weights_tensor,
    const ov::AnyMap& properties
) {
    ScopedVar env_manager(tokenizers_relative_to_genai());
    auto core = get_core_singleton();

    auto ov_tokenizer = core.read_model(tokenizer_model_str, tokenizer_weights_tensor);
    auto ov_detokenizer = core.read_model(detokenizer_model_str, detokenizer_weights_tensor);
    m_pimpl = std::make_shared<TokenizerImpl>(std::make_pair(ov_tokenizer, ov_detokenizer), properties);
}

Tokenizer::Tokenizer(const std::string& model_str, ov::Tensor& weights_tensor, const ov::AnyMap& properties) {
    ScopedVar env_manager(tokenizers_relative_to_genai());
    auto core = get_core_singleton();
    auto model = core.read_model(model_str, weights_tensor);

    auto parameters = model->get_parameters();
    OPENVINO_ASSERT(!parameters.empty());
    if (parameters.front()->get_element_type() == ov::element::string) {
        // It's a tokenizer
        m_pimpl = std::make_shared<TokenizerImpl>(std::make_pair(model, nullptr), properties);
    } else {
        // It's a detokenizer
        m_pimpl = std::make_shared<TokenizerImpl>(std::make_pair(nullptr, model), properties);
    }
}

TokenizedInputs Tokenizer::encode(const std::string& prompt, const ov::AnyMap& tokenization_params) {
    check_arguments(tokenization_params, {ov::genai::add_special_tokens.name(), ov::genai::max_length.name(), ov::genai::pad_to_max_length.name()});
    return m_pimpl->encode(std::move(prompt), tokenization_params);
}

TokenizedInputs Tokenizer::encode(const std::vector<std::pair<std::string, std::string>>& prompts, const ov::AnyMap& tokenization_params) {
    check_arguments(tokenization_params, {ov::genai::add_special_tokens.name(), ov::genai::max_length.name(), ov::genai::pad_to_max_length.name()});
    return m_pimpl->encode(prompts, tokenization_params);
}

TokenizedInputs Tokenizer::encode(const std::vector<std::string>& prompts_1, const std::vector<std::string>& prompts_2, const ov::AnyMap& tokenization_params) {
    check_arguments(tokenization_params, {ov::genai::add_special_tokens.name(), ov::genai::max_length.name(), ov::genai::pad_to_max_length.name()});
    return m_pimpl->encode(prompts_1, prompts_2, tokenization_params);
}

TokenizedInputs Tokenizer::encode(const std::vector<std::string>& prompts, const ov::AnyMap& tokenization_params) {
    check_arguments(tokenization_params, {ov::genai::add_special_tokens.name(), ov::genai::max_length.name(), ov::genai::pad_to_max_length.name()});
    return m_pimpl->encode(prompts, tokenization_params);
}

TokenizedInputs Tokenizer::encode(const std::initializer_list<std::string>& text, const ov::AnyMap& tokenization_params) {
    check_arguments(tokenization_params, {ov::genai::add_special_tokens.name(), ov::genai::max_length.name(), ov::genai::pad_to_max_length.name()});
    return encode(std::vector<std::string>(text.begin(), text.end()), tokenization_params);
}

std::string Tokenizer::decode(const std::vector<int64_t>& tokens, const ov::AnyMap& detokenization_params) {
    check_arguments(detokenization_params, {ov::genai::skip_special_tokens.name()});
    return m_pimpl->decode(tokens, detokenization_params);
}

std::vector<std::string> Tokenizer::decode(const ov::Tensor& tokens, const ov::AnyMap& detokenization_params) {
    check_arguments(detokenization_params, {ov::genai::skip_special_tokens.name()});
    return m_pimpl->decode(tokens, detokenization_params);
}

std::vector<std::string> Tokenizer::decode(const std::vector<std::vector<int64_t>>& lines, const ov::AnyMap& detokenization_params) {
    check_arguments(detokenization_params, {ov::genai::skip_special_tokens.name()});
    return m_pimpl->decode(lines, detokenization_params);
}

int64_t Tokenizer::get_bos_token_id() const {
    return m_pimpl->m_bos_token_id;
}

int64_t Tokenizer::get_eos_token_id() const {
    return m_pimpl->m_eos_token_id;
}

int64_t Tokenizer::get_pad_token_id() const {
    return m_pimpl->m_pad_token_id;
}

std::string Tokenizer::get_pad_token() const {
    return m_pimpl->m_pad_token;
}

std::string Tokenizer::get_bos_token() const {
    return m_pimpl->m_bos_token;
}

std::string Tokenizer::get_eos_token() const {
    return m_pimpl->m_eos_token;
}

std::string Tokenizer::apply_chat_template(ChatHistory history,
                                           bool add_generation_prompt,
                                           const std::string& chat_template) const {
    return m_pimpl->apply_chat_template(history, add_generation_prompt, chat_template);
}

std::string Tokenizer::get_chat_template() const {
    return m_pimpl->get_chat_template();
}

void Tokenizer::set_chat_template(const std::string& chat_template) {
    m_pimpl->set_chat_template(chat_template);
}

Vocab Tokenizer::get_vocab() const {
    const auto& vocab_vector = get_vocab_vector();

    Vocab vocab;
    vocab.reserve(vocab_vector.size());
    for (size_t i = 0; i < vocab_vector.size(); ++i) {
        vocab[vocab_vector[i]] = i;
    }
    return vocab;
}

const std::vector<std::string>& Tokenizer::get_vocab_vector() const {
    OPENVINO_ASSERT(!m_pimpl->m_vocab.empty(), "Tokenizer vocab is empty. Please check if the detokenizer model was provided and loaded correctly.");
    return m_pimpl->m_vocab;
}

Tokenizer::~Tokenizer() {
    m_pimpl.reset();

    // release CPU plugin ()
    try {
        get_core_singleton().unload_plugin("CPU");
    } catch (const ov::Exception&) {
        // Note: in a theory it can throw an exception when 2 different Tokenizers are created from
        // different threads and then both of them unload plugin for 'device' from ov::Core
    }
}

}  // namespace genai
}  // namespace ov
