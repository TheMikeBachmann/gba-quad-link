/* Copyright (c) 2026 Mike Bachmann
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// minijson.h — just enough JSON for the Archipelago connector protocol.
//
// That protocol is a line of JSON in and a line of JSON out, carrying flat
// objects with string, number and boolean fields. A general-purpose library
// would do, but it would be the third dependency in a project with two, for a
// message shape that fits on a postcard.
//
// What it must do is not crash on anything. The far end is somebody else's
// program and may be a version we have never seen, so every parse either
// returns a value or reports a failure, and nothing is assumed about depth,
// length or encoding.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gql::json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    Array array;
    Object object;

    static Value of(bool b);
    static Value of(double n);
    static Value of(std::int64_t n);
    static Value of(std::string s);
    static Value of(Array a);
    static Value of(Object o);

    // Convenience for reading a request without checking types everywhere.
    // Each returns the fallback when the field is missing or the wrong type,
    // which is the right answer for a protocol we did not define.
    const Value* find(const std::string& key) const;
    std::string str(const std::string& key, const std::string& fallback = {}) const;
    std::int64_t integer(const std::string& key, std::int64_t fallback = 0) const;
};

// Returns false and leaves `out` untouched if the text is not valid JSON.
bool parse(const std::string& text, Value* out);

// Compact, no whitespace. Always valid JSON for any Value.
std::string dump(const Value& v);

}  // namespace gql::json

namespace gql::base64 {

std::string encode(const std::uint8_t* data, std::size_t size);
// False if the input is not valid base64.
bool decode(const std::string& text, std::vector<std::uint8_t>* out);

}  // namespace gql::base64
