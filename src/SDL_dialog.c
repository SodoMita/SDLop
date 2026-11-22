/*
  SDLop - file dialogs (SDL_dialog.h), like stock SDL3 on Linux:

  - the xdg-desktop-portal FileChooser over the built-in D-Bus client
    (SDL_dbus.c): OpenFile / SaveFile with the request's Response signal
    delivered by SDL_PumpEvents (the callback runs on the main thread, as
    in stock). The match on the request path is added *before* the call
    (stock's TODO), the chosen filter is reported (stock: always -1).
  - zenity --file-selection otherwise, on a thread (as stock)
  - SDL_HINT_FILE_DIALOG_DRIVER = "portal" | "zenity" picks one

  Parent windows (as stock): the portal gets "wayland:<handle>" for
  Wayland toplevels exported with xdg-foreign (when the compositor has it)
  or "x11:<hex>" for X11 windows; zenity gets --attach for X11 windows.

  Based on src/dialog/{SDL_dialog.c, SDL_dialog_utils.c,
  unix/SDL_portaldialog.c, unix/SDL_zenitydialog.c} (zlib license).
*/

#include "internal/sdlop_internal.h"

#include <errno.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* stock's validate_list: [a-zA-Z0-9_.-] patterns separated by ';', or "*" */
static const char *validate_list(const char *list)
{
    if (!list) {
        return "NULL pattern";
    }
    if (strcmp(list, "*") == 0) {
        return NULL;
    }
    for (const char *c = list; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' ||
            *c == '_' || *c == '.') {
            continue;
        } else if (*c == ';') {
            if (c == list || c[-1] == ';') {
                return "Empty pattern not allowed";
            }
        } else {
            return "Invalid character in pattern (Only [a-zA-Z0-9_.-] allowed, or a single *)";
        }
    }
    if (!list[0] || list[strlen(list) - 1] == ';') {
        return "Empty pattern not allowed";
    }
    return NULL;
}

/* "png;jpg" -> "*.[pP][nN][gG]" ... one glob per pattern, case-insensitive
 * (stock); "*" stays "*". Calls fn for each glob. */
static void for_each_glob(const char *list, void (*fn)(const char *glob, void *ud), void *ud)
{
    if (strcmp(list, "*") == 0) {
        fn("*", ud);
        return;
    }
    const char *p = list;
    while (*p) {
        const char *end = strchr(p, ';');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        char glob[512];
        size_t g = 0;
        glob[g++] = '*';
        glob[g++] = '.';
        for (size_t i = 0; i < n && g + 5 < sizeof(glob); i++) {
            const char c = p[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                glob[g++] = '[';
                glob[g++] = (char)(c | 0x20);
                glob[g++] = (char)(c & ~0x20);
                glob[g++] = ']';
            } else {
                glob[g++] = c;
            }
        }
        glob[g] = '\0';
        fn(glob, ud);
        if (!end) {
            break;
        }
        p = end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* xdg-desktop-portal FileChooser                                      */
/* ------------------------------------------------------------------ */

typedef struct PortalRequest
{
    SDL_DialogFileCallback callback;
    void *userdata;
    char path[256];
    char rule[512];
    char **filter_names; /* to report which filter was chosen */
    int nfilters;
    struct PortalRequest *next;
} PortalRequest;

static PortalRequest *portal_requests;

#define PORTAL_RESPONSE_RULE                                                                                     \
    "type='signal',sender='org.freedesktop.portal.Desktop',interface='org.freedesktop.portal.Request',member=" \
    "'Response',path='%s'"

static void portal_free(PortalRequest *req)
{
    for (PortalRequest **pp = &portal_requests; *pp; pp = &(*pp)->next) {
        if (*pp == req) {
            *pp = req->next;
            break;
        }
    }
    for (int i = 0; i < req->nfilters; i++) {
        free(req->filter_names[i]);
    }
    free(req->filter_names);
    free(req);
}

/* a{sv} result -> uris + current_filter */
static void portal_results(SDLop_DBusIter *arr, char ***uris, int *nuris, const char **filter_name)
{
    SDLop_DBusIter ent, var, sub, st;
    while (SDLOP_DBus_IterType(arr)) {
        const char *key = NULL;
        if (SDLOP_DBus_IterRecurse(arr, &ent) && SDLOP_DBus_IterGetString(&ent, &key) && SDLOP_DBus_IterNext(&ent) &&
            SDLOP_DBus_IterRecurse(&ent, &var)) {
            if (strcmp(key, "uris") == 0 && SDLOP_DBus_IterRecurse(&var, &sub)) {
                while (SDLOP_DBus_IterType(&sub) == 's') {
                    const char *uri;
                    if (SDLOP_DBus_IterGetString(&sub, &uri)) {
                        char **nu = (char **)realloc(*uris, sizeof(char *) * (size_t)(*nuris + 2));
                        if (nu) {
                            *uris = nu;
                            (*uris)[(*nuris)++] = strdup(uri);
                            (*uris)[*nuris] = NULL;
                        }
                    }
                    if (!SDLOP_DBus_IterNext(&sub)) {
                        break;
                    }
                }
            } else if (strcmp(key, "current_filter") == 0 && SDLOP_DBus_IterRecurse(&var, &st)) {
                SDLOP_DBus_IterGetString(&st, filter_name); /* (sa(us)): the name */
            }
        }
        if (!SDLOP_DBus_IterNext(arr)) {
            break;
        }
    }
}

static bool portal_response(SDLop_DBusMsg *msg, void *userdata)
{
    PortalRequest *req = (PortalRequest *)userdata;
    const char *iface = SDLOP_DBus_MessageField(msg, 'i');
    const char *member = SDLOP_DBus_MessageField(msg, 'm');
    const char *path = SDLOP_DBus_MessageField(msg, 'p');
    if (SDLOP_DBus_MessageType(msg) != SDLOP_DBUS_SIGNAL || !iface || !member || !path ||
        strcmp(iface, "org.freedesktop.portal.Request") != 0 || strcmp(member, "Response") != 0 ||
        strcmp(path, req->path) != 0) {
        return false;
    }
    SDLop_DBusIter it, arr;
    Uint32 response = 2;
    char **uris = NULL;
    int nuris = 0;
    const char *filter_name = NULL;
    if (SDLOP_DBus_IterInit(msg, &it) && SDLOP_DBus_IterGetU32(&it, &response) && SDLOP_DBus_IterNext(&it) &&
        SDLOP_DBus_IterRecurse(&it, &arr)) {
        portal_results(&arr, &uris, &nuris, &filter_name);
    }
    int filter = -1;
    for (int i = 0; filter_name && i < req->nfilters; i++) {
        if (req->filter_names[i] && strcmp(req->filter_names[i], filter_name) == 0) {
            filter = i;
        }
    }
    SDLOP_DBus_RemoveMatch(req->rule);
    SDLOP_DBus_RemoveHandler(portal_response, req);
    if (response == 1 || response == 2) {
        /* cancelled (1) or ended otherwise (2): an empty list (stock) */
        const char *none[] = { NULL };
        req->callback(req->userdata, none, filter);
    } else if (response != 0) {
        SDL_SetError("Portal dialogs: response %u", (unsigned)response);
        req->callback(req->userdata, NULL, -1);
    } else {
        bool ok = true;
        for (int i = 0; i < nuris && ok; i++) {
            if (!uris[i] || SDLOP_URIToLocal(uris[i], uris[i]) < 0) {
                SDL_SetError("Portal dialogs: Unsupported protocol: %s", uris[i] ? uris[i] : "(null)");
                ok = false;
            }
        }
        const char *none[] = { NULL };
        req->callback(req->userdata, ok ? (uris ? (const char *const *)uris : none) : NULL, ok ? filter : -1);
    }
    for (int i = 0; i < nuris; i++) {
        free(uris[i]);
    }
    free(uris);
    portal_free(req);
    return true;
}

typedef struct
{
    SDLop_DBusMsg *m;
} GlobWriter;

static void put_glob(const char *glob, void *ud)
{
    SDLop_DBusMsg *m = ((GlobWriter *)ud)->m;
    SDLOP_DBus_OpenStruct(m);
    SDLOP_DBus_PutU32(m, 0); /* 0: glob, 1: MIME type */
    SDLOP_DBus_PutString(m, glob);
}

static void put_bytes_option(SDLop_DBusMsg *m, const char *key, const char *value)
{
    /* a NUL-terminated byte string (paths needn't be UTF-8) */
    SDLOP_DBus_PutDictVariantKey(m, key, "ay");
    SDLop_DBusArray a = SDLOP_DBus_OpenArray(m, 'y');
    do {
        SDLOP_DBus_PutByte(m, (Uint8)*value);
    } while (*value++);
    SDLOP_DBus_CloseArray(m, &a);
}

static bool portal_unavailable_error(void)
{
    const char *e = SDL_GetError();
    return strstr(e, "ServiceUnknown") || strstr(e, "UnknownMethod") || strstr(e, "UnknownObject") ||
           strstr(e, "UnknownInterface") || strstr(e, "NameHasNoOwner") || strstr(e, "no session bus") ||
           strstr(e, "not connected");
}

/* false: no portal (try zenity); true: handled (callback called or pending) */
static bool portal_show(SDL_FileDialogType type, SDL_DialogFileCallback callback, void *userdata,
                        SDL_PropertiesID props)
{
    if (!SDLOP_DBus_Connect() || !SDLOP_DBus_UniqueName()) {
        SDL_SetError("D-Bus: no session bus");
        return false;
    }
    SDL_Window *window = (SDL_Window *)SDL_GetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, NULL);
    const SDL_DialogFileFilter *filters =
        (const SDL_DialogFileFilter *)SDL_GetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, NULL);
    const int nfilters = filters ? (int)SDL_GetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 0) : 0;
    const bool many = SDL_GetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    const char *location = SDL_GetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, NULL);
    const char *accept = SDL_GetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, NULL);
    const char *method = type == SDL_FILEDIALOG_SAVEFILE ? "SaveFile" : "OpenFile";
    const char *title = SDL_GetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                                              type == SDL_FILEDIALOG_SAVEFILE     ? "Save File"
                                              : type == SDL_FILEDIALOG_OPENFOLDER ? "Open Folder"
                                                                                  : "Open File");

    PortalRequest *req = (PortalRequest *)calloc(1, sizeof(*req));
    if (!req) {
        SDL_OutOfMemory();
        callback(userdata, NULL, -1);
        return true;
    }
    req->callback = callback;
    req->userdata = userdata;
    if (nfilters > 0) {
        req->filter_names = (char **)calloc((size_t)nfilters, sizeof(char *));
        req->nfilters = req->filter_names ? nfilters : 0;
        for (int i = 0; i < req->nfilters; i++) {
            req->filter_names[i] = strdup(filters[i].name ? filters[i].name : "");
        }
    }
    req->next = portal_requests;
    portal_requests = req;

    /* the request object's path is known in advance (portal >= 0.9):
     * subscribe before calling so a fast Response can't be missed */
    static Uint32 token_counter;
    char token[32];
    snprintf(token, sizeof(token), "sdlop%u", ++token_counter);
    char sender[128];
    snprintf(sender, sizeof(sender), "%s", SDLOP_DBus_UniqueName() + 1); /* drop ':' */
    for (char *c = sender; *c; c++) {
        if (*c == '.') {
            *c = '_';
        }
    }
    snprintf(req->path, sizeof(req->path), "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    snprintf(req->rule, sizeof(req->rule), PORTAL_RESPONSE_RULE, req->path);
    SDLOP_DBus_AddMatch(req->rule);
    SDLOP_DBus_AddHandler(portal_response, req);

    SDLop_DBusMsg *m = SDLOP_DBus_NewMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                "org.freedesktop.portal.FileChooser", method, "ssa{sv}");
    if (!m) {
        goto fail_nomem;
    }
    char parent[320] = "";
    if (window) {
        const SDL_PropertiesID wprops = SDL_GetWindowProperties(window);
        const char *handle = SDL_GetStringProperty(wprops, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_EXPORT_HANDLE_STRING, NULL);
        const Sint64 xid = SDL_GetNumberProperty(wprops, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (handle && *handle) {
            snprintf(parent, sizeof(parent), "wayland:%s", handle);
        } else if (xid) {
            snprintf(parent, sizeof(parent), "x11:%llx", (unsigned long long)xid);
        }
    }
    SDLOP_DBus_PutString(m, parent);
    SDLOP_DBus_PutString(m, title);
    SDLop_DBusArray opts = SDLOP_DBus_OpenArray(m, '{');
    SDLOP_DBus_PutDictVariantKey(m, "handle_token", "s");
    SDLOP_DBus_PutString(m, token);
    SDLOP_DBus_PutDictVariantKey(m, "modal", "b");
    SDLOP_DBus_PutBool(m, window != NULL);
    if (many && type != SDL_FILEDIALOG_SAVEFILE) {
        SDLOP_DBus_PutDictVariantKey(m, "multiple", "b");
        SDLOP_DBus_PutBool(m, true);
    }
    if (type == SDL_FILEDIALOG_OPENFOLDER) {
        SDLOP_DBus_PutDictVariantKey(m, "directory", "b");
        SDLOP_DBus_PutBool(m, true);
    }
    if (nfilters > 0) {
        SDLOP_DBus_PutDictVariantKey(m, "filters", "a(sa(us))");
        SDLop_DBusArray fa = SDLOP_DBus_OpenArray(m, '(');
        for (int i = 0; i < nfilters; i++) {
            SDLOP_DBus_OpenStruct(m);
            SDLOP_DBus_PutString(m, filters[i].name ? filters[i].name : "");
            SDLop_DBusArray ga = SDLOP_DBus_OpenArray(m, '(');
            GlobWriter gw = { m };
            for_each_glob(filters[i].pattern, put_glob, &gw);
            SDLOP_DBus_CloseArray(m, &ga);
        }
        SDLOP_DBus_CloseArray(m, &fa);
    }
    if (location && *location) {
        struct stat st;
        if (type == SDL_FILEDIALOG_SAVEFILE) {
            char *d = strdup(location), *b = strdup(location);
            const bool exists = stat(location, &st) == 0;
            if (d && b && exists && S_ISREG(st.st_mode)) {
                /* at an existing file (current_name too: KDE drops the extension otherwise) */
                put_bytes_option(m, "current_file", location);
                SDLOP_DBus_PutDictVariantKey(m, "current_name", "s");
                SDLOP_DBus_PutString(m, basename(b));
            } else if (d && b && !exists && errno == ENOENT && stat(dirname(d), &st) == 0 && S_ISDIR(st.st_mode)) {
                /* a folder and a suggested new name */
                put_bytes_option(m, "current_folder", d);
                SDLOP_DBus_PutDictVariantKey(m, "current_name", "s");
                SDLOP_DBus_PutString(m, basename(b));
            } else {
                put_bytes_option(m, "current_folder", location);
            }
            free(d);
            free(b);
        } else {
            put_bytes_option(m, "current_folder", location);
        }
    }
    if (accept) {
        SDLOP_DBus_PutDictVariantKey(m, "accept_label", "s");
        SDLOP_DBus_PutString(m, accept);
    }
    SDLOP_DBus_CloseArray(m, &opts);

    /* the reply (the request handle) comes right away; the answer later
     * as Response. -1: the portal may have to be started first */
    SDLop_DBusMsg *reply = SDLOP_DBus_Call(m, -1);
    SDLOP_DBus_FreeMessage(m);
    if (!reply) {
        const bool unavailable = portal_unavailable_error();
        SDLOP_DBus_RemoveMatch(req->rule);
        SDLOP_DBus_RemoveHandler(portal_response, req);
        portal_free(req);
        if (unavailable) {
            return false;
        }
        callback(userdata, NULL, -1); /* SDL error set by the call */
        return true;
    }
    SDLop_DBusIter it;
    const char *handle = NULL;
    if (SDLOP_DBus_IterInit(reply, &it) && SDLOP_DBus_IterGetString(&it, &handle) && strcmp(handle, req->path) != 0) {
        /* old portal: a different request path */
        SDLOP_DBus_RemoveMatch(req->rule);
        snprintf(req->path, sizeof(req->path), "%s", handle);
        snprintf(req->rule, sizeof(req->rule), PORTAL_RESPONSE_RULE, req->path);
        SDLOP_DBus_AddMatch(req->rule);
    }
    SDLOP_DBus_FreeMessage(reply);
    return true;

fail_nomem:
    SDLOP_DBus_RemoveMatch(req->rule);
    SDLOP_DBus_RemoveHandler(portal_response, req);
    portal_free(req);
    SDL_OutOfMemory();
    callback(userdata, NULL, -1);
    return true;
}

/* video quit drops the bus: pending requests never get an answer */
void SDLOP_Dialog_Quit(void)
{
    while (portal_requests) {
        PortalRequest *req = portal_requests;
        SDLOP_DBus_RemoveHandler(portal_response, req);
        portal_free(req);
    }
}

/* ------------------------------------------------------------------ */
/* zenity                                                              */
/* ------------------------------------------------------------------ */

#ifndef __EMSCRIPTEN__
typedef struct
{
    SDL_DialogFileCallback callback;
    void *userdata;
    char **argv; /* malloc'ed strings */
    int argc;
} ZenityJob;

static void zenity_arg(ZenityJob *job, const char *s)
{
    job->argv[job->argc++] = strdup(s);
}

static void zenity_free(ZenityJob *job)
{
    for (int i = 0; i < job->argc; i++) {
        free(job->argv[i]);
    }
    free(job->argv);
    free(job);
}

typedef struct
{
    char buf[2048];
    size_t len;
} FilterArg;

static void zenity_glob(const char *glob, void *ud)
{
    FilterArg *f = (FilterArg *)ud;
    const int n = snprintf(f->buf + f->len, sizeof(f->buf) - f->len, "%s%s", f->buf[f->len - 1] == ' ' ? "" : " ", glob);
    if (n > 0 && f->len + (size_t)n < sizeof(f->buf)) {
        f->len += (size_t)n;
    }
}

static void *zenity_thread(void *arg)
{
    ZenityJob *job = (ZenityJob *)arg;
    /* recent zenity takes its exit codes from the environment (stock) */
    static const char *const env[] = { "ZENITY_OK=0",    "ZENITY_CANCEL=1", "ZENITY_ESC=1",
                                       "ZENITY_EXTRA=2", "ZENITY_ERROR=2",  "ZENITY_TIMEOUT=2",
                                       NULL };
    char *out = NULL;
    const int status = SDLOP_RunProcess((const char *const *)job->argv, env, &out, true);
    if (status == 0 || status == 1) {
        /* one path per line (--separator=\n); cancel: an empty list */
        size_t n = 0;
        char **list = (char **)calloc(1, sizeof(char *));
        for (char *line = out, *next; status == 0 && list && line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) {
                *next++ = '\0';
            }
            if (!*line) {
                continue;
            }
            char **nl = (char **)realloc(list, sizeof(char *) * (n + 2));
            if (!nl) {
                break;
            }
            list = nl;
            list[n++] = line;
            list[n] = NULL;
        }
        const char *none[] = { NULL };
        job->callback(job->userdata, list ? (const char *const *)list : none, -1);
        free(list);
    } else {
        if (status >= 0) {
            SDL_SetError("Could not run zenity: exit code %d", status);
        }
        job->callback(job->userdata, NULL, -1);
    }
    free(out);
    zenity_free(job);
    return NULL;
}

static bool zenity_show(SDL_FileDialogType type, SDL_DialogFileCallback callback, void *userdata,
                        SDL_PropertiesID props)
{
    if (!SDLOP_ProgramInPath("zenity")) {
        return SDL_SetError("zenity is not installed");
    }
    const SDL_DialogFileFilter *filters =
        (const SDL_DialogFileFilter *)SDL_GetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, NULL);
    const int nfilters = filters ? (int)SDL_GetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 0) : 0;
    ZenityJob *job = (ZenityJob *)calloc(1, sizeof(*job));
    if (job) {
        job->argv = (char **)calloc((size_t)(24 + (nfilters > 0 ? nfilters : 0)), sizeof(char *));
    }
    if (!job || !job->argv) {
        free(job);
        SDL_OutOfMemory();
        callback(userdata, NULL, -1);
        return true;
    }
    job->callback = callback;
    job->userdata = userdata;
    zenity_arg(job, "zenity");
    zenity_arg(job, "--file-selection");
    zenity_arg(job, "--separator=\n");
    if (SDL_GetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false)) {
        zenity_arg(job, "--multiple");
    }
    if (type == SDL_FILEDIALOG_SAVEFILE) {
        zenity_arg(job, "--save");
        zenity_arg(job, "--confirm-overwrite");
    } else if (type == SDL_FILEDIALOG_OPENFOLDER) {
        zenity_arg(job, "--directory");
    }
    const char *location = SDL_GetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, NULL);
    if (location) {
        zenity_arg(job, "--filename");
        zenity_arg(job, location);
    }
    SDL_Window *window = (SDL_Window *)SDL_GetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, NULL);
    const Sint64 xid =
        window ? SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0) : 0;
    int major = 0, minor = 0;
    if (xid && SDLOP_ZenityVersion(&major, &minor) && (major > 3 || (major == 3 && minor >= 6))) {
        char h[32];
        snprintf(h, sizeof(h), "0x%llx", (unsigned long long)xid);
        zenity_arg(job, "--modal");
        zenity_arg(job, "--attach");
        zenity_arg(job, h);
    }
    static const struct
    {
        const char *prop, *flag;
    } labels[] = { { SDL_PROP_FILE_DIALOG_TITLE_STRING, "--title" },
                   { SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "--ok-label" },
                   { SDL_PROP_FILE_DIALOG_CANCEL_STRING, "--cancel-label" } };
    for (size_t i = 0; i < SDL_arraysize(labels); i++) {
        const char *v = SDL_GetStringProperty(props, labels[i].prop, NULL);
        if (v) {
            zenity_arg(job, labels[i].flag);
            zenity_arg(job, v);
        }
    }
    for (int i = 0; i < nfilters; i++) {
        /* --file-filter=Name | *.[pP][nN][gG] *.[jJ][pP][gG] ('|' in names
         * is special to zenity: '/' instead, as stock) */
        FilterArg f;
        f.len = (size_t)snprintf(f.buf, sizeof(f.buf), "--file-filter=%s |", filters[i].name ? filters[i].name : "");
        if (f.len >= sizeof(f.buf)) {
            f.len = sizeof(f.buf) - 1;
        }
        for (char *c = f.buf + 14; c < f.buf + f.len - 1; c++) {
            if (*c == '|') {
                *c = '/';
            }
        }
        for_each_glob(filters[i].pattern, zenity_glob, &f);
        zenity_arg(job, f.buf);
    }
    job->argv[job->argc] = NULL;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const bool started = pthread_create(&th, &attr, zenity_thread, job) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
        zenity_free(job);
        SDL_SetError("Could not start the zenity thread");
        callback(userdata, NULL, -1);
    }
    return true;
}
#endif /* !__EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void SDL_ShowFileDialogWithProperties(SDL_FileDialogType type, SDL_DialogFileCallback callback, void *userdata,
                                      SDL_PropertiesID props)
{
    if (!callback) {
        return;
    }
    const SDL_DialogFileFilter *filters =
        (const SDL_DialogFileFilter *)SDL_GetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, NULL);
    const int nfilters = (int)SDL_GetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, -1);
    if (filters && nfilters == -1) {
        SDL_SetError("Set filter pointers, but didn't set number of filters (SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER)");
        callback(userdata, NULL, -1);
        return;
    }
    for (int i = 0; filters && i < nfilters; i++) {
        const char *msg = validate_list(filters[i].pattern);
        if (msg) {
            SDL_SetError("Invalid dialog file filters: %s", msg);
            callback(userdata, NULL, -1);
            return;
        }
    }
    if (type != SDL_FILEDIALOG_OPENFILE && type != SDL_FILEDIALOG_SAVEFILE && type != SDL_FILEDIALOG_OPENFOLDER) {
        SDL_SetError("Unsupported file dialog type: %d", (int)type);
        callback(userdata, NULL, -1);
        return;
    }
#ifdef __EMSCRIPTEN__
    SDL_SetError("SDL not built with dialog support");
    callback(userdata, NULL, -1);
#else
    const char *driver = SDL_GetHint(SDL_HINT_FILE_DIALOG_DRIVER);
    if (driver && *driver && strcmp(driver, "portal") != 0 && strcmp(driver, "zenity") != 0) {
        driver = "invalid";
    }
    if ((!driver || !*driver || strcmp(driver, "portal") == 0) && portal_show(type, callback, userdata, props)) {
        return;
    }
    if ((!driver || !*driver || strcmp(driver, "zenity") == 0) && zenity_show(type, callback, userdata, props)) {
        return;
    }
    SDL_SetError("File dialog driver unsupported (supported values for SDL_HINT_FILE_DIALOG_DRIVER are 'zenity' and "
                 "'portal')");
    callback(userdata, NULL, -1);
#endif
}

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int nfilters, const char *default_location,
                            bool allow_many)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void *)filters);
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_location);
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, allow_many);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, callback, userdata, props);
    SDL_DestroyProperties(props);
}

void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int nfilters, const char *default_location)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void *)filters);
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, nfilters);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_location);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_SAVEFILE, callback, userdata, props);
    SDL_DestroyProperties(props);
}

void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                              const char *default_location, bool allow_many)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, default_location);
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, allow_many);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, callback, userdata, props);
    SDL_DestroyProperties(props);
}
