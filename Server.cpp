#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;
// ======================= TIME-TRAVEL DEBUGGER - SERVER =======================
//
// PHASE 01 PIPELINE
//
// Stage 0 : source.bin is assumed to already be received from the client.
//
// Pass 0x0 : Validate FUNC / FUNC_END structure.
// Pass 0x1 : Resolve source.bin -> resolve.bin
//            [offset(8B)][string_size(4B)][string]
//            and patch CALL target offsets.
//
// Pass 0x2 : Execute resolve.bin line-by-line.
//            Maintain Call Stack.
//            Create Snapshot after every executed instruction.
//
// Pass 0x3 : Serialize Timeline -> session.tdbg
//            Header + Snapshot Stream + Dense Index
//
// ============================================================================

#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <cstring>

using namespace std;

// ============================================================================
// CONSTANTS
// ============================================================================

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;

const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;


// ============================================================================
// CORE DATA STRUCTURES
// ============================================================================

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


// ============================================================================
// STACK
// ============================================================================

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

    Stack()
    {
        top = nullptr;
        count = 0;
    }

    ~Stack()
    {
        while (!isEmpty())
        {
            pop();
        }
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
        if (out == nullptr || maxLen <= 0)
        {
            return 0;
        }

        int32_t written = 0;

        Node* current = top;

        // Top -> bottom
        while (current != nullptr && written < maxLen)
        {
            out[written] = current->data;

            written++;

            current = current->next;
        }

        return written;
    }
};


// ============================================================================
// TIMELINE
// ============================================================================

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

    ~Timeline()
    {
        TimelineNode* current = head;

        while (current != nullptr)
        {
            TimelineNode* next = current->next;

            delete current->data;
            delete current;

            current = next;
        }

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


// ============================================================================
// TTDB HEADER
// ============================================================================

struct TTDBHeader
{
    char magic[4];          // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

void writeHeader(FILE* f, const TTDBHeader& h)
{
    if (f == nullptr)
    {
        return;
    }

    fwrite(h.magic, 1, 4, f);

    fwrite(
        &h.version,
        sizeof(int32_t),
        1,
        f
    );

    fwrite(
        &h.stepCount,
        sizeof(int32_t),
        1,
        f
    );

    fwrite(
        &h.indexOffset,
        sizeof(int64_t),
        1,
        f
    );
}


// ============================================================================
// RESOLVE BOOKKEEPING
// ============================================================================

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


// ============================================================================
// COMMON PARSING HELPERS
// ============================================================================

bool readSourceLine(ifstream& in, string& out)
{
    string line;
    while (getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
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
    size_t start =
        line.find_first_not_of(" \t");

    if (start == string::npos)
    {
        return "";
    }

    size_t end =
        line.find_first_of(" \t", start);

    if (end == string::npos)
    {
        return line.substr(start);
    }

    return line.substr(
        start,
        end - start
    );
}


string secondWord(const string& line)
{
    size_t firstStart =
        line.find_first_not_of(" \t");

    if (firstStart == string::npos)
    {
        return "";
    }

    size_t firstEnd =
        line.find_first_of(
            " \t",
            firstStart
        );

    if (firstEnd == string::npos)
    {
        return "";
    }

    size_t secondStart =
        line.find_first_not_of(
            " \t",
            firstEnd
        );

    if (secondStart == string::npos)
    {
        return "";
    }

    size_t secondEnd =
        line.find_first_of(
            " \t",
            secondStart
        );

    if (secondEnd == string::npos)
    {
        return line.substr(secondStart);
    }

    return line.substr(
        secondStart,
        secondEnd - secondStart
    );
}


// ============================================================================
// PASS 0x0 : VALIDATION
// ============================================================================

bool validateProgram(const char* sourcePath)
{
    // Check source file size.
    ifstream sizeCheck(
        sourcePath,
        ios::binary | ios::ate
    );

    if (!sizeCheck.is_open())
    {
        return false;
    }

    streamoff sourceSize =
        sizeCheck.tellg();

    sizeCheck.close();

    if (sourceSize < 0)
    {
        return false;
    }

    if (
        static_cast<uint64_t>(sourceSize)
    > MAX_SOURCE_BYTES
        )
    {
        return false;
    }

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

        // --------------------------------------------------------
        // FUNC
        // --------------------------------------------------------

        if (keyword == "func")
        {
            // Nested functions are not allowed.
            if (insideFunction)
            {
                return false;
            }

            string functionName =
                secondWord(line);

            // FUNC must have a name.
            if (functionName.empty())
            {
                return false;
            }

            insideFunction = true;

            functionCount++;
        }

        // --------------------------------------------------------
        // FUNC_END
        // --------------------------------------------------------

        else if (keyword == "func_end")
        {
            // func_end without func.
            if (!insideFunction)
            {
                return false;
            }

            insideFunction = false;
        }
    }

    in.close();

    // Function left open.
    if (insideFunction)
    {
        return false;
    }

    // At least one function is required.
    if (functionCount == 0)
    {
        return false;
    }

    return true;
}


// ============================================================================
// PASS 0x1 : RESOLVE RECORD I/O
// ============================================================================

int64_t writeResolveRecord(
    FILE* f,
    int64_t offsetField,
    const string& text
)
{
    if (f == nullptr)
    {
        return -1;
    }

    int64_t recordPosition =
        ftell(f);

    if (recordPosition < 0)
    {
        return -1;
    }

    uint32_t stringSize =
        static_cast<uint32_t>(
            text.size()
            );

    // [offset : 8 bytes]
    if (
        fwrite(
            &offsetField,
            sizeof(int64_t),
            1,
            f
        ) != 1
        )
    {
        return -1;
    }

    // [string_size : 4 bytes]
    if (
        fwrite(
            &stringSize,
            sizeof(uint32_t),
            1,
            f
        ) != 1
        )
    {
        return -1;
    }

    // [string]
    if (stringSize > 0)
    {
        if (
            fwrite(
                text.data(),
                1,
                stringSize,
                f
            ) != stringSize
            )
        {
            return -1;
        }
    }

    return recordPosition;
}


int64_t readResolveRecord(
    FILE* f,
    string& outText
)
{
    if (f == nullptr)
    {
        return -1;
    }

    int64_t offsetField;

    uint32_t stringSize;

    // Read offset.
    if (
        fread(
            &offsetField,
            sizeof(int64_t),
            1,
            f
        ) != 1
        )
    {
        return -1;
    }

    // Read string size.
    if (
        fread(
            &stringSize,
            sizeof(uint32_t),
            1,
            f
        ) != 1
        )
    {
        return -1;
    }

    outText.clear();

    if (stringSize > 0)
    {
        outText.resize(stringSize);

        if (
            fread(
                &outText[0],
                1,
                stringSize,
                f
            ) != stringSize
            )
        {
            outText.clear();

            return -1;
        }
    }

    return offsetField;
}


// ============================================================================
// PASS 0x1 : RESOLVE PROGRAM
// ============================================================================

int64_t resolveProgram(
    const char* sourcePath,
    const char* resolveBinPath
)
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

    FILE* resolveFile =
        fopen(
            resolveBinPath,
            "wb+"
        );

    if (resolveFile == nullptr)
    {
        source.close();

        return -1;
    }

    string line;

    int64_t currentOffset = 0;

    int64_t mainOffset = -1;

    // --------------------------------------------------------
    // First pass:
    //
    // Write every source line as:
    //
    // [offset(8B)][size(4B)][string]
    //
    // --------------------------------------------------------

    while (readSourceLine(source, line))
    {
        string keyword =
            firstWord(line);

        int64_t recordPosition =
            writeResolveRecord(
                resolveFile,
                currentOffset,
                line
            );

        if (recordPosition < 0)
        {
            source.close();
            fclose(resolveFile);

            return -1;
        }

        // ----------------------------------------------------
        // FUNC
        // ----------------------------------------------------

        if (keyword == "func")
        {
            string functionName =
                secondWord(line);

            if (functionName.empty())
            {
                source.close();
                fclose(resolveFile);

                return -1;
            }

            if (funcCount >= MAX_FUNCS)
            {
                source.close();
                fclose(resolveFile);

                return -1;
            }

            // Reject duplicate function names.
            for (
                int32_t i = 0;
                i < funcCount;
                i++
                )
            {
                if (
                    funcArray[i].funcName
                    == functionName
                    )
                {
                    source.close();
                    fclose(resolveFile);

                    return -1;
                }
            }

            funcArray[funcCount].funcName =
                functionName;

            funcArray[funcCount]
                .byteOffsetInResolveBin =
                recordPosition;

            if (functionName == "main")
            {
                mainOffset =
                    recordPosition;
            }

            funcCount++;
        }

        // ----------------------------------------------------
        // CALL
        // ----------------------------------------------------

        else if (keyword == "call")
        {
            string targetFunction =
                secondWord(line);

            if (targetFunction.empty())
            {
                source.close();
                fclose(resolveFile);

                return -1;
            }

            if (patchCount >= MAX_PATCHES)
            {
                source.close();
                fclose(resolveFile);

                return -1;
            }

            // CALL target is stored in the first
            // 8-byte field of this record.
            patches[patchCount]
                .byteOffsetOfOffsetField =
                recordPosition;

            patches[patchCount]
                .targetFuncName =
                targetFunction;

            patchCount++;
        }

        // Next record starts at:
        //
        // offset + 8 + 4 + string_size
        //
        currentOffset +=
            static_cast<int64_t>(
                sizeof(int64_t)
                )
            +
            static_cast<int64_t>(
                sizeof(uint32_t)
                )
            +
            static_cast<int64_t>(
                line.size()
                );
    }

    source.close();

    // main is mandatory.
    if (mainOffset < 0)
    {
        fclose(resolveFile);

        return -1;
    }

    // --------------------------------------------------------
    // Second pass:
    // Patch every CALL target.
    // --------------------------------------------------------

    for (
        int32_t i = 0;
        i < patchCount;
        i++
        )
    {
        int64_t targetOffset = -1;

        for (
            int32_t j = 0;
            j < funcCount;
            j++
            )
        {
            if (
                funcArray[j].funcName
                ==
                patches[i].targetFuncName
                )
            {
                targetOffset =
                    funcArray[j]
                    .byteOffsetInResolveBin;

                break;
            }
        }

        // Undefined function.
        if (targetOffset < 0)
        {
            fclose(resolveFile);

            return -1;
        }

        // Seek to CALL's offset field.
        if (
            fseek(
                resolveFile,
                patches[i]
                .byteOffsetOfOffsetField,
                SEEK_SET
            ) != 0
            )
        {
            fclose(resolveFile);

            return -1;
        }

        // Replace target function name logically
        // by its byte offset.
        if (
            fwrite(
                &targetOffset,
                sizeof(int64_t),
                1,
                resolveFile
            ) != 1
            )
        {
            fclose(resolveFile);

            return -1;
        }
    }

    fclose(resolveFile);

    return mainOffset;
}


// ============================================================================
// PASS 0x2 : TOKENIZATION
// ============================================================================

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


int32_t tokenizeLine(
    const string& line,
    Token tokens[],
    int32_t maxTokens
)
{
    if (
        tokens == nullptr
        ||
        maxTokens <= 0
        )
    {
        return 0;
    }

    int32_t tokenCount = 0;

    size_t position = 0;

    while (position < line.size())
    {
        // Skip spaces/tabs.
        while (
            position < line.size()
            &&
            (
                line[position] == ' '
                ||
                line[position] == '\t'
                )
            )
        {
            position++;
        }

        if (position >= line.size())
        {
            break;
        }

        size_t end = position;

        while (
            end < line.size()
            &&
            line[end] != ' '
            &&
            line[end] != '\t'
            )
        {
            end++;
        }

        if (tokenCount >= maxTokens)
        {
            return -1;
        }

        string word =
            line.substr(
                position,
                end - position
            );

        if (tokenCount == 0)
        {
            tokens[tokenCount].type =
                KEYWORD;
        }
        else if (tokenCount == 1)
        {
            tokens[tokenCount].type =
                IDENTIFIER;
        }
        else
        {
            tokens[tokenCount].type =
                PARAM;
        }

        tokens[tokenCount].text =
            word;

        tokenCount++;

        position = end;
    }

    return tokenCount;
}


// ============================================================================
// SNAPSHOT CREATION
// ============================================================================

Snapshot* buildSnapshot(
    Stack<Frame>& callStack
)
{
    Snapshot* snapshot =
        new Snapshot;

    snapshot->stackDepth =
        callStack.snapshot_into(
            snapshot->callStack,
            MAX_STACK_DEPTH
        );

    return snapshot;
}


// ============================================================================
// VARIABLE LOOKUP
// ============================================================================

Variable* findVariable(
    Frame& frame,
    const string& name
)
{
    // Locals first.
    for (
        int32_t i = 0;
        i < frame.localCount;
        i++
        )
    {
        if (
            frame.locals[i].name
            == name
            )
        {
            return &frame.locals[i];
        }
    }

    // Then arguments.
    for (
        int32_t i = 0;
        i < frame.argc;
        i++
        )
    {
        if (
            frame.argv[i].name
            == name
            )
        {
            return &frame.argv[i];
        }
    }

    return nullptr;
}


// ============================================================================
// PASS 0x2 : EXECUTION
// ============================================================================

void executeProgram(
    const char* resolveBinPath,
    int64_t mainOffset,
    Timeline& timeline
)
{
    if (mainOffset < 0)
    {
        return;
    }

    Stack<Frame> callStack;

    // --------------------------------------------------------
    // Main frame
    // --------------------------------------------------------

    Frame mainFrame{};

    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    callStack.push(mainFrame);

    // Snapshot initial state.
    timeline.record(
        buildSnapshot(callStack)
    );

    // --------------------------------------------------------
    // Open resolve.bin
    // --------------------------------------------------------

    FILE* resolveFile =
        fopen(
            resolveBinPath,
            "rb"
        );

    if (resolveFile == nullptr)
    {
        return;
    }

    // --------------------------------------------------------
    // Start from main FUNC record
    // --------------------------------------------------------

    if (
        fseek(
            resolveFile,
            mainOffset,
            SEEK_SET
        ) != 0
        )
    {
        fclose(resolveFile);

        return;
    }

    string line;

    int64_t firstOffset =
        readResolveRecord(
            resolveFile,
            line
        );

    if (firstOffset < 0)
    {
        fclose(resolveFile);

        return;
    }

    Token tokens[MAX_TOKENS];

    int32_t tokenCount =
        tokenizeLine(
            line,
            tokens,
            MAX_TOKENS
        );

    if (
        tokenCount < 2
        ||
        tokens[0].text != "func"
        ||
        tokens[1].text != "main"
        )
    {
        fclose(resolveFile);

        return;
    }

    // --------------------------------------------------------
    // For each called frame:
    //
    // callerArgumentNames[depth][argument]
    // tells which caller variable maps to callee argument.
    // --------------------------------------------------------

    string callerArgumentNames
        [MAX_STACK_DEPTH]
        [MAX_VARS_PER_FRAME];

    int32_t callerArgumentCounts
        [MAX_STACK_DEPTH] = {};

    // --------------------------------------------------------
    // Execute line by line.
    // --------------------------------------------------------

    while (!callStack.isEmpty())
    {
        string instruction;

        int64_t instructionOffset =
            readResolveRecord(
                resolveFile,
                instruction
            );

        if (instructionOffset < 0)
        {
            break;
        }

        Token instructionTokens[MAX_TOKENS];

        int32_t instructionTokenCount =
            tokenizeLine(
                instruction,
                instructionTokens,
                MAX_TOKENS
            );

        if (instructionTokenCount < 0)
        {
            fclose(resolveFile);

            return;
        }

        if (instructionTokenCount == 0)
        {
            continue;
        }

        string keyword =
            instructionTokens[0].text;

        // ====================================================
        // SET
        // ====================================================

        if (keyword == "set")
        {
            if (instructionTokenCount != 3)
            {
                fclose(resolveFile);

                return;
            }

            Frame& currentFrame =
                callStack.peek();

            int32_t value;

            try
            {
                value =
                    stoi(
                        instructionTokens[2].text
                    );
            }
            catch (...)
            {
                fclose(resolveFile);

                return;
            }

            Variable* variable =
                findVariable(
                    currentFrame,
                    instructionTokens[1].text
                );

            if (variable != nullptr)
            {
                variable->value = value;
            }
            else
            {
                if (
                    currentFrame.localCount
                    >= MAX_VARS_PER_FRAME
                    )
                {
                    fclose(resolveFile);

                    return;
                }

                currentFrame
                    .locals[
                        currentFrame.localCount
                    ]
                    .name =
                    instructionTokens[1].text;

                currentFrame
                    .locals[
                        currentFrame.localCount
                    ]
                    .value =
                    value;

                currentFrame.localCount++;
            }

            // Snapshot after SET.
            timeline.record(
                buildSnapshot(callStack)
            );
        }

        // ====================================================
        // ADD / SUB / MUL / DIV
        // ====================================================

        else if (
            keyword == "add"
            ||
            keyword == "sub"
            ||
            keyword == "mul"
            ||
            keyword == "div"
            )
        {
            if (instructionTokenCount != 3)
            {
                fclose(resolveFile);

                return;
            }

            Frame& currentFrame =
                callStack.peek();

            Variable* destination =
                findVariable(
                    currentFrame,
                    instructionTokens[1].text
                );

            Variable* source =
                findVariable(
                    currentFrame,
                    instructionTokens[2].text
                );

            if (
                destination == nullptr
                ||
                source == nullptr
                )
            {
                fclose(resolveFile);

                return;
            }

            if (keyword == "add")
            {
                destination->value +=
                    source->value;
            }
            else if (keyword == "sub")
            {
                destination->value -=
                    source->value;
            }
            else if (keyword == "mul")
            {
                destination->value *=
                    source->value;
            }
            else if (keyword == "div")
            {
                if (source->value == 0)
                {
                    fclose(resolveFile);

                    return;
                }

                destination->value /=
                    source->value;
            }

            // Snapshot after arithmetic instruction.
            timeline.record(
                buildSnapshot(callStack)
            );
        }

        // ====================================================
        // CALL
        // ====================================================

        else if (keyword == "call")
        {
            if (instructionTokenCount < 2)
            {
                fclose(resolveFile);

                return;
            }

            Frame& callerFrame =
                callStack.peek();

            int32_t argc =
                instructionTokenCount - 2;

            if (argc > MAX_VARS_PER_FRAME)
            {
                fclose(resolveFile);

                return;
            }

            if (
                callStack.depth()
                >= MAX_STACK_DEPTH
                )
            {
                fclose(resolveFile);

                return;
            }

            // The file pointer is now immediately
            // after this CALL record.
            int64_t returnOffset =
                ftell(resolveFile);

            if (returnOffset < 0)
            {
                fclose(resolveFile);

                return;
            }

            // instructionOffset is the first
            // 8-byte field of the CALL record.
            //
            // During Pass 0x1 it was patched with
            // the target FUNC record offset.
            int64_t targetOffset =
                instructionOffset;

            if (
                fseek(
                    resolveFile,
                    targetOffset,
                    SEEK_SET
                ) != 0
                )
            {
                fclose(resolveFile);

                return;
            }

            string functionDeclaration;

            int64_t functionOffset =
                readResolveRecord(
                    resolveFile,
                    functionDeclaration
                );

            if (functionOffset < 0)
            {
                fclose(resolveFile);

                return;
            }

            Token functionTokens[MAX_TOKENS];

            int32_t functionTokenCount =
                tokenizeLine(
                    functionDeclaration,
                    functionTokens,
                    MAX_TOKENS
                );

            if (
                functionTokenCount < 2
                ||
                functionTokens[0].text != "func"
                )
            {
                fclose(resolveFile);

                return;
            }

            int32_t declaredArgc =
                functionTokenCount - 2;

            if (declaredArgc != argc)
            {
                fclose(resolveFile);

                return;
            }

            Frame calledFrame{};

            calledFrame.func_name =
                functionTokens[1].text;

            calledFrame.argc = argc;

            calledFrame.returnLine =
                static_cast<int32_t>(
                    returnOffset
                    );

            calledFrame.localCount = 0;

            // The new frame's depth index is
            // the caller's current depth.
            int32_t newDepth =
                callStack.depth();

            callerArgumentCounts[newDepth] =
                argc;

            // Bind caller arguments -> callee arguments.
            for (
                int32_t i = 0;
                i < argc;
                i++
                )
            {
                string callerVariableName =
                    instructionTokens[
                        i + 2
                    ].text;

                Variable* callerVariable =
                    findVariable(
                        callerFrame,
                        callerVariableName
                    );

                if (callerVariable == nullptr)
                {
                    fclose(resolveFile);

                    return;
                }

                string calleeParameterName =
                    functionTokens[
                        i + 2
                    ].text;

                calledFrame
                    .argv[i]
                    .name =
                    calleeParameterName;

                calledFrame
                    .argv[i]
                    .value =
                    callerVariable->value;

                callerArgumentNames[
                    newDepth
                ][i] =
                        callerVariableName;
            }

            // Push called function.
            callStack.push(calledFrame);

            // Snapshot after CALL.
            timeline.record(
                buildSnapshot(callStack)
            );

            // File pointer is already immediately
            // after the target FUNC record.
            //
            // Therefore execution begins at the
            // first instruction inside the function.
        }

        // ====================================================
        // FUNC_END
        // ====================================================

        else if (keyword == "func_end")
        {
            // Snapshot representing the state when
            // FUNC_END is reached.
            timeline.record(
                buildSnapshot(callStack)
            );

            // If main ended, execution is complete.
            if (callStack.depth() == 1)
            {
                callStack.pop();

                break;
            }

            // Save finished frame.
            Frame finishedFrame =
                callStack.peek();

            int32_t finishedDepth =
                callStack.depth() - 1;

            // Remove called function.
            callStack.pop();

            // Caller is now on top.
            Frame& callerFrame =
                callStack.peek();

            // Copy modified argument values back
            // into caller variables.
            int32_t argumentCount =
                callerArgumentCounts[
                    finishedDepth
                ];

            for (
                int32_t i = 0;
                i < argumentCount;
                i++
                )
            {
                Variable* callerVariable =
                    findVariable(
                        callerFrame,
                        callerArgumentNames[
                            finishedDepth
                        ][i]
                                );

                if (callerVariable != nullptr)
                {
                    callerVariable->value =
                        finishedFrame
                        .argv[i]
                        .value;
                }
            }

            // Continue immediately after CALL.
            int64_t returnOffset =
                finishedFrame.returnLine;

            if (
                fseek(
                    resolveFile,
                    returnOffset,
                    SEEK_SET
                ) != 0
                )
            {
                fclose(resolveFile);

                return;
            }
        }

        // ====================================================
        // FUNC
        // ====================================================

        else if (keyword == "func")
        {
            // FUNC declaration itself is not an
            // executable instruction.
            continue;
        }

        // ====================================================
        // UNKNOWN INSTRUCTION
        // ====================================================

        else
        {
            fclose(resolveFile);

            return;
        }
    }

    fclose(resolveFile);
}


// ============================================================================
// PASS 0x3 : SNAPSHOT SERIALIZATION
// ============================================================================
//
// Snapshot record format used here:
//
// [stackDepth : 4B]
//
// For each frame:
//
// [funcNameSize : 4B]
// [funcName bytes]
//
// [argc : 4B]
//
// For each argument:
// [nameSize : 4B]
// [name bytes]
// [value : 4B]
//
// [returnLine : 4B]
//
// [localCount : 4B]
//
// For each local:
// [nameSize : 4B]
// [name bytes]
// [value : 4B]
//
// ============================================================================

bool writeString(
    FILE* f,
    const string& value
)
{
    if (f == nullptr)
    {
        return false;
    }

    uint32_t size =
        static_cast<uint32_t>(
            value.size()
            );

    if (
        fwrite(
            &size,
            sizeof(uint32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    if (size > 0)
    {
        if (
            fwrite(
                value.data(),
                1,
                size,
                f
            ) != size
            )
        {
            return false;
        }
    }

    return true;
}


bool writeVariable(
    FILE* f,
    const Variable& variable
)
{
    if (!writeString(f, variable.name))
    {
        return false;
    }

    if (
        fwrite(
            &variable.value,
            sizeof(int32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    return true;
}


bool writeFrame(
    FILE* f,
    const Frame& frame
)
{
    if (!writeString(f, frame.func_name))
    {
        return false;
    }

    if (
        fwrite(
            &frame.argc,
            sizeof(int32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    for (
        int32_t i = 0;
        i < frame.argc;
        i++
        )
    {
        if (
            !writeVariable(
                f,
                frame.argv[i]
            )
            )
        {
            return false;
        }
    }

    if (
        fwrite(
            &frame.returnLine,
            sizeof(int32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    if (
        fwrite(
            &frame.localCount,
            sizeof(int32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    for (
        int32_t i = 0;
        i < frame.localCount;
        i++
        )
    {
        if (
            !writeVariable(
                f,
                frame.locals[i]
            )
            )
        {
            return false;
        }
    }

    return true;
}


bool writeSnapshot(
    FILE* f,
    const Snapshot* snapshot
)
{
    if (
        f == nullptr
        ||
        snapshot == nullptr
        )
    {
        return false;
    }

    if (
        fwrite(
            &snapshot->stackDepth,
            sizeof(int32_t),
            1,
            f
        ) != 1
        )
    {
        return false;
    }

    for (
        int32_t i = 0;
        i < snapshot->stackDepth;
        i++
        )
    {
        if (
            !writeFrame(
                f,
                snapshot->callStack[i]
            )
            )
        {
            return false;
        }
    }

    return true;
}


// ============================================================================
// PASS 0x3 : WRITE .TDBG
// ============================================================================

void writeTdbg(
    Timeline& timeline,
    const char* tdbgPath
)
{
    FILE* f =
        fopen(
            tdbgPath,
            "wb+"
        );

    if (f == nullptr)
    {
        return;
    }

    // --------------------------------------------------------
    // Header
    // --------------------------------------------------------

    TTDBHeader header{};

    memcpy(
        header.magic,
        "TTDB",
        4
    );

    header.version = 1;

    header.stepCount =
        timeline.getStepCount();

    header.indexOffset = 0;

    writeHeader(
        f,
        header
    );

    // --------------------------------------------------------
    // Dense index
    // --------------------------------------------------------

    int32_t totalSteps =
        timeline.getStepCount();

    int64_t* indexArray = nullptr;

    if (totalSteps > 0)
    {
        indexArray =
            new int64_t[
                totalSteps
            ];
    }

    // --------------------------------------------------------
    // Snapshot stream
    // --------------------------------------------------------

    TimelineNode* current =
        timeline.begin();

    int32_t stepIdx = 0;

    while (
        current != nullptr
        &&
        stepIdx < totalSteps
        )
    {
        long position =
            ftell(f);

        if (position < 0)
        {
            delete[] indexArray;
            fclose(f);

            return;
        }

        indexArray[stepIdx] =
            static_cast<int64_t>(
                position
                );

        if (
            !writeSnapshot(
                f,
                current->data
            )
            )
        {
            delete[] indexArray;
            fclose(f);

            return;
        }

        current =
            current->next;

        stepIdx++;
    }

    // --------------------------------------------------------
    // Dense index starts here.
    // --------------------------------------------------------

    long indexPosition =
        ftell(f);

    if (indexPosition < 0)
    {
        delete[] indexArray;
        fclose(f);

        return;
    }

    header.indexOffset =
        static_cast<int64_t>(
            indexPosition
            );

    for (
        int32_t i = 0;
        i < totalSteps;
        i++
        )
    {
        if (
            fwrite(
                &indexArray[i],
                sizeof(int64_t),
                1,
                f
            ) != 1
            )
        {
            delete[] indexArray;
            fclose(f);

            cerr << "Runtime error: division by zero" << endl;
            return;
        }
    }

    // --------------------------------------------------------
    // Update header with final indexOffset.
    // --------------------------------------------------------

    if (
        fseek(
            f,
            0,
            SEEK_SET
        ) != 0
        )
    {
        delete[] indexArray;
        fclose(f);

        return;
    }

    writeHeader(
        f,
        header
    );

    delete[] indexArray;

    fclose(f);
}


// ============================================================================
// MAIN
// ============================================================================

int32_t main()
{
    cout << "STEP 1: Starting Time-Travel Debugger..." << endl;

    // --------------------------------------------------------
    // PASS 0x0
    // --------------------------------------------------------

    if (
        !validateProgram(
            "source.bin"
        )
        )
    {
        cout
            << "ERROR: Validation failed."
            << endl;

        return 1;
    }

    cout
        << "STEP 2: Validation passed."
        << endl;

    // --------------------------------------------------------
    // PASS 0x1
    // --------------------------------------------------------

    int64_t mainOffset =
        resolveProgram(
            "source.bin",
            "resolve.bin"
        );

    if (mainOffset < 0)
    {
        cout
            << "ERROR: Resolve failed."
            << endl;

        return 1;
    }

    cout
        << "STEP 3: Resolve passed. "
        << "main offset = "
        << mainOffset
        << endl;

    // --------------------------------------------------------
    // PASS 0x2
    // --------------------------------------------------------

    Timeline timeline;

    executeProgram(
        "resolve.bin",
        mainOffset,
        timeline
    );

    cout
        << "STEP 4: Execution finished. "
        << "Snapshots = "
        << timeline.getStepCount()
        << endl;

    // --------------------------------------------------------
    // PASS 0x3
    // --------------------------------------------------------

    writeTdbg(
        timeline,
        "session.tdbg"
    );

    cout
        << "STEP 5: session.tdbg created."
        << endl;

    return 0;
}
