#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; 
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; 
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  
const int32_t SOCKET_TIMEOUT_SEC = 5;     

string global_error;

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack(){ 
        top = nullptr;
        count = 0;
    }
    ~Stack() {
        while (top != nullptr) {
            Node* n = top;
            top = top->next;
            delete n;
        }
    }
    void push(const T& val){
        if (count >= MAX_STACK_DEPTH) {
            return;
        }
        Node* n = new Node;
        n->data = val;
        n->next = top;
        top = n;
        count++;
    }
    T pop(){
        if (top == nullptr) {
            return T();
        }
        Node* n = top;
        T val = n->data;
        top = n->next;
        delete n;
        count--;
        return val;
    }
    T& peek(){
        return top->data;
    }
    bool isEmpty(){
        return top==nullptr;
    }
    int32_t depth(){
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen){
        int32_t i = 0;
        Node* curr = top;
        while (curr != nullptr && i < maxLen)
        {
            out[i++] = curr->data;
            curr = curr->next;
        }
        return i;
    }
};

//Timeline: Doubly list of snapshots

struct Snapshot;
struct TimelineNode {
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline {
private:
    TimelineNode* head;
    TimelineNode* tail;
    int32_t stepcount;
public:
    Timeline() {
        head = tail = nullptr;
        stepcount = 0;
    }
    ~Timeline();
    void record(Snapshot* s) {
        TimelineNode* n = new TimelineNode;
        n->data = s;
        n->next = nullptr;
        n->prev = tail;
        if (tail != nullptr) {
            tail->next = n;
        }
        else {
            head = n;
        }
        tail = n;
        stepcount++;
    }
    TimelineNode* begin() {
        return head;
    }
    int32_t getStepCount() {
        return stepcount;
    }
};

// Core structs
struct Variable{
    string name;
    int32_t value;
};
struct Frame{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

Timeline::~Timeline() {
    while (head != nullptr) {
        TimelineNode* n = head;
        head = head->next;
        delete n->data;
        delete n;
    }
}

void writeHeader(FILE* f, const TTDBHeader& h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; 
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; 
    string targetFuncName;
};

// Helper functions

bool isSpaceChar(char c){
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

string trimString(const string& s){
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

string lowerCase(string s){
    for (size_t i = 0; i < s.size(); i++){
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = s[i] - 'A' + 'a';
    }
    return s;
}

string nthword(const string& line, int32_t n){
    size_t i = 0;
    size_t len = line.size();
    int32_t idx = 0;
    while (true){
        while (i < len && isSpaceChar(line[i]))
            i++;
        if (i >= len)
            return "";
        size_t start = i;
        while (i < len && !isSpaceChar(line[i]))
            i++;
        if (idx == n)
            return line.substr(start, i - start);
        idx++;
    }
}


// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out){
    string l;
    while (getline(in, l))
    {
        l = trimString(l);
        if (!l.empty())
        {
            out = l;
            return true;
        }
    }
    return false;
}
string firstWord(const string& line){
    return nthword(line,0);
}
string secondWord(const string& line){
    return nthword(line, 1);
}
bool validateProgram(const char* sourcePath){
    ifstream in(sourcePath, ios::binary);
    if (!in)
    {
        global_error = "cannot open source file";
        return false;
    }
    Stack<int32_t> funcStack;
    string line;
    int32_t lineNo = 0;
    while (readSourceLine(in, line))
    {
        lineNo++;
        string kw = lowerCase(firstWord(line));
        if (kw == "func")
        {
            if (!funcStack.isEmpty())
            {
                global_error = "nested function declaration at instruction " + to_string(lineNo);
                return false;
            }
            if (secondWord(line).empty())
            {
                global_error = "func without a name at instruction " + to_string(lineNo);
                return false;
            }
            funcStack.push(lineNo);
        }
        else if (kw == "func_end")
        {
            if (funcStack.isEmpty())
            {
                global_error = "func_end without matching func at instruction " + to_string(lineNo);
                return false;
            }
            funcStack.pop();
        }
    }
    if (!funcStack.isEmpty())
    {
        global_error = "func without matching func_end";
        return false;
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text){
    int64_t at_this = (int64_t)ftell(f);
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    if (size > 0)
        fwrite(text.data(), 1, size, f);
    return at_this;
}
int64_t readResolveRecord(FILE* f, string& outText){
    int64_t off;
    int32_t size;
    outText.clear();
    if (fread(&off, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return -1;
    if (size < 0 || size > 1000000)
        return -1;
    outText.resize(size);
    if (size > 0 && fread(&outText[0], 1, size, f) != (size_t)size)
    {
        outText.clear();
        return -1;
    }
    return off;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath){
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath, ios::binary);
    if (!in)
    {
        global_error = "cannot open source file";
        return -1;
    }
    FILE* f = fopen(resolveBinPath, "wb+");
    if (f == nullptr)
    {
        global_error = "cannot create resolve.bin";
        return -1;
    }

    string line;
    while (readSourceLine(in, line))
    {
        string kw = lowerCase(firstWord(line));
        int64_t pos = (int64_t)ftell(f);
        if (kw == "func")
        {
            string name = secondWord(line);
            for (int32_t i = 0; i < funcCount; i++)
            {
                if (funcArray[i].funcName == name)
                {
                    global_error = "function defined more than once: " + name;
                    fclose(f);
                    return -1;
                }
            }
            if (funcCount >= MAX_FUNCS)
            {
                global_error = "too many functions";
                fclose(f);
                return -1;
            }
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
            writeResolveRecord(f, pos, line);
        }
        else if (kw == "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                global_error = "too many call instructions";
                fclose(f);
                return -1;
            }
            string target = secondWord(line);
            if (target.empty())
            {
                global_error = "call without a function name";
                fclose(f);
                return -1;
            }
            int64_t at = writeResolveRecord(f, 0, line);
            patches[patchCount].byteOffsetOfOffsetField = at;
            patches[patchCount].targetFuncName = target;
            patchCount++;
        }
        else
        {
            writeResolveRecord(f, pos, line);
        }
    }

    for (int32_t p = 0; p < patchCount; p++)
    {
        int64_t targetOffset = -1;
        for (int32_t i = 0; i < funcCount; i++)
        {
            if (funcArray[i].funcName == patches[p].targetFuncName)
            {
                targetOffset = funcArray[i].byteOffsetInResolveBin;
                break;
            }
        }
        if (targetOffset < 0)
        {
            global_error = "call to undefined function: " + patches[p].targetFuncName;
            fclose(f);
            return -1;
        }
        fseek(f, (long)patches[p].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, f);
    }

    int64_t mainOffset = -1;
    for (int32_t i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
        {
            mainOffset = funcArray[i].byteOffsetInResolveBin;
            break;
        }
    }
    fclose(f);
    if (mainOffset < 0)
    {
        global_error = "main function not found";
        return -1;
    }
    return mainOffset;
}

void reportError(const string& msg){
    cerr << "ERROR: " << msg << endl;
    ofstream out("error.txt");
    out << "ERROR: " << msg << endl;
}



// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens) {
    int32_t n = 0;
    size_t i = 0;
    size_t len = line.size();
    while (true)
    {
        while (i < len && isSpaceChar(line[i]))
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && !isSpaceChar(line[i]))
            i++;
        if (n >= maxTokens)
            return -1;
        string word = line.substr(start, i - start);
        if (n == 0)
        {
            tokens[n].type = KEYWORD;
            tokens[n].text = lowerCase(word);
        }
        else if (n == 1)
        {
            tokens[n].type = IDENTIFIER;
            tokens[n].text = word;
        }
        else
        {
            tokens[n].type = PARAM;
            tokens[n].text = word;
        }
        n++;
    }
    return n;
}
bool parseNumber(const string& s, int32_t& out) {
    if (s.empty())
        return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+')
        i = 1;
    if (i >= s.size())
        return false;
    for (size_t j = i; j < s.size(); j++)
    {
        if (s[j] < '0' || s[j] > '9')
            return false;
    }
    out = (int32_t)strtol(s.c_str(), nullptr, 10);
    return true;
}
Variable* findVariable(Frame& fr, const string& name) {
    for (int32_t i = 0; i < fr.argc; i++)
    {
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    }
    for (int32_t i = 0; i < fr.localCount; i++)
    {
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    }
    return nullptr;
}
bool getOperand(Frame& fr, const string& text, int32_t& out, bool& isVar) {
    if (parseNumber(text, out))
    {
        isVar = false;
        return true;
    }
    Variable* v = findVariable(fr, text);
    if (v == nullptr)
        return false;
    out = v->value;
    isVar = true;
    return true;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack) {
    Snapshot* s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline) {
    static string refNames[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];

    FILE* f = fopen(resolveBinPath, "rb");
    if (f == nullptr)
    {
        global_error = "cannot open resolve.bin";
        return;
    }

    Stack<Frame> callStack;
    Token tk[MAX_TOKENS];
    Token ht[MAX_TOKENS];
    string text;

    fseek(f, (long)mainOffset, SEEK_SET);
    if (readResolveRecord(f, text) < 0)
    {
        global_error = "cannot read main function record";
        fclose(f);
        return;
    }
    int64_t pos = (int64_t)ftell(f);
    int32_t n = tokenizeLine(text, tk, MAX_TOKENS);
    if (n < 2 || tk[0].text != "func")
    {
        global_error = "invalid main function header";
        fclose(f);
        return;
    }

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = n - 2;
    for (int32_t i = 0; i < mainFrame.argc; i++)
    {
        mainFrame.argv[i].name = tk[2 + i].text;
        mainFrame.argv[i].value = 0;
    }
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    while (!callStack.isEmpty())
    {
        fseek(f, (long)pos, SEEK_SET);
        int64_t target = readResolveRecord(f, text);
        if (target < 0)
        {
            global_error = "program ended without func_end";
            break;
        }
        int64_t next = (int64_t)ftell(f);

        n = tokenizeLine(text, tk, MAX_TOKENS);
        if (n < 1)
        {
            global_error = "invalid instruction (too many parameters): " + text;
            break;
        }

        Frame& curr = callStack.peek();
        string kw = tk[0].text;

        if (kw == "func_end")
        {
            Frame done = callStack.pop();
            if (!callStack.isEmpty())
            {
                Frame& caller = callStack.peek();
                int32_t idx = callStack.depth();
                for (int32_t i = 0; i < done.argc; i++)
                {
                    if (!refNames[idx][i].empty())
                    {
                        Variable* v = findVariable(caller, refNames[idx][i]);
                        if (v != nullptr)
                            v->value = done.argv[i].value;
                    }
                }
                pos = done.returnLine;
            }
            else
            {
                pos = next;
            }
        }
        else if (kw == "set")
        {
            if (n < 3)
            {
                global_error = "set needs a variable and a value: " + text;
                break;
            }
            int32_t val;
            bool isVar;
            if (!getOperand(curr, tk[2].text, val, isVar))
            {
                global_error = "undefined variable " + tk[2].text + " in " + curr.func_name;
                break;
            }
            Variable* v = findVariable(curr, tk[1].text);
            if (v == nullptr)
            {
                if (curr.localCount >= MAX_VARS_PER_FRAME)
                {
                    global_error = "too many local variables in " + curr.func_name;
                    break;
                }
                v = &curr.locals[curr.localCount++];
                v->name = tk[1].text;
            }
            v->value = val;
            pos = next;
        }
        else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
        {
            if (n < 3)
            {
                global_error = kw + " needs two operands: " + text;
                break;
            }
            Variable* v = findVariable(curr, tk[1].text);
            if (v == nullptr)
            {
                global_error = "undefined variable " + tk[1].text + " in " + curr.func_name;
                break;
            }
            int32_t operand;
            bool isVar;
            if (!getOperand(curr, tk[2].text, operand, isVar))
            {
                global_error = "undefined variable " + tk[2].text + " in " + curr.func_name;
                break;
            }
            int64_t a = v->value;
            int64_t b = operand;
            int64_t r = 0;
            if (kw == "add")
                r = a + b;
            else if (kw == "sub")
                r = a - b;
            else if (kw == "mul")
                r = a * b;
            else
            {
                if (b == 0)
                {
                    global_error = "division by zero in " + curr.func_name;
                    break;
                }
                r = a / b;
            }
            v->value = (int32_t)r;
            pos = next;
        }
        else if (kw == "call")
        {
            if (n < 2)
            {
                global_error = "call without a function name";
                break;
            }
            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                global_error = "stack overflow: call depth exceeded " + to_string(MAX_STACK_DEPTH);
                break;
            }
            int32_t m = n - 2;

            fseek(f, (long)target, SEEK_SET);
            string hdr;
            if (readResolveRecord(f, hdr) < 0)
            {
                global_error = "cannot read function header for " + tk[1].text;
                break;
            }
            int64_t calleeNext = (int64_t)ftell(f);
            int32_t hn = tokenizeLine(hdr, ht, MAX_TOKENS);
            if (hn < 2 || ht[0].text != "func")
            {
                global_error = "call target is not a function: " + tk[1].text;
                break;
            }
            if (hn - 2 != m)
            {
                global_error = "argument count mismatch calling " + tk[1].text;
                break;
            }

            Frame nf;
            nf.func_name = ht[1].text;
            nf.argc = m;
            nf.returnLine = (int32_t)next;
            nf.localCount = 0;
            bool failed = false;
            int32_t slot = callStack.depth();
            for (int32_t i = 0; i < m; i++)
            {
                int32_t val;
                bool isVar;
                if (!getOperand(curr, tk[2 + i].text, val, isVar))
                {
                    global_error = "undefined variable " + tk[2 + i].text + " in " + curr.func_name;
                    failed = true;
                    break;
                }
                nf.argv[i].name = ht[2 + i].text;
                nf.argv[i].value = val;
                if (isVar)
                    refNames[slot][i] = tk[2 + i].text;
                else
                    refNames[slot][i] = "";
            }
            if (failed)
                break;

            callStack.push(nf);
            pos = calleeNext;
        }
        else if (kw == "func")
        {
            global_error = "unexpected func inside execution";
            break;
        }
        else
        {
            global_error = "unknown instruction: " + kw;
            break;
        }

        timeline.record(buildSnapshot(callStack));
    }
    fclose(f);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeInt32(FILE* f, int32_t v) {
    fwrite(&v, sizeof(int32_t), 1, f);
}
void writeString(FILE* f, const string& s) {
    int32_t len = (int32_t)s.size();
    fwrite(&len, sizeof(int32_t), 1, f);
    if (len > 0)
        fwrite(s.data(), 1, len, f);
}
void writeVariable(FILE* f, const Variable& v) {
    writeString(f, v.name);
    writeInt32(f, v.value);
}
void writeFrame(FILE* f, const Frame& fr) {
    writeString(f, fr.func_name);
    writeInt32(f, fr.argc);
    for (int32_t i = 0; i < fr.argc; i++)
        writeVariable(f, fr.argv[i]);
    writeInt32(f, fr.returnLine);
    writeInt32(f, fr.localCount);
    for (int32_t i = 0; i < fr.localCount; i++)
        writeVariable(f, fr.locals[i]);
}
void writeSnapshot(FILE* f, const Snapshot& s) {
    writeInt32(f, s.stackDepth);
    for (int32_t i = 0; i < s.stackDepth; i++)
        writeFrame(f, s.callStack[i]);
}
void writeTdbg(Timeline& timeline, const char* tdbgPath) {
    FILE* f = fopen(tdbgPath, "wb");
    if (f == nullptr)
    {
        global_error = "cannot create session.tdbg";
        return;
    }

    TTDBHeader h;
    memcpy(h.magic, "TTDB", 4);
    h.version = 1;
    h.stepCount = timeline.getStepCount();
    h.indexOffset = 0;
    writeHeader(f, h);

    int32_t steps = timeline.getStepCount();
    int64_t* index = new int64_t[steps > 0 ? steps : 1];

    int32_t i = 0;
    TimelineNode* node = timeline.begin();
    while (node != nullptr)
    {
        index[i++] = (int64_t)ftell(f);
        writeSnapshot(f, *node->data);
        node = node->next;
    }

    h.indexOffset = (int64_t)ftell(f);
    if (steps > 0)
        fwrite(index, sizeof(int64_t), steps, f);

    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    delete[] index;
    fclose(f);
}



// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        reportError(global_error);
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    if (mainOffset < 0)
    {
        reportError(global_error);
        return 1;
    }

    cout << "resolve.bin created, main starts at offset " << mainOffset << endl;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);
    if (!global_error.empty())
    {
        reportError(global_error);
        return 1;
    }
 
    writeTdbg(timeline, "session.tdbg");
    if (!global_error.empty())
    {
        reportError(global_error);
        return 1;
    }

    return 0;
}