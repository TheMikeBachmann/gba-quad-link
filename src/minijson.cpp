/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "minijson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gql::json {
namespace {

// Depth is bounded because the far end is another program: a few thousand
// opening brackets would otherwise be a stack overflow rather than a parse
// error, and "malformed input crashes the emulator" is not a good trade.
constexpr int kMaxDepth = 32;

struct Parser {
    const std::string& s;
    std::size_t i = 0;
    bool ok = true;

    void skip() {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool eat(char c) {
        skip();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }
    bool literal(const char* word) {
        skip();
        const std::size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }

    bool string(std::string* out) {
        if (!eat('"')) return false;
        out->clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"':  out->push_back('"');  break;
                case '\\': out->push_back('\\'); break;
                case '/':  out->push_back('/');  break;
                case 'b':  out->push_back('\b'); break;
                case 'f':  out->push_back('\f'); break;
                case 'n':  out->push_back('\n'); break;
                case 'r':  out->push_back('\r'); break;
                case 't':  out->push_back('\t'); break;
                case 'u': {
                    // Kept as UTF-8. Surrogate pairs are not recombined: this
                    // protocol carries base64 and identifiers, and a lone
                    // surrogate should not be a parse failure either.
                    if (i + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                        else return false;
                    }
                    i += 4;
                    if (cp < 0x80) {
                        out->push_back(char(cp));
                    } else if (cp < 0x800) {
                        out->push_back(char(0xC0 | (cp >> 6)));
                        out->push_back(char(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back(char(0xE0 | (cp >> 12)));
                        out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(char(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool value(Value* out, int depth) {
        if (depth > kMaxDepth) return false;
        skip();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') {
            std::string str;
            if (!string(&str)) return false;
            *out = Value::of(std::move(str));
            return true;
        }
        if (c == '{') {
            ++i;
            out->type = Value::Type::Object;
            skip();
            if (eat('}')) return true;
            while (true) {
                std::string key;
                skip();
                if (!string(&key)) return false;
                if (!eat(':')) return false;
                Value v;
                if (!value(&v, depth + 1)) return false;
                out->object[key] = std::move(v);
                if (eat(',')) continue;
                return eat('}');
            }
        }
        if (c == '[') {
            ++i;
            out->type = Value::Type::Array;
            skip();
            if (eat(']')) return true;
            while (true) {
                Value v;
                if (!value(&v, depth + 1)) return false;
                out->array.push_back(std::move(v));
                if (eat(',')) continue;
                return eat(']');
            }
        }
        if (c == 't') { if (!literal("true")) return false;  *out = Value::of(true);  return true; }
        if (c == 'f') { if (!literal("false")) return false; *out = Value::of(false); return true; }
        if (c == 'n') { if (!literal("null")) return false;  *out = Value{};          return true; }

        // Number. strtod decides where it ends, which also rejects the cases
        // that look like numbers and are not.
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        const double n = std::strtod(begin, &end);
        if (end == begin || !std::isfinite(n)) return false;
        i += static_cast<std::size_t>(end - begin);
        *out = Value::of(n);
        return true;
    }
};

void dump_string(const std::string& in, std::string* out) {
    out->push_back('"');
    for (const char c : in) {
        switch (c) {
            case '"':  *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\b': *out += "\\b";  break;
            case '\f': *out += "\\f";  break;
            case '\n': *out += "\\n";  break;
            case '\r': *out += "\\r";  break;
            case '\t': *out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned char>(c));
                    *out += buf;
                } else {
                    out->push_back(c);
                }
        }
    }
    out->push_back('"');
}

void dump_into(const Value& v, std::string* out) {
    switch (v.type) {
        case Value::Type::Null:   *out += "null"; return;
        case Value::Type::Bool:   *out += v.boolean ? "true" : "false"; return;
        case Value::Type::Number: {
            // Whole numbers without a decimal point, because addresses and
            // sizes are what this protocol is mostly made of and 4096.0 would
            // be an odd thing to send.
            char buf[40];
            if (v.number == std::floor(v.number) &&
                std::fabs(v.number) < 9.0e15)
                std::snprintf(buf, sizeof buf, "%lld",
                              static_cast<long long>(v.number));
            else
                std::snprintf(buf, sizeof buf, "%.17g", v.number);
            *out += buf;
            return;
        }
        case Value::Type::String: dump_string(v.string, out); return;
        case Value::Type::Array: {
            out->push_back('[');
            bool first = true;
            for (const Value& e : v.array) {
                if (!first) out->push_back(',');
                first = false;
                dump_into(e, out);
            }
            out->push_back(']');
            return;
        }
        case Value::Type::Object: {
            out->push_back('{');
            bool first = true;
            for (const auto& [k, e] : v.object) {
                if (!first) out->push_back(',');
                first = false;
                dump_string(k, out);
                out->push_back(':');
                dump_into(e, out);
            }
            out->push_back('}');
            return;
        }
    }
}

}  // namespace

Value Value::of(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }
Value Value::of(double n) { Value v; v.type = Type::Number; v.number = n; return v; }
Value Value::of(std::int64_t n) { return of(static_cast<double>(n)); }
Value Value::of(std::string s) {
    Value v; v.type = Type::String; v.string = std::move(s); return v;
}
Value Value::of(Array a) { Value v; v.type = Type::Array; v.array = std::move(a); return v; }
Value Value::of(Object o) { Value v; v.type = Type::Object; v.object = std::move(o); return v; }

const Value* Value::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string Value::str(const std::string& key, const std::string& fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::String) ? v->string : fallback;
}

std::int64_t Value::integer(const std::string& key, std::int64_t fallback) const {
    const Value* v = find(key);
    return (v && v->type == Type::Number) ? static_cast<std::int64_t>(v->number)
                                          : fallback;
}

bool parse(const std::string& text, Value* out) {
    Parser p{text};
    Value v;
    if (!p.value(&v, 0)) return false;
    p.skip();
    if (p.i != text.size()) return false;   // trailing rubbish is not valid
    *out = std::move(v);
    return true;
}

std::string dump(const Value& v) {
    std::string out;
    dump_into(v, &out);
    return out;
}

}  // namespace gql::json

namespace gql::base64 {
namespace {
const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int index_of(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string encode(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = i + 1 < size ? data[i + 1] : 0;
        const unsigned b2 = i + 2 < size ? data[i + 2] : 0;
        out.push_back(kAlphabet[b0 >> 2]);
        out.push_back(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < size ? kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < size ? kAlphabet[b2 & 0x3F] : '=');
    }
    return out;
}

bool decode(const std::string& text, std::vector<std::uint8_t>* out) {
    out->clear();
    if (text.size() % 4 != 0) return false;
    out->reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int n[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                // Padding only at the very end, and at most two of it.
                if (i + 4 != text.size() || k < 2) return false;
                n[k] = 0;
                ++pad;
            } else {
                n[k] = index_of(c);
                if (n[k] < 0 || pad) return false;
            }
        }
        const unsigned v = (unsigned(n[0]) << 18) | (unsigned(n[1]) << 12) |
                           (unsigned(n[2]) << 6) | unsigned(n[3]);
        out->push_back(std::uint8_t((v >> 16) & 0xFF));
        if (pad < 2) out->push_back(std::uint8_t((v >> 8) & 0xFF));
        if (pad < 1) out->push_back(std::uint8_t(v & 0xFF));
    }
    return true;
}

}  // namespace gql::base64
