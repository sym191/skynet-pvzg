#include "LogService.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace skynet::runtime {
namespace {

struct ConfigValue {
    std::string text;
};

using ConfigValues = std::unordered_map<std::string, ConfigValue>;

[[nodiscard]] std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end &&
        std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string(begin, end);
}

[[nodiscard]] std::string strip_comment(std::string_view line) {
    bool in_quote = false;
    char quote = '\0';
    bool escaping = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char current = line[i];
        if (escaping) {
            escaping = false;
            continue;
        }
        if (in_quote) {
            if (current == '\\') {
                escaping = true;
            } else if (current == quote) {
                in_quote = false;
            }
            continue;
        }
        if (current == '"' || current == '\'') {
            in_quote = true;
            quote = current;
            continue;
        }
        if (current == '-' && line[i + 1] == '-') {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>>
parse_assignment(std::string_view line) {
    const std::string clean_line = trim(strip_comment(line));
    if (clean_line.empty()) {
        return std::nullopt;
    }

    const std::size_t equal_pos = clean_line.find('=');
    if (equal_pos == std::string::npos) {
        return std::nullopt;
    }

    std::string key = trim(std::string_view(clean_line).substr(0, equal_pos));
    if (key.empty()) {
        return std::nullopt;
    }

    const bool valid_key = std::ranges::all_of(key, [](unsigned char value) {
        return std::isalnum(value) || value == '_';
    });
    if (!valid_key) {
        return std::nullopt;
    }

    std::string value = trim(std::string_view(clean_line).substr(equal_pos + 1));
    return std::pair<std::string, std::string>{
            std::move(key),
            std::move(value),
    };
}

[[nodiscard]] std::expected<ConfigValues, std::error_code> read_config_values(
        const std::filesystem::path& startup_config_path) {
    std::ifstream input(startup_config_path);
    if (!input) {
        return std::unexpected(make_error_code(RuntimeErrc::config_open_failed));
    }

    ConfigValues values;
    std::string line;
    while (std::getline(input, line)) {
        auto assignment = parse_assignment(line);
        if (!assignment) {
            continue;
        }
        values.insert_or_assign(
                std::move(assignment->first),
                ConfigValue{.text = std::move(assignment->second)});
    }
    return values;
}

[[nodiscard]] std::expected<std::optional<std::string>, std::error_code>
parse_optional_string(std::string_view raw_value) {
    const std::string value = trim(raw_value);
    if (value.empty() || value == "nil") {
        return std::optional<std::string>{};
    }

    if (value.front() != '"' && value.front() != '\'') {
        return value;
    }

    const char quote = value.front();
    std::string result;
    bool escaping = false;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char current = value[i];
        if (escaping) {
            switch (current) {
                case 'n':
                    result.push_back('\n');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                default:
                    result.push_back(current);
                    break;
            }
            escaping = false;
            continue;
        }
        if (current == '\\') {
            escaping = true;
            continue;
        }
        if (current == quote) {
            return result;
        }
        result.push_back(current);
    }

    return std::unexpected(make_error_code(RuntimeErrc::invalid_config_string));
}

[[nodiscard]] std::optional<std::string> find_raw(
        const ConfigValues& values,
        std::string_view key) {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
        return std::nullopt;
    }
    return it->second.text;
}

[[nodiscard]] std::expected<std::optional<std::string>, std::error_code>
find_optional_string(const ConfigValues& values, std::string_view key) {
    auto raw = find_raw(values, key);
    if (!raw) {
        return std::optional<std::string>{};
    }
    return parse_optional_string(*raw);
}

[[nodiscard]] std::expected<std::optional<bool>, std::error_code>
find_optional_bool(const ConfigValues& values, std::string_view key) {
    auto raw = find_raw(values, key);
    if (!raw) {
        return std::optional<bool>{};
    }

    std::string value = trim(*raw);
    std::ranges::transform(value, value.begin(), [](unsigned char current) {
        return static_cast<char>(std::tolower(current));
    });

    if (value == "true" || value == "1" || value == "yes" ||
            value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" ||
            value == "off" || value == "nil") {
        return false;
    }
    return std::unexpected(make_error_code(RuntimeErrc::invalid_config_boolean));
}

[[nodiscard]] std::optional<spdlog::level::level_enum> parse_level(
        std::string_view value) {
    std::string level = trim(value);
    std::ranges::transform(level, level.begin(), [](unsigned char current) {
        return static_cast<char>(std::tolower(current));
    });

    if (level == "trace") {
        return spdlog::level::trace;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "info") {
        return spdlog::level::info;
    }
    if (level == "warn" || level == "warning") {
        return spdlog::level::warn;
    }
    if (level == "err" || level == "error") {
        return spdlog::level::err;
    }
    if (level == "critical") {
        return spdlog::level::critical;
    }
    if (level == "off") {
        return spdlog::level::off;
    }
    return std::nullopt;
}

[[nodiscard]] std::filesystem::path resolve_config_path(
        const std::filesystem::path& startup_config_path,
        std::filesystem::path configured_path) {
    if (configured_path.is_absolute()) {
        return configured_path;
    }
    return startup_config_path.parent_path() / configured_path;
}

[[nodiscard]] bool is_console_logger_name(std::string_view value) {
    return value.empty() || value == "stdout" || value == "stderr" ||
        value == "console";
}

}  // namespace

std::expected<LogConfig, std::error_code> load_log_config(
        const std::filesystem::path& startup_config_path) {
    auto values = read_config_values(startup_config_path);
    if (!values) {
        return std::unexpected(values.error());
    }

    LogConfig config;

    auto logger_value = find_optional_string(*values, "logger");
    if (!logger_value) {
        return std::unexpected(logger_value.error());
    }
    if (*logger_value && !is_console_logger_name(**logger_value)) {
        config.file_path = resolve_config_path(
            startup_config_path,
            std::filesystem::path{**logger_value});
    }

    for (const std::string_view key : {"logfile", "log_file"}) {
        auto file_value = find_optional_string(*values, key);
        if (!file_value) {
            return std::unexpected(file_value.error());
        }
        if (*file_value && !(**file_value).empty()) {
            config.file_path = resolve_config_path(
                startup_config_path,
                std::filesystem::path{**file_value});
            break;
        }
    }

    auto console_value = find_optional_bool(*values, "log_console");
    if (!console_value) {
        return std::unexpected(console_value.error());
    }
    if (*console_value) {
        config.console_enabled = **console_value;
    }

    for (const std::string_view key : {"log_level", "loglevel"}) {
        auto level_value = find_optional_string(*values, key);
        if (!level_value) {
            return std::unexpected(level_value.error());
        }
        if (!*level_value) {
            continue;
        }
        auto level = parse_level(**level_value);
        if (!level) {
            return std::unexpected(make_error_code(RuntimeErrc::invalid_log_level));
        }
        config.level = *level;
        break;
    }

    auto pattern_value = find_optional_string(*values, "log_pattern");
    if (!pattern_value) {
        return std::unexpected(pattern_value.error());
    }
    if (*pattern_value && !(**pattern_value).empty()) {
        config.pattern = **pattern_value;
    }

    if (!config.console_enabled && !config.file_path) {
        return std::unexpected(make_error_code(RuntimeErrc::invalid_log_sink));
    }

    return config;
}

std::expected<std::shared_ptr<spdlog::logger>, std::error_code>
create_runtime_logger(const LogConfig& config) {
    try {
        std::vector<spdlog::sink_ptr> sinks;
        if (config.console_enabled) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
        if (config.file_path) {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    config.file_path->string(), true));
        }
        if (sinks.empty()) {
            return std::unexpected(make_error_code(RuntimeErrc::invalid_log_sink));
        }

        auto logger = std::make_shared<spdlog::logger>(
                "skynet", sinks.begin(), sinks.end());
        logger->set_level(config.level);
        logger->set_pattern(config.pattern);
        logger->flush_on(spdlog::level::warn);
        return logger;
    } catch (const std::exception& error) {
        (void)error;
        return std::unexpected(make_error_code(RuntimeErrc::logger_creation_failed));
    }
}

LogService::LogService(std::shared_ptr<spdlog::logger> logger)
        : logger_(std::move(logger)) {}

std::expected<std::unique_ptr<LogService>, std::error_code>
LogService::create_from_startup_config(
        const std::filesystem::path& startup_config_path) {
    auto config = load_log_config(startup_config_path);
    if (!config) {
        return std::unexpected(config.error());
    }

    auto logger = create_runtime_logger(*config);
    if (!logger) {
        return std::unexpected(logger.error());
    }

    spdlog::set_default_logger(*logger);
    return std::make_unique<LogService>(*logger);
}

bool LogService::init(Context& context, std::string_view args) {
    (void)context;
    if (!args.empty()) {
        info(args);
    }
    return logger_ != nullptr;
}

void LogService::dispatch(Context& context, Message msg) {
    (void)context;
    if (!logger_) {
        return;
    }
    logger_->info("[source={:08x} session={}] {}", msg.source, msg.session,
        msg.text_payload());
}

void LogService::signal(Context& context, int signal) {
    (void)context;
    if (logger_) {
        logger_->info("log service received signal {}", signal);
        logger_->flush();
    }
}

void LogService::log(spdlog::level::level_enum level, std::string_view message) {
    if (!logger_) {
        return;
    }
    logger_->log(level, "{}", message);
}

void LogService::info(std::string_view message) {
    log(spdlog::level::info, message);
}

void LogService::flush() {
    if (logger_) {
        logger_->flush();
    }
}

std::shared_ptr<spdlog::logger> LogService::logger() const noexcept {
    return logger_;
}

}  // namespace skynet::runtime
