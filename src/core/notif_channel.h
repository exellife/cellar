#ifndef CEL_NOTIF_CHANNEL_H
#define CEL_NOTIF_CHANNEL_H

/* NotifChannel — off-site notification delivery behind a port. Adapters (email,
 * web-push, sms) register at boot; the fan-out (cellar.notify → a reserved
 * 'cel:notif' job, handled in cel_hooks_run_jobs) resolves a user's channels and
 * calls send() on each. The registry is dependency-free so any TU can link it;
 * adapters live where their deps do (email → mailer.c, wired in main).
 * See docs/notif-channel.md. */

/* A notification to deliver. All fields optional except title/body; `data_json`
 * is an opaque JSON string the client interprets (deep-link routing, etc.). */
typedef struct {
    const char *title;
    const char *body;
    const char *url;
    const char *data_json;
} cel_notif_msg_t;

/* A delivery channel. `recipient` is channel-specific (an email address for
 * "email", a push-subscription JSON for "webpush"). send() returns 0 on success,
 * -1 + err on failure; the fan-out logs the error and continues to other channels.
 * Return CEL_NOTIF_GONE to signal the recipient is permanently dead (e.g. a
 * push 404/410) so the fan-out can prune it. */
#define CEL_NOTIF_GONE (-2)
typedef struct {
    const char *name;
    int (*send)(const char *recipient, const cel_notif_msg_t *msg, char *err, int errlen);
} cel_notif_channel_t;

/* Register an adapter at boot. `ch` must outlive the process (use a static). A
 * second register of the same name replaces it (idempotent boot / test override). */
void cel_notif_register(const cel_notif_channel_t *ch);

/* Look up a registered adapter by name, or NULL if none is registered. */
const cel_notif_channel_t *cel_notif_get(const char *name);

#endif /* CEL_NOTIF_CHANNEL_H */
