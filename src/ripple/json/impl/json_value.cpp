//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/basics/contract.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/json/impl/json_assert.h>
#include <ripple/json/json_writer.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string_view>
#include <utility>

namespace Json {

namespace {

/** The default allocator to use, if no custom allocator is specified.
 */
ValueAllocator defaultValueAllocator;

/** A pointer to the allocator to use.
 */
constinit ValueAllocator* valueAllocator = &defaultValueAllocator;

}  // namespace

void
setAllocator(ValueAllocator* allocator)
{
    assert(allocator != nullptr && valueAllocator == &defaultValueAllocator);

    if (valueAllocator == &defaultValueAllocator)
        valueAllocator = allocator;
}

ValueAllocator*
getAllocator()
{
    return valueAllocator;
}

// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// class Value::CZString
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////

Value::CZString::CZString(std::string_view s)
    : data_(reinterpret_cast<std::uintptr_t>(
          valueAllocator->duplicateStringValue(s.data(), s.size())))
{
}

Value::CZString::CZString(CZString const& other)
    : data_(
          other.isStatic()
              ? other.data_
              : reinterpret_cast<std::uintptr_t>(
                    valueAllocator->duplicateStringValue(other.c_str())))
{
}

Value::CZString&
Value::CZString::operator=(CZString const& other)
{
    if (this != &other)
    {
        if (!isStatic() && c_str())
            valueAllocator->releaseStringValue(c_str());

        if (other.isStatic())
            data_ = other.data_;
        else
            data_ = reinterpret_cast<std::uintptr_t>(
                valueAllocator->duplicateStringValue(other.c_str()));
    }
    return *this;
}

Value::CZString&
Value::CZString::operator=(CZString&& other) noexcept
{
    if (this != &other)
    {
        if (!isStatic() && c_str())
            valueAllocator->releaseStringValue(c_str());
        data_ = other.data_;
        other.data_ = static_flag;
    }
    return *this;
}

Value::CZString::~CZString()
{
    if (!isStatic() && c_str())
        valueAllocator->releaseStringValue(c_str());
}

// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// class Value
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////

// We need a single instance of a null type, so that we can return
// a reference from things like `Value::operator[](...) const`.
Value const null{};

void
Value::initSSO(const char* str, std::size_t len) noexcept
{
    assert(len <= data_.buffer.size());
    std::copy_n(str, len, data_.buffer.data());
    if (len < data_.buffer.size())
        data_.buffer[len] = 0;
    data_.type = type_ssoString;
}

void
Value::initAllocatedString(const char* str, std::size_t len)
{
    const char* dup = valueAllocator->duplicateStringValue(str, len);
    assert(dup != nullptr);
    std::construct_at(reinterpret_cast<const char**>(data_.buffer.data()), dup);
    data_.type = type_allocatedString;
}

void
Value::initStaticString(const char* str) noexcept
{
    std::construct_at(reinterpret_cast<const char**>(data_.buffer.data()), str);
    data_.type = type_staticString;
}

Value::Value(ValueType type)
{
    switch (type)
    {
        case nullValue:
            data_.type = type_null;
            break;

        case intValue:
            std::construct_at(
                reinterpret_cast<std::int64_t*>(data_.buffer.data()),
                std::int64_t{0});
            data_.type = type_int;
            break;

        case uintValue:
            std::construct_at(
                reinterpret_cast<std::int64_t*>(data_.buffer.data()),
                std::int64_t{0});
            data_.type = type_uint;
            break;

        case realValue:
            std::construct_at(
                reinterpret_cast<double*>(data_.buffer.data()), 0.0);
            data_.type = type_real;
            break;

        case stringValue:
            data_.buffer[0] = '\0';
            data_.type = type_ssoString;
            break;

        case booleanValue:
            data_.buffer[0] = 0;
            data_.type = type_boolean;
            break;

        case arrayValue:
            std::construct_at(
                reinterpret_cast<ArrayStorage**>(data_.buffer.data()),
                new ArrayStorage());
            data_.type = type_array;
            break;

        case objectValue:
            std::construct_at(
                reinterpret_cast<ObjectStorage**>(data_.buffer.data()),
                new ObjectStorage());
            data_.type = type_object;
            break;

        default:
            JSON_ASSERT_UNREACHABLE;
    }
}

Value::Value(Int value) noexcept
{
    std::construct_at(
        reinterpret_cast<std::int64_t*>(data_.buffer.data()),
        static_cast<std::int64_t>(value));
    data_.type = type_int;
}

Value::Value(UInt value) noexcept
{
    std::construct_at(
        reinterpret_cast<std::int64_t*>(data_.buffer.data()),
        static_cast<std::int64_t>(value));
    data_.type = type_uint;
}

Value::Value(double value) noexcept
{
    std::construct_at(reinterpret_cast<double*>(data_.buffer.data()), value);
    data_.type = type_real;
}

Value::Value(std::string_view value)
{
    if (value.size() <= data_.buffer.size())
        initSSO(value.data(), value.size());
    else
        initAllocatedString(value.data(), value.size());
}

Value::Value(StaticString value) noexcept
{
    const char* str = value.c_str();

    if (str == nullptr || *str == '\0') [[unlikely]]
    {
        data_.type = type_ssoString;
        return;
    }

    std::size_t len = std::strlen(str);
    if (len <= data_.buffer.size())
        initSSO(str, len);
    else
        initStaticString(str);
}

Value::Value(bool value) noexcept
{
    data_.buffer[0] = value ? 1 : 0;
    data_.type = type_boolean;
}

Value::Value(Value const& other)
{
    switch (other.data_.type)
    {
        case type_ssoString:
            data_ = other.data_;
            break;

        case type_allocatedString:
            initAllocatedString(
                other.as<const char*>(), std::strlen(other.as<const char*>()));
            break;

        case type_staticString:
            initStaticString(other.as<const char*>());
            break;

        case type_int:
        case type_uint:
            std::construct_at(
                reinterpret_cast<std::int64_t*>(data_.buffer.data()),
                other.as<std::int64_t>());
            data_.type = other.data_.type;
            break;

        case type_real:
            std::construct_at(
                reinterpret_cast<double*>(data_.buffer.data()),
                other.as<double>());
            data_.type = type_real;
            break;

        case type_boolean:
            data_.buffer[0] = other.data_.buffer[0];
            data_.type = type_boolean;
            break;

        case type_array:
            std::construct_at(
                reinterpret_cast<ArrayStorage**>(data_.buffer.data()),
                new ArrayStorage(*other.as<ArrayStorage*>()));
            data_.type = type_array;
            break;

        case type_object:
            std::construct_at(
                reinterpret_cast<ObjectStorage**>(data_.buffer.data()),
                new ObjectStorage(*other.as<ObjectStorage*>()));
            data_.type = type_object;
            break;

        case type_null:
        default:
            data_.type = type_null;
            break;
    }
}

Value::Value(Value&& other) noexcept : data_(other.data_)
{
    other.data_.type = type_null;
}

Value::~Value() noexcept
{
    if (data_.type == type_allocatedString)
        valueAllocator->releaseStringValue(as<const char*>());

    if (data_.type == type_array)
        delete as<ArrayStorage*>();

    if (data_.type == type_object)
        delete as<ObjectStorage*>();

    data_.type = type_destroyed;
}

Value&
Value::operator=(Value&& other) noexcept
{
    if (this != &other) [[likely]]
    {
        // This temporary is needed to handle the case were an object
        // is assigned to one of its sub-objects, like the following:
        //     obj = obj["test"][3];
        Value tmp(std::move(other));
        std::swap(data_, tmp.data_);
    }

    return *this;
}

Value&
Value::operator=(Value const& other)
{
    if (this != &other)
        *this = Value(other);

    return *this;
}

ValueType
Value::type() const noexcept
{
    switch (data_.type)
    {
        case type_ssoString:
        case type_allocatedString:
        case type_staticString:
            return stringValue;

        case type_int:
            return intValue;

        case type_uint:
            return uintValue;

        case type_boolean:
            return booleanValue;

        case type_real:
            return realValue;

        case type_array:
            return arrayValue;

        case type_object:
            return objectValue;

        case type_null:
            return nullValue;

        default:
            return nullValue;
    }
}

char const*
Value::asCString() const noexcept
{
    if (isSSO())
        return reinterpret_cast<char const*>(data_.buffer.data());

    if (isAllocatedString() || isStaticStringType())
        return as<const char*>();

    return "";
}

std::string
Value::asString() const
{
    switch (data_.type)
    {
        case type_ssoString:
            return std::string(
                reinterpret_cast<const char*>(data_.buffer.data()));

        case type_allocatedString:
        case type_staticString:
            return std::string(as<const char*>());

        case type_null:
            return "";

        case type_boolean:
            return data_.buffer[0] ? "true" : "false";

        case type_int:
        case type_uint:
            return std::to_string(as<std::int64_t>());

        case type_real:
            return std::to_string(as<double>());

        case type_array:
        case type_object:
            JSON_ASSERT_MESSAGE(false, "Type is not convertible to string");

        default:
            JSON_ASSERT_UNREACHABLE;
    }

    return "";
}

Value::Int
Value::asInt() const
{
    switch (data_.type)
    {
        case type_int:
        case type_uint: {
            auto v = as<std::int64_t>();
            JSON_ASSERT_MESSAGE(
                v >= minInt && v <= maxInt,
                "integer out of signed integer range");
            return static_cast<Int>(v);
        }

        case type_real: {
            auto v = as<double>();
            JSON_ASSERT_MESSAGE(
                v >= minInt && v <= maxInt, "Real out of signed integer range");
            return static_cast<Int>(v);
        }

        case type_null:
            return 0;

        case type_boolean:
            return data_.buffer[0] ? 1 : 0;

        case type_ssoString:
            return beast::lexicalCastThrow<int>(
                reinterpret_cast<const char*>(data_.buffer.data()));

        case type_allocatedString:
        case type_staticString:
            return beast::lexicalCastThrow<int>(as<const char*>());

        case type_array:
        case type_object:
            JSON_ASSERT_MESSAGE(false, "Type is not convertible to int");

        default:
            JSON_ASSERT_UNREACHABLE;
    }

    return 0;
}

Value::UInt
Value::asUInt() const
{
    switch (data_.type)
    {
        case type_int:
        case type_uint: {
            auto v = as<std::int64_t>();
            JSON_ASSERT_MESSAGE(
                v >= 0 && static_cast<std::uint64_t>(v) <= maxUInt,
                "integer out of unsigned integer range");
            return static_cast<UInt>(v);
        }

        case type_real: {
            auto v = as<double>();
            JSON_ASSERT_MESSAGE(
                v >= 0 && v <= maxUInt, "Real out of unsigned integer range");
            return static_cast<UInt>(v);
        }

        case type_null:
            return 0;

        case type_boolean:
            return data_.buffer[0] ? 1 : 0;

        case type_ssoString:
            return beast::lexicalCastThrow<unsigned int>(
                reinterpret_cast<const char*>(data_.buffer.data()));

        case type_allocatedString:
        case type_staticString:
            return beast::lexicalCastThrow<unsigned int>(as<const char*>());

        case type_array:
        case type_object:
            JSON_ASSERT_MESSAGE(false, "Type is not convertible to uint");

        default:
            JSON_ASSERT_UNREACHABLE;
    }

    return 0;
}

double
Value::asDouble() const
{
    switch (data_.type)
    {
        case type_real:
            return as<double>();

        case type_int:
        case type_uint:
            return static_cast<double>(as<std::int64_t>());

        case type_null:
            return 0.0;

        case type_boolean:
            return data_.buffer[0] ? 1.0 : 0.0;

        case type_ssoString:
        case type_allocatedString:
        case type_staticString:
        case type_array:
        case type_object:
            JSON_ASSERT_MESSAGE(false, "Type is not convertible to double");

        default:
            JSON_ASSERT_UNREACHABLE;
    }

    return 0.0;
}

bool
Value::asBool() const noexcept
{
    switch (data_.type)
    {
        case type_boolean:
            return data_.buffer[0] != 0;

        case type_null:
            return false;

        case type_int:
        case type_uint:
            return as<std::int64_t>() != 0;

        case type_real:
            return as<double>() != 0.0;

        case type_ssoString:
            return data_.buffer[0] != '\0';

        case type_allocatedString:
        case type_staticString:
            return as<const char*>()[0] != '\0';

        case type_array:
            return !as<ArrayStorage*>()->empty();

        case type_object:
            return !as<ObjectStorage*>()->empty();

        default:
            return false;
    }
}

bool
Value::isNull() const noexcept
{
    return data_.type == type_null;
}

bool
Value::isBool() const noexcept
{
    return data_.type == type_boolean;
}

bool
Value::isInt() const noexcept
{
    return data_.type == type_int;
}

bool
Value::isUInt() const noexcept
{
    return data_.type == type_uint;
}

bool
Value::isIntegral() const noexcept
{
    return data_.type == type_int || data_.type == type_uint ||
        data_.type == type_boolean;
}

bool
Value::isDouble() const noexcept
{
    return data_.type == type_real;
}

bool
Value::isNumeric() const noexcept
{
    return isIntegral() || isDouble();
}

bool
Value::isString() const noexcept
{
    return isAnyString();
}

bool
Value::isArray() const noexcept
{
    return data_.type == type_array;
}

bool
Value::isArrayOrNull() const noexcept
{
    return data_.type == type_null || data_.type == type_array;
}

bool
Value::isObject() const noexcept
{
    return data_.type == type_object;
}

bool
Value::isObjectOrNull() const noexcept
{
    return data_.type == type_null || data_.type == type_object;
}

bool
Value::isConvertibleTo(ValueType other) const noexcept
{
    auto t = type();

    if (other == t || t == nullValue)
        return true;

    switch (data_.type)
    {
        case type_int: {
            auto v = as<std::int64_t>();
            return (other == nullValue && v == 0) ||
                (other == uintValue && v >= 0) || other == realValue ||
                other == stringValue || other == booleanValue;
        }

        case type_uint: {
            auto v = as<std::int64_t>();
            return (other == nullValue && v == 0) ||
                (other == intValue && v <= maxInt) || other == realValue ||
                other == stringValue || other == booleanValue;
        }

        case type_real: {
            auto v = as<double>();
            return (other == nullValue && v == 0.0) ||
                (other == intValue && v >= minInt && v <= maxInt) ||
                (other == uintValue && v >= 0 && v <= maxUInt) ||
                other == stringValue || other == booleanValue;
        }

        case type_boolean:
            return (other == nullValue && data_.buffer[0] == 0) ||
                other == intValue || other == uintValue || other == realValue ||
                other == stringValue;

        case type_ssoString:
            return other == nullValue && data_.buffer[0] == '\0';

        case type_allocatedString:
        case type_staticString:
            return other == nullValue && as<const char*>()[0] == '\0';

        case type_array:
            return other == nullValue && as<ArrayStorage*>()->empty();

        case type_object:
            return other == nullValue && as<ObjectStorage*>()->empty();

        default:
            break;
    }

    return false;
}

Value::UInt
Value::size() const noexcept
{
    switch (data_.type)
    {
        case type_array:
            return static_cast<UInt>(as<ArrayStorage*>()->size());

        case type_object:
            return static_cast<UInt>(as<ObjectStorage*>()->size());

        default:
            return 0;
    }
}

Value::operator bool() const noexcept
{
    if (isNull())
        return false;

    if (isAnyString())
        return asCString()[0] != '\0';

    if (isArray())
        return !as<ArrayStorage*>()->empty();

    if (isObject())
        return !as<ObjectStorage*>()->empty();

    return true;
}

void
Value::clear()
{
    JSON_ASSERT(
        data_.type == type_null || data_.type == type_array ||
        data_.type == type_object);

    if (data_.type == type_array)
        as<ArrayStorage*>()->clear();

    if (data_.type == type_object)
        as<ObjectStorage*>()->clear();
}

Value&
Value::operator[](UInt index)
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_array);

    if (data_.type == type_null)
        *this = Value(arrayValue);

    return as<ArrayStorage*>()->try_emplace(index).first->second;
}

Value const&
Value::operator[](UInt index) const
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_array);

    if (data_.type == type_array)
    {
        auto* arr = as<ArrayStorage const*>();

        if (auto it = arr->find(index); it != arr->end())
            return it->second;
    }

    return null;
}

Value
Value::get(UInt index, Value const& defaultValue) const
{
    const Value* value = &((*this)[index]);
    return value == &null ? defaultValue : *value;
}

Value&
Value::append(Value&& value)
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_array);

    if (data_.type == type_null)
        *this = Value(arrayValue);

    return (*this)[size()] = std::move(value);
}

Value&
Value::resolveReference(std::string_view key, bool isStatic)
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_object);

    if (data_.type == type_null)
        *this = Value(objectValue);

    auto* obj = as<ObjectStorage*>();
    assert(obj);

    if (auto it = obj->find(key); it != obj->end())
        return it->second;

    if (!isStatic)
        return obj->emplace(key, null).first->second;

    return obj->emplace(StaticString(key.data()), null).first->second;
}

Value&
Value::operator[](std::string_view key)
{
    return resolveReference(key, false);
}

Value const&
Value::operator[](std::string_view key) const
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_object);

    if (data_.type == type_object)
    {
        auto* obj = as<ObjectStorage const*>();
        auto it = obj->find(key);

        if (it != obj->end())
            return it->second;
    }

    return null;
}

Value&
Value::operator[](StaticString const& key)
{
    return resolveReference(key.c_str(), true);
}

Value const&
Value::operator[](StaticString const& key) const
{
    return (*this)[key.c_str()];
}

Value
Value::get(const char* key, Value const& defaultValue) const
{
    if (data_.type == type_object)
    {
        auto* obj = as<ObjectStorage const*>();
        auto it = obj->find(std::string_view(key));

        if (it != obj->end())
            return it->second;
    }

    return defaultValue;
}

Value
Value::get(std::string const& key, Value const& defaultValue) const
{
    return get(key.c_str(), defaultValue);
}

Value
Value::removeMember(const char* key)
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_object);

    if (data_.type == type_object)
    {
        auto* obj = as<ObjectStorage*>();

        if (auto it = obj->find(std::string_view(key)); it != obj->end())
        {
            Value old(std::move(it->second));
            obj->erase(it);
            return old;
        }
    }

    return null;
}

Value
Value::removeMember(std::string const& key)
{
    return removeMember(key.c_str());
}

bool
Value::isMember(const char* key) const
{
    if (data_.type != type_object)
        return false;

    auto* obj = as<ObjectStorage const*>();
    return obj->find(std::string_view(key)) != obj->end();
}

bool
Value::isMember(std::string const& key) const
{
    return isMember(key.c_str());
}

Value::Members
Value::getMemberNames() const
{
    JSON_ASSERT(data_.type == type_null || data_.type == type_object);

    Members members;

    if (data_.type == type_object)
    {
        auto* obj = as<ObjectStorage const*>();

        if (!obj->empty())
        {
            members.reserve(obj->size());

            for (const auto& [k, v] : *obj)
                members.push_back(std::string(k.c_str()));
        }
    }

    return members;
}

std::string
Value::toStyledString() const
{
    StyledWriter writer;
    return writer.write(*this);
}

Value::const_iterator
Value::cbegin() const
{
    if (data_.type == type_array)
        return const_iterator(as<ArrayStorage*>()->cbegin());

    if (data_.type == type_object)
        return const_iterator(as<ObjectStorage*>()->cbegin());

    return {};
}

Value::const_iterator
Value::begin() const
{
    if (data_.type == type_array)
        return const_iterator(as<ArrayStorage*>()->cbegin());

    if (data_.type == type_object)
        return const_iterator(as<ObjectStorage*>()->cbegin());

    return {};
}

Value::const_iterator
Value::cend() const
{
    if (data_.type == type_array)
        return const_iterator(as<ArrayStorage*>()->cend());

    if (data_.type == type_object)
        return const_iterator(as<ObjectStorage*>()->cend());

    return {};
}

Value::const_iterator
Value::end() const
{
    if (data_.type == type_array)
        return const_iterator(as<ArrayStorage*>()->cend());

    if (data_.type == type_object)
        return const_iterator(as<ObjectStorage*>()->cend());

    return {};
}

Value::iterator
Value::begin()
{
    if (data_.type == type_array)
        return iterator(as<ArrayStorage*>()->begin());

    if (data_.type == type_object)
        return iterator(as<ObjectStorage*>()->begin());

    return {};
}

Value::iterator
Value::end()
{
    if (data_.type == type_array)
        return iterator(as<ArrayStorage*>()->end());

    if (data_.type == type_object)
        return iterator(as<ObjectStorage*>()->end());

    return {};
}

// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// Comparison operators
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////

std::partial_ordering
operator<=>(Value const& x, Value const& y) noexcept
{
    auto const xType = x.type();
    auto const yType = y.type();

    if ((xType == intValue || xType == uintValue) &&
        (yType == intValue || yType == uintValue))
        return x.as<std::int64_t>() <=> y.as<std::int64_t>();

    if (xType != yType)
        return xType <=> yType;

    if (xType == nullValue)
        return std::partial_ordering::equivalent;

    if (xType == realValue)
        return x.as<double>() <=> y.as<double>();

    if (xType == booleanValue)
        return x.data_.buffer[0] <=> y.data_.buffer[0];

    if (xType == stringValue)
        return std::string_view(x.asCString()) <=>
            std::string_view(y.asCString());

    if (xType == arrayValue)
        return *x.as<Value::ArrayStorage const*>() <=>
            *y.as<Value::ArrayStorage const*>();

    if (xType == objectValue)
        return *x.as<Value::ObjectStorage const*>() <=>
            *y.as<Value::ObjectStorage const*>();

    JSON_ASSERT_UNREACHABLE;
}

bool
operator==(Value const& x, Value const& y) noexcept
{
    auto const xType = x.type();
    auto const yType = y.type();

    if ((xType == intValue || xType == uintValue) &&
        (yType == intValue || yType == uintValue))
        return x.as<std::int64_t>() == y.as<std::int64_t>();

    if (xType != yType)
        return false;

    if (xType == nullValue)
        return true;

    if (xType == realValue)
        return x.as<double>() == y.as<double>();

    if (xType == booleanValue)
        return x.data_.buffer[0] == y.data_.buffer[0];

    if (xType == stringValue)
        return std::string_view(x.asCString()) ==
            std::string_view(y.asCString());

    if (xType == arrayValue)
        return *x.as<Value::ArrayStorage const*>() ==
            *y.as<Value::ArrayStorage const*>();

    if (xType == objectValue)
        return *x.as<Value::ObjectStorage const*>() ==
            *y.as<Value::ObjectStorage const*>();

    JSON_ASSERT_UNREACHABLE;
}

}  // namespace Json
