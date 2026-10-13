/* Portable IPv4 transport for Quake's existing datagram API.
 * Native sockets stay outside Quake's int handles and address representation.
 * GPL-2.0-or-later; see gnu.txt. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET native_socket_t;
typedef int socket_length_t;
#define BAD_SOCKET INVALID_SOCKET
#define CloseSocket closesocket
#else
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int native_socket_t;
typedef socklen_t socket_length_t;
#define BAD_SOCKET (-1)
#define CloseSocket close
#endif
#include "quakedef.h"
#include "net_udp.h"

#define SOCKET_SLOTS 64
static native_socket_t sockets[SOCKET_SLOTS];
static qboolean used[SOCKET_SLOTS];
static int control_socket = -1, accept_socket = -1, broadcast_socket = -1;
static struct qsockaddr broadcast_address;
static qboolean transport_initialized;
static uint32_t local_address;
extern cvar_t hostname;

static native_socket_t Native(int handle)
{
    if (handle < 0 || handle >= SOCKET_SLOTS || !used[handle])
        Sys_Error("Invalid UDP socket handle %d", handle);
    return sockets[handle];
}
static void ToNative(const struct qsockaddr *from, struct sockaddr_in *to)
{
    memset(to, 0, sizeof(*to));
    to->sin_family = AF_INET;
    memcpy(&to->sin_port, from->sa_data, 2);
    memcpy(&to->sin_addr.s_addr, from->sa_data + 2, 4);
}
static void FromNative(const struct sockaddr_in *from, struct qsockaddr *to)
{
    memset(to, 0, sizeof(*to));
    to->sa_family = AF_INET;
    memcpy(to->sa_data, &from->sin_port, 2);
    memcpy(to->sa_data + 2, &from->sin_addr.s_addr, 4);
}
static qboolean WouldBlock(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
int UDP_OpenSocket(int port)
{
    native_socket_t sock;
    struct sockaddr_in address;
    int i;
    for (i = 0; i < SOCKET_SLOTS; ++i) if (!used[i]) break;
    if (i == SOCKET_SLOTS) return -1;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == BAD_SOCKET) return -1;
#ifdef _WIN32
    { u_long nonblocking = 1;
      if (ioctlsocket(sock, FIONBIO, &nonblocking)) { CloseSocket(sock); return -1; } }
#else
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) { CloseSocket(sock); return -1; }
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    { int option = COM_CheckParm("-ip");
      if (option) {
          if (option + 1 >= com_argc || inet_pton(AF_INET, com_argv[option + 1], &address.sin_addr) != 1)
              Sys_Error("-ip needs a numeric IPv4 address");
      } }
    address.sin_port = htons((unsigned short)port);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address))) {
        CloseSocket(sock); return -1;
    }
    sockets[i] = sock; used[i] = true;
    return i;
}
int UDP_CloseSocket(int handle)
{
    int result = CloseSocket(Native(handle));
    used[handle] = false;
    if (broadcast_socket == handle) broadcast_socket = -1;
    return result;
}
int UDP_Init(void)
{
    char name[256];
    struct sockaddr_in broadcast;
    if (COM_CheckParm("-noudp")) return -1;
#ifdef _WIN32
    { WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) return -1; }
#endif
    transport_initialized = true;
    local_address = htonl(UINT32_C(0x7f000001));
    { int option = COM_CheckParm("-ip");
      if (option && option + 1 < com_argc) {
          if (inet_pton(AF_INET, com_argv[option + 1], &local_address) != 1)
              Sys_Error("-ip needs a numeric IPv4 address");
      } else if (!gethostname(name, sizeof(name))) {
          struct addrinfo hints, *addresses;
          name[sizeof(name) - 1] = 0;
          memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET;
          if (!getaddrinfo(name, NULL, &hints, &addresses)) {
              local_address = ((struct sockaddr_in *)addresses->ai_addr)->sin_addr.s_addr;
              freeaddrinfo(addresses);
          }
      } }
    control_socket = UDP_OpenSocket(0);
    if (control_socket < 0) { UDP_Shutdown(); return -1; }
    if (!gethostname(name, sizeof(name))) {
        name[sizeof(name) - 1] = 0;
        if (!strcmp(hostname.string, "UNNAMED")) {
            name[15] = 0; Cvar_Set("hostname", name);
        }
    }
    memset(&broadcast, 0, sizeof(broadcast));
    broadcast.sin_family = AF_INET;
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast.sin_port = htons((unsigned short)net_hostport);
    FromNative(&broadcast, &broadcast_address);
    if (!inet_ntop(AF_INET, &local_address, my_tcpip_address, NET_NAMELEN)) {
        UDP_Shutdown(); return -1;
    }
    tcpipAvailable = true;
    return control_socket;
}
void UDP_Shutdown(void)
{
    int i;
    if (!transport_initialized) return;
    for (i = 0; i < SOCKET_SLOTS; ++i) if (used[i]) UDP_CloseSocket(i);
    control_socket = accept_socket = -1;
    transport_initialized = false;
    tcpipAvailable = false;
#ifdef _WIN32
    WSACleanup();
#endif
}
void UDP_Listen(qboolean state)
{
    if (state && accept_socket < 0) {
        accept_socket = UDP_OpenSocket(net_hostport);
        if (accept_socket < 0) Sys_Error("Cannot listen on UDP port %d", net_hostport);
        UDP_SetSocketPort(&broadcast_address, net_hostport);
    } else if (!state && accept_socket >= 0) {
        UDP_CloseSocket(accept_socket); accept_socket = -1;
    }
}
int UDP_Connect(int handle, struct qsockaddr *addr)
{ (void)handle; (void)addr; return 0; }
int UDP_CheckNewConnections(void)
{
    char value;
    int result;
    if (accept_socket < 0) return -1;
    result = (int)recv(Native(accept_socket), &value, 1, MSG_PEEK);
    return result >= 0 ? accept_socket : -1;
}
int UDP_Read(int handle, byte *buf, int len, struct qsockaddr *addr)
{
    struct sockaddr_in from;
    socket_length_t size = sizeof(from);
    int result = (int)recvfrom(Native(handle), (char *)buf, len, 0,
        (struct sockaddr *)&from, &size);
    if (result < 0) return WouldBlock() ? 0 : -1;
    FromNative(&from, addr);
    return result;
}
int UDP_Write(int handle, byte *buf, int len, struct qsockaddr *addr)
{
    struct sockaddr_in to;
    int result;
    ToNative(addr, &to);
    result = (int)sendto(Native(handle), (const char *)buf, len, 0,
        (struct sockaddr *)&to, sizeof(to));
    return result < 0 && WouldBlock() ? 0 : result;
}
int UDP_Broadcast(int handle, byte *buf, int len)
{
    int enable = 1;
    if (broadcast_socket != handle) {
        if (broadcast_socket >= 0) Sys_Error("Multiple broadcast sockets");
        if (setsockopt(Native(handle), SOL_SOCKET, SO_BROADCAST,
            (const char *)&enable, sizeof(enable))) return -1;
        broadcast_socket = handle;
    }
    return UDP_Write(handle, buf, len, &broadcast_address);
}
char *UDP_AddrToString(struct qsockaddr *addr)
{
    static char text[32];
    struct sockaddr_in native;
    char ip[INET_ADDRSTRLEN];
    ToNative(addr, &native);
    if (!inet_ntop(AF_INET, &native.sin_addr, ip, sizeof(ip))) return NULL;
    snprintf(text, sizeof(text), "%s:%u", ip, (unsigned)ntohs(native.sin_port));
    return text;
}
int UDP_StringToAddr(char *text, struct qsockaddr *addr)
{
    unsigned a, b, c, d, port;
    char extra;
    struct sockaddr_in native;
    if (sscanf(text, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &extra) != 5 ||
        a > 255 || b > 255 || c > 255 || d > 255 || port > 65535) return -1;
    memset(&native, 0, sizeof(native));
    native.sin_family = AF_INET;
    native.sin_port = htons((unsigned short)port);
    native.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    FromNative(&native, addr);
    return 0;
}
int UDP_GetSocketAddr(int handle, struct qsockaddr *addr)
{
    struct sockaddr_in native;
    socket_length_t size = sizeof(native);
    if (getsockname(Native(handle), (struct sockaddr *)&native, &size)) return -1;
    if (native.sin_addr.s_addr == htonl(INADDR_ANY))
        native.sin_addr.s_addr = local_address;
    FromNative(&native, addr);
    return 0;
}
int UDP_GetNameFromAddr(struct qsockaddr *addr, char *name)
{
    struct sockaddr_in native;
    ToNative(addr, &native);
    if (getnameinfo((struct sockaddr *)&native, sizeof(native), name, NET_NAMELEN,
        NULL, 0, NI_NAMEREQD)) {
        char *text = UDP_AddrToString(addr);
        if (!text) return -1;
        snprintf(name, NET_NAMELEN, "%s", text);
    }
    return 0;
}
/* Preserve Quake's local-subnet shorthand, using unsigned masks. */
static int PartialAddress(const char *name, struct qsockaddr *addr)
{
    const char *cursor = name;
    uint32_t value = 0, mask = UINT32_MAX;
    unsigned parts = 0, port = (unsigned)net_hostport;
    struct sockaddr_in native;
    if (*cursor == '.') ++cursor;
    for (;;) {
        unsigned number = 0, digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            number = number * 10 + (unsigned)(*cursor++ - '0');
            if (++digits > 3 || number > 255) return -1;
        }
        if (!digits || ++parts > 4) return -1;
        value = (value << 8) | number; mask <<= 8;
        if (*cursor != '.') break;
        ++cursor;
    }
    if (*cursor == ':') {
        char *end;
        unsigned long parsed = strtoul(++cursor, &end, 10);
        if (!*cursor || *end || parsed > 65535) return -1;
        port = (unsigned)parsed;
    } else if (*cursor) return -1;
    memset(&native, 0, sizeof(native)); native.sin_family = AF_INET;
    native.sin_port = htons((unsigned short)port);
    native.sin_addr.s_addr = (local_address & htonl(mask)) | htonl(value);
    FromNative(&native, addr);
    return 0;
}
int UDP_GetAddrFromName(char *name, struct qsockaddr *addr)
{
    struct addrinfo hints, *result;
    struct sockaddr_in native;
    char host[256], service[8], *colon;
    unsigned long port = (unsigned long)net_hostport;
    if ((name[0] >= '0' && name[0] <= '9') || name[0] == '.')
        return PartialAddress(name, addr);
    if (strlen(name) >= sizeof(host)) return -1;
    strcpy(host, name);
    colon = strrchr(host, ':');
    if (colon) {
        char *end;
        *colon++ = 0;
        port = strtoul(colon, &end, 10);
        if (!*colon || *end || port > 65535) return -1;
    }
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, service, &hints, &result)) return -1;
    memcpy(&native, result->ai_addr, sizeof(native));
    freeaddrinfo(result);
    FromNative(&native, addr);
    return 0;
}
int UDP_AddrCompare(struct qsockaddr *a, struct qsockaddr *b)
{
    if (a->sa_family != b->sa_family || memcmp(a->sa_data + 2, b->sa_data + 2, 4)) return -1;
    return memcmp(a->sa_data, b->sa_data, 2) ? 1 : 0;
}
int UDP_GetSocketPort(struct qsockaddr *addr)
{
    unsigned short port; memcpy(&port, addr->sa_data, 2); return ntohs(port);
}
int UDP_SetSocketPort(struct qsockaddr *addr, int port)
{
    unsigned short native = htons((unsigned short)port);
    memcpy(addr->sa_data, &native, 2); return 0;
}
