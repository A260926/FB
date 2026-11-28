#pragma once

#include "cal3d/BoostToStd.h"
#include <rapidjson/document.h>

#include <cstring>
#include <iterator>
#include <string>

namespace cal3d {
    typedef rapidjson::Value Json;
    typedef ISTD::optional<std::string> MaybeError;

    inline MaybeError jsonSuccess() {
        return MaybeError();
    }

    inline MaybeError jsonError(const std::string& message) {
        return MaybeError(message);
    }

    template<typename IntType>
    MaybeError jsonGetInt(const Json& val, IntType& result) {
        if (!val.IsInt()) {
            return jsonError("Json is not an integer");
        }
        result = val.GetInt();
        return jsonSuccess();
    }

    template<typename IntType>
    MaybeError jsonGetInt64(const Json& val, IntType& result) {
        if (!val.IsInt64()) {
            return jsonError("Json is not an integer");
        }
        result = static_cast<IntType>(val.GetInt64());
        return jsonSuccess();
    }

    template<typename UintType>
    MaybeError jsonGetUint(const Json& val, UintType& result) {
        if (!val.IsUint()) {
            return jsonError("Json is not an unsigned integer");
        }
        result = val.GetUint();
        return jsonSuccess();
    }

    template<typename UintType>
    MaybeError jsonGetUint64(const Json& val, UintType& result) {
        if (!val.IsUint64()) {
            return jsonError("Json is not an unsigned integer");
        }
        result = static_cast<UintType>(val.GetUint64());
        return jsonSuccess();
    }

    template<typename FloatType>
    MaybeError jsonGetFloat(const Json& val, FloatType& result) {
        if (!val.IsNumber()) {
            return jsonError("Json is not a float");
        }
        result = static_cast<FloatType>(val.GetDouble());
        return jsonSuccess();
    }

    // MSVC 10.0 doesn't support " = delete" yet.
    //struct CannotExtractThisType { CannotExtractThisType() = delete; };
    struct CannotExtractThisType {
    private:
        CannotExtractThisType() {}
    };
    inline void cannotExtractThisType(CannotExtractThisType) { }

    template<typename InvalidType>
    MaybeError jsonGet(const Json& val, InvalidType& result) {
        cannotExtractThisType(result);
        return MaybeError(""); // to avoid compile error in g++
    }

    template<typename ValType>
    MaybeError jsonGet(const Json& obj, const char* key, ValType& result) {
        const Json* valOfObj;
        if (MaybeError error = jsonGet(obj, key, valOfObj)) {
            return error;
        }
        if (MaybeError error = jsonGet(*valOfObj, result)) {
            return jsonError(*error + " in jsonGet(obj, key...)");
        }
        return jsonSuccess();
    }

    template<>
    MaybeError jsonGet(const Json& obj, const char* key, const Json*& result) {
        if (key == nullptr) {
            return jsonError("Key is null in jsonGet(obj, key...)");
        } else if (strlen(key) == 0) {
            return jsonError("Key is empty in jsonGet(obj, key...)");
        } else if (!obj.IsObject()) {
            return jsonError("Json is not an object in jsonGet(obj, \"" + std::string(key) + "\" ...)");
        } else if (!obj.HasMember(key)) {
            return jsonError((std::string("Key '") + key + "' does not exist in jsonGet(obj, key...)").c_str());
        }
        result = &obj[key];
        return jsonSuccess();
    }

    template<typename ValType>
    MaybeError jsonGet(const Json& obj, std::vector<ValType>& result) {
        if (!obj.IsArray()) {
            return jsonError("Expected array");
        }
        size_t size = obj.Size();
        for (size_t i = 0; i < size; ++i) {
            ValType value;
            if (auto error = jsonGet(obj[i], value)) {
                return error;
            }
            result.push_back(value);
        }
        return jsonSuccess();
    }

    template<>
    inline MaybeError jsonGet(const Json& val, unsigned int& result) {
        return jsonGetUint(val, result);
    }

    template<>
    inline MaybeError jsonGet(const Json& val, unsigned long& result) {
        return jsonGetUint64(val, result);
    }

    template<>
    inline MaybeError jsonGet(const Json& val, float& result) {
        return jsonGetFloat(val, result);
    }

    template<>
    inline MaybeError jsonGet(const Json& val, std::string& result) {
        if (!val.IsString()) {
            return jsonError("Json is not an string");
        }
        result = val.GetString();
        return jsonSuccess();
    }

    inline size_t getJsonMemberCount(const Json& val) {
        const auto memberBegin = val.MemberBegin();
        const auto memberEnd = val.MemberEnd();
        return std::distance(memberBegin, memberEnd);
    }
}
