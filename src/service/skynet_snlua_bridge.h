#ifndef SKYNET_PVZG_SERVICE_SKYNET_SNLUA_BRIDGE_H_
#define SKYNET_PVZG_SERVICE_SKYNET_SNLUA_BRIDGE_H_

#include <stddef.h>

struct lua_State;
struct skynet_context;
struct snlua;

#ifdef __cplusplus
extern "C" {
#endif

struct snlua* skynet_snlua_create(void);
void skynet_snlua_release(struct snlua* service);
int skynet_snlua_init_direct(
    struct snlua* service,
    struct skynet_context* context,
    const char* arguments,
    size_t size);
void skynet_snlua_signal(struct snlua* service, int signal);
struct lua_State* skynet_snlua_state(struct snlua* service);
size_t skynet_snlua_memory(struct snlua* service);
size_t skynet_snlua_memory_limit(struct snlua* service);

#ifdef __cplusplus
}
#endif

#endif  // SKYNET_PVZG_SERVICE_SKYNET_SNLUA_BRIDGE_H_
