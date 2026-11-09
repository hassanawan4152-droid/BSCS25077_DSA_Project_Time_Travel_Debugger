// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
//#include <unistd.h>
#include <signal.h>
#include <vector>
//#include <sys/socket.h>
#include <sstream>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const char* TDBG_FILE_ALT = "ttdb_last_alt.tdbg";
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
const int CLIENT_TIMEOUT_SEC = 30;
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    //ssize_t file_index;
    FILE* file;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
    bool usingAlt;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t got = 0;
    while (got < n)
    {
        int r = (int)recv(sock, buf + got, (int)(n - got), 0);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}
bool sendAll(int sock, const char* buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
    return netSendAll(sock, buf, n);
}

int connectToHost(const char* host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    return netConnect(host, port);
}
bool uploadSource(int sock, const char* sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE* f = fopen(sourcePath, "rb");
    if (f == nullptr)
        return false;

    uint32_t state = CHECKSUM_START;
    char status = 0;
    if (!sendAll(sock, &status, 1))
    {
        fclose(f);
        return false;
    }
    state = checksumStep(state, &status, 1);

    vector<char> buf(IO_BUFFER_SIZE);
    while (true)
    {
        size_t got = fread(buf.data(), 1, buf.size(), f);
        if (got == 0)
            break;
        if (!sendAll(sock, buf.data(), got))
        {
            fclose(f);
            return false;
        }
        state = checksumStep(state, buf.data(), got);
    }
    fclose(f);

    char trailer[4];
    putChecksum(checksumFinish(state), trailer);
    if (!sendAll(sock, trailer, 4))
        return false;

    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    TrailerChecker checker;
    vector<char> buf(IO_BUFFER_SIZE);
    string released;
    string errorText;
    bool gotStatus = false;
    char status = 0;
    FILE* out = nullptr;
    long long totalReceived = 0;

    while (true)
    {
        int r = (int)recv(sock, buf.data(), IO_BUFFER_SIZE, 0);
        if (r < 0)
        {
            if (out != nullptr)
                fclose(out);
            remove(outTdbgPath);
            outError = "connection lost or timed out while waiting for the reply";
            return false;
        }
        if (r == 0)
            break;
        totalReceived += r;

        released.clear();
        checker.feed(buf.data(), (size_t)r, released);

        size_t offset = 0;
        if (!gotStatus && !released.empty())
        {
            status = released[0];
            gotStatus = true;
            offset = 1;
            if (status == 0)
            {
                out = fopen(outTdbgPath, "wb");
                if (out == nullptr)
                {
                    outError = "cannot create the .tdbg file on disk";
                    return false;
                }
            }
            else if (status != 1)
            {
                outError = "unknown reply from the server";
                return false;
            }
        }
        if (gotStatus && released.size() > offset)
        {
            if (status == 0)
            {
                size_t want = released.size() - offset;
                if (fwrite(released.data() + offset, 1, want, out) != want)
                {
                    fclose(out);
                    remove(outTdbgPath);
                    outError = "cannot write the .tdbg file on disk";
                    return false;
                }
            }
            else
            {
                errorText.append(released, offset, string::npos);
            }
        }
    }

    if (out != nullptr)
    {
        fclose(out);
        out = nullptr;
    }

    if (totalReceived == 0)
    {
        outError = "no reply from the load balancer (connection closed)";
        return false;
    }
    if (!gotStatus || !checker.verify())
    {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 1)
    {
        remove(outTdbgPath);
        outError = errorText.empty() ? string("unknown error from the server") : errorText;
        return false;
    }
    return true;
}
bool requestDebugFile(const char* host, int port, const char* sourcePath, const char* outTdbgPath, string& outError)
{
    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer";
        return false;
    }
    netSetRecvTimeout(sock, CLIENT_TIMEOUT_SEC);
    if (!uploadSource(sock, sourcePath))
    {
        closeSock(sock);
        outError = "could not read or upload the source file";
        return false;
    }
    bool ok = receiveResponse(sock, outTdbgPath, outError);
    closeSock(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readInt32Field(FILE* f, int32_t& v)
{
    return fread(&v, sizeof(int32_t), 1, f) == 1;
}
bool readStringField(FILE* f, string& out)
{
    int32_t len = 0;
    if (!readInt32Field(f, len))
        return false;
    if (len < 0 || len > 1000000)
        return false;
    out.clear();
    if (len > 0)
    {
        out.resize((size_t)len);
        if (fread(&out[0], 1, (size_t)len, f) != (size_t)len)
            return false;
    }
    return true;
}
bool readVariableField(FILE* f, Variable& v)
{
    if (!readStringField(f, v.name))
        return false;
    return readInt32Field(f, v.value);
}
bool readFrameField(FILE* f, Frame& fr)
{
    if (!readStringField(f, fr.func_name))
        return false;
    if (!readInt32Field(f, fr.argc))
        return false;
    if (fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.argc; i++)
    {
        if (!readVariableField(f, fr.argv[i]))
            return false;
    }
    if (!readInt32Field(f, fr.returnLine))
        return false;
    if (!readInt32Field(f, fr.localCount))
        return false;
    if (fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        return false;
    for (int32_t i = 0; i < fr.localCount; i++)
    {
        if (!readVariableField(f, fr.locals[i]))
            return false;
    }
    return true;
}

bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    if (r.file == nullptr)
        return false;

    memset(&r.header, 0, sizeof(TTDBHeader));
    bool ok = true;
    if (fread(r.header.magic, 1, 4, r.file) != 4)
        ok = false;
    if (ok && fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1)
        ok = false;
    if (ok && fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1)
        ok = false;
    if (ok && fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1)
        ok = false;
    if (ok && memcmp(r.header.magic, "TTDB", 4) != 0)
        ok = false;
    if (ok && r.header.stepCount <= 0)
        ok = false;

    long fileSize = 0;
    if (ok)
    {
        fseek(r.file, 0, SEEK_END);
        fileSize = ftell(r.file);
        int64_t expected = r.header.indexOffset + (int64_t)r.header.stepCount * 8;
        if (r.header.indexOffset < 20 || expected != (int64_t)fileSize)
            ok = false;
    }
    if (ok)
    {
        fseek(r.file, (long)r.header.indexOffset, SEEK_SET);
        r.index.resize((size_t)r.header.stepCount);
        if (fread(&r.index[0], sizeof(int64_t), (size_t)r.header.stepCount, r.file) != (size_t)r.header.stepCount)
            ok = false;
    }
    if (ok)
    {
        for (size_t i = 0; i < r.index.size(); i++)
        {
            if (r.index[i] < 20 || r.index[i] >= r.header.indexOffset)
            {
                ok = false;
                break;
            }
        }
    }

    if (!ok)
    {
        fclose(r.file);
        r.file = nullptr;
        vector<int64_t>().swap(r.index);
        return false;
    }
    return true;
}
void closeTdbg(TdbgReader& r)
{
    // Free r.index and close the file.
    vector<int64_t>().swap(r.index);
    if (r.file != nullptr)
    {
        fclose(r.file);
        r.file = nullptr;
    }
}
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (r.file == nullptr)
        return nullptr;
    if (step < 0 || step >= r.header.stepCount || (size_t)step >= r.index.size())
        return nullptr;

    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0)
        return nullptr;

    Snapshot* snap = new Snapshot;
    snap->stackDepth = 0;
    if (!readInt32Field(r.file, snap->stackDepth) || snap->stackDepth < 0 || snap->stackDepth > MAX_STACK_DEPTH)
    {
        delete snap;
        return nullptr;
    }
    for (int32_t i = 0; i < snap->stackDepth; i++)
    {
        if (!readFrameField(r.file, snap->callStack[i]))
        {
            delete snap;
            return nullptr;
        }
    }
    return snap;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession& s)
{
    s.active_flag = 0;
    s.currentStep = 0;
    s.usingAlt = false;
    s.reader.file = nullptr;
    memset(&s.reader.header, 0, sizeof(TTDBHeader));
    s.reader.index.clear();
}
void closeSession(DebugSession& s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot* snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no active session, use: start <source file>" << endl;
        return false;
    }
    int64_t target = s.currentStep + (int64_t)delta;
    if (target < 0)
    {
        cout << "cannot go back that far, you are at step " << (s.currentStep + 1) << " and the first step is 1" << endl;
        return false;
    }
    if (target >= s.reader.header.stepCount)
    {
        cout << "cannot go forward that far, you are at step " << (s.currentStep + 1)
            << " and the last step is " << s.reader.header.stepCount << endl;
        return false;
    }
    int64_t old = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = old;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    const char* path = TDBG_FILE;
    if (s.active_flag && !s.usingAlt)
        path = TDBG_FILE_ALT;

    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), path, err))
    {
        remove(path);
        cout << "error: " << err << endl;
        return false;
    }

    TdbgReader fresh;
    fresh.file = nullptr;
    if (!openTdbg(path, fresh))
    {
        remove(path);
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = fresh;
    s.usingAlt = (path == TDBG_FILE_ALT);
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession& s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession& s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession& s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession& s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << endl;
    cout << "==== Step " << step << " of " << totalSteps << " ====" << endl;
    cout << "Call stack depth: " << s.stackDepth << " (#0 is the running function)" << endl;
    if (s.stackDepth == 0)
        cout << "  (program finished, the call stack is empty)" << endl;
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame& fr = s.callStack[i];
        cout << "  #" << i << " " << fr.func_name << endl;
        cout << "      args  : ";
        if (fr.argc == 0)
            cout << "(none)";
        for (int32_t k = 0; k < fr.argc; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << fr.argv[k].name << " = " << fr.argv[k].value;
        }
        cout << endl;
        cout << "      locals: ";
        if (fr.localCount == 0)
            cout << "(none)";
        for (int32_t k = 0; k < fr.localCount; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << fr.locals[k].name << " = " << fr.locals[k].value;
        }
        cout << endl;
    }
    cout << endl;
}
void printHelp()
{
    // Print the commands
    cout << "Commands:" << endl;
    cout << "  start <source file>     send the source to the server and show step 1" << endl;
    cout << "  next                    go one step forward" << endl;
    cout << "  prv                     go one step back" << endl;
    cout << "  jmp next <n>            go n steps forward" << endl;
    cout << "  jmp prv <n>             go n steps back" << endl;
    cout << "  help                    show this list" << endl;
    cout << "  quit | exit             leave the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool parsePositive(const string& text, int32_t& out)
{
    if (text.empty() || text.size() > 10)
        return false;
    long long v = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;
        v = v * 10 + (text[i] - '0');
    }
    if (v < 1 || v > MAX_JUMP)
        return false;
    out = (int32_t)v;
    return true;
}

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> words;
    istringstream in(line);
    string w;
    while (in >> w)
        words.push_back(w);

    if (words.empty())
        return true;
    if (words[0] == PROGRAM_NAME)
        words.erase(words.begin());

    if (!words.empty())
    {
        const string& cmd = words[0];
        if (cmd == "start" && words.size() == 2)
            return cmdStart(s, words[1]);
        if (cmd == "next" && words.size() == 1)
            return cmdNext(s);
        if (cmd == "prv" && words.size() == 1)
            return cmdPrv(s);
        if (cmd == "jmp" && words.size() == 3)
        {
            int32_t n = 0;
            if ((words[1] == "next" || words[1] == "prv") && parsePositive(words[2], n))
            {
                if (words[1] == "next")
                    return cmdJmpNext(s, n);
                return cmdJmpPrv(s, n);
            }
        }
        if (cmd == "help" && words.size() == 1)
        {
            printHelp();
            return true;
        }
        if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
        {
            quitRequested = true;
            return true;
        }
    }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char* argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 
    if (argc >= 2)
        LB_HOST = argv[1];
    if (argc >= 3)
    {
        LB_PORT = atoi(argv[2]);
        if (LB_PORT < 1 || LB_PORT > 65535)
        {
            cout << "invalid load balancer port" << endl;
            return 1;
        }
    }
    if (!initNetwork())
    {
        cout << "cannot start the network" << endl;
        return 1;
    }

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger (load balancer " << LB_HOST << ":" << LB_PORT << ")" << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (true)
    {
        if (quitRequested)
            break;
        cout << PROMPT << flush;
        if (!getline(cin, line))
            break;
        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    cleanupNetwork();
    return 0;
}