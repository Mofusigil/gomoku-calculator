#include "server/json.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace gomoku::server::json {
namespace {

[[noreturn]] void throw_type_error(std::string_view expected) {
    throw std::logic_error("JSON value is not " + std::string(expected));
}

std::string parse_error_message(std::size_t offset, std::string_view message) {
    return "JSON parse error at byte " + std::to_string(offset) + ": " +
           std::string(message);
}

bool is_json_whitespace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool is_digit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

unsigned hex_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<unsigned>(ch - 'A' + 10);
    }
    return 16;
}

void append_code_point(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7f) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

class Parser final {
public:
    Parser(std::string_view input, ParseLimits limits)
        : input_(input), limits_(limits) {
        if (input_.size() > limits_.max_input_bytes) {
            fail_at(0, "input exceeds configured byte limit");
        }
    }

    Value parse_document() {
        skip_whitespace();
        if (at_end()) {
            fail("expected a JSON value");
        }
        Value result = parse_value(0);
        skip_whitespace();
        if (!at_end()) {
            fail("unexpected trailing data");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        fail_at(position_, message);
    }

    [[noreturn]] void fail_at(std::size_t offset,
                              std::string_view message) const {
        throw ParseError(offset, std::string(message));
    }

    bool at_end() const noexcept { return position_ >= input_.size(); }

    char peek() const noexcept { return input_[position_]; }

    void skip_whitespace() noexcept {
        while (!at_end() && is_json_whitespace(peek())) {
            ++position_;
        }
    }

    void count_value() {
        if (total_values_ >= limits_.max_total_values) {
            fail("document exceeds configured value count limit");
        }
        ++total_values_;
    }

    Value parse_value(std::size_t depth) {
        count_value();
        if (at_end()) {
            fail("expected a JSON value");
        }

        switch (peek()) {
            case 'n':
                consume_literal("null");
                return nullptr;
            case 't':
                consume_literal("true");
                return true;
            case 'f':
                consume_literal("false");
                return false;
            case '"':
                return parse_string();
            case '[':
                return parse_array(depth);
            case '{':
                return parse_object(depth);
            default:
                if (peek() == '-' || is_digit(peek())) {
                    return parse_number();
                }
                fail("unexpected character while reading value");
        }
    }

    void consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
    }

    std::uint16_t parse_hex_quad() {
        if (input_.size() - position_ < 4) {
            fail("incomplete Unicode escape");
        }
        std::uint16_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned digit = hex_value(input_[position_ + i]);
            if (digit == 16) {
                fail_at(position_ + static_cast<std::size_t>(i),
                        "invalid hexadecimal digit in Unicode escape");
            }
            result = static_cast<std::uint16_t>((result << 4) | digit);
        }
        position_ += 4;
        return result;
    }

    void check_string_limit(std::size_t size) const {
        if (size > limits_.max_string_bytes) {
            fail("string exceeds configured byte limit");
        }
    }

    void append_raw_utf8(std::string& output) {
        const std::size_t start = position_;
        const auto first = static_cast<unsigned char>(input_[position_]);
        std::size_t length = 0;

        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            fail("invalid UTF-8 leading byte in string");
        }

        if (input_.size() - position_ < length) {
            fail("truncated UTF-8 sequence in string");
        }

        const auto second = static_cast<unsigned char>(input_[position_ + 1]);
        if ((second & 0xc0) != 0x80) {
            fail_at(position_ + 1, "invalid UTF-8 continuation byte in string");
        }
        if ((first == 0xe0 && second < 0xa0) ||
            (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second > 0x8f)) {
            fail("invalid UTF-8 code point in string");
        }

        for (std::size_t i = 2; i < length; ++i) {
            const auto continuation =
                static_cast<unsigned char>(input_[position_ + i]);
            if ((continuation & 0xc0) != 0x80) {
                fail_at(position_ + i,
                        "invalid UTF-8 continuation byte in string");
            }
        }

        output.append(input_.substr(start, length));
        position_ += length;
        check_string_limit(output.size());
    }

    std::string parse_string() {
        ++position_;
        std::string result;

        while (!at_end()) {
            const auto byte = static_cast<unsigned char>(peek());
            if (byte == '"') {
                ++position_;
                return result;
            }
            if (byte == '\\') {
                ++position_;
                if (at_end()) {
                    fail("unterminated escape sequence");
                }
                const char escape = input_[position_++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        const std::uint16_t first = parse_hex_quad();
                        std::uint32_t code_point = first;
                        if (first >= 0xd800 && first <= 0xdbff) {
                            if (input_.size() - position_ < 2 ||
                                input_[position_] != '\\' ||
                                input_[position_ + 1] != 'u') {
                                fail("high surrogate must be followed by a low surrogate");
                            }
                            position_ += 2;
                            const std::uint16_t second = parse_hex_quad();
                            if (second < 0xdc00 || second > 0xdfff) {
                                fail("invalid low surrogate");
                            }
                            code_point =
                                0x10000u +
                                ((static_cast<std::uint32_t>(first) - 0xd800u)
                                 << 10) +
                                (static_cast<std::uint32_t>(second) - 0xdc00u);
                        } else if (first >= 0xdc00 && first <= 0xdfff) {
                            fail("unexpected low surrogate");
                        }
                        append_code_point(result, code_point);
                        break;
                    }
                    default:
                        fail_at(position_ - 1, "invalid escape sequence");
                }
                check_string_limit(result.size());
                continue;
            }
            if (byte < 0x20) {
                fail("unescaped control character in string");
            }
            if (byte < 0x80) {
                result.push_back(static_cast<char>(byte));
                ++position_;
                check_string_limit(result.size());
            } else {
                append_raw_utf8(result);
            }
        }

        fail("unterminated string");
    }

    Value parse_number() {
        const std::size_t start = position_;
        bool floating_point = false;

        if (peek() == '-') {
            ++position_;
            if (at_end()) {
                fail("incomplete number");
            }
        }

        if (peek() == '0') {
            ++position_;
            if (!at_end() && is_digit(peek())) {
                fail("leading zero is not allowed in number");
            }
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && is_digit(peek())) {
                ++position_;
            }
        } else {
            fail("expected a digit in number");
        }

        if (!at_end() && peek() == '.') {
            floating_point = true;
            ++position_;
            if (at_end() || !is_digit(peek())) {
                fail("fraction requires at least one digit");
            }
            while (!at_end() && is_digit(peek())) {
                ++position_;
            }
        }

        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            floating_point = true;
            ++position_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++position_;
            }
            if (at_end() || !is_digit(peek())) {
                fail("exponent requires at least one digit");
            }
            while (!at_end() && is_digit(peek())) {
                ++position_;
            }
        }

        const std::string_view token = input_.substr(start, position_ - start);
        if (token.size() > limits_.max_number_bytes) {
            fail_at(start, "number exceeds configured byte limit");
        }

        if (!floating_point) {
            if (token.front() == '-') {
                std::int64_t integer = 0;
                const auto [end, error] = std::from_chars(
                    token.data(), token.data() + token.size(), integer);
                if (error == std::errc{} && end == token.data() + token.size()) {
                    return integer;
                }
            } else {
                std::uint64_t integer = 0;
                const auto [end, error] = std::from_chars(
                    token.data(), token.data() + token.size(), integer);
                if (error == std::errc{} && end == token.data() + token.size()) {
                    return integer;
                }
            }
            fail_at(start, "integer is outside the supported 64-bit range");
        }

        double number = 0.0;
        const auto [end, error] = std::from_chars(
            token.data(), token.data() + token.size(), number,
            std::chars_format::general);
        if (error != std::errc{} || end != token.data() + token.size() ||
            !std::isfinite(number)) {
            fail_at(start, "number is outside the supported finite range");
        }
        return number;
    }

    Value parse_array(std::size_t depth) {
        if (depth >= limits_.max_depth) {
            fail("document exceeds configured nesting depth");
        }
        ++position_;
        skip_whitespace();

        Value::Array result;
        if (!at_end() && peek() == ']') {
            ++position_;
            return result;
        }

        while (true) {
            if (result.size() >= limits_.max_container_items) {
                fail("array exceeds configured item limit");
            }
            result.push_back(parse_value(depth + 1));
            skip_whitespace();
            if (at_end()) {
                fail("unterminated array");
            }
            if (peek() == ']') {
                ++position_;
                return result;
            }
            if (peek() != ',') {
                fail("expected ',' or ']' in array");
            }
            ++position_;
            skip_whitespace();
            if (at_end() || peek() == ']') {
                fail("expected a value after ',' in array");
            }
        }
    }

    Value parse_object(std::size_t depth) {
        if (depth >= limits_.max_depth) {
            fail("document exceeds configured nesting depth");
        }
        ++position_;
        skip_whitespace();

        Value::Object result;
        if (!at_end() && peek() == '}') {
            ++position_;
            return result;
        }

        while (true) {
            if (result.size() >= limits_.max_container_items) {
                fail("object exceeds configured member limit");
            }
            if (at_end() || peek() != '"') {
                fail("object member name must be a string");
            }
            std::string key = parse_string();
            skip_whitespace();
            if (at_end() || peek() != ':') {
                fail("expected ':' after object member name");
            }
            ++position_;
            skip_whitespace();
            if (at_end()) {
                fail("expected a value after ':'");
            }
            Value value = parse_value(depth + 1);
            const auto [unused, inserted] =
                result.emplace(std::move(key), std::move(value));
            (void)unused;
            if (!inserted) {
                fail("duplicate object member name");
            }
            skip_whitespace();
            if (at_end()) {
                fail("unterminated object");
            }
            if (peek() == '}') {
                ++position_;
                return result;
            }
            if (peek() != ',') {
                fail("expected ',' or '}' in object");
            }
            ++position_;
            skip_whitespace();
            if (at_end() || peek() == '}') {
                fail("expected a member after ',' in object");
            }
        }
    }

    std::string_view input_;
    ParseLimits limits_;
    std::size_t position_ = 0;
    std::size_t total_values_ = 0;
};

class Writer final {
public:
    explicit Writer(SerializeOptions options) : options_(options) {
        if (options_.indent_width > 16) {
            throw std::invalid_argument("JSON indent width must not exceed 16");
        }
    }

    std::string write(const Value& value) {
        write_value(value, 0);
        return std::move(output_);
    }

private:
    void ensure_space(std::size_t additional) const {
        if (additional > options_.max_output_bytes -
                             std::min(output_.size(), options_.max_output_bytes)) {
            throw std::length_error("serialized JSON exceeds configured byte limit");
        }
    }

    void append(char ch) {
        ensure_space(1);
        output_.push_back(ch);
    }

    void append(std::string_view text) {
        ensure_space(text.size());
        output_.append(text);
    }

    void newline_and_indent(std::size_t depth) {
        if (!options_.pretty) {
            return;
        }
        append('\n');
        const std::size_t spaces = depth * options_.indent_width;
        ensure_space(spaces);
        output_.append(spaces, ' ');
    }

    std::size_t validated_utf8_length(std::string_view text,
                                      std::size_t position) const {
        const auto first = static_cast<unsigned char>(text[position]);
        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            throw std::invalid_argument("JSON string contains invalid UTF-8");
        }
        if (text.size() - position < length) {
            throw std::invalid_argument("JSON string contains truncated UTF-8");
        }
        const auto second = static_cast<unsigned char>(text[position + 1]);
        if ((second & 0xc0) != 0x80 ||
            (first == 0xe0 && second < 0xa0) ||
            (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second > 0x8f)) {
            throw std::invalid_argument("JSON string contains invalid UTF-8");
        }
        for (std::size_t i = 2; i < length; ++i) {
            if ((static_cast<unsigned char>(text[position + i]) & 0xc0) != 0x80) {
                throw std::invalid_argument("JSON string contains invalid UTF-8");
            }
        }
        return length;
    }

    void write_string(std::string_view value) {
        static constexpr char hex[] = "0123456789abcdef";
        append('"');
        for (std::size_t i = 0; i < value.size();) {
            const auto byte = static_cast<unsigned char>(value[i]);
            switch (byte) {
                case '"': append("\\\""); ++i; continue;
                case '\\': append("\\\\"); ++i; continue;
                case '\b': append("\\b"); ++i; continue;
                case '\f': append("\\f"); ++i; continue;
                case '\n': append("\\n"); ++i; continue;
                case '\r': append("\\r"); ++i; continue;
                case '\t': append("\\t"); ++i; continue;
                default: break;
            }
            if (byte < 0x20) {
                char escaped[6] = {'\\', 'u', '0', '0',
                                   hex[(byte >> 4) & 0x0f], hex[byte & 0x0f]};
                append(std::string_view(escaped, sizeof(escaped)));
                ++i;
            } else if (byte < 0x80) {
                append(static_cast<char>(byte));
                ++i;
            } else {
                const std::size_t length = validated_utf8_length(value, i);
                append(value.substr(i, length));
                i += length;
            }
        }
        append('"');
    }

    void write_number(const Value& value) {
        char buffer[128];
        std::to_chars_result result{};
        switch (value.number_kind()) {
            case Value::NumberKind::signed_integer:
                result = std::to_chars(std::begin(buffer), std::end(buffer),
                                       value.as_int64());
                break;
            case Value::NumberKind::unsigned_integer:
                result = std::to_chars(std::begin(buffer), std::end(buffer),
                                       value.as_uint64());
                break;
            case Value::NumberKind::floating_point:
                result = std::to_chars(
                    std::begin(buffer), std::end(buffer), value.as_double(),
                    std::chars_format::general);
                break;
        }
        if (result.ec != std::errc{}) {
            throw std::runtime_error("failed to serialize JSON number");
        }
        append(std::string_view(buffer,
                                static_cast<std::size_t>(result.ptr - buffer)));
    }

    void write_value(const Value& value, std::size_t depth) {
        switch (value.type()) {
            case Value::Type::null:
                append("null");
                return;
            case Value::Type::boolean:
                append(value.as_bool() ? "true" : "false");
                return;
            case Value::Type::number:
                write_number(value);
                return;
            case Value::Type::string:
                write_string(value.as_string());
                return;
            case Value::Type::array:
                write_array(value.as_array(), depth);
                return;
            case Value::Type::object:
                write_object(value.as_object(), depth);
                return;
        }
    }

    void check_depth(std::size_t depth) const {
        if (depth >= options_.max_depth) {
            throw std::length_error("JSON exceeds configured nesting depth");
        }
    }

    void write_array(const Value::Array& value, std::size_t depth) {
        check_depth(depth);
        append('[');
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (i != 0) {
                append(',');
            }
            newline_and_indent(depth + 1);
            write_value(value[i], depth + 1);
        }
        if (!value.empty()) {
            newline_and_indent(depth);
        }
        append(']');
    }

    void write_object(const Value::Object& value, std::size_t depth) {
        check_depth(depth);
        append('{');
        std::size_t index = 0;
        for (const auto& [key, member] : value) {
            if (index++ != 0) {
                append(',');
            }
            newline_and_indent(depth + 1);
            write_string(key);
            append(options_.pretty ? ": " : ":");
            write_value(member, depth + 1);
        }
        if (!value.empty()) {
            newline_and_indent(depth);
        }
        append('}');
    }

    SerializeOptions options_;
    std::string output_;
};

}  // namespace

Value::Value() noexcept : data_(nullptr) {}

Value::Value(std::nullptr_t) noexcept : data_(nullptr) {}

Value::Value(bool value) noexcept : data_(value) {}

Value::Value(double value) : data_(value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("JSON numbers must be finite");
    }
}

Value::Value(const char* value) : data_(std::string(value != nullptr ? value : "")) {
    if (value == nullptr) {
        throw std::invalid_argument("JSON string cannot be constructed from null");
    }
}

Value::Value(std::string value) noexcept : data_(std::move(value)) {}

Value::Value(Array value) noexcept : data_(std::move(value)) {}

Value::Value(Object value) noexcept : data_(std::move(value)) {}

Value::Type Value::type() const noexcept {
    switch (data_.index()) {
        case 0: return Type::null;
        case 1: return Type::boolean;
        case 2:
        case 3:
        case 4: return Type::number;
        case 5: return Type::string;
        case 6: return Type::array;
        case 7: return Type::object;
        default: return Type::null;
    }
}

Value::NumberKind Value::number_kind() const {
    if (std::holds_alternative<std::int64_t>(data_)) {
        return NumberKind::signed_integer;
    }
    if (std::holds_alternative<std::uint64_t>(data_)) {
        return NumberKind::unsigned_integer;
    }
    if (std::holds_alternative<double>(data_)) {
        return NumberKind::floating_point;
    }
    throw_type_error("a number");
}

bool Value::is_null() const noexcept {
    return std::holds_alternative<std::nullptr_t>(data_);
}

bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(data_); }

bool Value::is_number() const noexcept {
    return std::holds_alternative<std::int64_t>(data_) ||
           std::holds_alternative<std::uint64_t>(data_) ||
           std::holds_alternative<double>(data_);
}

bool Value::is_string() const noexcept {
    return std::holds_alternative<std::string>(data_);
}

bool Value::is_array() const noexcept {
    return std::holds_alternative<Array>(data_);
}

bool Value::is_object() const noexcept {
    return std::holds_alternative<Object>(data_);
}

bool Value::as_bool() const {
    if (const auto* value = std::get_if<bool>(&data_)) {
        return *value;
    }
    throw_type_error("a boolean");
}

std::int64_t Value::as_int64() const {
    if (const auto* value = std::get_if<std::int64_t>(&data_)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::uint64_t>(&data_)) {
        if (*value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(*value);
        }
        throw std::out_of_range("JSON integer does not fit in int64_t");
    }
    if (const auto* value = std::get_if<double>(&data_)) {
        if (*value >= static_cast<double>(
                          std::numeric_limits<std::int64_t>::min()) &&
            *value < -static_cast<double>(
                         std::numeric_limits<std::int64_t>::min()) &&
            std::trunc(*value) == *value) {
            return static_cast<std::int64_t>(*value);
        }
        throw std::out_of_range("JSON number is not an int64_t value");
    }
    throw_type_error("a signed integer");
}

std::uint64_t Value::as_uint64() const {
    if (const auto* value = std::get_if<std::uint64_t>(&data_)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int64_t>(&data_)) {
        if (*value >= 0) {
            return static_cast<std::uint64_t>(*value);
        }
        throw std::out_of_range("negative JSON integer does not fit in uint64_t");
    }
    if (const auto* value = std::get_if<double>(&data_)) {
        if (*value >= 0.0 &&
            *value < -2.0 * static_cast<double>(
                                std::numeric_limits<std::int64_t>::min()) &&
            std::trunc(*value) == *value) {
            return static_cast<std::uint64_t>(*value);
        }
        throw std::out_of_range("JSON number is not a uint64_t value");
    }
    throw_type_error("an unsigned integer");
}

double Value::as_double() const {
    if (const auto* value = std::get_if<double>(&data_)) {
        return *value;
    }
    if (const auto* value = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*value);
    }
    if (const auto* value = std::get_if<std::uint64_t>(&data_)) {
        return static_cast<double>(*value);
    }
    throw_type_error("a number");
}

const std::string& Value::as_string() const {
    if (const auto* value = std::get_if<std::string>(&data_)) {
        return *value;
    }
    throw_type_error("a string");
}

std::string& Value::as_string() {
    if (auto* value = std::get_if<std::string>(&data_)) {
        return *value;
    }
    throw_type_error("a string");
}

const Value::Array& Value::as_array() const {
    if (const auto* value = std::get_if<Array>(&data_)) {
        return *value;
    }
    throw_type_error("an array");
}

Value::Array& Value::as_array() {
    if (auto* value = std::get_if<Array>(&data_)) {
        return *value;
    }
    throw_type_error("an array");
}

const Value::Object& Value::as_object() const {
    if (const auto* value = std::get_if<Object>(&data_)) {
        return *value;
    }
    throw_type_error("an object");
}

Value::Object& Value::as_object() {
    if (auto* value = std::get_if<Object>(&data_)) {
        return *value;
    }
    throw_type_error("an object");
}

const Value* Value::find(std::string_view key) const noexcept {
    const auto* object = std::get_if<Object>(&data_);
    if (object == nullptr) {
        return nullptr;
    }
    const auto iterator = object->find(key);
    return iterator == object->end() ? nullptr : &iterator->second;
}

Value* Value::find(std::string_view key) noexcept {
    auto* object = std::get_if<Object>(&data_);
    if (object == nullptr) {
        return nullptr;
    }
    const auto iterator = object->find(key);
    return iterator == object->end() ? nullptr : &iterator->second;
}

bool Value::contains(std::string_view key) const noexcept {
    return find(key) != nullptr;
}

const Value& Value::at(std::string_view key) const {
    if (!is_object()) {
        throw_type_error("an object");
    }
    if (const Value* value = find(key)) {
        return *value;
    }
    throw std::out_of_range("JSON object has no member named '" +
                            std::string(key) + "'");
}

Value& Value::at(std::string_view key) {
    if (!is_object()) {
        throw_type_error("an object");
    }
    if (Value* value = find(key)) {
        return *value;
    }
    throw std::out_of_range("JSON object has no member named '" +
                            std::string(key) + "'");
}

ParseError::ParseError(std::size_t offset, std::string message)
    : std::runtime_error(parse_error_message(offset, message)), offset_(offset) {}

std::size_t ParseError::offset() const noexcept { return offset_; }

Value parse(std::string_view input, const ParseLimits& limits) {
    return Parser(input, limits).parse_document();
}

std::string stringify(const Value& value, const SerializeOptions& options) {
    return Writer(options).write(value);
}

}  // namespace gomoku::server::json
