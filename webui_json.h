#ifndef WEBUI_JSON_H
#define WEBUI_JSON_H

#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

// Simple JSON encoding utilities (RFC 7159 compliant)
// No external dependencies - pure C++17

namespace webui_json {

// Encode a string value (escape special characters)
inline std::string encode_string(const std::string& str) {
    std::string result = "\"";
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control character - escape as \uXXXX
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') 
                        << static_cast<unsigned int>(static_cast<unsigned char>(c));
                    result += oss.str();
                } else {
                    result += c;
                }
                break;
        }
    }
    result += "\"";
    return result;
}

// Encode a number
inline std::string encode_number(double num) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << num;
    std::string str = oss.str();
    // Remove trailing zeros
    if (str.find('.') != std::string::npos) {
        while (str.back() == '0') {
            str.pop_back();
        }
        if (str.back() == '.') {
            str.pop_back();
        }
    }
    return str;
}

// Encode an integer
inline std::string encode_int(int64_t num) {
    return std::to_string(num);
}

// Encode a boolean
inline std::string encode_bool(bool b) {
    return b ? "true" : "false";
}

// Encode null
inline std::string encode_null() {
    return "null";
}

// Start a JSON object
inline std::string object_start() {
    return "{";
}

// End a JSON object
inline std::string object_end() {
    return "}";
}

// Start a JSON array
inline std::string array_start() {
    return "[";
}

// End a JSON array
inline std::string array_end() {
    return "]";
}

// Add a key-value pair to an object (returns formatted string)
inline std::string object_pair(const std::string& key, const std::string& value) {
    return encode_string(key) + ":" + value;
}

// Add a comma separator
inline std::string comma() {
    return ",";
}

// Build a JSON object from key-value pairs
inline std::string build_object(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string result = "{";
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) result += ",";
        result += encode_string(pairs[i].first) + ":" + pairs[i].second;
    }
    result += "}";
    return result;
}

// Build a JSON array from values
inline std::string build_array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) result += ",";
        result += values[i];
    }
    result += "]";
    return result;
}

} // namespace webui_json

#endif // WEBUI_JSON_H
