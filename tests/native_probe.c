// Compiled unchanged against the original public C header and loaded by both runtimes.
#include "skynet.h"
#include <stdlib.h>
#include <string.h>

struct nativeprobe { struct skynet_context *context; };
struct nativeprobe *nativeprobe_create(void) { return calloc(1, sizeof(struct nativeprobe)); }
static int callback(struct skynet_context *context, void *user, int type, int session,
                    uint32_t source, const void *message, size_t size) {
    (void)user;
    if (type != PTYPE_ERROR && session != 0) {
        skynet_send(context, 0, source, PTYPE_RESPONSE, session, (void *)message, size);
    }
    return 0;
}
int nativeprobe_init(struct nativeprobe *self, struct skynet_context *context, const char *args) {
    if (args && strcmp(args, "fail") == 0) { return 1; }
    self->context = context;
    skynet_callback(context, self, callback);
    return 0;
}
void nativeprobe_release(struct nativeprobe *self) { free(self); }
