#ifndef HELPER_H
#define HELPER_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#define CLOSE_SOCKET closesocket
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#define CLOSE_SOCKET close
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

inline bool initNetwork()
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

inline void cleanupNetwork()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

inline void closeSock(int sock)
{
    if (sock >= 0)
        CLOSE_SOCKET(sock);
}

inline void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline bool netSendAll(int sock, const char* buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        int r = (int)send(sock, buf + sent, (int)(n - sent), MSG_NOSIGNAL);
        if (r <= 0)
            return false;
        sent += (size_t)r;
    }
    return true;
}

inline int netConnect(const char* host, int port)
{
    int s = (int)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    const char* h = host;
    if (strcmp(host, "localhost") == 0)
        h = "127.0.0.1";
    if (inet_pton(AF_INET, h, &addr.sin_addr) != 1)
    {
        closeSock(s);
        return -1;
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        closeSock(s);
        return -1;
    }
    return s;
}

inline int netAccept(int listenSock, struct sockaddr* addr, socklen_t* len)
{
#ifdef _WIN32
    int s = (int)accept(listenSock, addr, len);
    if (s >= 0)
        SetHandleInformation((HANDLE)(SOCKET)s, HANDLE_FLAG_INHERIT, 0);
    return s;
#elif defined(__linux__)
    return accept4(listenSock, addr, len, SOCK_CLOEXEC);
#else
    int s = accept(listenSock, addr, len);
    if (s >= 0)
        fcntl(s, F_SETFD, FD_CLOEXEC);
    return s;
#endif
}

inline void netSetRecvTimeout(int sock, int seconds)
{
#ifdef _WIN32
    DWORD ms = (DWORD)(seconds * 1000);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const void*)&tv, sizeof(tv));
#endif
}

const uint32_t CHECKSUM_START = 0;

inline uint32_t checksumStep(uint32_t state, const char* data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        state += (unsigned char)data[i];
    return state;
}

inline uint32_t checksumFinish(uint32_t state)
{
    return state;
}

inline void putChecksum(uint32_t value, char out[4])
{
    out[0] = (char)((value >> 24) & 0xFF);
    out[1] = (char)((value >> 16) & 0xFF);
    out[2] = (char)((value >> 8) & 0xFF);
    out[3] = (char)(value & 0xFF);
}

inline uint32_t getChecksum(const char in[4])
{
    return ((uint32_t)(unsigned char)in[0] << 24) |
        ((uint32_t)(unsigned char)in[1] << 16) |
        ((uint32_t)(unsigned char)in[2] << 8) |
        ((uint32_t)(unsigned char)in[3]);
}

class TrailerChecker
{
    uint32_t state;
    std::string held;

public:
    TrailerChecker()
    {
        state = CHECKSUM_START;
    }

    void feed(const char* data, size_t n, std::string& released)
    {
        held.append(data, n);
        if (held.size() > 4)
        {
            size_t k = held.size() - 4;
            state = checksumStep(state, held.data(), k);
            released.append(held, 0, k);
            held.erase(0, k);
        }
    }

    std::string trailer() const
    {
        return held;
    }

    bool verify() const
    {
        if (held.size() != 4)
            return false;
        return getChecksum(held.data()) == checksumFinish(state);
    }
};

#endif