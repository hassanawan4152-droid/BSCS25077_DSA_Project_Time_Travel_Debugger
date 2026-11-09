// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
//#include <unistd.h>
#include <signal.h>
//#include <sys/socket.h>
//#include <netinet/in.h>
//#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
#ifdef _WIN32
const char* SERVER_PATH = "server.exe";
#else
const char* SERVER_PATH = "./server";      // the program we start when the server is down
#endif
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
const int CLIENT_WAIT_TIMEOUT_SEC = 30;
const int CLIENT_RECV_TIMEOUT_SEC = 30;
const int SERVER_REPLY_TIMEOUT_SEC = 30;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int count;


public:
    // Implement these functions:
    Queue() {
        count = 0;
        front = nullptr;
        rear = nullptr;
    }
    ~Queue() {
        Node* temp = front;
        while (temp != nullptr) {
            Node* t = temp;
            temp = temp->next;
            delete t;
        }
    }

    void enqueue(const T& val) {
        Node* n = new Node;
        n->data = val;
        n->next = nullptr;
        if (front == nullptr) {
            front = n;
            rear = n;
        }
        else {
            rear->next = n;
            rear = n;
        }
        count++;
    }

    T dequeue() {
        if (front == nullptr) {
            return T();
        }
        Node* n = front;
        T v = n->data;
        front = n->next;
        if (front == nullptr) {
            rear = nullptr;
        }
        delete n;
        count--;
        return v;
    }

    bool isEmpty() {
        return count == 0;
    }
    int size() {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    return netSendAll(sock, buf, n);
}
bool sendError(int sock, const string& message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    string out;
    out.push_back((char)1);
    out += message;
    uint32_t state = checksumStep(CHECKSUM_START, out.data(), out.size());
    char trailer[4];
    putChecksum(checksumFinish(state), trailer);
    out.append(trailer, 4);
    return sendAll(sock, out.data(), out.size());
}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int s = (int)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
    {
        cerr << "cannot create the listen socket" << endl;
        exit(1);
    }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        cerr << "bind failed on port " << port << " (is it already in use?)" << endl;
        exit(1);
    }
    if (listen(s, LISTEN_BACKLOG) != 0)
    {
        cerr << "listen failed on port " << port << endl;
        exit(1);
    }
    return s;
}
int connectToServer(const char* host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    return netConnect(host, port);
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string cmd;
#ifdef _WIN32
    cmd = string("start /B ") + SERVER_PATH + " " + to_string(port);
#else
    cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
#endif
    return system(cmd.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int s = connectToServer(serverHost.c_str(), serverPort);
    if (s >= 0)
        return s;

    if (!startServer(serverPort))
        return -1;

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        s = connectToServer(serverHost.c_str(), serverPort);
        if (s >= 0)
            return s;
        sleepMs(STARTUP_POLL_INTERVAL_MS);
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo& c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        closeSock(c.sock);
        return;
    }

    netSetRecvTimeout(c.sock, CLIENT_RECV_TIMEOUT_SEC);
    netSetRecvTimeout(serverSock, SERVER_REPLY_TIMEOUT_SEC);

    vector<char> buf(IO_BUFFER_SIZE);
    string released;
    string uploadError;
    TrailerChecker uploadChecker;

    while (true)
    {
        int r = (int)recv(c.sock, buf.data(), IO_BUFFER_SIZE, 0);
        if (r < 0)
        {
            uploadError = "upload failed or timed out";
            break;
        }
        if (r == 0)
            break;
        released.clear();
        uploadChecker.feed(buf.data(), (size_t)r, released);
        if (!released.empty() && !sendAll(serverSock, released.data(), released.size()))
        {
            uploadError = "backend server connection lost";
            break;
        }
    }

    if (uploadError.empty() && !uploadChecker.verify())
        uploadError = "upload checksum mismatch";
    if (uploadError.empty())
    {
        string trailer = uploadChecker.trailer();
        if (!sendAll(serverSock, trailer.data(), trailer.size()))
            uploadError = "backend server connection lost";
    }
    if (!uploadError.empty())
    {
        sendError(c.sock, uploadError);
        closeSock(serverSock);
        closeSock(c.sock);
        return;
    }

    shutdown(serverSock, SHUT_WR);

    TrailerChecker replyChecker;
    long long replyBytes = 0;
    size_t forwardedBytes = 0;
    bool replyBroken = false;
    bool clientGone = false;

    while (true)
    {
        int r = (int)recv(serverSock, buf.data(), IO_BUFFER_SIZE, 0);
        if (r < 0)
        {
            replyBroken = true;
            break;
        }
        if (r == 0)
            break;
        replyBytes += r;
        released.clear();
        replyChecker.feed(buf.data(), (size_t)r, released);
        if (!released.empty() && !sendAll(c.sock, released.data(), released.size()))
        {
            clientGone = true;
            break;
        }
        forwardedBytes += released.size();
    }

    if (!clientGone)
    {
        if (replyBytes == 0)
        {
            sendError(c.sock, "backend server did not reply");
        }
        else if (!replyBroken && replyChecker.verify())
        {
            string trailer = replyChecker.trailer();
            sendAll(c.sock, trailer.data(), trailer.size());
        }
        else if (forwardedBytes == 0)
        {
            sendError(c.sock, "checksum mismatch, response corrupted");
        }
    }

    closeSock(serverSock);
    closeSock(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag)
    {
        sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int cs = netAccept(listenSock, (struct sockaddr*)&clientAddr, &addrLen);
        if (cs < 0)
        {
            cerr << "accept failed, trying again" << endl;
            sleepMs(100);
            continue;
        }

        ClientInfo info;
        info.sock = cs;
        char ip[64];
        ip[0] = '\0';
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        info.address = ip;
        info.port = (int)ntohs(clientAddr.sin_port);
        info.arrivedAt = time(nullptr);

        bool full = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (pendingClients.size() >= MAX_PENDING_CLIENTS)
                full = true;
            else
                pendingClients.enqueue(info);
        }

        if (full)
        {
            sendError(cs, "server busy, try again later");
            closeSock(cs);
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag)
    {
        ClientInfo c;
        bool got = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty())
            {
                c = pendingClients.dequeue();
                got = true;
            }
        }

        if (!got)
        {
            sleepMs(50);
            continue;
        }

        if (time(nullptr) - c.arrivedAt > CLIENT_WAIT_TIMEOUT_SEC)
        {
            closeSock(c.sock);
            continue;
        }

        relayOneClient(c);
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char* argv[]) {

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    if (argc >= 2)
        serverHost = argv[1];
    if (argc >= 3)
        serverPort = atoi(argv[2]);
    if (serverPort < 1 || serverPort > 65535)
    {
        cerr << "invalid server port" << endl;
        return 1;
    }
    if (!initNetwork())
    {
        cerr << "cannot start the network" << endl;
        return 1;
    }

    int listenSock = createListenSocket(lbPort);
    cout << "load balancer listening on port " << lbPort << ", server at " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    closeSock(listenSock);
    cleanupNetwork();
    return 0;
}