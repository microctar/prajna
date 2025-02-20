#pragma once

#include <boost/algorithm/string.hpp>
#include <unordered_map>

#include "fmt/color.h"
#include "fmt/format.h"
#include "prajna/assert.hpp"
#include "prajna/ast/ast.hpp"
#include "prajna/compiler/stdio_callback.h"
#include "prajna/exception.hpp"
#include "prajna/rich_bash.hpp"

namespace prajna {

enum struct LogLevel {
    info = 1,
    warning,
    error,
};
}

namespace fmt {

// @ref https://fmt.dev/latest/api.html#formatting-user-defined-types
template <>
struct formatter<prajna::LogLevel> {
    constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
        auto it = ctx.begin(), end = ctx.end();
        if (it != end && *it != '}') throw format_error("invalid format");
        return it;
    }

    template <typename FormatContext>
    auto format(const prajna::LogLevel& level, FormatContext& ctx) const -> decltype(ctx.out()) {
        // ctx.out() is an output iterator to write to.
        switch (level) {
            case prajna::LogLevel::info:
                return fmt::format_to(ctx.out(), "{}",
                                      fmt::styled("info", fmt::fg(fmt::color::gray)));
            case prajna::LogLevel::warning:
                return fmt::format_to(ctx.out(), "{}",
                                      fmt::styled("warning", fmt::fg(fmt::color::orange)));
            case prajna::LogLevel::error:
                return fmt::format_to(ctx.out(), "{}",
                                      fmt::styled("error", fmt::fg(fmt::color::red)));
        }

        return ctx.out();
    }
};

}  // namespace fmt

namespace prajna {

class Logger {
   protected:
    Logger() = default;

   public:
    static std::shared_ptr<Logger> create(std::string code) {
        std::shared_ptr<Logger> self(new Logger);
        boost::split(self->_code_lines, code, boost::is_any_of("\n"));
        return self;
    }

    void log(std::string message, ast::SourcePosition first_position,
             ast::SourcePosition last_position, std::string log_level,
             std::string locator_ascii_color, bool throw_error) {
        std::string what_message;
        if (first_position.file.empty()) {
            what_message = fmt::format("{}:{}: {}: {}\n", first_position.line,
                                       first_position.column, log_level, message);
        } else {
            what_message =
                fmt::format("{}:{}:{}: {}: {}\n", first_position.file, first_position.line,
                            first_position.column, log_level, message);
        }

        // 确保位置信息是有效的
        PRAJNA_ASSERT(first_position.line >= 0);
        PRAJNA_ASSERT(last_position.line >= 0);

        std::vector<std::string> code_lines;
        for (size_t i = first_position.line; i <= last_position.line; ++i) {
            code_lines.push_back(_code_lines[i - 1]);
        }

        if (last_position.column - 1 >
            code_lines[last_position.line - first_position.line].size()) {
            std::string spaces(last_position.column - 1 -
                                   code_lines[last_position.line - first_position.line].size(),
                               ' ');
            code_lines[last_position.line - first_position.line].append(spaces);
        }
        code_lines[last_position.line - first_position.line].insert(last_position.column - 1,
                                                                    std::string(RESET));
        // 不存在越界的情况
        PRAJNA_ASSERT(first_position.column - 1 >= 0);
        code_lines[0].insert(first_position.column - 1, locator_ascii_color);
        std::string code_region;
        for (auto code_line : code_lines) {
            code_region.append(code_line);
            code_region.append("\n");
        }

        print_callback(what_message);
        print_callback(code_region);

        if (throw_error) {
            throw CompileError();
        }
    }

    void error(std::string message, ast::SourcePosition first_position,
               ast::SourcePosition last_position, std::string locator_ascii_color) {
        auto error_prompt = fmt::format("{}", fmt::styled("error", fmt::fg(fmt::color::red)));
        this->log(message, first_position, last_position, error_prompt, locator_ascii_color, true);
    }

    void error(std::string message) {
        fmt::print("{}: {}\n", fmt::styled("error", fmt::fg(fmt::color::red)), message);
    }

    void error(std::string message, ast::SourcePosition first_position) {
        auto last_position = first_position;
        last_position.column = first_position.column + 1;
        error(message, first_position, last_position, std::string(BLU));
    }

    void error(std::string message, ast::SourceLocation source_location) {
        error(message, source_location.first_position, source_location.last_position,
              std::string(BLU));
    }

    void error(std::string message, ast::Operand ast_operand) {
        boost::apply_visitor([=](auto x) { this->error(message, x); }, ast_operand);
    }

    void warning(std::string message, ast::SourceLocation source_location) {
        auto warning_prompt =
            fmt::format("{}", fmt::styled("warning", fmt::fg(fmt::color::orange)));
        this->log(message, source_location.first_position, source_location.last_position,
                  warning_prompt, std::string(BLU), false);
    }

    void warning(std::string message, ast::Operand ast_operand) {
        boost::apply_visitor([=](auto x) { this->warning(message, x); }, ast_operand);
    }

    void note(ast::SourceLocation source_location) {
        auto warning_prompt = fmt::format("{}", fmt::styled("note", fmt::fg(fmt::color::gray)));
        this->log("", source_location.first_position, source_location.last_position, warning_prompt,
                  std::string(""), false);
    }

   private:
    std::vector<std::string> _code_lines;
};

}  // namespace prajna
