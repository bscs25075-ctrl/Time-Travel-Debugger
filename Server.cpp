// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
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
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0;
    }

    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH)
        {
            return;
        }

        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;

        top = newNode;
        count++;
    }

    T pop()
    {
        if (top == nullptr)
        {
            return T{};
        }

        Node* oldTop = top;
        T value = oldTop->data;

        top = oldTop->next;
        delete oldTop;

        count--;

        return value;
    }

    T& peek()
    {
        return top->data;
    }

    bool isEmpty()
    {
        return top == nullptr;
    }

    int32_t depth()
    {
        return count;
    }

    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t written = 0;
        Node* current = top;

        while (current != nullptr && written < maxLen)
        {
            out[written] = current->data;
            written++;

            current = current->next;
        }

        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head;
    TimelineNode* tail;
    int32_t stepCount;

public:
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }

    void record(Snapshot* s)
    {
        if (s == nullptr)
        {
            return;
        }

        TimelineNode* newNode = new TimelineNode;

        newNode->data = s;
        newNode->next = nullptr;
        newNode->prev = tail;

        if (head == nullptr)
        {
            head = newNode;
        }
        else
        {
            tail->next = newNode;
        }

        tail = newNode;
        stepCount++;
    }

    TimelineNode* begin()
    {
        return head;
    }

    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string line;

    while (getline(in, line))
    {
        if (!line.empty())
        {
            out = line;
            return true;
        }
    }

    return false;
}
string firstWord(const string& line)
{
    size_t start = line.find_first_not_of(" \t");

    if (start == string::npos)
    {
        return "";
    }

    size_t end = line.find_first_of(" \t", start);

    if (end == string::npos)
    {
        return line.substr(start);
    }

    return line.substr(start, end - start);
}
string secondWord(const string& line)
{
    size_t firstStart = line.find_first_not_of(" \t");

    if (firstStart == string::npos)
    {
        return "";
    }

    size_t firstEnd = line.find_first_of(" \t", firstStart);

    if (firstEnd == string::npos)
    {
        return "";
    }

    size_t secondStart = line.find_first_not_of(" \t", firstEnd);

    if (secondStart == string::npos)
    {
        return "";
    }

    size_t secondEnd = line.find_first_of(" \t", secondStart);

    if (secondEnd == string::npos)
    {
        return line.substr(secondStart);
    }

    return line.substr(secondStart, secondEnd - secondStart);
}
bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath);

    if (!in.is_open())
    {
        return false;
    }

    string line;
    bool insideFunction = false;
    int32_t functionCount = 0;

    while (readSourceLine(in, line))
    {
        string keyword = firstWord(line);

        if (keyword.empty())
        {
            continue;
        }

        if (keyword == "func")
        {
            // Nested functions are not allowed.
            if (insideFunction)
            {
                return false;
            }

            // FUNC must have a function name.
            string functionName = secondWord(line);

            if (functionName.empty())
            {
                return false;
            }

            insideFunction = true;
            functionCount++;
        }
        else if (keyword == "func_end")
        {
            // FUNC_END without a matching FUNC is invalid.
            if (!insideFunction)
            {
                return false;
            }

            insideFunction = false;
        }
    }

    // A function cannot be left open.
    if (insideFunction)
    {
        return false;
    }

    // Program must contain at least one function.
    if (functionCount == 0)
    {
        return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    if (f == nullptr)
    {
        return -1;
    }

    int64_t recordPosition = ftell(f);

    if (recordPosition < 0)
    {
        return -1;
    }

    uint32_t stringSize = static_cast<uint32_t>(text.size());

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&stringSize, sizeof(uint32_t), 1, f);

    if (stringSize > 0)
    {
        fwrite(text.data(), 1, stringSize, f);
    }

    return recordPosition;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    if (f == nullptr)
    {
        return -1;
    }

    int64_t offsetField;
    uint32_t stringSize;

    size_t readOffset = fread(
        &offsetField,
        sizeof(int64_t),
        1,
        f
    );

    if (readOffset != 1)
    {
        return -1;
    }

    size_t readSize = fread(
        &stringSize,
        sizeof(uint32_t),
        1,
        f
    );

    if (readSize != 1)
    {
        return -1;
    }

    outText.clear();

    if (stringSize > 0)
    {
        outText.resize(stringSize);

        size_t readString = fread(
            &outText[0],
            1,
            stringSize,
            f
        );

        if (readString != stringSize)
        {
            outText.clear();
            return -1;
        }
    }

    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;

    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream source(sourcePath);

    if (!source.is_open())
    {
        return -1;
    }

    FILE* resolveFile = fopen(resolveBinPath, "wb+");

    if (resolveFile == nullptr)
    {
        return -1;
    }

    string line;
    int64_t currentOffset = 0;
    int64_t mainOffset = -1;

    // -------------------------------------------------
    // First pass:
    // Write every source line into resolve.bin
    // and remember FUNC/CALL information.
    // -------------------------------------------------

    while (readSourceLine(source, line))
    {
        string keyword = firstWord(line);

        // Write this line as one resolve record.
        int64_t recordPosition =
            writeResolveRecord(resolveFile, currentOffset, line);

        if (recordPosition < 0)
        {
            fclose(resolveFile);
            return -1;
        }

        // ---------------------------------------------
        // FUNC
        // ---------------------------------------------

        if (keyword == "func")
        {
            string functionName = secondWord(line);

            if (functionName.empty())
            {
                fclose(resolveFile);
                return -1;
            }

            if (funcCount >= MAX_FUNCS)
            {
                fclose(resolveFile);
                return -1;
            }

            // Check duplicate function name.
            for (int32_t i = 0; i < funcCount; i++)
            {
                if (funcArray[i].funcName == functionName)
                {
                    fclose(resolveFile);
                    return -1;
                }
            }

            funcArray[funcCount].funcName = functionName;
            funcArray[funcCount].byteOffsetInResolveBin =
                recordPosition;

            if (functionName == "main")
            {
                mainOffset = recordPosition;
            }

            funcCount++;
        }

        // ---------------------------------------------
        // CALL
        // ---------------------------------------------

        else if (keyword == "call")
        {
            string targetFunction = secondWord(line);

            if (targetFunction.empty())
            {
                fclose(resolveFile);
                return -1;
            }

            if (patchCount >= MAX_PATCHES)
            {
                fclose(resolveFile);
                return -1;
            }

            /*
                Record layout:

                [8 bytes offset]
                [4 bytes string size]
                [string]

                The CALL target offset is stored in
                the 8-byte offset field.

                This field begins at recordPosition.
            */

            patches[patchCount].byteOffsetOfOffsetField =
                recordPosition;

            patches[patchCount].targetFuncName =
                targetFunction;

            patchCount++;
        }

        // Next record begins after:
        // 8 bytes offset
        // 4 bytes size
        // string bytes
        currentOffset +=
            sizeof(int64_t) +
            sizeof(uint32_t) +
            static_cast<int64_t>(line.size());
    }

    source.close();

    // -------------------------------------------------
    // main must exist.
    // -------------------------------------------------

    if (mainOffset < 0)
    {
        fclose(resolveFile);
        return -1;
    }

    // -------------------------------------------------
    // Second pass:
    // Resolve every CALL target.
    // -------------------------------------------------

    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = -1;

        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName ==
                patches[i].targetFuncName)
            {
                targetOffset =
                    funcArray[j].byteOffsetInResolveBin;

                break;
            }
        }

        // Undefined function.
        if (targetOffset < 0)
        {
            fclose(resolveFile);
            return -1;
        }

        // Go to the offset field of this CALL record.
        if (fseek(
            resolveFile,
            patches[i].byteOffsetOfOffsetField,
            SEEK_SET) != 0)
        {
            fclose(resolveFile);
            return -1;
        }

        // Patch the CALL target offset.
        if (fwrite(
            &targetOffset,
            sizeof(int64_t),
            1,
            resolveFile) != 1)
        {
            fclose(resolveFile);
            return -1;
        }
    }

    fclose(resolveFile);

    return mainOffset;
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
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    if (tokens == nullptr || maxTokens <= 0)
    {
        return 0;
    }

    int32_t tokenCount = 0;
    size_t position = 0;

    while (position < line.size())
    {
        // Skip spaces and tabs.
        while (position < line.size() &&
            (line[position] == ' ' || line[position] == '\t'))
        {
            position++;
        }

        // End of line.
        if (position >= line.size())
        {
            break;
        }

        // Find the end of this word.
        size_t end = position;

        while (end < line.size() &&
            line[end] != ' ' &&
            line[end] != '\t')
        {
            end++;
        }

        if (tokenCount >= maxTokens)
        {
            return -1;
        }

        string word = line.substr(position, end - position);

        // First word is always the instruction keyword.
        if (tokenCount == 0)
        {
            tokens[tokenCount].type = KEYWORD;
        }
        // Second word is the identifier.
        else if (tokenCount == 1)
        {
            tokens[tokenCount].type = IDENTIFIER;
        }
        // Remaining words are parameters/arguments.
        else
        {
            tokens[tokenCount].type = PARAM;
        }

        tokens[tokenCount].text = word;

        tokenCount++;
        position = end;
    }

    return tokenCount;
}Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}