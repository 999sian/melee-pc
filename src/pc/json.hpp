/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_JSON_HPP
#define PC_JSON_HPP

#include <cctype>
#include <charconv>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pc::json {

enum class Type { Null, Boolean, Number, String, Array, Object };

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    Array arr_val;
    Object obj_val;

    Value() : type(Type::Null) {}
    Value(bool b) : type(Type::Boolean), bool_val(b) {}
    Value(int n) : type(Type::Number), num_val(n) {}
    Value(int64_t n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    Value(double n) : type(Type::Number), num_val(n) {}
    Value(const char* s) : type(Type::String), str_val(s ? s : "") {}
    Value(std::string s) : type(Type::String), str_val(std::move(s)) {}
    Value(Array a) : type(Type::Array), arr_val(std::move(a)) {}
    Value(Object o) : type(Type::Object), obj_val(std::move(o)) {}

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Boolean; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    bool as_bool(bool default_val = false) const { return is_bool() ? bool_val : default_val; }

    int64_t as_int(int64_t default_val = 0) const {
        return is_number() ? static_cast<int64_t>(num_val) : default_val;
    }

    double as_double(double default_val = 0.0) const { return is_number() ? num_val : default_val; }

    const std::string& as_string(const std::string& default_val = "") const {
        return is_string() ? str_val : default_val;
    }

    const Array& as_array() const {
        static const Array s_empty_arr;
        return is_array() ? arr_val : s_empty_arr;
    }

    const Object& as_object() const {
        static const Object s_empty_obj;
        return is_object() ? obj_val : s_empty_obj;
    }

    bool has(const std::string& key) const {
        if (!is_object())
            return false;
        return obj_val.find(key) != obj_val.end();
    }

    const Value& operator[](const std::string& key) const {
        static const Value s_null_val;
        if (!is_object())
            return s_null_val;
        auto it = obj_val.find(key);
        return it != obj_val.end() ? it->second : s_null_val;
    }

    Value& operator[](const std::string& key) {
        if (!is_object()) {
            type = Type::Object;
            obj_val.clear();
        }
        return obj_val[key];
    }

    const Value& operator[](size_t index) const {
        static const Value s_null_val;
        if (!is_array() || index >= arr_val.size())
            return s_null_val;
        return arr_val[index];
    }

    Value& operator[](size_t index) {
        if (!is_array()) {
            type = Type::Array;
            arr_val.clear();
        }
        if (index >= arr_val.size()) {
            arr_val.resize(index + 1);
        }
        return arr_val[index];
    }
};

class Parser {
    std::string_view src;
    size_t pos = 0;

    void skip_whitespace() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                pos++;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                // Line comment support (common in json5/game configs)
                pos += 2;
                while (pos < src.size() && src[pos] != '\n') {
                    pos++;
                }
            } else {
                break;
            }
        }
    }

    char peek() {
        skip_whitespace();
        return pos < src.size() ? src[pos] : '\0';
    }

    char get() {
        skip_whitespace();
        return pos < src.size() ? src[pos++] : '\0';
    }

    std::string parse_string() {
        if (get() != '"')
            return "";
        std::string result;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') {
                return result;
            }
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    // 4 hex digits
                    if (pos + 4 <= src.size()) {
                        pos += 4;  // Skip hex unicode for now
                        result += '?';
                    }
                    break;
                }
                default:
                    result += esc;
                    break;
                }
            } else {
                result += c;
            }
        }
        return result;
    }

    Value parse_number() {
        size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) {
            pos++;
        }
        while (pos < src.size() &&
               (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.' ||
                   src[pos] == 'e' || src[pos] == 'E' || src[pos] == '+' || src[pos] == '-'))
        {
            pos++;
        }
        std::string num_str(src.substr(start, pos - start));
        try {
            return Value(std::stod(num_str));
        } catch (...) {
            return Value(0.0);
        }
    }

public:
    explicit Parser(std::string_view s) : src(s), pos(0) {}

    Value parse_value() {
        skip_whitespace();
        if (pos >= src.size())
            return Value();

        char c = src[pos];
        if (c == '{') {
            return parse_object();
        } else if (c == '[') {
            return parse_array();
        } else if (c == '"') {
            return Value(parse_string());
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            return parse_number();
        } else if (src.substr(pos, 4) == "true") {
            pos += 4;
            return Value(true);
        } else if (src.substr(pos, 5) == "false") {
            pos += 5;
            return Value(false);
        } else if (src.substr(pos, 4) == "null") {
            pos += 4;
            return Value();
        }

        pos++;  // Skip unrecognized character
        return Value();
    }

    Value parse_object() {
        get();  // Consume '{'
        Value::Object obj;
        skip_whitespace();
        if (peek() == '}') {
            get();
            return Value(obj);
        }

        while (pos < src.size()) {
            skip_whitespace();
            if (peek() != '"')
                break;
            std::string key = parse_string();
            skip_whitespace();
            if (get() != ':')
                break;
            Value val = parse_value();
            obj[std::move(key)] = std::move(val);

            skip_whitespace();
            char next = peek();
            if (next == ',') {
                get();
                skip_whitespace();
                if (peek() == '}') {  // Trailing comma tolerance
                    get();
                    break;
                }
            } else if (next == '}') {
                get();
                break;
            } else {
                break;
            }
        }
        return Value(obj);
    }

    Value parse_array() {
        get();  // Consume '['
        Value::Array arr;
        skip_whitespace();
        if (peek() == ']') {
            get();
            return Value(arr);
        }

        while (pos < src.size()) {
            Value val = parse_value();
            arr.push_back(std::move(val));

            skip_whitespace();
            char next = peek();
            if (next == ',') {
                get();
                skip_whitespace();
                if (peek() == ']') {  // Trailing comma tolerance
                    get();
                    break;
                }
            } else if (next == ']') {
                get();
                break;
            } else {
                break;
            }
        }
        return Value(arr);
    }
};

inline Value parse(std::string_view json_str) {
    Parser p(json_str);
    return p.parse_value();
}

}  // namespace pc::json

#endif  // PC_JSON_HPP
