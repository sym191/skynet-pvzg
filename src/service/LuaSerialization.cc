#include "LuaSerialization.hpp"

#include "lua_api.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace skynet::service::detail {
namespace {

constexpr std::uint8_t kTypeMask = 0x07;
constexpr std::uint8_t kNil = 0;
constexpr std::uint8_t kBoolean = 1;
constexpr std::uint8_t kNumber = 2;
constexpr std::uint8_t kUserData = 3;
constexpr std::uint8_t kShortString = 4;
constexpr std::uint8_t kLongString = 5;
constexpr std::uint8_t kTable = 6;

constexpr std::uint8_t kNumberZero = 0;
constexpr std::uint8_t kNumberByte = 1;
constexpr std::uint8_t kNumberWord = 2;
constexpr std::uint8_t kNumberDword = 4;
constexpr std::uint8_t kNumberQword = 6;
constexpr std::uint8_t kNumberReal = 8;

constexpr std::size_t kShortStringLimit = 32;
constexpr std::size_t kInlineArrayLimit = 31;
constexpr int kMaximumDepth = 32;

static_assert(std::endian::native == std::endian::little,
              "Skynet serialization requires a little-endian host");
static_assert(sizeof(lua_Integer) == sizeof(std::int64_t));
static_assert(sizeof(lua_Number) == sizeof(double));

class SerializationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] constexpr std::uint8_t Header(
        std::uint8_t type, std::uint8_t cookie = 0) {
    return static_cast<std::uint8_t>(type | (cookie << 3));
}

class Writer {
public:
    void PushByte(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void PushBytes(const void* data, std::size_t size) {
        if (size == 0) {
            return;
        }
        const auto* begin = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), begin, begin + size);
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void PushValue(const T& value) {
        PushBytes(&value, sizeof(value));
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    Reader(const void* data, std::size_t size)
            : current_(static_cast<const std::byte*>(data)), remaining_(size) {}

    [[nodiscard]] bool empty() const noexcept { return remaining_ == 0; }

    [[nodiscard]] std::uint8_t ReadByte() {
        return ReadValue<std::uint8_t>();
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] T ReadValue() {
        if (remaining_ < sizeof(T)) {
            throw SerializationError("truncated serialized value");
        }
        T value{};
        std::memcpy(&value, current_, sizeof(value));
        current_ += sizeof(value);
        remaining_ -= sizeof(value);
        return value;
    }

    [[nodiscard]] const char* ReadString(std::size_t size) {
        if (remaining_ < size) {
            throw SerializationError("truncated serialized string");
        }
        const char* result = reinterpret_cast<const char*>(current_);
        current_ += size;
        remaining_ -= size;
        return result;
    }

private:
    const std::byte* current_;
    std::size_t remaining_;
};

void EncodeValue(lua_State* state, int index, int depth, Writer& writer);

void EncodeInteger(lua_Integer value, Writer& writer) {
    if (value == 0) {
        writer.PushByte(Header(kNumber, kNumberZero));
    } else if (value != static_cast<std::int32_t>(value)) {
        writer.PushByte(Header(kNumber, kNumberQword));
        writer.PushValue(static_cast<std::int64_t>(value));
    } else if (value < 0) {
        writer.PushByte(Header(kNumber, kNumberDword));
        writer.PushValue(static_cast<std::int32_t>(value));
    } else if (value < 0x100) {
        writer.PushByte(Header(kNumber, kNumberByte));
        writer.PushValue(static_cast<std::uint8_t>(value));
    } else if (value < 0x1'0000) {
        writer.PushByte(Header(kNumber, kNumberWord));
        writer.PushValue(static_cast<std::uint16_t>(value));
    } else {
        writer.PushByte(Header(kNumber, kNumberDword));
        writer.PushValue(static_cast<std::uint32_t>(value));
    }
}

void EncodeString(const char* data, std::size_t size, Writer& writer) {
    if (size < kShortStringLimit) {
        writer.PushByte(Header(kShortString, static_cast<std::uint8_t>(size)));
    } else if (size < 0x1'0000) {
        writer.PushByte(Header(kLongString, 2));
        writer.PushValue(static_cast<std::uint16_t>(size));
    } else {
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            throw SerializationError("string is too large to serialize");
        }
        writer.PushByte(Header(kLongString, 4));
        writer.PushValue(static_cast<std::uint32_t>(size));
    }
    writer.PushBytes(data, size);
}

void EncodePairsTable(
        lua_State* state, int table_index, int depth, Writer& writer) {
    const int base = lua_gettop(state) - 1;
    lua_pushvalue(state, table_index);
    if (lua_pcall(state, 1, 3, 0) != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        const std::string message = error == nullptr
            ? "__pairs failed while serializing"
            : error;
        lua_settop(state, base);
        throw SerializationError(message);
    }

    const int iterator_index = base + 1;
    const int state_index = base + 2;
    const int key_index = base + 3;
    writer.PushByte(Header(kTable));
    while (true) {
        lua_pushvalue(state, iterator_index);
        lua_pushvalue(state, state_index);
        lua_pushvalue(state, key_index);
        if (lua_pcall(state, 2, 2, 0) != LUA_OK) {
            const char* error = lua_tostring(state, -1);
            const std::string message = error == nullptr
                ? "iterator failed while serializing"
                : error;
            lua_settop(state, base);
            throw SerializationError(message);
        }
        if (lua_isnil(state, -2)) {
            lua_pop(state, 2);
            break;
        }
        EncodeValue(state, -2, depth, writer);
        EncodeValue(state, -1, depth, writer);
        lua_pop(state, 1);
        lua_replace(state, key_index);
    }
    writer.PushByte(Header(kNil));
    lua_settop(state, base);
}

void EncodeRawTable(
        lua_State* state, int table_index, int depth, Writer& writer) {
    const std::size_t array_size = lua_rawlen(state, table_index);
    if (array_size >= kInlineArrayLimit) {
        writer.PushByte(Header(kTable, kInlineArrayLimit));
        EncodeInteger(static_cast<lua_Integer>(array_size), writer);
    } else {
        writer.PushByte(Header(kTable, static_cast<std::uint8_t>(array_size)));
    }

    for (std::size_t index = 1; index <= array_size; ++index) {
        lua_rawgeti(state, table_index, static_cast<lua_Integer>(index));
        EncodeValue(state, -1, depth, writer);
        lua_pop(state, 1);
    }

    lua_pushnil(state);
    while (lua_next(state, table_index) != 0) {
        bool belongs_to_array = false;
        if (lua_isinteger(state, -2)) {
            const lua_Integer key = lua_tointeger(state, -2);
            belongs_to_array = key > 0 &&
                static_cast<std::uint64_t>(key) <= array_size;
        }
        if (!belongs_to_array) {
            EncodeValue(state, -2, depth, writer);
            EncodeValue(state, -1, depth, writer);
        }
        lua_pop(state, 1);
    }
    writer.PushByte(Header(kNil));
}

void EncodeTable(lua_State* state, int index, int depth, Writer& writer) {
    if (depth > kMaximumDepth) {
        throw SerializationError("serialize can't pack too deep table");
    }
    const int table_index = lua_absindex(state, index);
    if (luaL_getmetafield(state, table_index, "__pairs") != LUA_TNIL) {
        EncodePairsTable(state, table_index, depth, writer);
        return;
    }
    EncodeRawTable(state, table_index, depth, writer);
}

void EncodeValue(lua_State* state, int index, int depth, Writer& writer) {
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            writer.PushByte(Header(kNil));
            return;
        case LUA_TBOOLEAN:
            writer.PushByte(Header(kBoolean, lua_toboolean(state, index) ? 1 : 0));
            return;
        case LUA_TNUMBER:
            if (lua_isinteger(state, index)) {
                EncodeInteger(lua_tointeger(state, index), writer);
            } else {
                writer.PushByte(Header(kNumber, kNumberReal));
                writer.PushValue(static_cast<double>(lua_tonumber(state, index)));
            }
            return;
        case LUA_TSTRING: {
            std::size_t size = 0;
            const char* value = lua_tolstring(state, index, &size);
            EncodeString(value, size, writer);
            return;
        }
        case LUA_TLIGHTUSERDATA:
            writer.PushByte(Header(kUserData));
            writer.PushValue(lua_touserdata(state, index));
            return;
        case LUA_TTABLE:
            EncodeTable(state, index, depth + 1, writer);
            return;
        default:
            throw SerializationError(
                std::string("unsupported type ") +
                lua_typename(state, lua_type(state, index)) +
                " in serialization");
    }
}

[[nodiscard]] lua_Integer DecodeInteger(Reader& reader, std::uint8_t cookie) {
    switch (cookie) {
        case kNumberZero:
            return 0;
        case kNumberByte:
            return reader.ReadValue<std::uint8_t>();
        case kNumberWord:
            return reader.ReadValue<std::uint16_t>();
        case kNumberDword:
            return reader.ReadValue<std::int32_t>();
        case kNumberQword:
            return reader.ReadValue<std::int64_t>();
        default:
            throw SerializationError("invalid integer encoding");
    }
}

void DecodeValue(lua_State* state, Reader& reader, int depth);

void DecodeTable(
        lua_State* state, Reader& reader, std::uint8_t cookie, int depth) {
    if (depth > kMaximumDepth) {
        throw SerializationError("serialized table nesting is too deep");
    }
    lua_Integer array_size = cookie;
    if (cookie == kInlineArrayLimit) {
        const std::uint8_t header = reader.ReadByte();
        const std::uint8_t type = header & kTypeMask;
        const std::uint8_t number_cookie = header >> 3;
        if (type != kNumber || number_cookie == kNumberReal) {
            throw SerializationError("invalid serialized array size");
        }
        array_size = DecodeInteger(reader, number_cookie);
    }
    if (array_size < 0 ||
            static_cast<std::uint64_t>(array_size) >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw SerializationError("serialized array size is out of range");
    }

    lua_createtable(state, static_cast<int>(array_size), 0);
    for (lua_Integer index = 1; index <= array_size; ++index) {
        DecodeValue(state, reader, depth + 1);
        lua_rawseti(state, -2, index);
    }
    while (true) {
        DecodeValue(state, reader, depth + 1);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            return;
        }
        DecodeValue(state, reader, depth + 1);
        lua_rawset(state, -3);
    }
}

void DecodeValue(lua_State* state, Reader& reader, int depth) {
    const std::uint8_t header = reader.ReadByte();
    const std::uint8_t type = header & kTypeMask;
    const std::uint8_t cookie = header >> 3;
    switch (type) {
        case kNil:
            lua_pushnil(state);
            return;
        case kBoolean:
            lua_pushboolean(state, cookie != 0);
            return;
        case kNumber:
            if (cookie == kNumberReal) {
                lua_pushnumber(state, reader.ReadValue<double>());
            } else {
                lua_pushinteger(state, DecodeInteger(reader, cookie));
            }
            return;
        case kUserData:
            lua_pushlightuserdata(state, reader.ReadValue<void*>());
            return;
        case kShortString: {
            const char* value = reader.ReadString(cookie);
            lua_pushlstring(state, value, cookie);
            return;
        }
        case kLongString: {
            std::size_t size = 0;
            if (cookie == 2) {
                size = reader.ReadValue<std::uint16_t>();
            } else if (cookie == 4) {
                size = reader.ReadValue<std::uint32_t>();
            } else {
                throw SerializationError("invalid long string encoding");
            }
            const char* value = reader.ReadString(size);
            lua_pushlstring(state, value, size);
            return;
        }
        case kTable:
            DecodeTable(state, reader, cookie, depth);
            return;
        default:
            throw SerializationError("invalid serialized type");
    }
}

}  // namespace

int LuaPack(lua_State* state) {
    try {
        Writer writer;
        const int count = lua_gettop(state);
        for (int index = 1; index <= count; ++index) {
            EncodeValue(state, index, 0, writer);
        }

        const std::size_t size = writer.bytes().size();
        void* data = size == 0 ? nullptr : std::malloc(size);
        if (size != 0 && data == nullptr) {
            throw SerializationError("out of memory while serializing");
        }
        if (size != 0) {
            std::memcpy(data, writer.bytes().data(), size);
        }
        lua_pushlightuserdata(state, data);
        lua_pushinteger(state, static_cast<lua_Integer>(size));
        return 2;
    } catch (const SerializationError& error) {
        return luaL_error(state, "%s", error.what());
    } catch (const std::bad_alloc&) {
        return luaL_error(state, "out of memory while serializing");
    }
}

int LuaUnpack(lua_State* state) {
    if (lua_isnoneornil(state, 1)) {
        return 0;
    }

    const void* data = nullptr;
    std::size_t size = 0;
    if (lua_type(state, 1) == LUA_TSTRING) {
        data = lua_tolstring(state, 1, &size);
    } else {
        data = lua_touserdata(state, 1);
        const lua_Integer requested_size = luaL_checkinteger(state, 2);
        if (requested_size < 0) {
            return luaL_error(state, "deserialize size cannot be negative");
        }
        size = static_cast<std::size_t>(requested_size);
    }
    if (size == 0) {
        return 0;
    }
    if (data == nullptr) {
        return luaL_error(state, "deserialize null pointer");
    }

    const int input_count = lua_gettop(state);
    try {
        Reader reader(data, size);
        int result_count = 0;
        while (!reader.empty()) {
            DecodeValue(state, reader, 0);
            ++result_count;
        }
        return result_count;
    } catch (const SerializationError& error) {
        lua_settop(state, input_count);
        return luaL_error(state, "invalid serialize stream: %s", error.what());
    } catch (const std::bad_alloc&) {
        lua_settop(state, input_count);
        return luaL_error(state, "out of memory while deserializing");
    }
}

}  // namespace skynet::service::detail
