/*
  SDLop: Win32 stubs for the D-Bus session-bus layer.

  D-Bus is how a Linux desktop exposes the system theme, portal file
  choosers and progress reporting. Windows has no session bus and none of
  those consumers exist there, so every entry point reports "not connected"
  and the callers fall back to their non-portal paths.

  The message encoder/decoder half of SDL_dbus.c is portable C, but it is
  only reachable through a live connection, so it is not compiled here.
  If a Windows consumer ever needs the wire format, this file should shrink
  to just the socket calls and the real codec should be shared.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include "internal/sdlop_internal.h"

#ifdef _WIN32

/* --- connection ------------------------------------------------------ */

bool SDLOP_DBus_Connect(void) { return false; }
bool SDLOP_DBus_IsConnected(void) { return false; }
const char *SDLOP_DBus_UniqueName(void) { return NULL; }
void SDLOP_DBus_Pump(void) { }
int SDLOP_DBus_GetFD(void) { return -1; }
void SDLOP_DBus_Quit(void) { }

/* --- calls and subscriptions ----------------------------------------- */

SDLop_DBusMsg *SDLOP_DBus_Call(SDLop_DBusMsg *m, int timeout_ms)
{
    (void)timeout_ms;
    SDLOP_DBus_FreeMessage(m);
    SDL_SetError("D-Bus is not available on this platform");
    return NULL;
}

bool SDLOP_DBus_Send(SDLop_DBusMsg *m)
{
    SDLOP_DBus_FreeMessage(m);
    return false;
}

Uint32 SDLOP_DBus_CallAsync(SDLop_DBusMsg *m)
{
    SDLOP_DBus_FreeMessage(m);
    return 0;
}

SDLop_DBusMsg *SDLOP_DBus_WaitReply(Uint32 serial, int timeout_ms)
{
    (void)serial; (void)timeout_ms;
    return NULL;
}

bool SDLOP_DBus_AddMatch(const char *rule) { (void)rule; return false; }
bool SDLOP_DBus_RemoveMatch(const char *rule) { (void)rule; return false; }
bool SDLOP_DBus_AddHandler(SDLop_DBusHandler fn, void *userdata) { (void)fn; (void)userdata; return false; }
void SDLOP_DBus_RemoveHandler(SDLop_DBusHandler fn, void *userdata) { (void)fn; (void)userdata; }

/* --- message construction -------------------------------------------- */

SDLop_DBusMsg *SDLOP_DBus_NewMessage(Uint8 type, const char *dest, const char *path, const char *iface,
                                     const char *member, const char *body_sig)
{
    (void)type; (void)dest; (void)path; (void)iface; (void)member; (void)body_sig;
    return NULL;
}

SDLop_DBusMsg *SDLOP_DBus_NewMethodCall(const char *dest, const char *path, const char *iface, const char *member,
                                        const char *body_sig)
{
    return SDLOP_DBus_NewMessage(SDLOP_DBUS_METHOD_CALL, dest, path, iface, member, body_sig);
}

SDLop_DBusMsg *SDLOP_DBus_NewSignal(const char *path, const char *iface, const char *member, const char *body_sig)
{
    return SDLOP_DBus_NewMessage(SDLOP_DBUS_SIGNAL, NULL, path, iface, member, body_sig);
}

SDLop_DBusMsg *SDLOP_DBus_NewReply(const SDLop_DBusMsg *call, const char *body_sig)
{
    (void)call; (void)body_sig;
    return NULL;
}

SDLop_DBusMsg *SDLOP_DBus_NewError(const SDLop_DBusMsg *call, const char *error_name, const char *text)
{
    (void)call; (void)error_name; (void)text;
    return NULL;
}

void SDLOP_DBus_FreeMessage(SDLop_DBusMsg *m) { (void)m; }
Uint8 SDLOP_DBus_MessageType(const SDLop_DBusMsg *m) { (void)m; return 0; }
Uint32 SDLOP_DBus_MessageSerial(const SDLop_DBusMsg *m) { (void)m; return 0; }
Uint32 SDLOP_DBus_MessageReplySerial(const SDLop_DBusMsg *m) { (void)m; return 0; }
const char *SDLOP_DBus_MessageField(const SDLop_DBusMsg *m, char which) { (void)m; (void)which; return NULL; }
void SDLOP_DBus_SetSender(SDLop_DBusMsg *m, const char *sender) { (void)m; (void)sender; }
void SDLOP_DBus_SetDestination(SDLop_DBusMsg *m, const char *dest) { (void)m; (void)dest; }
void SDLOP_DBus_SetNoReply(SDLop_DBusMsg *m) { (void)m; }

Uint8 *SDLOP_DBus_Serialize(SDLop_DBusMsg *m, Uint32 serial, size_t *len)
{
    (void)m; (void)serial;
    if (len) { *len = 0; }
    return NULL;
}

int SDLOP_DBus_Parse(const Uint8 *buf, size_t len, SDLop_DBusMsg **out, size_t *consumed)
{
    (void)buf; (void)len;
    if (out) { *out = NULL; }
    if (consumed) { *consumed = 0; }
    return -1;
}

/* --- body writers ---------------------------------------------------- */

void SDLOP_DBus_PutByte(SDLop_DBusMsg *m, Uint8 v) { (void)m; (void)v; }
void SDLOP_DBus_PutBool(SDLop_DBusMsg *m, bool v) { (void)m; (void)v; }
void SDLOP_DBus_PutU32(SDLop_DBusMsg *m, Uint32 v) { (void)m; (void)v; }
void SDLOP_DBus_PutDouble(SDLop_DBusMsg *m, double v) { (void)m; (void)v; }
void SDLOP_DBus_PutString(SDLop_DBusMsg *m, const char *s) { (void)m; (void)s; }
void SDLOP_DBus_PutSignature(SDLop_DBusMsg *m, const char *sig) { (void)m; (void)sig; }

SDLop_DBusArray SDLOP_DBus_OpenArray(SDLop_DBusMsg *m, char elem_type)
{
    (void)m; (void)elem_type;
    SDLop_DBusArray a = { 0, 0 };
    return a;
}

void SDLOP_DBus_CloseArray(SDLop_DBusMsg *m, const SDLop_DBusArray *a) { (void)m; (void)a; }
void SDLOP_DBus_OpenStruct(SDLop_DBusMsg *m) { (void)m; }
void SDLOP_DBus_PutDictVariantKey(SDLop_DBusMsg *m, const char *key, const char *value_sig)
{
    (void)m; (void)key; (void)value_sig;
}

/* --- body readers ---------------------------------------------------- */

bool SDLOP_DBus_IterInit(const SDLop_DBusMsg *msg, SDLop_DBusIter *it)
{
    (void)msg;
    if (it) {
        memset(it, 0, sizeof(*it));
    }
    return false;
}

int SDLOP_DBus_IterType(const SDLop_DBusIter *it) { (void)it; return 0; }
bool SDLOP_DBus_IterNext(SDLop_DBusIter *it) { (void)it; return false; }
bool SDLOP_DBus_IterRecurse(const SDLop_DBusIter *it, SDLop_DBusIter *sub) { (void)it; (void)sub; return false; }
bool SDLOP_DBus_IterGetByte(const SDLop_DBusIter *it, Uint8 *v) { (void)it; (void)v; return false; }
bool SDLOP_DBus_IterGetBool(const SDLop_DBusIter *it, bool *v) { (void)it; (void)v; return false; }
bool SDLOP_DBus_IterGetU32(const SDLop_DBusIter *it, Uint32 *v) { (void)it; (void)v; return false; }
bool SDLOP_DBus_IterGetI32(const SDLop_DBusIter *it, Sint32 *v) { (void)it; (void)v; return false; }
bool SDLOP_DBus_IterGetDouble(const SDLop_DBusIter *it, double *v) { (void)it; (void)v; return false; }
bool SDLOP_DBus_IterGetString(const SDLop_DBusIter *it, const char **v) { (void)it; (void)v; return false; }

#endif /* _WIN32 */
