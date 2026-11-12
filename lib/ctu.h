/* -*- C++ -*-
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2025 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


//---------------------------------------------------------------------------
#ifndef ctuH
#define ctuH
//---------------------------------------------------------------------------

#include "config.h"
#include "check.h"
#include "errorlogger.h"
#include "mathlib.h"
#include "vfvalue.h"

#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

class Function;
class Settings;
class Token;
class Tokenizer;

namespace tinyxml2 {
    class XMLElement;
}

/// @addtogroup Core
/// @{


/** @brief Whole program analysis (ctu=Cross Translation Unit) */
namespace CTU {
    class CPPCHECKLIB FileInfo : public Check::FileInfo {
    public:
        enum class InvalidValueType : std::uint8_t { null, uninit, bufferOverflow };

        std::string toString() const override;

        struct Location {
            Location() = default;
            Location(const Tokenizer &tokenizer, const Token *tok);
            Location(std::string fileName, nonneg int lineNumber, nonneg int column) : fileName(std::move(fileName)), lineNumber(lineNumber), column(column) {}
            std::string fileName;
            nonneg int lineNumber{};
            nonneg int column{};
        };

        struct Value {
            Value& operator=(const ValueFlow::Value& val) & {
                value = val.intvalue;
                unknownFunctionReturn = val.unknownFunctionReturn;
                return *this;
            }
            MathLib::bigint value{};
            ValueFlow::Value::UnknownFunctionReturn unknownFunctionReturn{};
        };

        struct UnsafeUsage {
            UnsafeUsage() = default;
            UnsafeUsage(std::string myId, nonneg int myArgNr, std::string myArgumentName, Location location, MathLib::bigint value)
                : myId(std::move(myId))
                , myArgNr(myArgNr)
                , myArgumentName(std::move(myArgumentName))
                , location(std::move(location))
                , value(value) {}
            std::string myId;
            nonneg int myArgNr{};
            std::string myArgumentName;
            Location location;
            MathLib::bigint value{};
            std::string toString() const;
        };

        class CallBase {
        public:
            CallBase() = default;
            CallBase(std::string callId, int callArgNr, std::string callFunctionName, Location loc)
                : callId(std::move(callId)), callArgNr(callArgNr), callFunctionName(std::move(callFunctionName)), location(std::move(loc))
            {}
            CallBase(const Tokenizer &tokenizer, const Token *callToken);
            virtual ~CallBase() = default;
            CallBase(const CallBase&) = default;
            std::string callId;
            int callArgNr{};
            std::string callFunctionName;
            Location location;
        protected:
            std::string toBaseXmlString() const;
            bool loadBaseFromXml(const tinyxml2::XMLElement *xmlElement);
        };

        class FunctionCall : public CallBase {
        public:
            std::string callArgumentExpression;
            Value callArgValue;
            ValueFlow::Value::ValueType callValueType;
            std::vector<ErrorMessage::FileLocation> callValuePath;
            bool warning;
            /** true if this is a known integer argument value recorded for CTU parameter index analysis */
            bool callArgValueKnown = false;

            std::string toXmlString() const;
            bool loadFromXml(const tinyxml2::XMLElement *xmlElement);
        };

        class NestedCall : public CallBase {
        public:
            NestedCall() = default;

            NestedCall(std::string myId, nonneg int myArgNr, const std::string &callId, nonneg int callArgnr, const std::string &callFunctionName, const Location &location)
                : CallBase(callId, callArgnr, callFunctionName, location),
                myId(std::move(myId)),
                myArgNr(myArgNr) {}

            NestedCall(const Tokenizer &tokenizer, const Function *myFunction, const Token *callToken);

            std::string toXmlString() const;
            bool loadFromXml(const tinyxml2::XMLElement *xmlElement);

            std::string myId;
            nonneg int myArgNr{};
        };

        /** @brief How a function parameter is passed on to a called function */
        class ForwardCall : public CallBase {
        public:
            ForwardCall() = default;

            ForwardCall(std::string myId, nonneg int myArgNr, const std::string &callId, nonneg int callArgnr, const std::string &callFunctionName, const Location &location, MathLib::bigint delta)
                : CallBase(callId, callArgnr, callFunctionName, location),
                myId(std::move(myId)),
                myArgNr(myArgNr),
                callArgDelta(delta) {}

            std::string toXmlString() const;
            bool loadFromXml(const tinyxml2::XMLElement *xmlElement);

            std::string myId;
            nonneg int myArgNr{};
            /** constant added to the forwarded integer argument (0 when the argument is passed as-is) */
            MathLib::bigint callArgDelta{};
        };

        /** @brief A function unconditionally accesses a pointer parameter at an index computed from an integer parameter */
        struct ArrayIndexUsage {
            ArrayIndexUsage() = default;
            ArrayIndexUsage(std::string myId, nonneg int pointerArgNr, nonneg int indexArgNr, std::string myArgumentName, Location location, MathLib::bigint indexOffset, MathLib::bigint elementSize)
                : myId(std::move(myId))
                , pointerArgNr(pointerArgNr)
                , indexArgNr(indexArgNr)
                , myArgumentName(std::move(myArgumentName))
                , location(std::move(location))
                , indexOffset(indexOffset)
                , elementSize(elementSize) {}
            std::string myId;
            nonneg int pointerArgNr{};
            nonneg int indexArgNr{};
            std::string myArgumentName;
            Location location;
            /** constant offset in elements added to the integer parameter in the subscript expression */
            MathLib::bigint indexOffset{};
            /** size in bytes of the accessed element */
            MathLib::bigint elementSize{};
            std::string toString() const;
        };

        std::list<FunctionCall> functionCalls;
        std::list<NestedCall> nestedCalls;
        std::list<ForwardCall> forwardCalls;

        void loadFromXml(const tinyxml2::XMLElement *xmlElement);
        std::map<std::string, std::list<const CallBase *>> getCallsMap() const;

        static std::list<ErrorMessage::FileLocation> getErrorPath(InvalidValueType invalidValue,
                                                                  const UnsafeUsage &unsafeUsage,
                                                                  const std::map<std::string, std::list<const CallBase *>> &callsMap,
                                                                  const char info[],
                                                                  const FunctionCall ** functionCallPtr,
                                                                  bool warning,
                                                                  int maxCtuDepth,
                                                                  ValueFlow::Value::UnknownFunctionReturn *unknownFunctionReturn = nullptr);
    };

    CPPCHECKLIB std::string toString(const std::list<FileInfo::UnsafeUsage> &unsafeUsage);

    CPPCHECKLIB std::string toString(const std::list<FileInfo::ArrayIndexUsage> &arrayIndexUsage);

    CPPCHECKLIB std::string getFunctionId(const Tokenizer &tokenizer, const Function *function);

    /** @brief Parse current TU and extract file info */
    CPPCHECKLIB RET_NONNULL FileInfo *getFileInfo(const Tokenizer &tokenizer);

    CPPCHECKLIB std::list<FileInfo::UnsafeUsage> getUnsafeUsage(const Tokenizer &tokenizer, const Settings &settings, bool (*isUnsafeUsage)(const Settings &settings, const Token *argtok, FileInfo::Value *value));

    CPPCHECKLIB std::list<FileInfo::UnsafeUsage> loadUnsafeUsageListFromXml(const tinyxml2::XMLElement *xmlElement);

    CPPCHECKLIB std::list<FileInfo::ArrayIndexUsage> loadArrayIndexUsageListFromXml(const tinyxml2::XMLElement *xmlElement);
}

/// @}
//---------------------------------------------------------------------------
#endif // ctuH
