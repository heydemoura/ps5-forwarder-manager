/*
 * ps5fwdgen - Minimal JSON value, parser and writer (no exceptions, no RTTI).
 * Copyright (C) 2026 heydemoura
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json
{

class Value
{
  public:
    enum class Type
    {
        null,
        boolean,
        number,
        string,
        array,
        object
    };

    Value() noexcept = default;
    explicit Value(bool value) noexcept : type_{Type::boolean}, boolean_{value} {}
    explicit Value(double value) noexcept : type_{Type::number}, number_{value} {}
    explicit Value(int value) noexcept : type_{Type::number}, number_{static_cast<double>(value)} {}
    explicit Value(long value) noexcept : type_{Type::number}, number_{static_cast<double>(value)} {}
    explicit Value(std::string value) : type_{Type::string}, string_{std::move(value)} {}
    explicit Value(std::string_view value) : type_{Type::string}, string_{value} {}
    explicit Value(const char *value) : type_{Type::string}, string_{value} {}

    static Value array();
    static Value object();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::string; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::object; }

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
    [[nodiscard]] long as_int(long fallback = 0) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;

    // Array and object element count; 0 for scalars.
    [[nodiscard]] std::size_t size() const noexcept;
    // Array element; a shared null value when out of range or not an array.
    [[nodiscard]] const Value &at(std::size_t index) const noexcept;
    // Object member; nullptr when missing or not an object.
    [[nodiscard]] const Value *find(std::string_view key) const noexcept;
    // Object member; a shared null value when missing.
    [[nodiscard]] const Value &get(std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

    [[nodiscard]] const std::vector<Value> &items() const noexcept { return array_; }
    [[nodiscard]] const std::vector<std::pair<std::string, Value>> &members() const noexcept
    {
        return object_;
    }

    // Mutation (turns the value into an array/object when it is null).
    Value &push(Value value);
    Value &set(std::string key, Value value);
    Value *find_mutable(std::string_view key) noexcept;

    // Serialise. indent <= 0 writes a compact document; otherwise pretty-print
    // with that many spaces, matching the layout JSON.stringify(v, null, 2) uses.
    [[nodiscard]] std::string dump(int indent = 2) const;

  private:
    void dump_into(std::string &out, int indent, int depth) const;

    Type type_{Type::null};
    bool boolean_{false};
    double number_{0.0};
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Parse a JSON document. On failure returns false and (when given) fills
// error with a short message including the byte offset.
bool parse(std::string_view text, Value &out, std::string *error = nullptr);

// Escape a string for inclusion in a JSON document (adds the quotes).
std::string quote(std::string_view text);

} // namespace json
