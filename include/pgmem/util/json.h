#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <boost/property_tree/ptree.hpp>

namespace pgmem::util {

using Json = boost::property_tree::ptree;

bool ParseJson(const std::string& text, Json* out, std::string* error);
std::string ToJsonString(const Json& json, bool pretty = false);

Json MakeArray(const std::vector<std::string>& values);
std::vector<std::string> ReadStringArray(const Json& json, const std::string& path);

std::string GetStringOr(const Json& json, const std::string& path, const std::string& fallback);
int GetIntOr(const Json& json, const std::string& path, int fallback);
uint64_t GetUint64Or(const Json& json, const std::string& path, uint64_t fallback);
bool GetBoolOr(const Json& json, const std::string& path, bool fallback);

}  // namespace pgmem::util
