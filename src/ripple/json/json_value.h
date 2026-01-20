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

#ifndef RIPPLE_JSON_JSON_VALUE_H_INCLUDED
#define RIPPLE_JSON_JSON_VALUE_H_INCLUDED

#include <ripple/json/json_forwards.h>

#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>

#include <array>
#include <cassert>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/** JSON (JavaScript Object Notation).
 */
namespace Json {

/** Type of the value held by a Value object.

    These values are actually _meaningful_ and form part of the public
    API for a surprising-but-not-surprising reason: when comparing two
    Value instances, the one whose ValueType is numerically less comes
    first (except: intValue and uintValue are treated as equal in that
    case and we simply compare the underlying integers).
 */
enum ValueType : std::uint8_t {
    nullValue = 0,
    intValue = 1,
    uintValue = 2,
    realValue = 3,
    stringValue = 4,
    booleanValue = 5,
    arrayValue = 6,
    objectValue = 7
};

/** Lightweight wrapper around string literals.

    We use this type of string to avoid the cost of string duplication
    when storing the string and/or member name. Such strings cannot be
    null and must be null-terminated.
 */
class StaticString
{
public:
    constexpr explicit StaticString(char const* czstring) noexcept
        : str_(czstring ? czstring : "")
    {
    }

    constexpr StaticString(StaticString const&) = default;
    constexpr StaticString&
    operator=(StaticString const&) = default;

    StaticString(std::nullptr_t) = delete;

    constexpr
    operator char const*() const noexcept
    {
        return str_;
    }

    [[nodiscard]] constexpr char const*
    c_str() const noexcept
    {
        return str_;
    }

private:
    char const* str_;
};

inline bool
operator==(StaticString x, StaticString y) noexcept
{
    return std::strcmp(x.c_str(), y.c_str()) == 0;
}

inline bool
operator!=(StaticString x, StaticString y) noexcept
{
    return !(x == y);
}

inline bool
operator==(std::string const& x, StaticString y) noexcept
{
    return std::strcmp(x.c_str(), y.c_str()) == 0;
}

inline bool
operator!=(std::string const& x, StaticString y) noexcept
{
    return !(x == y);
}

inline bool
operator==(StaticString x, std::string const& y) noexcept
{
    return y == x;
}

inline bool
operator!=(StaticString x, std::string const& y) noexcept
{
    return !(y == x);
}

/** Allocator to optimize string value memory management done by Value.

    - makeMemberName() and releaseMemberName() are called to respectively
      duplicate and free an Json::objectValue member name.
    - duplicateStringValue() and releaseStringValue() are called similarly to
      duplicate and free a Json::stringValue value.
 */
class ValueAllocator
{
protected:
    virtual char*
    allocate(std::size_t length)
    {
        assert(length > 1);
        return new char[length];
    }

    virtual void
    release(char const* value)
    {
        delete[] value;
    }

public:
    virtual ~ValueAllocator() = default;

    char const*
    duplicateStringValue(
        char const* value,
        std::size_t length = std::numeric_limits<std::size_t>::max())
    {
        if (value != nullptr &&
            length == std::numeric_limits<std::size_t>::max())
            length = std::strlen(value);

        if (value == nullptr || *value == 0 || length == 0)
            return nullptr;

        auto ret = allocate(length + 1);
        std::memcpy(ret, value, length);
        ret[length] = 0;
        return ret;
    }

    void
    releaseStringValue(char const* value)
    {
        release(value);
    }

    char const*
    makeMemberName(char const* memberName)
    {
        return duplicateStringValue(memberName);
    }

    void
    releaseMemberName(char const* memberName)
    {
        releaseStringValue(memberName);
    }
};

/** Assigns an allocator to use for string-related JSON memory requests.

    @param allocator The allocator to use. Must not be null.

    @note This can only be called once, and should be called early.
 */
void
setAllocator(ValueAllocator* allocator);

// Forward declaration for access in CZString
ValueAllocator*
getAllocator();

/** Represents a JSON value.

    This class is a discriminated union wrapper that can represent a:
    - signed integer [range: Value::minInt - Value::maxInt]
    - unsigned integer (range: 0 - Value::maxUInt)
    - double
    - UTF-8 string
    - boolean
    - 'null'
    - an ordered list of Value
    - collection of name/value pairs (javascript object)

    The type of the held value is represented by a ValueType and
    can be obtained using type().

    Values of an objectValue or arrayValue can be accessed using operator[]()
    methods. Non-const methods will automatically create a nullValue element
    if it does not exist.

    @note Small string optimization: strings of 15 characters or fewer are
          stored inline without allocation.

    @note Integer values (both signed and unsigned) are stored internally as
          std::int64_t. Range checking is performed on extraction.
 */
class Value
{
public:
    /** Key type for object members: a wrapper around a pointer to a C string.
     */
    class CZString
    {
        static constexpr std::uintptr_t static_flag = std::uintptr_t{1} << 63;
        static constexpr std::uintptr_t pointer_mask = ~static_flag;

        /** A tagged pointer to the key string.

            If the high bit is set, then the string pointed to by this value,
            after the high bit is cleared, either is a string literal, or it
            has its lifetime managed externally and is guaranteed to outlive
            this object.

            @note We assume that the high bit is never set on the platforms
                  we support. This assumption is unlikely to ever change in
                  the future.
         */
        std::uintptr_t data_;

    public:
        CZString() noexcept : data_(static_flag)
        {
        }

        CZString(StaticString s) noexcept
            : data_(reinterpret_cast<std::uintptr_t>(s.c_str()) | static_flag)
        {
        }

        CZString(std::string_view s);

        CZString(char const* s) : CZString(std::string_view(s ? s : ""))
        {
        }

        CZString(const CZString& other);
        CZString&
        operator=(const CZString&);

        CZString(CZString&& other) noexcept : data_(other.data_)
        {
            other.data_ = static_flag;
        }

        ~CZString();
        CZString&
        operator=(CZString&& other) noexcept;

        [[nodiscard]] char const*
        c_str() const noexcept
        {
            return reinterpret_cast<char const*>(data_ & pointer_mask);
        }

        [[nodiscard]] bool
        isStatic() const noexcept
        {
            return (data_ & static_flag) != 0;
        }

        [[nodiscard]] std::strong_ordering
        operator<=>(const CZString& other) const
        {
            return std::string_view(c_str()) <=>
                std::string_view(other.c_str());
        }

        [[nodiscard]] bool
        operator==(const CZString& other) const
        {
            return std::string_view(c_str()) == std::string_view(other.c_str());
        }
    };

    static_assert(sizeof(CZString) == 8);

    // Storage types - defined after Value is complete
    struct ArrayStorage;
    struct ObjectStorage;

private:
    template <bool IsConst>
    class ValueIteratorImpl;

    friend std::partial_ordering
    operator<=>(const Value&, const Value&) noexcept;
    friend bool
    operator==(const Value&, const Value&) noexcept;

public:
    using Members = std::vector<std::string>;
    using iterator = ValueIteratorImpl<false>;
    using const_iterator = ValueIteratorImpl<true>;
    using UInt = Json::UInt;
    using Int = Json::Int;
    using ArrayIndex = UInt;

    static constexpr Int minInt = std::numeric_limits<Int>::min();
    static constexpr Int maxInt = std::numeric_limits<Int>::max();
    static constexpr UInt maxUInt = std::numeric_limits<UInt>::max();

    static constexpr std::size_t defaultArrayCapacity = 8;
    static constexpr std::size_t defaultObjectCapacity = 16;

private:
    // Internal storage types. We can't directly use ValueType
    // because we need the value 0 to serve double duty: it is
    // the identifier used for "small strings" and also serves
    // as the NUL for the longest possible small string.
    static constexpr std::uint8_t type_ssoString = 0;
    static constexpr std::uint8_t type_allocatedString = 1;
    static constexpr std::uint8_t type_staticString = 2;
    static constexpr std::uint8_t type_int = 3;
    static constexpr std::uint8_t type_uint = 4;
    static constexpr std::uint8_t type_boolean = 5;
    static constexpr std::uint8_t type_real = 6;
    static constexpr std::uint8_t type_array = 7;
    static constexpr std::uint8_t type_object = 8;
    static constexpr std::uint8_t type_null = 9;
    static constexpr std::uint8_t type_destroyed = 10;

    static_assert(
        type_ssoString == 0,
        "JSON SSO support requires type_ssoString to be 0");

public:
    /** Default constructor.

        Creates a null value.
     */
    constexpr Value() noexcept = default;

    /** Create a Value of the given type.

        To create an empty array, pass arrayValue.
        To create an empty object, pass objectValue.
        Another Value can then be set to this one by assignment.

        @param type The type of value to create.
     */
    Value(ValueType type);

    /** Create a signed integer value.

        @param value The integer value. Stored internally as std::int64_t.
     */
    Value(Int value) noexcept;

    /** Create an unsigned integer value.

        @param value The unsigned integer value. Stored internally as
       std::int64_t.
     */
    Value(UInt value) noexcept;

    /** Create a double value.

        @param value The floating-point value.
     */
    Value(double value) noexcept;

    /** @{ */
    /** Create a string value.

        Uses small string optimization for strings <= 15 characters.

        @param value The string. If empty, creates an empty SSO string.
     */

    Value(std::string_view value);

    Value(char const* value) : Value(std::string_view{value ? value : ""})
    {
    }

    Value(std::string const& value) : Value(std::string_view(value))
    {
    }
    /** @} */

    /** Prevent construction from nullptr. */
    Value(std::nullptr_t) = delete;

    /** Create a string value from a static string.

        Does not duplicate the string for internal storage. The given string
        must remain alive for the lifetime of this Value.

        @param value The static string wrapper.

        @note If the string is <= 15 characters, it will be copied into
              SSO storage regardless.
     */
    Value(StaticString value) noexcept;

    /** Create a boolean value.

        @param value The boolean value.
     */
    Value(bool value) noexcept;

    /** Copy constructor.

        @param other The value to copy.
     */
    Value(const Value& other);

    /** Move constructor.

        @param other The value to move from. Left in null state.
     */
    Value(Value&& other) noexcept;

    ~Value() noexcept;

    Value&
    operator=(Value const& other);
    Value&
    operator=(Value&& other) noexcept;

    //     /** Swap values.
    //
    //         @param other The value to swap with.
    //      */
    //     void
    //     swap(Value& other) noexcept;

    /** Returns the type of the held value.

        @note All internal string representations return stringValue.
     */
    [[nodiscard]] ValueType
    type() const noexcept;

    /** Returns the value as a C string.

        @return The string value, or empty string if not a string type.
     */
    [[nodiscard]] char const*
    asCString() const noexcept;

    /** Returns the value as a std::string.

        Numeric types are converted to their string representation.

        @return The string representation of the value.
     */
    [[nodiscard]] std::string
    asString() const;

    /** Returns the value as a signed integer.

        @return The integer value.

        @note Asserts if the stored value is outside [minInt, maxInt].
     */
    [[nodiscard]] Int
    asInt() const;

    /** Returns the value as an unsigned integer.

        @return The unsigned integer value.

        @note Asserts if the stored value is negative or exceeds maxUInt.
     */
    [[nodiscard]] UInt
    asUInt() const;

    /** Returns the value as a double.

        @return The floating-point value.
     */
    [[nodiscard]] double
    asDouble() const;

    /** Returns the value as a boolean.

        @return The boolean value.
     */
    [[nodiscard]] bool
    asBool() const noexcept;

    [[nodiscard]] bool
    isNull() const noexcept;
    [[nodiscard]] bool
    isBool() const noexcept;
    [[nodiscard]] bool
    isInt() const noexcept;
    [[nodiscard]] bool
    isUInt() const noexcept;
    [[nodiscard]] bool
    isIntegral() const noexcept;
    [[nodiscard]] bool
    isDouble() const noexcept;
    [[nodiscard]] bool
    isNumeric() const noexcept;
    [[nodiscard]] bool
    isString() const noexcept;
    [[nodiscard]] bool
    isArray() const noexcept;
    [[nodiscard]] bool
    isArrayOrNull() const noexcept;
    [[nodiscard]] bool
    isObject() const noexcept;
    [[nodiscard]] bool
    isObjectOrNull() const noexcept;

    /** Check if this value can be converted to the given type.

        @param other The target type.

        @return true if conversion is possible.
     */
    [[nodiscard]] bool
    isConvertibleTo(ValueType other) const noexcept;

    /** Returns the number of elements in an array or object.

        @return For arrays, returns element count. For objects, returns
                member count. For other types, returns 0.
     */
    [[nodiscard]] UInt
    size() const noexcept;

    /** Returns false if this is null, empty array/object, or empty string.
     */
    explicit
    operator bool() const noexcept;

    /** Remove all object members and array elements.

        @note type() must be arrayValue, objectValue, or nullValue.
     */
    void
    clear();

    /** Access an array element by index.

        If the array contains fewer than index+1 elements, null values are
        inserted to extend the array.

        @param index Zero-based array index.

        @return Reference to the element.

        @note You may need to write value[0u] to disambiguate from the
              string key overload.
     */
    Value&
    operator[](UInt index);

    /** Access an array element by index (const).

        @param index Zero-based array index.

        @return Reference to the element, or null if out of bounds.
     */
    [[nodiscard]] const Value&
    operator[](UInt index) const;

    /** Get an array element with default.

        @param index        Zero-based array index.
        @param defaultValue Value to return if index is out of bounds.

        @return The element value or defaultValue.
     */
    [[nodiscard]] Value
    get(UInt index, const Value& defaultValue) const;

    /** Append value to array at the end.

        Equivalent to jsonvalue[jsonvalue.size()] = value.

        @param value The value to append.

        @return Reference to the appended element.
     */
    Value&
    append(Value&& value);

    Value&
    append(Value const& value)
    {
        return append(Value(value));
    }

    /** Access an object member by key.

        Creates a null member if it does not exist.

        @param key The member name.

        @return Reference to the member value.
     */
    Value&
    operator[](std::string_view key);

    /** Access an object member by key (const).

        @param key The member name.

        @return Reference to the member value, or null if not found.
     */
    [[nodiscard]] const Value&
    operator[](std::string_view key) const;

    /** Access an object member by static string key.

        The member name is not duplicated if it doesn't exist.

        @param key The static string key.

        @return Reference to the member value.
     */
    Value&
    operator[](StaticString const& key);
    Value const&
    operator[](StaticString const& key) const;

    /** Get an object member with default.

        @param key          The member name.
        @param defaultValue Value to return if member doesn't exist.

        @return The member value or defaultValue.
     */
    [[nodiscard]] Value
    get(char const* key, const Value& defaultValue) const;
    [[nodiscard]] Value
    get(std::string const& key, const Value& defaultValue) const;

    /** Remove and return a member.

        @param key The member name.

        @return The removed value, or null if not found.

        @note type() must be objectValue or nullValue.
     */
    Value
    removeMember(char const* key);
    Value
    removeMember(std::string const& key);

    /** Check if a member exists.

        @param key The member name.

        @return true if the member exists.
     */
    [[nodiscard]] bool
    isMember(char const* key) const;
    [[nodiscard]] bool
    isMember(std::string const& key) const;

    /** Return a list of member names.

        @return Vector of member names. Empty if null or not an object.

        @note type() must be objectValue or nullValue.
     */
    [[nodiscard]] Members
    getMemberNames() const;

    /** Return a styled string representation.

        @return JSON-formatted string.
     */
    [[nodiscard]] std::string
    toStyledString() const;

    [[nodiscard]] const_iterator
    cbegin() const;

    [[nodiscard]] const_iterator
    begin() const;

    [[nodiscard]] const_iterator
    cend() const;

    [[nodiscard]] const_iterator
    end() const;

    [[nodiscard]] iterator
    begin();

    [[nodiscard]] iterator
    end();

private:
    Value&
    resolveReference(std::string_view key, bool isStatic);

    template <typename T>
    [[nodiscard]] T&
    as() noexcept
    {
        static_assert(sizeof(T) <= sizeof(data_.buffer));
        static_assert(alignof(T) <= alignof(decltype(data_)));
        return *std::launder(reinterpret_cast<T*>(data_.buffer.data()));
    }

    template <typename T>
    [[nodiscard]] T const&
    as() const noexcept
    {
        static_assert(sizeof(T) <= sizeof(data_.buffer));
        static_assert(alignof(T) <= alignof(decltype(data_)));
        return *std::launder(reinterpret_cast<const T*>(data_.buffer.data()));
    }

    [[nodiscard]] bool
    isSSO() const noexcept
    {
        return data_.type == type_ssoString;
    }

    [[nodiscard]] bool
    isAllocatedString() const noexcept
    {
        return data_.type == type_allocatedString;
    }

    [[nodiscard]] bool
    isStaticStringType() const noexcept
    {
        return data_.type == type_staticString;
    }

    [[nodiscard]] bool
    isAnyString() const noexcept
    {
        return data_.type <= type_staticString;
    }

    [[nodiscard]] bool
    isIntegralType() const noexcept
    {
        return data_.type == type_int || data_.type == type_uint;
    }

    void
    initSSO(char const* str, std::size_t len) noexcept;
    void
    initAllocatedString(char const* str, std::size_t len);
    void
    initStaticString(char const* str) noexcept;

    // This structure defines the binary layout of the Value type and
    // is manually and deliberately aligned to ensure that the buffer
    // is, itself, aligned correctly for any of the types that it may
    // contain.
    struct alignas(8) Data
    {
        std::array<std::uint8_t, 15> buffer;
        std::uint8_t type = type_null;

        constexpr Data() noexcept : buffer{}
        {
        }

        explicit Data(std::uint8_t t) noexcept : type(t)
        {
        }
    } data_;
};

static_assert(sizeof(Value) == 16, "Value must be exactly 16 bytes");
static_assert(alignof(Value) == 8, "Value alignment mismatch");

// Define storage types now that Value is complete
struct Value::ArrayStorage : std::map<ArrayIndex, Value>
{
    using map::map;

    std::partial_ordering
    operator<=>(const ArrayStorage& other) const noexcept
    {
        return std::lexicographical_compare_three_way(
            begin(),
            end(),
            other.begin(),
            other.end(),
            [](const value_type& a,
               const value_type& b) -> std::partial_ordering {
                if (auto cmp = a.first <=> b.first; cmp != 0)
                    return cmp;
                return a.second <=> b.second;
            });
    }

    bool
    operator==(const ArrayStorage& other) const noexcept
    {
        return size() == other.size() &&
            std::equal(
                   begin(),
                   end(),
                   other.begin(),
                   [](const value_type& a, const value_type& b) {
                       return a.first == b.first && a.second == b.second;
                   });
    }
};

struct Value::ObjectStorage : std::map<CZString, Value, std::less<>>
{
    using map::map;

    std::partial_ordering
    operator<=>(const ObjectStorage& other) const noexcept
    {
        return std::lexicographical_compare_three_way(
            begin(),
            end(),
            other.begin(),
            other.end(),
            [](const value_type& a,
               const value_type& b) -> std::partial_ordering {
                if (auto cmp = a.first <=> b.first; cmp != 0)
                    return cmp;
                return a.second <=> b.second;
            });
    }

    bool
    operator==(const ObjectStorage& other) const noexcept
    {
        return size() == other.size() &&
            std::equal(
                   begin(),
                   end(),
                   other.begin(),
                   [](const value_type& a, const value_type& b) {
                       return a.first == b.first && a.second == b.second;
                   });
    }
};

// Iterator implementation - must come after storage types are defined
template <bool IsConst>
class Value::ValueIteratorImpl
{
    friend class Value;
    friend class ValueIteratorImpl<!IsConst>;

    using ArrayIterator = std::conditional_t<
        IsConst,
        ArrayStorage::const_iterator,
        ArrayStorage::iterator>;

    using ObjectIterator = std::conditional_t<
        IsConst,
        ObjectStorage::const_iterator,
        ObjectStorage::iterator>;

    std::variant<std::monostate, ArrayIterator, ObjectIterator> current_;

public:
    using difference_type = std::ptrdiff_t;
    using value_type = Value;
    using reference = std::conditional_t<IsConst, const Value&, Value&>;
    using pointer = std::conditional_t<IsConst, const Value*, Value*>;
    using iterator_category = std::bidirectional_iterator_tag;

    ValueIteratorImpl() = default;

    template <bool OtherConst>
        requires(IsConst && !OtherConst)
    ValueIteratorImpl(ValueIteratorImpl<OtherConst> const& other)
        : current_(std::visit(
              [](const auto& it) -> decltype(current_) {
                  if constexpr (std::is_same_v<
                                    std::decay_t<decltype(it)>,
                                    std::monostate>)
                      return std::monostate{};
                  else
                      return it;
              },
              other.current_))
    {
    }

    [[nodiscard]] bool
    operator==(ValueIteratorImpl const& other) const noexcept
    {
        return current_ == other.current_;
    }

    ValueIteratorImpl&
    operator++()
    {
        std::visit(
            [](auto& it) {
                if constexpr (!std::is_same_v<
                                  std::decay_t<decltype(it)>,
                                  std::monostate>)
                    ++it;
            },
            current_);
        return *this;
    }

    ValueIteratorImpl&
    operator--()
    {
        std::visit(
            [](auto& it) {
                if constexpr (!std::is_same_v<
                                  std::decay_t<decltype(it)>,
                                  std::monostate>)
                    --it;
            },
            current_);
        return *this;
    }

    ValueIteratorImpl
    operator++(int)
    {
        auto tmp = *this;
        ++*this;
        return tmp;
    }

    ValueIteratorImpl
    operator--(int)
    {
        auto tmp = *this;
        --*this;
        return tmp;
    }

    [[nodiscard]] reference
    operator*() const
    {
        return std::visit(
            [](const auto& it) -> reference {
                if constexpr (std::is_same_v<
                                  std::decay_t<decltype(it)>,
                                  std::monostate>)
                    std::terminate();
                else
                    return it->second;
            },
            current_);
    }

    [[nodiscard]] pointer
    operator->() const
    {
        return &**this;
    }

    [[nodiscard]] char const*
    memberName() const
    {
        return std::visit(
            [](const auto& it) -> char const* {
                if constexpr (std::is_same_v<
                                  std::decay_t<decltype(it)>,
                                  ObjectIterator>)
                    return it->first.c_str();
                else
                    return "";
            },
            current_);
    }

    [[nodiscard]] Value
    key() const
    {
        return std::visit(
            [](const auto& it) -> Value {
                if constexpr (std::is_same_v<
                                  std::decay_t<decltype(it)>,
                                  ArrayIterator>)
                    return Value(it->first);
                else if constexpr (std::is_same_v<
                                       std::decay_t<decltype(it)>,
                                       ObjectIterator>)
                {
                    if (it->first.isStatic())
                        return Value(StaticString(it->first.c_str()));
                    return Value(it->first.c_str());
                }
                else
                    return {};
            },
            current_);
    }

private:
    explicit ValueIteratorImpl(ArrayIterator it) : current_(it)
    {
    }

    explicit ValueIteratorImpl(ObjectIterator it) : current_(it)
    {
    }
};

std::partial_ordering
operator<=>(const Value&, const Value&) noexcept;
bool
operator==(const Value&, const Value&) noexcept;

}  // namespace Json

#endif  // RIPPLE_JSON_JSON_VALUE_H_INCLUDED
