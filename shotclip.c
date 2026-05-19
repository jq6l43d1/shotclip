/*
 * copy-screenshot: put a file on the Wayland clipboard with the same
 * MIME types Nautilus uses, including application/vnd.portal.{files,filetransfer}.
 *
 * Uses libwayland directly so we can capture the keyboard.enter serial and
 * pass it to set_selection — GTK4's clipboard layer drops the serial.
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

typedef struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_data_device_manager *ddm;
    struct xdg_wm_base *wm_base;

    struct wl_keyboard *keyboard;
    struct wl_data_device *data_device;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *buffer;
    int configured;

    struct wl_data_source *source;
    uint32_t enter_serial;
    int cancelled;

    char *abs_path;
    char *file_uri;
    GDBusConnection *bus;
    char *ft_key;
    char *doc_key;
} state_t;

static state_t S = {0};

/* ---------- xdg_wm_base ---------- */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

/* ---------- xdg_surface ---------- */
static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    S.configured = 1;
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };

/* ---------- xdg_toplevel ---------- */
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static void tl_configure_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_wm_capabilities(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure,
    .close = tl_close,
    .configure_bounds = tl_configure_bounds,
    .wm_capabilities = tl_wm_capabilities,
};

/* ---------- wl_keyboard ---------- */
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz) { close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf, struct wl_array *keys) {
    S.enter_serial = serial;
    fprintf(stderr, "keyboard.enter serial=%u\n", serial);
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t md, uint32_t ml, uint32_t mk, uint32_t group) {}
static void kb_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat_info,
};

/* ---------- wl_seat ---------- */
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !S.keyboard) {
        S.keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(S.keyboard, &kb_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* ---------- D-Bus portal helpers ---------- */
static char *call_filetransfer_start_and_add(const char *path) {
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(
        S.bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.FileTransfer",
        "StartTransfer",
        g_variant_new("(a{sv})", NULL),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (!r) {
        fprintf(stderr, "StartTransfer: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return NULL;
    }
    char *key = NULL;
    g_variant_get(r, "(s)", &key);
    g_variant_unref(r);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { g_free(key); return NULL; }
    GUnixFDList *fdl = g_unix_fd_list_new();
    gint32 idx = g_unix_fd_list_append(fdl, fd, &err);
    close(fd);
    if (idx < 0) { g_object_unref(fdl); g_free(key); g_clear_error(&err); return NULL; }

    GVariantBuilder fdb;
    g_variant_builder_init(&fdb, G_VARIANT_TYPE("ah"));
    g_variant_builder_add(&fdb, "h", idx);

    GVariant *r2 = g_dbus_connection_call_with_unix_fd_list_sync(
        S.bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.FileTransfer",
        "AddFiles",
        g_variant_new("(s@aha{sv})", key, g_variant_builder_end(&fdb), NULL),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, fdl, NULL, NULL, &err);
    g_object_unref(fdl);
    if (!r2) {
        fprintf(stderr, "AddFiles: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        g_free(key);
        return NULL;
    }
    g_variant_unref(r2);
    return key;
}

static char *call_documents_addfull(const char *path) {
    GError *err = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    GUnixFDList *fdl = g_unix_fd_list_new();
    gint32 idx = g_unix_fd_list_append(fdl, fd, &err);
    close(fd);
    if (idx < 0) { g_object_unref(fdl); g_clear_error(&err); return NULL; }

    GVariantBuilder fdb, perms;
    g_variant_builder_init(&fdb, G_VARIANT_TYPE("ah"));
    g_variant_builder_add(&fdb, "h", idx);
    g_variant_builder_init(&perms, G_VARIANT_TYPE("as"));

    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
        S.bus,
        "org.freedesktop.portal.Documents",
        "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.Documents",
        "AddFull",
        g_variant_new("(@ahusas)", g_variant_builder_end(&fdb), 0u, "", &perms),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, fdl, NULL, NULL, &err);
    g_object_unref(fdl);
    if (!r) {
        fprintf(stderr, "AddFull: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return NULL;
    }
    GVariant *ids = g_variant_get_child_value(r, 0);
    GString *out = g_string_new(NULL);
    gsize n = g_variant_n_children(ids);
    for (gsize i = 0; i < n; i++) {
        const char *id = NULL;
        GVariant *c = g_variant_get_child_value(ids, i);
        g_variant_get(c, "&s", &id);
        if (i > 0) g_string_append_c(out, ' ');
        g_string_append(out, id);
        g_variant_unref(c);
    }
    g_variant_unref(ids);
    g_variant_unref(r);
    return g_string_free(out, FALSE);
}

/* ---------- data source ---------- */
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

static void ds_target(void *d, struct wl_data_source *s, const char *mt) {}
static void ds_send(void *d, struct wl_data_source *s, const char *mt, int32_t fd) {
    fprintf(stderr, "data_source.send: %s\n", mt);
    if (strcmp(mt, "image/png") == 0) {
        int rfd = open(S.abs_path, O_RDONLY);
        if (rfd >= 0) {
            char buf[8192]; ssize_t n;
            while ((n = read(rfd, buf, sizeof(buf))) > 0) write_all(fd, buf, n);
            close(rfd);
        }
    } else if (strcmp(mt, "text/uri-list") == 0) {
        char *u = g_strdup_printf("%s\r\n", S.file_uri);
        write_all(fd, u, strlen(u));
        g_free(u);
    } else if (strcmp(mt, "x-special/gnome-copied-files") == 0) {
        char *u = g_strdup_printf("copy\n%s", S.file_uri);
        write_all(fd, u, strlen(u));
        g_free(u);
    } else if (strcmp(mt, "text/plain;charset=utf-8") == 0 || strcmp(mt, "text/plain") == 0) {
        write_all(fd, S.abs_path, strlen(S.abs_path));
    } else if (strcmp(mt, "application/vnd.portal.filetransfer") == 0) {
        if (S.ft_key) write_all(fd, S.ft_key, strlen(S.ft_key) + 1);
    } else if (strcmp(mt, "application/vnd.portal.files") == 0) {
        if (S.doc_key) write_all(fd, S.doc_key, strlen(S.doc_key) + 1);
    }
    close(fd);
}
static void ds_cancelled(void *d, struct wl_data_source *s) {
    fprintf(stderr, "data_source.cancelled\n");
    S.cancelled = 1;
}
static void ds_dnd_drop_performed(void *d, struct wl_data_source *s) {}
static void ds_dnd_finished(void *d, struct wl_data_source *s) {}
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) {}
static const struct wl_data_source_listener ds_listener = {
    .target = ds_target, .send = ds_send, .cancelled = ds_cancelled,
    .dnd_drop_performed = ds_dnd_drop_performed, .dnd_finished = ds_dnd_finished,
    .action = ds_action,
};

/* ---------- registry ---------- */
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    if (!strcmp(iface, "wl_compositor")) {
        S.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, "wl_shm")) {
        S.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, "wl_seat")) {
        S.seat = wl_registry_bind(r, name, &wl_seat_interface, version > 7 ? 7 : version);
        wl_seat_add_listener(S.seat, &seat_listener, NULL);
    } else if (!strcmp(iface, "wl_data_device_manager")) {
        S.ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    } else if (!strcmp(iface, "xdg_wm_base")) {
        S.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, version > 5 ? 5 : version);
        xdg_wm_base_add_listener(S.wm_base, &wm_listener, NULL);
    }
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_global_remove };

/* ---------- SHM buffer ---------- */
static struct wl_buffer *create_buffer_1x1(void) {
    int stride = 4, size = 4;
    int fd = memfd_create("buf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(S.shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file>\n", argv[0]); return 1; }

    S.abs_path = realpath(argv[1], NULL);
    if (!S.abs_path) { perror("realpath"); return 1; }

    char *enc = g_uri_escape_string(S.abs_path, "/", FALSE);
    S.file_uri = g_strdup_printf("file://%s", enc);
    g_free(enc);

    GError *err = NULL;
    S.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!S.bus) { fprintf(stderr, "bus: %s\n", err->message); return 1; }

    /* Pre-compute portal keys so wl_data_source.send doesn't have to wait. */
    S.ft_key = call_filetransfer_start_and_add(S.abs_path);
    if (S.ft_key) fprintf(stderr, "FileTransfer key: %s\n", S.ft_key);
    S.doc_key = call_documents_addfull(S.abs_path);
    if (S.doc_key) fprintf(stderr, "Documents key: %s\n", S.doc_key);

    /* Connect to Wayland and bind globals. */
    S.display = wl_display_connect(NULL);
    if (!S.display) { fprintf(stderr, "wl_display_connect failed\n"); return 1; }

    struct wl_registry *reg = wl_display_get_registry(S.display);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(S.display);
    wl_display_roundtrip(S.display);

    if (!S.compositor || !S.shm || !S.seat || !S.ddm || !S.wm_base) {
        fprintf(stderr, "Missing globals\n"); return 1;
    }

    /* Surface + xdg_toplevel. */
    S.surface = wl_compositor_create_surface(S.compositor);
    S.xdg_surface = xdg_wm_base_get_xdg_surface(S.wm_base, S.surface);
    xdg_surface_add_listener(S.xdg_surface, &xs_listener, NULL);
    S.toplevel = xdg_surface_get_toplevel(S.xdg_surface);
    xdg_toplevel_add_listener(S.toplevel, &tl_listener, NULL);
    xdg_toplevel_set_title(S.toplevel, "Copying to clipboard…");
    xdg_toplevel_set_app_id(S.toplevel, "org.satty.clipcopy");
    wl_surface_commit(S.surface);

    /* Wait for first configure. */
    while (!S.configured) {
        if (wl_display_dispatch(S.display) < 0) { perror("dispatch"); return 1; }
    }

    /* Attach a 1x1 transparent buffer to actually map the surface. */
    S.buffer = create_buffer_1x1();
    wl_surface_attach(S.surface, S.buffer, 0, 0);
    wl_surface_damage_buffer(S.surface, 0, 0, 1, 1);
    wl_surface_commit(S.surface);

    /* Get data device. */
    S.data_device = wl_data_device_manager_get_data_device(S.ddm, S.seat);

    /* Wait for keyboard.enter to capture a valid serial. */
    while (S.enter_serial == 0) {
        if (wl_display_dispatch(S.display) < 0) { perror("dispatch"); return 1; }
    }

    /* Build wl_data_source and advertise MIME types. */
    S.source = wl_data_device_manager_create_data_source(S.ddm);
    wl_data_source_add_listener(S.source, &ds_listener, NULL);
    wl_data_source_offer(S.source, "image/png");
    wl_data_source_offer(S.source, "text/uri-list");
    wl_data_source_offer(S.source, "x-special/gnome-copied-files");
    wl_data_source_offer(S.source, "text/plain;charset=utf-8");
    wl_data_source_offer(S.source, "text/plain");
    if (S.ft_key)  wl_data_source_offer(S.source, "application/vnd.portal.filetransfer");
    if (S.doc_key) wl_data_source_offer(S.source, "application/vnd.portal.files");

    wl_data_device_set_selection(S.data_device, S.source, S.enter_serial);
    fprintf(stderr, "set_selection serial=%u\n", S.enter_serial);

    /* Serve clipboard until the compositor cancels us or window is closed. */
    while (!S.cancelled) {
        if (wl_display_dispatch(S.display) < 0) break;
    }

    return 0;
}
