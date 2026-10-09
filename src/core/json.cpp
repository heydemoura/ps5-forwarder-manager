// ps5fwdgen - Minimal JSON value, parser and writer (no exceptions, no RTTI).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json
{

namespace
{

const Value &null_value()
{
    static const Value kNull;
    return kNull;
}

void write_escaped(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buffer[8];
                (void)std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                    static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += buffer;
            }
            else
            {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

void write_number(std::string &out, double value)
{
    if (value == static_cast<double>(static_cast<long long>(value)) && std::abs(value) < 1e15)
    {
        char buffer[32];
        (void)std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        out += buffer;
        return;
    }
    char buffer[32];
    (void)std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    out += buffer;
}

class Parser
{
  public:
    Parser(std::string_view text, std::string *error) : text_(text), error_(error)
    {
    }

    bool run(Value &out)
    {
        skip_ws();
        if (!parse_value(out))
            return false;
        skip_ws();
        if (pos_ != text_.size())
            return fail("trailing characters");
        return true;
    }

  private:
    bool fail(const char *message)
    {
        if (error_ != nullptr && error_->empty())
        {
            char buffer[96];
            (void)std::snprintf(buffer, sizeof(buffer), "%s at byte %zu", message, pos_);
            *error_ = buffer;
        }
        return false;
    }

    void skip_ws()
    {
        while (pos_ < text_.size())
        {
            const char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool parse_value(Value &out)
    {
        if (pos_ >= text_.size())
            return fail("unexpected end");
        const char ch = text_[pos_];
        switch (ch)
        {
        case '{':
            return parse_object(out);
        case '[':
            return parse_array(out);
        case '"':
        {
            std::string value;
            if (!parse_string(value))
                return false;
            out = Value(std::move(value));
            return true;
        }
        case 't':
        case 'f':
            return parse_bool(out);
        case 'n':
            return parse_null(out);
        default:
            return parse_number(out);
        }
    }

    bool parse_object(Value &out)
    {
        out = Value::object();
        ++pos_; // {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("expected object key");
            std::string key;
            if (!parse_string(key))
                return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value value;
            if (!parse_value(value))
                return false;
            out.set(std::move(key), std::move(value));
            skip_ws();
            if (pos_ >= text_.size())
                return fail("unterminated object");
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value &out)
    {
        out = Value::array();
        ++pos_; // [
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            skip_ws();
            Value value;
            if (!parse_value(value))
                return false;
            out.push(std::move(value));
            skip_ws();
            if (pos_ >= text_.size())
                return fail("unterminated array");
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string(std::string &out)
    {
        ++pos_; // opening quote
        while (pos_ < text_.size())
        {
            const char ch = text_[pos_++];
            if (ch == '"')
                return true;
            if (ch == '\\')
            {
                if (pos_ >= text_.size())
                    return fail("unterminated escape");
                const char esc = text_[pos_++];
                switch (esc)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u':
                {
                    if (pos_ + 4 > text_.size())
                        return fail("bad \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        const char hex = text_[pos_++];
                        code <<= 4;
                        if (hex >= '0' && hex <= '9')
                            code |= static_cast<unsigned>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f')
                            code |= static_cast<unsigned>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F')
                            code |= static_cast<unsigned>(hex - 'A' + 10);
                        else
                            return fail("bad \\u digit");
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return fail("bad escape");
                }
            }
            else
            {
                out.push_back(ch);
            }
        }
        return fail("unterminated string");
    }

    static void append_utf8(std::string &out, unsigned code)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parse_bool(Value &out)
    {
        if (text_.compare(pos_, 4, "true") == 0)
        {
            pos_ += 4;
            out = Value(true);
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0)
        {
            pos_ += 5;
            out = Value(false);
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_null(Value &out)
    {
        if (text_.compare(pos_, 4, "null") == 0)
        {
            pos_ += 4;
            out = Value();
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_number(Value &out)
    {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
            ++pos_;
        bool any = false;
        while (pos_ < text_.size())
        {
            const char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' ||
                ch == '-')
            {
                any = true;
                ++pos_;
            }
            else
            {
                break;
            }
        }
        if (!any)
            return fail("invalid number");
        const std::string token(text_.substr(start, pos_ - start));
        out = Value(std::strtod(token.c_str(), nullptr));
        return true;
    }

    std::string_view text_;
    std::string *error_;
    std::size_t pos_ = 0;
};

} // namespace

Value Value::array()
{
    Value value;
    value.type_ = Type::array;
    return value;
}

Value Value::object()
{
    Value value;
    value.type_ = Type::object;
    return value;
}

bool Value::as_bool(bool fallback) const noexcept
{
    if (type_ == Type::boolean)
        return boolean_;
    if (type_ == Type::number)
        return number_ != 0.0;
    return fallback;
}

double Value::as_number(double fallback) const noexcept
{
    if (type_ == Type::number)
        return number_;
    if (type_ == Type::boolean)
        return boolean_ ? 1.0 : 0.0;
    return fallback;
}

long Value::as_int(long fallback) const noexcept
{
    if (type_ == Type::number)
        return static_cast<long>(number_);
    return fallback;
}

std::string_view Value::as_string(std::string_view fallback) const noexcept
{
    if (type_ == Type::string)
        return string_;
    return fallback;
}

std::size_t Value::size() const noexcept
{
    if (type_ == Type::array)
        return array_.size();
    if (type_ == Type::object)
        return object_.size();
    return 0;
}

const Value &Value::at(std::size_t index) const noexcept
{
    if (type_ == Type::array && index < array_.size())
        return array_[index];
    return null_value();
}

const Value *Value::find(std::string_view key) const noexcept
{
    if (type_ != Type::object)
        return nullptr;
    for (const auto &member : object_)
    {
        if (member.first == key)
            return &member.second;
    }
    return nullptr;
}

const Value &Value::get(std::string_view key) const noexcept
{
    const Value *found = find(key);
    return found != nullptr ? *found : null_value();
}

Value &Value::push(Value value)
{
    if (type_ != Type::array)
    {
        *this = Value::array();
    }
    array_.push_back(std::move(value));
    return array_.back();
}

Value &Value::set(std::string key, Value value)
{
    if (type_ != Type::object)
    {
        *this = Value::object();
    }
    for (auto &member : object_)
    {
        if (member.first == key)
        {
            member.second = std::move(value);
            return member.second;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return object_.back().second;
}

Value *Value::find_mutable(std::string_view key) noexcept
{
    if (type_ != Type::object)
        return nullptr;
    for (auto &member : object_)
    {
        if (member.first == key)
            return &member.second;
    }
    return nullptr;
}

void Value::dump_into(std::string &out, int indent, int depth) const
{
    const bool pretty = indent > 0;
    const auto newline_indent = [&](int level)
    {
        if (!pretty)
            return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(level), ' ');
    };
    switch (type_)
    {
    case Type::null:
        out += "null";
        break;
    case Type::boolean:
        out += boolean_ ? "true" : "false";
        break;
    case Type::number:
        write_number(out, number_);
        break;
    case Type::string:
        write_escaped(out, string_);
        break;
    case Type::array:
    {
        if (array_.empty())
        {
            out += "[]";
            break;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < array_.size(); ++i)
        {
            if (i != 0)
                out.push_back(',');
            newline_indent(depth + 1);
            array_[i].dump_into(out, indent, depth + 1);
        }
        newline_indent(depth);
        out.push_back(']');
        break;
    }
    case Type::object:
    {
        if (object_.empty())
        {
            out += "{}";
            break;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < object_.size(); ++i)
        {
            if (i != 0)
                out.push_back(',');
            newline_indent(depth + 1);
            write_escaped(out, object_[i].first);
            out += pretty ? ": " : ":";
            object_[i].second.dump_into(out, indent, depth + 1);
        }
        newline_indent(depth);
        out.push_back('}');
        break;
    }
    }
}

std::string Value::dump(int indent) const
{
    std::string out;
    dump_into(out, indent, 0);
    return out;
}

bool parse(std::string_view text, Value &out, std::string *error)
{
    if (error != nullptr)
        error->clear();
    Parser parser(text, error);
    return parser.run(out);
}

std::string quote(std::string_view text)
{
    std::string out;
    write_escaped(out, text);
    return out;
}

} // namespace json
