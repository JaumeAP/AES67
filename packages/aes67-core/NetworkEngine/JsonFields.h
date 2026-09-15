//
// JsonFields.h
// AES67 core
// Reading one field out of a small JSON document.
//
// Not a parser. These match a key and its value with a regular expression and
// say nothing about the rest of the document, which is all the settings files
// and small API bodies in this repository ever needed -- and, being all they
// needed, the same eight functions had been written four times over, in
// aes67-core twice and in the macOS driver again, each with its own signature
// and the same regular expression inside.
//
// The limits are the limits of that approach and are worth saying out loud: a
// key that appears twice gives the first, and a key inside a nested object is
// found as readily as one at the top level. Anything that needs more than that
// wants a parser, and aes67-ravenna has one.
//
// A string value containing an escaped quote used to end early here, which
// made this the reading half of a round trip that lost data: jsonEscape wrote
// the escapes and nothing ever undid them. extractStringField now matches a
// whole JSON string and returns it unescaped.
//
// The unsigned readers narrower than 64 bits return nothing for a value that
// does not fit, rather than the value reduced modulo their width.
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace AES67 {

std::optional<std::string> extractStringField(const std::string& json, const std::string& field);
std::optional<uint64_t> extractUInt64Field(const std::string& json, const std::string& field);
std::optional<uint32_t> extractUInt32Field(const std::string& json, const std::string& field);
std::optional<uint16_t> extractUInt16Field(const std::string& json, const std::string& field);
std::optional<uint8_t> extractUInt8Field(const std::string& json, const std::string& field);
std::optional<double> extractDoubleField(const std::string& json, const std::string& field);
std::optional<bool> extractBoolField(const std::string& json, const std::string& field);
std::optional<int> extractIntField(const std::string& json, const std::string& field);

}  // namespace AES67
