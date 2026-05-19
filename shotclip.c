/*
 * shotclip — put files on the Wayland clipboard with the full Nautilus-style
 * MIME set (text/uri-list, x-special/gnome-copied-files, the application/vnd.portal.*
 * portal types, plus the actual file bytes for single-file image-like copies).
 *
 * Why this is non-trivial:
 *   - wl-copy advertises one MIME type per invocation; we need ~7 simultaneously.
 *   - The application/vnd.portal.* types are not literal data — they are keys to
 *     live D-Bus sessions registered with xdg-desktop-portal.
 *   - wl_data_device.set_selection needs a serial from a recent input event.
 *     GTK4's clipboard plumbing silently passes serial 0 from non-interactive
 *     code paths, so the compositor discards the request.
 *
 * Approach:
 *   1. Make our own xdg-toplevel and wait for wl_keyboard.enter to capture a real serial.
 *   2. Register all files with org.freedesktop.portal.FileTransfer and
 *      org.freedesktop.portal.Documents up front.
 *   3. Advertise the full MIME set on a wl_data_source. set_selection with our serial.
 *   4. Destroy the toplevel — the data_source survives, the window disappears.
 *   5. Serve send() events from the (now windowless) process until cancelled.
 *
 * Licensed GPL-3.0-or-later. See LICENSE.
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#define SHOTCLIP_VERSION "0.2.0"
#define SHOTCLIP_APP_ID  "io.github.jq6l43d1.shotclip"

/* =============================================================== */
/* SECTION: types                                                  */
/* =============================================================== */

typedef struct {
    char *path;     /* absolute path */
    char *uri;      /* file:// + percent-encoded path */
    char *mime;     /* detected content type, e.g. "image/png", "application/pdf" */
} file_t;

typedef struct {
    /* CLI */
    int primary;
    int paste_once;
    int foreground;
    int clear_only;
    int no_image_data;
    int keep_visible;
    const char *force_type;     /* -t MIME; NULL = offer everything */
    file_t *files;
    int n_files;

    /* Wayland globals */
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_data_device_manager *ddm;
    struct zwp_primary_selection_device_manager_v1 *psdm;
    struct xdg_wm_base *wm_base;

    /* Seat */
    struct wl_keyboard *keyboard;

    /* Selection */
    struct wl_data_device *data_device;
    struct zwp_primary_selection_device_v1 *primary_device;
    struct wl_data_source *source;
    struct zwp_primary_selection_source_v1 *primary_source;

    /* Surface (only alive briefly to harvest the keyboard.enter serial) */
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    int configured;
    uint32_t enter_serial;
    int surface_destroyed;

    /* State machine flags */
    int cancelled;
    int sent_at_least_one_data_mime;  /* for --paste-once exit */

    /* D-Bus + portals */
    GDBusConnection *bus;
    char *ft_key;
    char *doc_key;
} ctx_t;

/* =============================================================== */
/* SECTION: error helpers                                          */
/* =============================================================== */

static const char *prog = "shotclip";

static void warn_(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

__attribute__((format(printf, 1, 2), noreturn))
static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n;
        len -= n;
    }
    return 0;
}

/* =============================================================== */
/* SECTION: per-file metadata                                      */
/* =============================================================== */

static char *guess_content_type(const char *path) {
    char head[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    gssize n = 0;
    if (fd >= 0) { n = read(fd, head, sizeof(head)); close(fd); }
    if (n < 0) n = 0;

    gboolean certain = FALSE;
    char *ctype = g_content_type_guess(path, (const guchar *)head, (gsize)n, &certain);
    if (!ctype) return g_strdup("application/octet-stream");
    char *mime = g_content_type_get_mime_type(ctype);
    g_free(ctype);
    return mime ? mime : g_strdup("application/octet-stream");
}

static void file_init(file_t *f, const char *arg) {
    f->path = realpath(arg, NULL);
    if (!f->path) die("realpath %s: %s", arg, strerror(errno));
    char *enc = g_uri_escape_string(f->path, "/", FALSE);
    f->uri = g_strdup_printf("file://%s", enc);
    g_free(enc);
    f->mime = guess_content_type(f->path);
}

static void file_free(file_t *f) {
    free(f->path);
    g_free(f->uri);
    g_free(f->mime);
}

/* =============================================================== */
/* SECTION: D-Bus portal helpers                                   */
/* =============================================================== */

/* Open all files, append them to a GUnixFDList, return the list + ah variant. */
static gboolean build_fd_list(const ctx_t *c, GUnixFDList **out_fdl,
                              GVariant **out_handles, GError **err) {
    GUnixFDList *fdl = g_unix_fd_list_new();
    GVariantBuilder hb;
    g_variant_builder_init(&hb, G_VARIANT_TYPE("ah"));
    for (int i = 0; i < c->n_files; i++) {
        int fd = open(c->files[i].path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "open %s: %s", c->files[i].path, strerror(errno));
            g_object_unref(fdl);
            return FALSE;
        }
        gint32 idx = g_unix_fd_list_append(fdl, fd, err);
        close(fd);
        if (idx < 0) { g_object_unref(fdl); return FALSE; }
        g_variant_builder_add(&hb, "h", idx);
    }
    *out_fdl = fdl;
    *out_handles = g_variant_builder_end(&hb);
    return TRUE;
}

static char *call_filetransfer(ctx_t *c) {
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(
        c->bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.FileTransfer",
        "StartTransfer",
        g_variant_new("(a{sv})", NULL),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!r) { warn_("StartTransfer: %s", err->message); g_clear_error(&err); return NULL; }
    char *key = NULL;
    g_variant_get(r, "(s)", &key);
    g_variant_unref(r);

    GUnixFDList *fdl;
    GVariant *handles;
    if (!build_fd_list(c, &fdl, &handles, &err)) {
        warn_("build fd list: %s", err->message);
        g_clear_error(&err);
        g_free(key);
        return NULL;
    }
    GVariant *r2 = g_dbus_connection_call_with_unix_fd_list_sync(
        c->bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.FileTransfer",
        "AddFiles",
        g_variant_new("(s@aha{sv})", key, handles, NULL),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, fdl, NULL, NULL, &err);
    g_object_unref(fdl);
    if (!r2) { warn_("AddFiles: %s", err->message); g_clear_error(&err); g_free(key); return NULL; }
    g_variant_unref(r2);
    return key;
}

static char *call_documents(ctx_t *c) {
    GError *err = NULL;
    GUnixFDList *fdl;
    GVariant *handles;
    if (!build_fd_list(c, &fdl, &handles, &err)) {
        warn_("build fd list: %s", err->message); g_clear_error(&err); return NULL;
    }
    GVariantBuilder perms;
    g_variant_builder_init(&perms, G_VARIANT_TYPE("as"));

    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
        c->bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.Documents",
        "AddFull",
        g_variant_new("(@ahusas)", handles, 0u, "", &perms),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, fdl, NULL, NULL, &err);
    g_object_unref(fdl);
    if (!r) { warn_("AddFull: %s", err->message); g_clear_error(&err); return NULL; }

    GVariant *ids = g_variant_get_child_value(r, 0);
    GString *out = g_string_new(NULL);
    gsize n = g_variant_n_children(ids);
    for (gsize i = 0; i < n; i++) {
        const char *id = NULL;
        GVariant *child = g_variant_get_child_value(ids, i);
        g_variant_get(child, "&s", &id);
        if (i > 0) g_string_append_c(out, ' ');
        g_string_append(out, id);
        g_variant_unref(child);
    }
    g_variant_unref(ids);
    g_variant_unref(r);
    return g_string_free(out, FALSE);
}

/* =============================================================== */
/* SECTION: MIME writers (table-driven)                            */
/* =============================================================== */

typedef int (*writer_fn)(ctx_t *c, int fd);

static int write_uri_list(ctx_t *c, int fd) {
    GString *s = g_string_new(NULL);
    for (int i = 0; i < c->n_files; i++)
        g_string_append_printf(s, "%s\r\n", c->files[i].uri);
    int r = write_all(fd, s->str, s->len);
    g_string_free(s, TRUE);
    return r;
}
static int write_gnome_copied(ctx_t *c, int fd) {
    GString *s = g_string_new("copy");
    for (int i = 0; i < c->n_files; i++)
        g_string_append_printf(s, "\n%s", c->files[i].uri);
    int r = write_all(fd, s->str, s->len);
    g_string_free(s, TRUE);
    return r;
}
static int write_text_plain(ctx_t *c, int fd) {
    GString *s = g_string_new(NULL);
    for (int i = 0; i < c->n_files; i++) {
        if (i) g_string_append_c(s, '\n');
        g_string_append(s, c->files[i].path);
    }
    int r = write_all(fd, s->str, s->len);
    g_string_free(s, TRUE);
    return r;
}
static int write_ft_key(ctx_t *c, int fd) {
    return c->ft_key ? write_all(fd, c->ft_key, strlen(c->ft_key) + 1) : -1;
}
static int write_doc_key(ctx_t *c, int fd) {
    return c->doc_key ? write_all(fd, c->doc_key, strlen(c->doc_key) + 1) : -1;
}
static int write_file_bytes(ctx_t *c, int fd) {
    if (c->n_files != 1) return -1;
    int rfd = open(c->files[0].path, O_RDONLY);
    if (rfd < 0) return -1;
    char buf[8192]; ssize_t n;
    int rc = 0;
    while ((n = read(rfd, buf, sizeof(buf))) > 0) {
        if (write_all(fd, buf, n) < 0) { rc = -1; break; }
    }
    close(rfd);
    return rc;
}

typedef struct {
    const char *mime;
    writer_fn   write;
    int         needs_portal; /* 0 = always offered; 1 = needs ft_key; 2 = needs doc_key */
    int         is_data_mime; /* 1 = bytes-of-content (image/png etc.); 0 = description type */
} mime_entry_t;

/* mime_entry_t for the file's actual content type is built dynamically. */
static const mime_entry_t fixed_mimes[] = {
    { "text/uri-list",                       write_uri_list,    0, 0 },
    { "x-special/gnome-copied-files",        write_gnome_copied, 0, 0 },
    { "text/plain;charset=utf-8",            write_text_plain,  0, 0 },
    { "text/plain",                          write_text_plain,  0, 0 },
    { "application/vnd.portal.filetransfer", write_ft_key,      1, 0 },
    { "application/vnd.portal.files",        write_doc_key,     2, 0 },
};

/* Look up a writer by MIME type; returns NULL if not advertised. */
static writer_fn lookup_writer(ctx_t *c, const char *mime) {
    for (size_t i = 0; i < sizeof(fixed_mimes)/sizeof(fixed_mimes[0]); i++) {
        if (strcmp(fixed_mimes[i].mime, mime) == 0) {
            if (fixed_mimes[i].needs_portal == 1 && !c->ft_key) return NULL;
            if (fixed_mimes[i].needs_portal == 2 && !c->doc_key) return NULL;
            return fixed_mimes[i].write;
        }
    }
    /* The file's own content-type — only when N=1 and matches what we offered. */
    if (!c->no_image_data && c->n_files == 1 && strcmp(c->files[0].mime, mime) == 0)
        return write_file_bytes;
    return NULL;
}

/* =============================================================== */
/* SECTION: wl_data_source / primary_source listeners              */
/* =============================================================== */

static void do_send(ctx_t *c, const char *mime, int fd) {
    writer_fn w = lookup_writer(c, mime);
    if (w) {
        w(c, fd);
        c->sent_at_least_one_data_mime = 1;
    }
    close(fd);
    if (c->paste_once && c->sent_at_least_one_data_mime) {
        c->cancelled = 1; /* trigger main loop exit */
    }
}

static void ds_target(void *d, struct wl_data_source *s, const char *m) {}
static void ds_send(void *data, struct wl_data_source *s, const char *m, int32_t fd) { do_send(data, m, fd); }
static void ds_cancelled(void *data, struct wl_data_source *s) { ((ctx_t *)data)->cancelled = 1; }
static void ds_dnd_drop_performed(void *d, struct wl_data_source *s) {}
static void ds_dnd_finished(void *d, struct wl_data_source *s) {}
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) {}
static const struct wl_data_source_listener ds_listener = {
    .target = ds_target, .send = ds_send, .cancelled = ds_cancelled,
    .dnd_drop_performed = ds_dnd_drop_performed, .dnd_finished = ds_dnd_finished,
    .action = ds_action,
};

static void ps_send(void *data, struct zwp_primary_selection_source_v1 *s, const char *m, int32_t fd) { do_send(data, m, fd); }
static void ps_cancelled(void *data, struct zwp_primary_selection_source_v1 *s) { ((ctx_t *)data)->cancelled = 1; }
static const struct zwp_primary_selection_source_v1_listener ps_listener = {
    .send = ps_send, .cancelled = ps_cancelled,
};

/* =============================================================== */
/* SECTION: keyboard / seat / xdg listeners                        */
/* =============================================================== */

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xs_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    ctx_t *c = data;
    xdg_surface_ack_configure(s, serial);
    c->configured = 1;
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {}
static void tl_close(void *d, struct xdg_toplevel *t) { ((ctx_t *)d)->cancelled = 1; }
static void tl_configure_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_wm_capabilities(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure, .close = tl_close,
    .configure_bounds = tl_configure_bounds, .wm_capabilities = tl_wm_capabilities,
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz) { close(fd); }
static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf, struct wl_array *keys) {
    ((ctx_t *)data)->enter_serial = serial;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t md, uint32_t ml, uint32_t mk, uint32_t g) {}
static void kb_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat_info,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    ctx_t *c = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !c->keyboard) {
        c->keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(c->keyboard, &kb_listener, c);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* =============================================================== */
/* SECTION: wl_registry                                            */
/* =============================================================== */

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    ctx_t *c = data;
    if (!strcmp(iface, "wl_compositor")) {
        c->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, "wl_shm")) {
        c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, "wl_seat")) {
        c->seat = wl_registry_bind(r, name, &wl_seat_interface, version > 7 ? 7 : version);
        wl_seat_add_listener(c->seat, &seat_listener, c);
    } else if (!strcmp(iface, "wl_data_device_manager")) {
        c->ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    } else if (!strcmp(iface, "xdg_wm_base")) {
        c->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, version > 5 ? 5 : version);
        xdg_wm_base_add_listener(c->wm_base, &wm_listener, c);
    } else if (!strcmp(iface, "zwp_primary_selection_device_manager_v1")) {
        c->psdm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    }
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_global_remove };

/* =============================================================== */
/* SECTION: surface lifecycle                                      */
/* =============================================================== */

static struct wl_buffer *create_buffer_1x1(ctx_t *c) {
    int size = 4;
    int fd = memfd_create("buf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void create_surface(ctx_t *c) {
    c->surface = wl_compositor_create_surface(c->compositor);
    c->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
    xdg_surface_add_listener(c->xdg_surface, &xs_listener, c);
    c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
    xdg_toplevel_add_listener(c->toplevel, &tl_listener, c);
    xdg_toplevel_set_title(c->toplevel, "shotclip");
    xdg_toplevel_set_app_id(c->toplevel, SHOTCLIP_APP_ID);
    wl_surface_commit(c->surface);

    while (!c->configured) {
        if (wl_display_dispatch(c->display) < 0) die("wl_display_dispatch (configure): %s", strerror(errno));
    }
    c->buffer = create_buffer_1x1(c);
    if (!c->buffer) die("failed to create SHM buffer");
    wl_surface_attach(c->surface, c->buffer, 0, 0);
    wl_surface_damage_buffer(c->surface, 0, 0, 1, 1);
    wl_surface_commit(c->surface);
}

static void destroy_surface(ctx_t *c) {
    if (c->surface_destroyed) return;
    if (c->toplevel)    { xdg_toplevel_destroy(c->toplevel);    c->toplevel = NULL; }
    if (c->xdg_surface) { xdg_surface_destroy(c->xdg_surface);  c->xdg_surface = NULL; }
    if (c->surface)     { wl_surface_destroy(c->surface);       c->surface = NULL; }
    if (c->buffer)      { wl_buffer_destroy(c->buffer);         c->buffer = NULL; }
    c->surface_destroyed = 1;
    wl_display_flush(c->display);
}

/* =============================================================== */
/* SECTION: argv parsing                                           */
/* =============================================================== */

static void usage(FILE *f) {
    fprintf(f,
"Usage: %s [OPTIONS] FILE [FILE ...]\n"
"\n"
"Put files on the Wayland clipboard with the same MIME types Nautilus\n"
"produces (text/uri-list, x-special/gnome-copied-files, the\n"
"application/vnd.portal.* portal types, plus the file's content-type\n"
"bytes when a single file is given).\n"
"\n"
"Options:\n"
"  -p, --primary         Set the primary selection, not the regular clipboard\n"
"  -o, --paste-once      Exit after the first paste of a data MIME type\n"
"  -f, --foreground      Don't fork to background after set_selection\n"
"  -c, --clear           Clear the clipboard (no FILE args needed)\n"
"  -t, --type MIME       Only advertise this single MIME type\n"
"      --no-image-data   Don't offer the file's content-type bytes\n"
"      --keep-visible    Don't destroy the focus window after set_selection\n"
"  -h, --help            Show this help and exit\n"
"  -V, --version         Show version and exit\n",
    prog);
}

static const struct option longopts[] = {
    { "primary",        no_argument,       0, 'p' },
    { "paste-once",     no_argument,       0, 'o' },
    { "foreground",     no_argument,       0, 'f' },
    { "clear",          no_argument,       0, 'c' },
    { "type",           required_argument, 0, 't' },
    { "no-image-data",  no_argument,       0,  1  },
    { "keep-visible",   no_argument,       0,  2  },
    { "help",           no_argument,       0, 'h' },
    { "version",        no_argument,       0, 'V' },
    { 0, 0, 0, 0 }
};

static void parse_args(ctx_t *c, int argc, char *argv[]) {
    int ch;
    while ((ch = getopt_long(argc, argv, "pofct:hV", longopts, NULL)) != -1) {
        switch (ch) {
            case 'p': c->primary = 1; break;
            case 'o': c->paste_once = 1; break;
            case 'f': c->foreground = 1; break;
            case 'c': c->clear_only = 1; break;
            case 't': c->force_type = optarg; break;
            case  1 : c->no_image_data = 1; break;
            case  2 : c->keep_visible = 1; break;
            case 'h': usage(stdout); exit(0);
            case 'V': printf("shotclip %s\n", SHOTCLIP_VERSION); exit(0);
            default: usage(stderr); exit(2);
        }
    }
    int n = argc - optind;
    if (!c->clear_only && n <= 0) { usage(stderr); exit(2); }
    if (c->clear_only && n > 0)   die("--clear takes no FILE args");
    if (n > 0) {
        c->files = calloc(n, sizeof(file_t));
        if (!c->files) die("out of memory");
        c->n_files = n;
        for (int i = 0; i < n; i++) file_init(&c->files[i], argv[optind + i]);
    }
}

/* =============================================================== */
/* SECTION: main                                                   */
/* =============================================================== */

/* Build the MIME list we'll advertise based on context + flags. */
static GPtrArray *build_offer_list(ctx_t *c) {
    GPtrArray *out = g_ptr_array_new();
    if (c->force_type) {
        g_ptr_array_add(out, (gpointer)c->force_type);
        return out;
    }
    for (size_t i = 0; i < sizeof(fixed_mimes)/sizeof(fixed_mimes[0]); i++) {
        if (fixed_mimes[i].needs_portal == 1 && !c->ft_key) continue;
        if (fixed_mimes[i].needs_portal == 2 && !c->doc_key) continue;
        g_ptr_array_add(out, (gpointer)fixed_mimes[i].mime);
    }
    /* file's content-type (single-file only) */
    if (!c->no_image_data && c->n_files == 1) {
        const char *m = c->files[0].mime;
        gboolean dup = FALSE;
        for (guint i = 0; i < out->len; i++)
            if (strcmp(out->pdata[i], m) == 0) { dup = TRUE; break; }
        if (!dup) g_ptr_array_add(out, (gpointer)m);
    }
    return out;
}

static void set_clipboard_selection(ctx_t *c) {
    GPtrArray *mimes = build_offer_list(c);

    if (c->primary) {
        if (!c->psdm) die("compositor does not support zwp_primary_selection_v1");
        c->primary_device = zwp_primary_selection_device_manager_v1_get_device(c->psdm, c->seat);
        c->primary_source = zwp_primary_selection_device_manager_v1_create_source(c->psdm);
        zwp_primary_selection_source_v1_add_listener(c->primary_source, &ps_listener, c);
        for (guint i = 0; i < mimes->len; i++)
            zwp_primary_selection_source_v1_offer(c->primary_source, mimes->pdata[i]);
        zwp_primary_selection_device_v1_set_selection(c->primary_device, c->primary_source, c->enter_serial);
    } else {
        c->data_device = wl_data_device_manager_get_data_device(c->ddm, c->seat);
        c->source = wl_data_device_manager_create_data_source(c->ddm);
        wl_data_source_add_listener(c->source, &ds_listener, c);
        for (guint i = 0; i < mimes->len; i++)
            wl_data_source_offer(c->source, mimes->pdata[i]);
        wl_data_device_set_selection(c->data_device, c->source, c->enter_serial);
    }
    g_ptr_array_free(mimes, TRUE);
    wl_display_flush(c->display);
    /* roundtrip so the compositor processes set_selection before we destroy surface */
    wl_display_roundtrip(c->display);
}

static void clear_clipboard(ctx_t *c) {
    if (c->primary) {
        c->primary_device = zwp_primary_selection_device_manager_v1_get_device(c->psdm, c->seat);
        zwp_primary_selection_device_v1_set_selection(c->primary_device, NULL, c->enter_serial);
    } else {
        c->data_device = wl_data_device_manager_get_data_device(c->ddm, c->seat);
        wl_data_device_set_selection(c->data_device, NULL, c->enter_serial);
    }
    wl_display_roundtrip(c->display);
}

int main(int argc, char *argv[]) {
    if (argc > 0 && argv[0] && *argv[0]) prog = argv[0];

    ctx_t c = {0};
    parse_args(&c, argc, argv);

    /* D-Bus + portals: only needed when actually setting (not for --clear). */
    if (!c.clear_only) {
        GError *err = NULL;
        c.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!c.bus) die("session bus: %s", err->message);
        if (!c.force_type ||
            strcmp(c.force_type, "application/vnd.portal.filetransfer") == 0) {
            c.ft_key = call_filetransfer(&c);
        }
        if (!c.force_type ||
            strcmp(c.force_type, "application/vnd.portal.files") == 0) {
            c.doc_key = call_documents(&c);
        }
    }

    /* Wayland connect + registry. */
    c.display = wl_display_connect(NULL);
    if (!c.display) die("wl_display_connect failed (is WAYLAND_DISPLAY set?)");
    struct wl_registry *reg = wl_display_get_registry(c.display);
    wl_registry_add_listener(reg, &reg_listener, &c);
    wl_display_roundtrip(c.display);
    wl_display_roundtrip(c.display);
    if (!c.compositor || !c.shm || !c.seat || !c.ddm || !c.wm_base)
        die("compositor is missing required Wayland globals");

    /* Surface + keyboard.enter to harvest a valid serial. */
    create_surface(&c);
    while (c.enter_serial == 0) {
        if (wl_display_dispatch(c.display) < 0) die("dispatch waiting for enter: %s", strerror(errno));
    }

    if (c.clear_only) {
        clear_clipboard(&c);
        destroy_surface(&c);
        return 0;
    }

    set_clipboard_selection(&c);

    /* Hide the focus window. wl_data_source is bound to the data_device,
     * not the surface, so destroying the surface here leaves the clipboard intact. */
    if (!c.keep_visible) destroy_surface(&c);

    /* Optional fork to background — wl-copy default. */
    if (!c.foreground) {
        pid_t pid = fork();
        if (pid < 0) die("fork: %s", strerror(errno));
        if (pid > 0) return 0;        /* parent exits, child keeps serving */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
    }

    /* Serve send() events until the compositor cancels or paste-once fires. */
    while (!c.cancelled) {
        if (wl_display_dispatch(c.display) < 0) break;
    }

    if (!c.surface_destroyed) destroy_surface(&c);
    for (int i = 0; i < c.n_files; i++) file_free(&c.files[i]);
    free(c.files);
    g_free(c.ft_key);
    g_free(c.doc_key);
    if (c.bus) g_object_unref(c.bus);
    wl_display_disconnect(c.display);
    return 0;
}
