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

    return 0;
}