#include "pgmem/util/json.h"

#include <sstream>

#include <boost/property_tree/json_parser.hpp>

namespace pgmem::util {

bool ParseJson(const std::string& text, Json* out, std::string* error) {
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "ParseJson received null output pointer";
        }
        return false;
    }

    std::istringstream iss(text);
    try {
        boost::property_tree::read_json(iss, *out);
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
        return false;
    }
}

std::string ToJsonString(const Json& json, bool pretty) {
    std::ostringstream oss;
    boost::property_tree::write_json(oss, json, pretty);
    return oss.str();
}

Json MakeArray(const std::vector<std::string>& values) {
    Json arr;
    for (const std::string& value : values) {
        Json node;
        node.put("", value);
        arr.push_back(std::make_pair("", node));
    }
    return arr;
}

std::vector<std::string> ReadStringArray(const Json& json, const std::string& path) {
    std::vector<std::string> out;
    auto child_opt = json.get_child_optional(path);
    if (!child_opt) {
        return out;
    }

    for (const auto& it : *child_opt) {
        out.push_back(it.second.get_value<std::string>());
    }
    return out;
}

std::string GetStringOr(const Json& json, const std::string& path, const std::string& fallback) {
    return json.get<std::string>(path, fallback);
}

int GetIntOr(const Json& json, const std::string& path, int fallback) {
    return json.get<int>(path, fallback);
}

uint64_t GetUint64Or(const Json& json, const std::string& path, uint64_t fallback) {
    return json.get<uint64_t>(path, fallback);
}

bool GetBoolOr(const Json& json, const std::string& path, bool fallback) {
    return json.get<bool>(path, fallback);
}

}  // namespace pgmem::util
