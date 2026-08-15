#include "LuaRuntime.hpp"

#include "Context.hpp"
#include "LuaCore.hpp"
#include "lua_api.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace skynet::service::detail {
namespace {

constexpr std::size_t kInitialMemoryWarning = 32U * 1024U * 1024U;
char kRuntimeRegistryKey;
char kProfileStartRegistryKey;
char kProfileTotalRegistryKey;

[[nodiscard]] double MonotonicSeconds() {
    const auto duration = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

void CreateWeakKeyTable(lua_State* state, const void* registry_key) {
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "k");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    lua_rawsetp(state, LUA_REGISTRYINDEX, registry_key);
}

void PushRegistryTable(lua_State* state, const void* registry_key) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, registry_key);
}

int PushProfileThread(lua_State* state) {
    if (lua_gettop(state) == 0) {
        lua_pushthread(state);
    } else {
        lua_settop(state, 1);
        luaL_checktype(state, 1, LUA_TTHREAD);
    }
    return 1;
}

[[nodiscard]] bool ReadProfileNumber(
        lua_State* state,
        const void* registry_key,
        int thread_index,
        double* value) {
    thread_index = lua_absindex(state, thread_index);
    PushRegistryTable(state, registry_key);
    lua_pushvalue(state, thread_index);
    lua_rawget(state, -2);
    const bool found = lua_isnumber(state, -1);
    if (found) {
        *value = lua_tonumber(state, -1);
    }
    lua_pop(state, 2);
    return found;
}

void WriteProfileNumber(
        lua_State* state,
        const void* registry_key,
        int thread_index,
        std::optional<double> value) {
    thread_index = lua_absindex(state, thread_index);
    PushRegistryTable(state, registry_key);
    lua_pushvalue(state, thread_index);
    if (value) {
        lua_pushnumber(state, *value);
    } else {
        lua_pushnil(state);
    }
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

int ProfileStart(lua_State* state) {
    const int thread_index = PushProfileThread(state);
    double ignored = 0;
    if (ReadProfileNumber(
            state, &kProfileStartRegistryKey, thread_index, &ignored)) {
        return luaL_error(state, "thread profile started more than once");
    }
    WriteProfileNumber(state, &kProfileTotalRegistryKey, thread_index, 0.0);
    WriteProfileNumber(
        state, &kProfileStartRegistryKey, thread_index, MonotonicSeconds());
    return 0;
}

int ProfileStop(lua_State* state) {
    const int thread_index = PushProfileThread(state);
    double start = 0;
    if (!ReadProfileNumber(
            state, &kProfileStartRegistryKey, thread_index, &start)) {
        return luaL_error(state, "call profile.start() before profile.stop()");
    }
    double total = 0;
    (void)ReadProfileNumber(
        state, &kProfileTotalRegistryKey, thread_index, &total);
    if (start > 0) {
        total += MonotonicSeconds() - start;
    }
    WriteProfileNumber(
        state, &kProfileStartRegistryKey, thread_index, std::nullopt);
    WriteProfileNumber(
        state, &kProfileTotalRegistryKey, thread_index, std::nullopt);
    lua_pushnumber(state, total);
    return 1;
}

int ProfileResume(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTHREAD);
    double start = 0;
    const bool profiled = ReadProfileNumber(
        state, &kProfileStartRegistryKey, 1, &start);
    if (profiled) {
        WriteProfileNumber(
            state, &kProfileStartRegistryKey, 1, MonotonicSeconds());
    }
    lua_pushvalue(state, 1);
    const int thread_reference = luaL_ref(state, LUA_REGISTRYINDEX);

    const int argument_count = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, argument_count, LUA_MULTRET);

    const int result_count = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, thread_reference);
    const int thread_index = lua_gettop(state);
    if (ReadProfileNumber(
            state, &kProfileStartRegistryKey, thread_index, &start) && start > 0) {
        double total = 0;
        (void)ReadProfileNumber(
            state, &kProfileTotalRegistryKey, thread_index, &total);
        total += MonotonicSeconds() - start;
        WriteProfileNumber(
            state, &kProfileTotalRegistryKey, thread_index, total);
        WriteProfileNumber(
            state, &kProfileStartRegistryKey, thread_index, 0.0);
    }
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, thread_reference);
    return result_count;
}

int ProfileWrappedResume(lua_State* state) {
    const int argument_count = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(2));
    lua_pushcclosure(state, ProfileResume, 1);
    lua_insert(state, 1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 2);
    lua_call(state, argument_count + 1, LUA_MULTRET);

    if (!lua_toboolean(state, 1)) {
        lua_remove(state, 1);
        return lua_error(state);
    }
    lua_remove(state, 1);
    return lua_gettop(state);
}

int ProfileWrap(lua_State* state) {
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_State* coroutine = lua_newthread(state);
    lua_pushvalue(state, 1);
    lua_xmove(state, coroutine, 1);
    lua_pushvalue(state, -1);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushcclosure(state, ProfileWrappedResume, 2);
    return 1;
}

int OpenProfile(lua_State* state) {
    CreateWeakKeyTable(state, &kProfileStartRegistryKey);
    CreateWeakKeyTable(state, &kProfileTotalRegistryKey);

    lua_newtable(state);
    lua_pushcfunction(state, ProfileStart);
    lua_setfield(state, -2, "start");
    lua_pushcfunction(state, ProfileStop);
    lua_setfield(state, -2, "stop");

    lua_getglobal(state, "coroutine");
    lua_getfield(state, -1, "resume");
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, ProfileResume, 1);
    lua_setfield(state, -4, "resume");
    lua_pushcclosure(state, ProfileWrap, 1);
    lua_setfield(state, -3, "wrap");
    lua_pop(state, 1);
    return 1;
}

int NoOperation(lua_State*) {
    return 0;
}

int DirectLoadFile(lua_State* state) {
    const char* file_name = luaL_optstring(state, 1, nullptr);
    const char* mode = luaL_optstring(state, 2, "bt");
    const int environment_index = lua_isnone(state, 3) ? 0 : 3;
    const int status = luaL_loadfilex_(state, file_name, mode);
    if (status != LUA_OK) {
        lua_pushnil(state);
        lua_insert(state, -2);
        return 2;
    }
    if (environment_index != 0) {
        lua_pushvalue(state, environment_index);
        if (lua_setupvalue(state, -2, 1) == nullptr) {
            lua_pop(state, 1);
        }
    }
    return 1;
}

int DirectLuaSearcher(lua_State* state) {
    luaL_checkstring(state, 1);
    lua_getglobal(state, "package");
    const int package_index = lua_gettop(state);
    lua_getfield(state, package_index, "searchpath");
    lua_pushvalue(state, 1);
    lua_getfield(state, package_index, "path");
    lua_call(state, 2, 2);
    if (lua_isnil(state, -2)) {
        lua_remove(state, -2);
        return 1;
    }

    lua_pop(state, 1);
    const char* file_name = lua_tostring(state, -1);
    if (luaL_loadfilex_(state, file_name, nullptr) != LUA_OK) {
        return luaL_error(
            state,
            "error loading module '%s' from file '%s':\n\t%s",
            lua_tostring(state, 1),
            file_name,
            lua_tostring(state, -1));
    }
    lua_pushvalue(state, -2);
    return 2;
}

void InstallDirectLuaLoader(lua_State* state) {
    lua_pushcfunction(state, DirectLoadFile);
    lua_setglobal(state, "loadfile");
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "searchers");
    lua_pushcfunction(state, DirectLuaSearcher);
    lua_rawseti(state, -2, 2);
    lua_pop(state, 2);
}

int OpenCodeCache(lua_State* state) {
    static constexpr luaL_Reg kFunctions[] = {
        {"clear", NoOperation},
        {"mode", NoOperation},
        {nullptr, nullptr},
    };
    luaL_newlib(state, kFunctions);
    lua_pushcfunction(state, DirectLoadFile);
    lua_setfield(state, -2, "loadfile");
    return 1;
}

int Traceback(lua_State* state) {
    const char* message = lua_tostring(state, 1);
    if (message == nullptr) {
        lua_pushliteral(state, "(no error message)");
    } else {
        luaL_traceback(state, state, message, 1);
    }
    return 1;
}

void SignalHook(lua_State* state, lua_Debug*) {
    lua_sethook(state, nullptr, 0, 0);
    LuaRuntime* runtime = LuaRuntime::FromState(state);
    if (runtime != nullptr && runtime->ConsumePendingSignal()) {
        luaL_error(state, "signal 0");
    }
}

void PreloadCore(lua_State* state) {
    luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(state, OpenLuaCore);
    lua_setfield(state, -2, "skynet.core");
    lua_pop(state, 1);
}

void SetGlobalString(
        lua_State* state, const char* name, const std::optional<std::string>& value) {
    if (value) {
        lua_pushlstring(state, value->data(), value->size());
    } else {
        lua_pushnil(state);
    }
    lua_setglobal(state, name);
}

[[nodiscard]] std::optional<std::string> GetEnvironment(
        runtime::Context& context,
        std::string_view name,
        std::optional<std::string_view> fallback) {
    if (auto value = context.command("GETENV", name)) {
        return value;
    }
    return fallback ? std::optional<std::string>(*fallback) : std::nullopt;
}

void ReportLauncherError(runtime::Context& context) {
    constexpr std::string_view kLauncher = ".launcher";
    constexpr std::string_view kMessage = "ERROR";
    const auto* data = reinterpret_cast<const std::byte*>(kMessage.data());
    (void)context.send(
        kLauncher,
        0,
        static_cast<int>(runtime::MessageType::text),
        0,
        std::span<const std::byte>(data, kMessage.size()));
}

}  // namespace

struct LuaRuntime::Impl {
    Impl() {
        unsigned seed = luaL_makeseed(nullptr);
        if (seed == 0) {
            seed = 1;
        }
        state = lua_newstate(Allocate, this, seed);
    }

    ~Impl() {
        if (state != nullptr) {
            lua_close(state);
        }
    }

    static void* Allocate(
            void* user_data,
            void* pointer,
            std::size_t old_size,
            std::size_t new_size) {
        auto* impl = static_cast<Impl*>(user_data);
        if (new_size == 0) {
            std::free(pointer);
            if (pointer != nullptr) {
                impl->memory_used -= old_size;
            }
            return nullptr;
        }

        const std::size_t previous = pointer == nullptr ? 0 : old_size;
        const std::size_t base = impl->memory_used - previous;
        if (new_size > std::numeric_limits<std::size_t>::max() - base) {
            return nullptr;
        }
        const std::size_t next = base + new_size;
        if (impl->memory_limit != 0 && next > impl->memory_limit &&
                (pointer == nullptr || new_size > old_size)) {
            return nullptr;
        }

        void* result = std::realloc(pointer, new_size);
        if (result == nullptr) {
            return nullptr;
        }
        impl->memory_used = next;
        if (next > impl->memory_warning_threshold) {
            while (next > impl->memory_warning_threshold) {
                impl->memory_warning_threshold *= 2;
            }
            if (impl->context != nullptr) {
                impl->context->log("Lua service memory usage crossed warning threshold");
            }
        }
        return result;
    }

    lua_State* state = nullptr;
    runtime::Context* context = nullptr;
    std::size_t memory_used = 0;
    std::size_t memory_limit = 0;
    std::size_t memory_warning_threshold = kInitialMemoryWarning;
    int callback_reference = LUA_NOREF;
    bool forward_messages = false;
    bool initialized = false;
    std::atomic<bool> signal_requested{false};
    std::atomic<lua_State*> active_state{nullptr};
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}

LuaRuntime::~LuaRuntime() = default;

bool LuaRuntime::Init(runtime::Context& context, std::string_view args) {
    if (impl_->state == nullptr || impl_->context != nullptr) {
        return false;
    }
    impl_->context = &context;
    lua_State* state = impl_->state;

    lua_pushlightuserdata(state, this);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kRuntimeRegistryKey);
    lua_gc(state, LUA_GCSTOP, 0);
    lua_pushboolean(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "LUA_NOENV");
    luaL_openlibs(state);
    InstallDirectLuaLoader(state);
    PreloadCore(state);

    luaL_requiref(state, "skynet.profile", OpenProfile, 0);
    lua_getglobal(state, "coroutine");
    lua_getfield(state, -2, "resume");
    lua_setfield(state, -2, "resume");
    lua_getfield(state, -2, "wrap");
    lua_setfield(state, -2, "wrap");
    lua_pop(state, 2);

    luaL_requiref(state, "skynet.codecache", OpenCodeCache, 0);
    lua_pop(state, 1);
    lua_gc(state, LUA_GCGEN, 0, 0);

    SetGlobalString(
        state,
        "LUA_PATH",
        GetEnvironment(
            context,
            "lua_path",
            "./lualib/?.lua;./lualib/?/init.lua"));
    SetGlobalString(
        state,
        "LUA_CPATH",
        GetEnvironment(context, "lua_cpath", "./luaclib/?.so"));
    SetGlobalString(
        state,
        "LUA_SERVICE",
        GetEnvironment(context, "luaservice", "./service/?.lua"));
    SetGlobalString(
        state,
        "LUA_PRELOAD",
        GetEnvironment(context, "preload", std::nullopt));

    const auto loader = GetEnvironment(
        context, "lualoader", "./lualib/loader.lua");
    lua_settop(state, 0);
    lua_pushcfunction(state, Traceback);
    if (luaL_loadfilex_(state, loader->c_str(), nullptr) != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        std::string message = "Can't load ";
        message.append(*loader);
        message.append(" : ");
        message.append(error == nullptr ? "unknown Lua error" : error);
        context.log(message);
        ReportLauncherError(context);
        (void)context.command("EXIT");
        lua_settop(state, 0);
        lua_gc(state, LUA_GCRESTART, 0);
        return false;
    }
    lua_pushlstring(state, args.data(), args.size());
    impl_->active_state.store(state, std::memory_order_release);
    const int result = lua_pcall(state, 1, 0, 1);
    impl_->active_state.store(nullptr, std::memory_order_release);
    if (result != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        std::string message = "lua loader error : ";
        message.append(error == nullptr ? "unknown Lua error" : error);
        context.log(message);
        ReportLauncherError(context);
        (void)context.command("EXIT");
        lua_settop(state, 0);
        lua_gc(state, LUA_GCRESTART, 0);
        return false;
    }

    lua_settop(state, 0);
    if (lua_getfield(state, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
        const lua_Integer limit = lua_tointeger(state, -1);
        if (limit > 0) {
            impl_->memory_limit = static_cast<std::size_t>(limit);
            context.log("Lua service memory limit enabled");
        }
        lua_pushnil(state);
        lua_setfield(state, LUA_REGISTRYINDEX, "memlimit");
    }
    lua_pop(state, 1);
    lua_gc(state, LUA_GCRESTART, 0);
    impl_->initialized = true;
    return true;
}

bool LuaRuntime::Dispatch(runtime::Message message) {
    if (!impl_->initialized || impl_->callback_reference == LUA_NOREF) {
        return false;
    }
    impl_->context->record_dispatch();
    lua_State* state = impl_->state;
    const int stack_top = lua_gettop(state);
    lua_pushcfunction(state, Traceback);
    const int error_handler = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, impl_->callback_reference);

    void* data = message.data.empty() ? nullptr : message.data.data();
    if (impl_->forward_messages && !message.data.empty()) {
        data = std::malloc(message.data.size());
        if (data == nullptr) {
            lua_settop(state, stack_top);
            impl_->context->log("failed to allocate forwarded Lua message");
            return true;
        }
        std::memcpy(data, message.data.data(), message.data.size());
    }

    lua_pushinteger(state, static_cast<int>(message.type));
    lua_pushlightuserdata(state, data);
    lua_pushinteger(state, static_cast<lua_Integer>(message.data.size()));
    lua_pushinteger(state, message.session);
    lua_pushinteger(state, message.source);
    if (impl_->signal_requested.load(std::memory_order_acquire)) {
        lua_sethook(state, SignalHook, LUA_MASKCOUNT, 1);
    }
    impl_->active_state.store(state, std::memory_order_release);
    const int result = lua_pcall(state, 5, 0, error_handler);
    impl_->active_state.store(nullptr, std::memory_order_release);
    if (result != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        impl_->context->log(
            error == nullptr ? "Lua message callback failed" : error);
    }
    lua_settop(state, stack_top);
    return true;
}

void LuaRuntime::Signal(int signal) {
    if (impl_->context == nullptr) {
        return;
    }
    if (signal == 0) {
        impl_->signal_requested.store(true, std::memory_order_release);
        lua_State* active = impl_->active_state.load(std::memory_order_acquire);
        if (active != nullptr) {
            lua_sethook(active, SignalHook, LUA_MASKCOUNT, 1);
        }
    } else if (signal == 1) {
        const double kilobytes = static_cast<double>(impl_->memory_used) / 1024.0;
        impl_->context->log(
            "Current Memory " + std::to_string(kilobytes) + "K");
    }
}

lua_State* LuaRuntime::state() const noexcept {
    return impl_->state;
}

std::size_t LuaRuntime::memory_used() const noexcept {
    return impl_->memory_used;
}

std::size_t LuaRuntime::memory_limit() const noexcept {
    return impl_->memory_limit;
}

bool LuaRuntime::initialized() const noexcept {
    return impl_->initialized;
}

runtime::Context& LuaRuntime::context() const {
    return *impl_->context;
}

void LuaRuntime::SetCallback(
        lua_State* state, int function_index, bool forward) {
    if (impl_->callback_reference != LUA_NOREF) {
        luaL_unref(
            state, LUA_REGISTRYINDEX, impl_->callback_reference);
    }
    lua_pushvalue(state, function_index);
    impl_->callback_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    impl_->forward_messages = forward;
}

bool LuaRuntime::ConsumePendingSignal() noexcept {
    return impl_->signal_requested.exchange(false, std::memory_order_acq_rel);
}

LuaRuntime* LuaRuntime::FromState(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kRuntimeRegistryKey);
    auto* runtime = static_cast<LuaRuntime*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return runtime;
}

}  // namespace skynet::service::detail
