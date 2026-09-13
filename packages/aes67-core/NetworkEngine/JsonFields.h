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
// key that appears twice gives the first, a key inside a nested object is
// found as readily as one at the top level, and a string value containing an
// escaped quote ends early. Anything that needs more than that wants a parser,
// and aes67-ravenna has one.
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
