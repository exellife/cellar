/* NotifChannel registry — dependency-free (no DB/crypto), so every TU that links
 * cel_hooks (which dispatches the fan-out) can pull it without extra deps. The
 * adapters themselves live where their dependencies do. See docs/notif-channel.md. */
#include "notif_channel.h"

#include <string.h>

#define CEL_NOTIF_MAX_CHANNELS 8

static const cel_notif_channel_t *g_channels[CEL_NOTIF_MAX_CHANNELS];
static int g_n = 0;

void cel_notif_register(const cel_notif_channel_t *ch) {
    if (!ch || !ch->name || !ch->send) return;
    for (int i = 0; i < g_n; i++) {                 /* same name → replace (idempotent) */
        if (strcmp(g_channels[i]->name, ch->name) == 0) { g_channels[i] = ch; return; }
    }
    if (g_n < CEL_NOTIF_MAX_CHANNELS) g_channels[g_n++] = ch;
}

const cel_notif_channel_t *cel_notif_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_n; i++) {
        if (strcmp(g_channels[i]->name, name) == 0) return g_channels[i];
    }
    return NULL;
}
