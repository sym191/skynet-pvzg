#include "NativeService.hpp"

#include "Context.hpp"

#include <dlfcn.h>
#include <memory>
#include <string>

namespace skynet::service {
namespace {
struct ModuleCloser { void operator()(void* value) const { if (value) { dlclose(value); } } };
using Module = std::unique_ptr<void, ModuleCloser>;

class NativeService final : public runtime::Service {
public:
    using Create = void* (*)();
    using Init = int (*)(void*, skynet_context*, const char*);
    using Release = void (*)(void*);
    using Signal = void (*)(void*, int);
    NativeService(Module module, std::string_view name) : module_(std::move(module)) {
        auto symbol = [&](const char* suffix) {
            return dlsym(module_.get(), (std::string(name.substr(name.find_last_of('.') + 1)) + suffix).c_str());
        };
        init_ = reinterpret_cast<Init>(symbol("_init"));
        release_ = reinterpret_cast<Release>(symbol("_release"));
        signal_ = reinterpret_cast<Signal>(symbol("_signal"));
        if (init_) {
            const auto create = reinterpret_cast<Create>(symbol("_create"));
            instance_ = create ? create() : reinterpret_cast<void*>(~std::uintptr_t{0});
        }
    }
    ~NativeService() override { if (instance_ && release_) { release_(instance_); } }
    bool valid() const { return init_ && instance_; }
    bool init(runtime::Context& context, std::string_view arguments) override {
        const std::string terminated(arguments);
        return init_(instance_, reinterpret_cast<skynet_context*>(&context), terminated.c_str()) == 0;
    }
    void dispatch(runtime::Context& context, runtime::Message message) override {
        context.dispatch_native(std::move(message));
    }
    void signal(runtime::Context&, int value) override { if (signal_) { signal_(instance_, value); } }
private:
    Module module_; // Unloaded only after instance release.
    void* instance_{nullptr};
    Init init_{nullptr};
    Release release_{nullptr};
    Signal signal_{nullptr};
};
}  // namespace

runtime::ServicePtr load_native_service(std::string_view name, std::string_view path) {
    while (!path.empty()) {
        const auto separator = path.find(';');
        std::string candidate(path.substr(0, separator));
        const auto marker = candidate.find('?');
        if (marker != std::string::npos) {
            candidate.replace(marker, 1, name);
            Module module(dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL));
            if (module) {
                auto service = std::make_unique<NativeService>(std::move(module), name);
                if (service->valid()) { return service; }
            }
        }
        if (separator == std::string_view::npos) { break; }
        path.remove_prefix(separator + 1);
    }
    return nullptr;
}
}  // namespace skynet::service
