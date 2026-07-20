#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace gomoku::server::json {

class Value final {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;

    enum class Type {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    enum class NumberKind {
        signed_integer,
        unsigned_integer,
        floating_point,
    };

private:
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t,
                                 double, std::string, Array, Object>;

    template <std::integral Integer>
    static Storage make_integer(Integer value) noexcept {
        static_assert(sizeof(Integer) <= sizeof(std::uint64_t));
        if constexpr (std::is_signed_v<Integer>) {
            return Storage(std::in_place_type<std::int64_t>,
                           static_cast<std::int64_t>(value));
        } else {
            return Storage(std::in_place_type<std::uint64_t>,
                           static_cast<std::uint64_t>(value));
        }
    }

public:
    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;

    template <std::integral Integer>
        requires(!std::same_as<std::remove_cv_t<Integer>, bool>)
    Value(Integer value) noexcept : data_(make_integer(value)) {}

    Value(double value);
    Value(const char* value);
    Value(std::string value) noexcept;
    Value(Array value) noexcept;
    Value(Object value) noexcept;

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] NumberKind number_kind() const;

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_int64() const;
    [[nodiscard]] std::uint64_t as_uint64() const;
    [[nodiscard]] double as_double() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] std::string& as_string();
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] Value* find(std::string_view key) noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] const Value& at(std::string_view key) const;
    [[nodiscard]] Value& at(std::string_view key);

    friend bool operator==(const Value&, const Value&) = default;

private:
    Storage data_;
};

struct ParseLimits {
    std::size_t max_input_bytes = 1024 * 1024;
    std::size_t max_string_bytes = 1024 * 1024;
    std::size_t max_number_bytes = 128;
    std::size_t max_container_items = 100'000;
    std::size_t max_total_values = 200'000;
    std::size_t max_depth = 64;
};

struct SerializeOptions {
    bool pretty = false;
    std::size_t indent_width = 2;
    std::size_t max_depth = 64;
    std::size_t max_output_bytes = 4 * 1024 * 1024;
};

class ParseError final : public std::runtime_error {
public:
    ParseError(std::size_t offset, std::string message);

    [[nodiscard]] std::size_t offset() const noexcept;

private:
    std::size_t offset_;
};

[[nodiscard]] Value parse(std::string_view input,
                          const ParseLimits& limits = {});
[[nodiscard]] std::string stringify(const Value& value,
                                    const SerializeOptions& options = {});

}  // namespace gomoku::server::json
