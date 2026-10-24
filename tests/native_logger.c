// Loaded unchanged by both runtimes to verify the configurable logger C ABI.
#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>

struct nativelogger {
    FILE *file;
};

struct nativelogger *nativelogger_create(void) {
    return calloc(1, sizeof(struct nativelogger));
}

static int log_message(struct skynet_context *context, void *user, int type,
                       int session, uint32_t source, const void *message, size_t size) {
    struct nativelogger *self = user;
    if (type == PTYPE_TEXT) {
        fwrite(message, 1, size, self->file);
        fputc('\n', self->file);
        fflush(self->file);
    } else if (type == PTYPE_RESERVED_LUA && session != 0) {
        // A same-producer barrier confirms preceding text logs were consumed.
        skynet_send(context, 0, source, PTYPE_RESPONSE, session, (void *)message, size);
    }
    return 0;
}

int nativelogger_init(struct nativelogger *self, struct skynet_context *context,
                     const char *args) {
    self->file = args ? fopen(args, "a") : NULL;
    if (!self->file) {
        return 1;
    }
    skynet_callback(context, self, log_message);
    return 0;
}

void nativelogger_release(struct nativelogger *self) {
    if (self->file) {
        fclose(self->file);
    }
    free(self);
}
