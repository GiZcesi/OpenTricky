/* winsock2.h -- non-Windows shim: Winsock calls mapped onto BSD sockets (only what the
 * port's UDP netplay uses). SOCKET is a plain int descriptor here. */
#ifndef OT_SHIM_WINSOCK2_H
#define OT_SHIM_WINSOCK2_H
#include "xbox_winnt.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
typedef struct { WORD wVersion, wHighVersion; } WSADATA;
static inline int WSAStartup(WORD ver, WSADATA *d) { if (d) { d->wVersion = ver; d->wHighVersion = ver; } return 0; }
static inline int WSACleanup(void) { return 0; }
static inline int WSAGetLastError(void) { return errno; }
static inline int closesocket(SOCKET s) { return close(s); }
#define WSAEWOULDBLOCK EWOULDBLOCK
static inline int ioctlsocket(SOCKET s, long cmd, unsigned long *arg)
{   /* FIONBIO is the only command the port uses */
    int fl = fcntl(s, F_GETFL, 0);
    (void)cmd;
    return fcntl(s, F_SETFL, (arg && *arg) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
#define FIONBIO 0x8004667E
#endif
