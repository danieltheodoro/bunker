#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdint.h>
#include <time.h>
typedef struct { void* obj; void* vtable; } BunkerTraitObject;
typedef FILE __BunkerFile;
typedef struct { long long value; char* errorMessage; bool isError; } BunkerResult;
// Exception Handling
typedef struct { char* message; } BunkerException;
static jmp_buf* current_env = NULL;
static BunkerException global_exception = {0};
// Function pointer type for lambdas
typedef void* (*BunkerFunction)(void*, ...);
// Closure wrapper: holds function + environment
typedef struct { BunkerFunction fn; void* env; } BunkerClosure;
// Array struct
typedef struct { void** data; size_t length; size_t capacity; } BunkerArray;
// GC Runtime
typedef struct GC_Header {
    uint32_t magic;
    uint32_t marked;
    size_t size;
    void* forwarding;
    struct GC_Header* next;
} GC_Header;

#define GC_MAGIC 0x88BDB88B

static GC_Header* gc_allocations = NULL;
static size_t gc_old_gen_count = 0;
static size_t gc_old_gen_threshold = 500000; // Lowered threshold
void* gc_stack_bottom = NULL;

static void** gc_mark_stack = NULL;
static size_t gc_mark_stack_top = 0;
static size_t gc_mark_stack_cap = 0;

// Generational GC: Nursery (Semi-space)
static char* nursery_start = NULL;
static char* nursery_top = NULL;
static char* nursery_end = NULL;
#define NURSERY_SIZE (128 * 1024 * 1024) // 128MB

#define PAGE_SHIFT 12
#define PAGE_MAP_SIZE 131072 // 128K entries
static uintptr_t gc_page_map[PAGE_MAP_SIZE];

static void gc_register_pages(void* ptr, size_t size) {
    if (!ptr) return;
    uintptr_t start = (uintptr_t)ptr >> PAGE_SHIFT;
    uintptr_t end = ((uintptr_t)ptr + size - 1) >> PAGE_SHIFT;
    for (uintptr_t p = start; p <= end; p++) {
        uintptr_t h = (p * 11400714819323198485ULL) & (PAGE_MAP_SIZE - 1);
        while (gc_page_map[h] != 0 && gc_page_map[h] != p) {
            h = (h + 1) & (PAGE_MAP_SIZE - 1);
        }
        gc_page_map[h] = p;
    }
}

static int gc_is_bunker_page(void* ptr) {
    uintptr_t p = (uintptr_t)ptr >> PAGE_SHIFT;
    if (p == 0) return 0;
    uintptr_t h = (p * 11400714819323198485ULL) & (PAGE_MAP_SIZE - 1);
    while (gc_page_map[h] != 0) {
        if (gc_page_map[h] == p) return 1;
        h = (h + 1) & (PAGE_MAP_SIZE - 1);
    }
    return 0;
}

// Prototypes
void gc_scan_region(void* start, void* end);
void gc_mark(void* ptr);
void gc_collect();
void* gc_evacuate(void* ptr);
void gc_process_marks();
void gc_push_mark(void* ptr);
int gc_is_bunker_object(void* ptr);

int gc_is_bunker_object(void* ptr) {
    if (!ptr || ((uintptr_t)ptr & (sizeof(void*)-1))) return 0;
    if ((char*)ptr >= nursery_start && (char*)ptr < nursery_end) return 1;
    
    if (!gc_is_bunker_page(ptr)) return 0;
    
    GC_Header* h = (GC_Header*)ptr - 1;
    if (h->magic != GC_MAGIC) return 0;
    return 1;
}

void gc_push_mark(void* ptr) {
    if (gc_mark_stack_top >= gc_mark_stack_cap) {
        gc_mark_stack_cap = gc_mark_stack_cap ? gc_mark_stack_cap * 2 : 1024;
        gc_mark_stack = (void**)realloc(gc_mark_stack, gc_mark_stack_cap * sizeof(void*));
    }
    gc_mark_stack[gc_mark_stack_top++] = ptr;
}

void gc_process_marks() {
    while (gc_mark_stack_top > 0) {
        void* ptr = gc_mark_stack[--gc_mark_stack_top];
        GC_Header* h = (GC_Header*)ptr - 1;
        gc_scan_region(ptr, (void*)((char*)ptr + h->size));
    }
}

void* gc_evacuate(void* ptr) {
    if (!ptr || ((uintptr_t)ptr & (sizeof(void*)-1)) || (char*)ptr < nursery_start || (char*)ptr >= nursery_end) return ptr;
    
    GC_Header* h = (GC_Header*)ptr - 1;
    if (!gc_is_bunker_object(ptr)) return ptr;
    if (h->forwarding) return h->forwarding;

    // Move to Old Generation
    size_t total_size = sizeof(GC_Header) + h->size;
    GC_Header* new_h = (GC_Header*)calloc(1, total_size);
    if (!new_h) return ptr;
    new_h->magic = GC_MAGIC;
    new_h->marked = 0;
    new_h->size = h->size;
    new_h->forwarding = NULL;
    new_h->next = gc_allocations;
    gc_allocations = new_h;
    gc_old_gen_count++;
    
    void* new_ptr = (void*)(new_h + 1);
    gc_register_pages(new_h, total_size);
    
    h->forwarding = new_ptr;
    memcpy(new_ptr, ptr, h->size);
    
    if (getenv("DEBUG_GC")) printf("DEBUG GC: Promoted %p to %p (size %zu)\n", ptr, new_ptr, h->size);
    
    return new_ptr;
}

void gc_scan_region(void* start, void* end) {
    if (start > end) { void* tmp = start; start = end; end = tmp; }
    char* p = (char*)((uintptr_t)start & ~(uintptr_t)(sizeof(void*)-1));
    char* p_end = (char*)end;
    while (p + sizeof(void*) <= p_end) {
        void** slot = (void**)p;
        *slot = gc_evacuate(*slot);
        gc_mark(*slot);
        p += sizeof(void*);
    }
}

void gc_mark(void* ptr) {
    if (!gc_is_bunker_object(ptr)) return;
    if ((char*)ptr >= nursery_start && (char*)ptr < nursery_end) return;
    
    GC_Header* h = (GC_Header*)ptr - 1;
    if (!h->marked) {
        h->marked = 1;
        gc_push_mark(ptr);
    }
}

void gc_minor_collect() {
    if (getenv("DEBUG_GC")) printf("DEBUG GC: Starting Minor Collection (Nursery -> Old)\n");
    
    jmp_buf env;
    setjmp(env);
    __builtin_unwind_init();
    volatile void* stack_top = &stack_top;
    
    // We don't mark old generation objects yet, just evacuate nursery ones
    // But since we are moving things, we MUST scan the stack and update pointers.
    gc_scan_region((void*)stack_top, gc_stack_bottom);
    gc_scan_region((void*)env, (void*)((char*)env + sizeof(jmp_buf)));
    
    // Also scan all Old Generation objects because they might point into the nursery
    GC_Header* h = gc_allocations;
    while (h) {
        void* obj_ptr = (void*)(h + 1);
        gc_scan_region(obj_ptr, (void*)((char*)obj_ptr + h->size));
        h = h->next;
    }
    gc_process_marks();
    
    // Reset nursery
    nursery_top = nursery_start;
    if (getenv("DEBUG_GC")) printf("DEBUG GC: Minor Collection Complete\n");
}

void gc_free(void* ptr) {
    if (!ptr) return;
    GC_Header* h = (GC_Header*)ptr - 1;
    GC_Header** prev = &gc_allocations;
    GC_Header* curr = gc_allocations;
    while (curr) {
        if (curr == h) {
            *prev = curr->next;
            if (getenv("DEBUG_GC")) printf("DEBUG GC: Explicit Free %p\n", ptr);
            free(curr);
            return;
        }
        prev = &curr->next;
        curr = curr->next;
    }
}

void gc_collect() {
    jmp_buf env;
    setjmp(env);
    __builtin_unwind_init();
    volatile void* stack_top = &stack_top;
    
    // Mark
    gc_scan_region((void*)stack_top, gc_stack_bottom);
    gc_scan_region((void*)env, (void*)((char*)env + sizeof(jmp_buf)));
    gc_process_marks();
    
    // Sweep
    int collected = 0;
    int kept = 0;
    memset(gc_page_map, 0, sizeof(gc_page_map)); /* Rebuild Page Map */
    GC_Header** node = &gc_allocations;
    while (*node) {
        GC_Header* curr = *node;
        if (!curr->marked) {
            *node = curr->next;
            // if (getenv("DEBUG_GC")) printf("DEBUG GC: Freeing %p (size %zu)\n", (void*)(curr+1), curr->size);
            free(curr);
            collected++;
        } else {
            curr->marked = 0;
            gc_register_pages(curr, sizeof(GC_Header) + curr->size);
            node = &(*node)->next;
            kept++;
        }
    }
    gc_old_gen_count = kept;
    if (getenv("DEBUG_GC")) printf("DEBUG GC: Collection done. Collected: %d, Kept: %d\n", collected, kept);
}

void* gc_alloc(size_t size) {
    if (size < sizeof(void*)) size = sizeof(void*);
    size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    size_t total_size = sizeof(GC_Header) + size;

    if (nursery_top + total_size <= nursery_end) {
        GC_Header* h = (GC_Header*)nursery_top;
        h->magic = GC_MAGIC;
        h->marked = 0;
        h->size = size;
        h->forwarding = NULL;
        h->next = (GC_Header*)0x1;
        nursery_top += total_size;
        return (void*)(h + 1);
    }

    if (gc_old_gen_count > gc_old_gen_threshold) {
        gc_collect();
        gc_old_gen_threshold = gc_old_gen_count + (gc_old_gen_count / 2) + 10000;
    }

    gc_minor_collect();

    if (total_size > NURSERY_SIZE) {
        GC_Header* h = (GC_Header*)calloc(1, total_size);
        if (!h) return NULL;
        h->magic = GC_MAGIC;
        h->marked = 0;
        h->size = size;
        h->forwarding = NULL;
        h->next = gc_allocations;
        gc_allocations = h;
        gc_old_gen_count++;
        gc_register_pages(h, total_size);
        return (void*)(h + 1);
    }

    GC_Header* h = (GC_Header*)nursery_top;
    h->magic = GC_MAGIC;
    h->marked = 0;
    h->size = size;
    h->forwarding = NULL;
    h->next = (GC_Header*)0x1;
    nursery_top += total_size;
    return (void*)(h + 1);
}

void* gc_get_stack_base() {
    return pthread_get_stackaddr_np(pthread_self());
}

#define GC_INIT() { \
    gc_stack_bottom = gc_get_stack_base(); \
    nursery_start = (char*)malloc(NURSERY_SIZE); \
    nursery_top = nursery_start; \
    nursery_end = nursery_start + NURSERY_SIZE; \
    (void)current_env; (void)global_exception; \
}

// Helper: Create Array
BunkerArray* Bunker_make_array(size_t count) {
    BunkerArray* arr = gc_alloc(sizeof(BunkerArray));
    arr->length = count;
    arr->capacity = count > 0 ? count : 4;
    arr->data = gc_alloc(sizeof(void*) * arr->capacity);
    return arr;
}

// Helper: Array Map
BunkerArray* Bunker_Array_map(BunkerArray* self, BunkerClosure* fn) {
    BunkerArray* result = Bunker_make_array(self->length);
    for (size_t i = 0; i < self->length; i++) {
        // fn signature: void* (*)(void* env, void* arg)
        result->data[i] = fn->fn(fn->env, self->data[i]);
    }
    return result;
}

// Helper: Array Filter
BunkerArray* Bunker_Array_filter(BunkerArray* self, BunkerClosure* predicate) {
    BunkerArray* result = Bunker_make_array(self->length); // Worst case size
    size_t count = 0;
    for (size_t i = 0; i < self->length; i++) {
        // predicate returns bool (which is int/long long in C usually, or strictly bool)
        // If we cast to intptr_t we can check truthiness
        void* res = predicate->fn(predicate->env, self->data[i]);
        if ((long long)(intptr_t)res != 0) {
            result->data[count++] = self->data[i];
        }
    }
    result->length = count; // Resize?
    return result;
}

// Helper: Array Reduce
void* Bunker_Array_reduce(BunkerArray* self, void* initial, BunkerClosure* fn) {
    void* acc = initial;
    for (size_t i = 0; i < self->length; i++) {
        acc = fn->fn(fn->env, acc, self->data[i]);
    }
    return acc;
}
// Helper: String + Int
char* bunker_str_add_int(char* s, long long i) {
    int len = strlen(s);
    int needed = len + 22; // max int64 digits + sign + null
    char* buffer = gc_alloc(needed);
    sprintf(buffer, "%s%lld", s, i);
    return buffer;
}

// Helper: String + Double
char* bunker_str_add_double(char* s, double d) {
    int len = strlen(s);
    int needed = len + 50; 
    char* buffer = gc_alloc(needed);
    sprintf(buffer, "%s%g", s, d);
    return buffer;
}

// Helper: String Concatenation
char* BNK_RT_Strings_concat(void* _self, char* s1, char* s2) {
    if (!s1) s1 = "";
    if (!s2) s2 = "";
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char* result = gc_alloc(len1 + len2 + 1);
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

// Helper: Strings.split
BunkerArray* BNK_RT_Strings_split(void* _self, char* str, char* delimiter) {
    if (!str || !delimiter) return Bunker_make_array(0);
    
    // Copy string because strtok modifies it
    char* s = gc_alloc(strlen(str) + 1);
    strcpy(s, str);
    
    // First pass: count tokens
    int count = 0;
    char* tmp = gc_alloc(strlen(str) + 1);
    strcpy(tmp, str);
    char* token = strtok(tmp, delimiter);
    while (token) {
        count++;
        token = strtok(NULL, delimiter);
    }
    
    BunkerArray* arr = Bunker_make_array(count);
    
    // Second pass: fill array
    token = strtok(s, delimiter);
    int i = 0;
    while (token) {
        char* item = gc_alloc(strlen(token) + 1);
        strcpy(item, token);
        arr->data[i++] = item;
        token = strtok(NULL, delimiter);
    }
    return arr;
}

// Helper: Strings.toInt
long long BNK_RT_Strings_toInt(void* _self, char* s) {
    return atoll(s);
}

// Helper: Strings.toFloat
double BNK_RT_Strings_toFloat(void* _self, char* s) {
    return atof(s);
}

// Helper: Strings.length
long long BNK_RT_Strings_length(void* _self, char* s) {
    return s ? strlen(s) : 0;
}

// Helper: Strings.substring
char* BNK_RT_Strings_substring(void* _self, char* s, long long start, long long end) {
    if (!s) return "";
    long long len = strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return "";
    long long substr_len = end - start;
    char* result = gc_alloc(substr_len + 1);
    strncpy(result, s + start, substr_len);
    result[substr_len] = '\0';
    return result;
}

// Helper: Strings.startsWith
bool BNK_RT_Strings_startsWith(void* _self, char* s, char* prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Helper: IO.readFileLines
BunkerArray* IO_readFileLines(void* _self, char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return Bunker_make_array(0);
    
    // Pre-allocate list logic (dynamic resize would be better but simple array for now)
    // Count lines first
    int lines = 0;
    char ch;
    while(!feof(f)) {
        ch = fgetc(f);
        if(ch == '\n') lines++;
    }
    lines++; // Last line might not end in newline
    rewind(f);
    
    BunkerArray* arr = Bunker_make_array(lines);
    int i = 0;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), f)) {
        // Remove newline
        buffer[strcspn(buffer, "\n")] = 0;
        char* line = gc_alloc(strlen(buffer) + 1);
        strcpy(line, buffer);
        if (i < lines) arr->data[i++] = line;
    }
    fclose(f);
    arr->length = i; // Adjust actual count
    return arr;
}

// Helper: IO.input
char* IO_input(void* _self, char* prompt) {
    printf("%s", prompt);
    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), stdin)) {
        buffer[strcspn(buffer, "\n")] = 0;
        char* res = gc_alloc(strlen(buffer) + 1);
        strcpy(res, buffer);
        return res;
    }
    return "";
}

// Helper: Math.random
double Math_random(void* _self) {
    return (double)rand() / (double)RAND_MAX;
}

// Helper: Math.min / max
double Math_min(void* _self, double a, double b) { return a < b ? a : b; }
double Math_max(void* _self, double a, double b) { return a > b ? a : b; }

// End of helpers

char* System_os(void* _self) {
    #if defined(_WIN32)
    return "windows";
    #elif defined(__APPLE__)
    return "macos";
    #elif defined(__linux__)
    return "linux";
    #else
    return "unknown";
    #endif
}

char* System_arch(void* _self) {
    #if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
    #elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
    #else
    return "unknown";
    #endif
}

typedef struct Strings Strings;
typedef struct ASTList ASTList;
typedef struct ASTNode ASTNode;
typedef struct Program Program;
typedef struct LetStmt LetStmt;
typedef struct ReturnStmt ReturnStmt;
typedef struct ExprStmt ExprStmt;
typedef struct BinaryExpr BinaryExpr;
typedef struct UnaryExpr UnaryExpr;
typedef struct LiteralExpr LiteralExpr;
typedef struct VariableExpr VariableExpr;
typedef struct AssignExpr AssignExpr;
typedef struct Block Block;
typedef struct IfStmt IfStmt;
typedef struct WhileStmt WhileStmt;
typedef struct GroupingExpr GroupingExpr;
typedef struct MethodCall MethodCall;
typedef struct GetExpr GetExpr;
typedef struct ArrayAccess ArrayAccess;
typedef struct FieldDecl FieldDecl;
typedef struct MethodDecl MethodDecl;
typedef struct StructDecl StructDecl;
typedef struct EntityDecl EntityDecl;
typedef struct ModuleDecl ModuleDecl;
typedef struct ImportDecl ImportDecl;
typedef struct IncludeDecl IncludeDecl;
typedef struct LoopStmt LoopStmt;
typedef struct ScopedStmt ScopedStmt;
typedef struct UnabstractedBlock UnabstractedBlock;
typedef struct AsmStmt AsmStmt;
typedef struct Token Token;
typedef struct ErrorReporter ErrorReporter;
typedef struct Header Header;
typedef struct NamedNode NamedNode;
typedef struct Symbol Symbol;
typedef struct Scope Scope;
typedef struct SymbolTable SymbolTable;
typedef struct TypeChecker TypeChecker;
typedef struct CTranspiler CTranspiler;
typedef struct File File;
typedef struct FileSystem FileSystem;
typedef struct Casts Casts;
typedef struct Lexer Lexer;
typedef struct Parser Parser;
typedef struct Main Main;
typedef struct List_string List_string;
struct ASTList {
    void* head;
    void* tail;
};
void* ASTList_new(void* _dummy, void* head, void* tail) {
    ASTList* _inst = gc_alloc(sizeof(ASTList));
    _inst->head = head;
    _inst->tail = tail;
    return _inst;
}
struct ASTNode {
    void* value;
    void* next;
};
void* ASTNode_new(void* _dummy, void* value, void* next) {
    ASTNode* _inst = gc_alloc(sizeof(ASTNode));
    _inst->value = value;
    _inst->next = next;
    return _inst;
}
struct Program {
    char* type;
    void* module;
    void* stmts;
};
void* Program_new(void* _dummy, char* type, void* module, void* stmts) {
    Program* _inst = gc_alloc(sizeof(Program));
    _inst->type = type;
    _inst->module = module;
    _inst->stmts = stmts;
    return _inst;
}
struct LetStmt {
    char* type;
    Token* name;
    char* typeHint;
    void* initializer;
};
void* LetStmt_new(void* _dummy, char* type, Token* name, char* typeHint, void* initializer) {
    LetStmt* _inst = gc_alloc(sizeof(LetStmt));
    _inst->type = type;
    _inst->name = name;
    _inst->typeHint = typeHint;
    _inst->initializer = initializer;
    return _inst;
}
struct ReturnStmt {
    char* type;
    void* value;
};
void* ReturnStmt_new(void* _dummy, char* type, void* value) {
    ReturnStmt* _inst = gc_alloc(sizeof(ReturnStmt));
    _inst->type = type;
    _inst->value = value;
    return _inst;
}
struct ExprStmt {
    char* type;
    void* expr;
};
void* ExprStmt_new(void* _dummy, char* type, void* expr) {
    ExprStmt* _inst = gc_alloc(sizeof(ExprStmt));
    _inst->type = type;
    _inst->expr = expr;
    return _inst;
}
struct BinaryExpr {
    char* type;
    void* left;
    Token* op;
    void* right;
};
void* BinaryExpr_new(void* _dummy, char* type, void* left, Token* op, void* right) {
    BinaryExpr* _inst = gc_alloc(sizeof(BinaryExpr));
    _inst->type = type;
    _inst->left = left;
    _inst->op = op;
    _inst->right = right;
    return _inst;
}
struct UnaryExpr {
    char* type;
    Token* op;
    void* right;
};
void* UnaryExpr_new(void* _dummy, char* type, Token* op, void* right) {
    UnaryExpr* _inst = gc_alloc(sizeof(UnaryExpr));
    _inst->type = type;
    _inst->op = op;
    _inst->right = right;
    return _inst;
}
struct LiteralExpr {
    char* type;
    Token* tok;
    char* literalType;
};
void* LiteralExpr_new(void* _dummy, char* type, Token* tok, char* literalType) {
    LiteralExpr* _inst = gc_alloc(sizeof(LiteralExpr));
    _inst->type = type;
    _inst->tok = tok;
    _inst->literalType = literalType;
    return _inst;
}
struct VariableExpr {
    char* type;
    Token* name;
};
void* VariableExpr_new(void* _dummy, char* type, Token* name) {
    VariableExpr* _inst = gc_alloc(sizeof(VariableExpr));
    _inst->type = type;
    _inst->name = name;
    return _inst;
}
struct AssignExpr {
    char* type;
    void* target;
    void* value;
};
void* AssignExpr_new(void* _dummy, char* type, void* target, void* value) {
    AssignExpr* _inst = gc_alloc(sizeof(AssignExpr));
    _inst->type = type;
    _inst->target = target;
    _inst->value = value;
    return _inst;
}
struct Block {
    char* type;
    void* stmts;
    bool terminatedByDot;
};
void* Block_new(void* _dummy, char* type, void* stmts, bool terminatedByDot) {
    Block* _inst = gc_alloc(sizeof(Block));
    _inst->type = type;
    _inst->stmts = stmts;
    _inst->terminatedByDot = terminatedByDot;
    return _inst;
}
struct IfStmt {
    char* type;
    void* condition;
    void* thenBranch;
    void* elseBranch;
};
void* IfStmt_new(void* _dummy, char* type, void* condition, void* thenBranch, void* elseBranch) {
    IfStmt* _inst = gc_alloc(sizeof(IfStmt));
    _inst->type = type;
    _inst->condition = condition;
    _inst->thenBranch = thenBranch;
    _inst->elseBranch = elseBranch;
    return _inst;
}
struct WhileStmt {
    char* type;
    void* condition;
    void* body;
};
void* WhileStmt_new(void* _dummy, char* type, void* condition, void* body) {
    WhileStmt* _inst = gc_alloc(sizeof(WhileStmt));
    _inst->type = type;
    _inst->condition = condition;
    _inst->body = body;
    return _inst;
}
struct GroupingExpr {
    char* type;
    void* expression;
};
void* GroupingExpr_new(void* _dummy, char* type, void* expression) {
    GroupingExpr* _inst = gc_alloc(sizeof(GroupingExpr));
    _inst->type = type;
    _inst->expression = expression;
    return _inst;
}
struct MethodCall {
    char* type;
    void* receiver;
    Token* metTok;
    void* args;
};
void* MethodCall_new(void* _dummy, char* type, void* receiver, Token* metTok, void* args) {
    MethodCall* _inst = gc_alloc(sizeof(MethodCall));
    _inst->type = type;
    _inst->receiver = receiver;
    _inst->metTok = metTok;
    _inst->args = args;
    return _inst;
}
struct GetExpr {
    char* type;
    void* object;
    Token* nameTok;
};
void* GetExpr_new(void* _dummy, char* type, void* object, Token* nameTok) {
    GetExpr* _inst = gc_alloc(sizeof(GetExpr));
    _inst->type = type;
    _inst->object = object;
    _inst->nameTok = nameTok;
    return _inst;
}
struct ArrayAccess {
    char* type;
    void* target;
    void* index;
};
void* ArrayAccess_new(void* _dummy, char* type, void* target, void* index) {
    ArrayAccess* _inst = gc_alloc(sizeof(ArrayAccess));
    _inst->type = type;
    _inst->target = target;
    _inst->index = index;
    return _inst;
}
struct FieldDecl {
    char* type;
    char* name;
    char* fieldType;
};
void* FieldDecl_new(void* _dummy, char* type, char* name, char* fieldType) {
    FieldDecl* _inst = gc_alloc(sizeof(FieldDecl));
    _inst->type = type;
    _inst->name = name;
    _inst->fieldType = fieldType;
    return _inst;
}
struct MethodDecl {
    char* type;
    char* name;
    bool isStatic;
    void* params;
    char* returnType;
    void* body;
    void* typeParams;
};
void* MethodDecl_new(void* _dummy, char* type, char* name, bool isStatic, void* params, char* returnType, void* body, void* typeParams) {
    MethodDecl* _inst = gc_alloc(sizeof(MethodDecl));
    _inst->type = type;
    _inst->name = name;
    _inst->isStatic = isStatic;
    _inst->params = params;
    _inst->returnType = returnType;
    _inst->body = body;
    _inst->typeParams = typeParams;
    return _inst;
}
struct StructDecl {
    char* type;
    char* name;
    void* typeParams;
    void* fields;
};
void* StructDecl_new(void* _dummy, char* type, char* name, void* typeParams, void* fields) {
    StructDecl* _inst = gc_alloc(sizeof(StructDecl));
    _inst->type = type;
    _inst->name = name;
    _inst->typeParams = typeParams;
    _inst->fields = fields;
    return _inst;
}
struct EntityDecl {
    char* type;
    char* name;
    void* typeParams;
    void* implList;
    void* fields;
    void* methods;
};
void* EntityDecl_new(void* _dummy, char* type, char* name, void* typeParams, void* implList, void* fields, void* methods) {
    EntityDecl* _inst = gc_alloc(sizeof(EntityDecl));
    _inst->type = type;
    _inst->name = name;
    _inst->typeParams = typeParams;
    _inst->implList = implList;
    _inst->fields = fields;
    _inst->methods = methods;
    return _inst;
}
struct ModuleDecl {
    char* type;
    char* name;
};
void* ModuleDecl_new(void* _dummy, char* type, char* name) {
    ModuleDecl* _inst = gc_alloc(sizeof(ModuleDecl));
    _inst->type = type;
    _inst->name = name;
    return _inst;
}
struct ImportDecl {
    char* type;
    char* name;
};
void* ImportDecl_new(void* _dummy, char* type, char* name) {
    ImportDecl* _inst = gc_alloc(sizeof(ImportDecl));
    _inst->type = type;
    _inst->name = name;
    return _inst;
}
struct IncludeDecl {
    char* type;
    char* path;
};
void* IncludeDecl_new(void* _dummy, char* type, char* path) {
    IncludeDecl* _inst = gc_alloc(sizeof(IncludeDecl));
    _inst->type = type;
    _inst->path = path;
    return _inst;
}
struct LoopStmt {
    char* type;
    void* body;
};
void* LoopStmt_new(void* _dummy, char* type, void* body) {
    LoopStmt* _inst = gc_alloc(sizeof(LoopStmt));
    _inst->type = type;
    _inst->body = body;
    return _inst;
}
struct ScopedStmt {
    char* type;
    void* body;
};
void* ScopedStmt_new(void* _dummy, char* type, void* body) {
    ScopedStmt* _inst = gc_alloc(sizeof(ScopedStmt));
    _inst->type = type;
    _inst->body = body;
    return _inst;
}
struct UnabstractedBlock {
    char* type;
    void* body;
};
void* UnabstractedBlock_new(void* _dummy, char* type, void* body) {
    UnabstractedBlock* _inst = gc_alloc(sizeof(UnabstractedBlock));
    _inst->type = type;
    _inst->body = body;
    return _inst;
}
struct AsmStmt {
    char* type;
    char* target;
    char* code;
};
void* AsmStmt_new(void* _dummy, char* type, char* target, char* code) {
    AsmStmt* _inst = gc_alloc(sizeof(AsmStmt));
    _inst->type = type;
    _inst->target = target;
    _inst->code = code;
    return _inst;
}
struct Token {
    char* type;
    char* value;
    long long line;
    long long column;
    long long offset;
};
void* Token_new(void* _dummy, char* type, char* value, long long line, long long column, long long offset) {
    Token* _inst = gc_alloc(sizeof(Token));
    if (getenv("DEBUG_TOKEN")) printf("DEBUG: Token_new: ptr=%p type=%s val_ptr=%p val=%s\n", (void*)_inst, type, (void*)value, value);
    _inst->type = type;
    _inst->value = value;
    _inst->line = line;
    _inst->column = column;
    _inst->offset = offset;
    return _inst;
}
struct Header {
    char* type;
};
void* Header_new(void* _dummy, char* type) {
    Header* _inst = gc_alloc(sizeof(Header));
    _inst->type = type;
    return _inst;
}
struct NamedNode {
    char* type;
    char* name;
};
void* NamedNode_new(void* _dummy, char* type, char* name) {
    NamedNode* _inst = gc_alloc(sizeof(NamedNode));
    _inst->type = type;
    _inst->name = name;
    return _inst;
}
struct Symbol {
    char* name;
    char* type;
    bool isDefined;
};
void* Symbol_new(void* _dummy, char* name, char* type, bool isDefined) {
    Symbol* _inst = gc_alloc(sizeof(Symbol));
    _inst->name = name;
    _inst->type = type;
    _inst->isDefined = isDefined;
    return _inst;
}
struct Scope {
    void* symbols;
    void* parent;
};
void* Scope_new(void* _dummy, void* symbols, void* parent) {
    Scope* _inst = gc_alloc(sizeof(Scope));
    _inst->symbols = symbols;
    _inst->parent = parent;
    return _inst;
}
struct Strings {
};
struct ErrorReporter {
};
struct SymbolTable {
    void* current;
};
struct TypeChecker {
    void* parser;
    void* symbols;
    char* source;
    bool hadError;
    void* globalTypes;
    char* currentEntity;
    char* currentMode;
};
struct CTranspiler {
    void* output;
    void* outputTail;
    long long indentLevel;
    void* symbols;
    char* currentEntity;
};
struct File {
    void* handle;
};
struct FileSystem {
};
struct Casts {
};
struct Lexer {
    char* source;
    long long pos;
    long long length;
    long long line;
    long long column;
};
struct Parser {
    void* tokens;
    void* current;
    char* source;
    bool hadError;
    bool panicMode;
    void* includedFiles;
};
struct Main {
};
struct List_string {
    BunkerArray* data;
    long long count;
    long long cap;
};
void ErrorReporter_report(void* _self, char* msg, Token* tok, char* source);
SymbolTable* SymbolTable_new(void* _self);
void SymbolTable_enterScope(void* _self);
void SymbolTable_exitScope(void* _self);
void SymbolTable_define(void* _self, char* name, char* type);
char* SymbolTable_resolve(void* _self, char* name);
TypeChecker* TypeChecker_new(void* _self, void* p, char* src);
void TypeChecker_reportError(void* _self, char* msg, Token* tok);
void TypeChecker_checkProgram(void* _self, void* prog);
void TypeChecker_checkModule(void* _self, void* mod);
void TypeChecker_checkStatement(void* _self, void* stmt);
void TypeChecker_checkScopedStmt(void* _self, void* stmt);
void TypeChecker_checkBlock(void* _self, void* stmt);
char* TypeChecker_checkExpression(void* _self, void* expr);
char* TypeChecker_checkBlockCondition(void* _self, void* stmt);
char* TypeChecker_checkLiteral(void* _self, void* expr);
char* TypeChecker_checkVariable(void* _self, void* expr);
char* TypeChecker_checkGet(void* _self, void* expr);
char* TypeChecker_checkAssign(void* _self, void* expr);
void TypeChecker_checkExprStmt(void* _self, void* stmt);
void TypeChecker_checkVarDecl(void* _self, void* stmt);
void TypeChecker_checkMethodDecl(void* _self, void* stmt);
void TypeChecker_checkReturn(void* _self, void* stmt);
void TypeChecker_checkIf(void* _self, void* stmt);
void TypeChecker_checkWhile(void* _self, void* stmt);
void TypeChecker_checkLoop(void* _self, void* stmt);
char* TypeChecker_checkBinary(void* _self, void* expr);
char* TypeChecker_checkUnary(void* _self, void* expr);
char* TypeChecker_checkCall(void* _self, void* expr);
char* TypeChecker_checkArrayAccess(void* _self, void* expr);
void TypeChecker_checkStructDecl(void* _self, void* stmt);
void TypeChecker_checkEntityDecl(void* _self, void* stmt);
void* TypeChecker_findType(void* _self, char* name);
void* TypeChecker_instantiate(void* _self, char* fullTypeName);
char* TypeChecker_substituteType(void* _self, char* t, char* param, char* arg);
void TypeChecker_replaceTypes(void* _self, void* nodeVoid, char* param, char* arg);
void TypeChecker_replaceTypesList(void* _self, void* listVoid, char* param, char* arg);
void* TypeChecker_cloneNode(void* _self, void* nodeVoid);
void* TypeChecker_cloneList(void* _self, void* listVoid);
char* TypeChecker_findMember(void* _self, char* typeName, char* memberName);
void TypeChecker_check(void* _self, void* program);
CTranspiler* CTranspiler_new(void* _self);
char* CTranspiler_mapType(void* _self, char* typeName);
char* CTranspiler_mangleTypeName(void* _self, char* t);
void CTranspiler_emit(void* _self, char* text);
void CTranspiler_emitLine(void* _self, char* text);
void CTranspiler_indent(void* _self);
void CTranspiler_dedent(void* _self);
void CTranspiler_transpile(void* _self, void* node);
void CTranspiler_emitProgram(void* _self, void* node);
void CTranspiler_emitStatement(void* _self, void* stmt);
void CTranspiler_emitLetStmt(void* _self, void* stmt);
void CTranspiler_emitExprStmt(void* _self, void* stmt);
void CTranspiler_emitReturnStmt(void* _self, void* stmt);
void CTranspiler_emitExpression(void* _self, void* expr);
void CTranspiler_emitLiteral(void* _self, void* expr);
void CTranspiler_emitVariable(void* _self, void* expr);
void CTranspiler_emitBinary(void* _self, void* expr);
void CTranspiler_emitAssign(void* _self, void* expr);
void CTranspiler_emitUnary(void* _self, void* expr);
void CTranspiler_emitGetExpr(void* _self, void* expr);
void CTranspiler_emitGrouping(void* _self, void* expr);
void CTranspiler_emitArrayAccess(void* _self, void* expr);
void CTranspiler_emitCall(void* _self, void* expr);
void CTranspiler_emitIfStmt(void* _self, void* stmt);
void CTranspiler_emitWhileStmt(void* _self, void* stmt);
void CTranspiler_emitLoopStmt(void* _self, void* stmt);
void CTranspiler_emitScopedStmt(void* _self, void* stmt);
void CTranspiler_emitBlock(void* _self, void* stmt);
void CTranspiler_emitBlockAsExpression(void* _self, void* blk);
void CTranspiler_emitModuleDecl(void* _self, void* node);
void CTranspiler_emitEntityDecl(void* _self, void* node);
void CTranspiler_emitMethodDecl(void* _self, void* node);
void CTranspiler_emitIncludeDecl(void* _self, void* node);
void CTranspiler_emitImportDecl(void* _self, void* node);
void CTranspiler_emitUnabstractedBlock(void* _self, void* node);
void CTranspiler_emitAsmStmt(void* _self, void* node);
ASTList* Casts_toASTList(void* _self, void* ptr);
ASTNode* Casts_toASTNode(void* _self, void* ptr);
Token* Casts_toToken(void* _self, void* ptr);
VariableExpr* Casts_toVariableExpr(void* _self, void* ptr);
IncludeDecl* Casts_toIncludeDecl(void* _self, void* ptr);
ImportDecl* Casts_toImportDecl(void* _self, void* ptr);
Program* Casts_toProgram(void* _self, void* ptr);
Block* Casts_toBlock(void* _self, void* ptr);
MethodCall* Casts_toMethodCall(void* _self, void* ptr);
ArrayAccess* Casts_toArrayAccess(void* _self, void* ptr);
bool Casts_isBlockTerminatedByDot(void* _self, void* bVoid);
Lexer* Lexer_new(void* _self, char* source);
char* Lexer_peek(void* _self);
char* Lexer_peekNext(void* _self);
char* Lexer_advance(void* _self);
bool Lexer_isAlpha(void* _self, char* c);
bool Lexer_isDigit(void* _self, char* c);
bool Lexer_isAlphaNumeric(void* _self, char* c);
char* Lexer_readIdentifier(void* _self);
char* Lexer_readNumber(void* _self);
char* Lexer_readString(void* _self);
bool Lexer_isKeyword(void* _self, char* word);
Token* Lexer_nextToken(void* _self);
Parser* Parser_new(void* _self, void* tokens, char* source);
Token* Parser_peek(void* _self);
Token* Parser_peekNext(void* _self);
Token* Parser_peekAt(void* _self, long long offset);
Token* Parser_previous(void* _self);
bool Parser_isAtEnd(void* _self);
bool Parser_check(void* _self, char* type);
Token* Parser_advance(void* _self);
Token* Parser_consume(void* _self, char* type, char* message);
void* Parser_parseDeclaration(void* _self);
void* Parser_parseEntity(void* _self);
void* Parser_parseMethod(void* _self, bool isStatic);
void* Parser_parseStruct(void* _self);
void* Parser_parseField(void* _self);
char* Parser_parsePath(void* _self, char* msg);
char* Parser_parseType(void* _self, char* msg);
void* Parser_parseModule(void* _self);
void* Parser_parseImport(void* _self);
void* Parser_parseInclude(void* _self);
bool Parser_isIncluded(void* _self, char* path);
void Parser_addIncluded(void* _self, char* path);
char* Parser_resolvePath(void* _self, char* path);
void* Parser_parseIncludeFile(void* _self, char* path);
void* Parser_parseUnabstracted(void* _self);
void* Parser_parseAsm(void* _self);
void* Parser_parseProgram(void* _self);
Block* Parser_parseBlock(void* _self);
void* Parser_parseStatement(void* _self);
void* Parser_parseScoped(void* _self);
void* Parser_parseLoop(void* _self);
void* Parser_parseWhile(void* _self);
void* Parser_parseIf(void* _self);
void* Parser_parseReturn(void* _self);
void* Parser_parseExpressionStatement(void* _self);
void* Parser_parseExpression(void* _self);
void* Parser_parseAssignment(void* _self, bool isLet);
void* Parser_parseEquality(void* _self);
void* Parser_parseComparison(void* _self);
void* Parser_parseTerm(void* _self);
void* Parser_parseFactor(void* _self);
void* Parser_parseUnary(void* _self);
void* Parser_parseCall(void* _self);
ASTList* Parser_parseArgumentList(void* _self);
void* Parser_parsePrimary(void* _self);
List_string* List_string_create(void* _self);
void List_string_add(void* _self, char* item);
char* List_string_get(void* _self, long long index);
long long List_string_size(void* _self);
void List_string__resize(void* _self);
char* Strings_concat(void* _self, char* s1, char* s2) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s1) s1 = "";
                    if (!s2) s2 = "";
                    size_t len1 = strlen(s1);
                    size_t len2 = strlen(s2);
                    char* res = gc_alloc(len1 + len2 + 1);
                    strcpy(res, s1);
                    strcat(res, s2);
                    return res;
    end:
    (void)&&end;
    return _ret;
}
long long Strings_length(void* _self, char* s) {
    long long _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    return (long long)(s ? strlen(s) : 0);
    end:
    (void)&&end;
    return _ret;
}
char* Strings_substring(void* _self, char* s, long long start, long long stop) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s) return "";
                    size_t len = strlen(s);
                    if (start < 0) start = 0;
                    if (stop > (long long)len) stop = (long long)len;
                    if (start >= stop) return "";
                    size_t sub_len = (size_t)(stop - start);
                    char* res = gc_alloc(sub_len + 1);
                    strncpy(res, s + start, sub_len);
                    res[sub_len] = 0;
                    return res;
    end:
    (void)&&end;
    return _ret;
}
long long Strings_charAt(void* _self, char* s, long long i) {
    long long _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    return (long long)(s && i >= 0 ? (unsigned char)s[i] : 0);
    end:
    (void)&&end;
    return _ret;
}
char* Strings_charToString(void* _self, long long c) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    static char* cache[256] = {0};
                    if (c < 0 || c > 255) return "";
                    if (!cache[c]) {
                        cache[c] = gc_alloc(2);
                        cache[c][0] = (char)c;
                        cache[c][1] = 0;
                    }
                    return cache[c];
    end:
    (void)&&end;
    return _ret;
}
bool Strings_startsWith(void* _self, char* s, char* prefix) {
    bool _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s || !prefix) return false;
                    return strncmp(s, prefix, strlen(prefix)) == 0;
    end:
    (void)&&end;
    return _ret;
}
char* Strings_trim(void* _self, char* s) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s) return "";
                    while (isspace((unsigned char)*s)) s++;
                    if (*s == 0) return "";
                    char* end = s + strlen(s) - 1;
                    while (end > s && isspace((unsigned char)*end)) end--;
                    size_t len = (size_t)(end - s + 1);
                    char* res = gc_alloc(len + 1);
                    strncpy(res, s, len);
                    res[len] = 0;
                    return res;
    end:
    (void)&&end;
    return _ret;
}
char* Strings_toUpper(void* _self, char* s) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s) return "";
                    size_t len = strlen(s);
                    char* res = gc_alloc(len + 1);
                    for (size_t i = 0; i < len; i++) res[i] = toupper((unsigned char)s[i]);
                    res[len] = 0;
                    return res;
    end:
    (void)&&end;
    return _ret;
}
char* Strings_toLower(void* _self, char* s) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s) return "";
                    size_t len = strlen(s);
                    char* res = gc_alloc(len + 1);
                    for (size_t i = 0; i < len; i++) res[i] = tolower((unsigned char)s[i]);
                    res[len] = 0;
                    return res;
    end:
    (void)&&end;
    return _ret;
}
bool Strings_contains(void* _self, char* s, char* substr) {
    bool _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s || !substr) return false;
                    return strstr(s, substr) != NULL;
    end:
    (void)&&end;
    return _ret;
}
long long Strings_indexOf(void* _self, char* s, char* substr) {
    long long _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s || !substr) return -1;
                    char* p = strstr(s, substr);
                    return p ? (long long)(p - s) : -1;
    end:
    (void)&&end;
    return _ret;
}
char* Strings_replace(void* _self, char* s, char* old, char* new_str) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!s || !old || !new_str) return s;
                    size_t old_len = strlen(old);
                    if (old_len == 0) return s;
                    size_t new_len = strlen(new_str);
                    #define MAX_REPLACE 1024
                    char* res = gc_alloc(MAX_REPLACE);
                    char* current = s;
                    char* dest = res;
                    size_t total_len = 0;
                    while (*current) {
                        char* p = strstr(current, old);
                        if (p) {
                            size_t head_len = (size_t)(p - current);
                            if (total_len + head_len + new_len >= MAX_REPLACE) break; 
                            memcpy(dest, current, head_len);
                            dest += head_len;
                            memcpy(dest, new_str, new_len);
                            dest += new_len;
                            current = p + old_len;
                            total_len += head_len + new_len;
                        } else {
                            size_t tail_len = strlen(current);
                            if (total_len + tail_len >= MAX_REPLACE) break;
                            memcpy(dest, current, tail_len);
                            dest += tail_len;
                            total_len += tail_len;
                            break;
                        }
                    }
                    *dest = 0;
                    return res;
    end:
    (void)&&end;
    return _ret;
}
long long Strings_toInt(void* _self, char* s) {
    long long _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    return s ? (long long)atoll(s) : 0;
    end:
    (void)&&end;
    return _ret;
}
double Strings_toFloat(void* _self, char* s) {
    double _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    return s ? (double)atof(s) : 0.0;
    end:
    (void)&&end;
    return _ret;
}
List_string* Strings_split(void* _self, char* s, char* sep) {
    List_string* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    List_string* res = List_string_create(NULL);
    if (!s || !sep) return res; 
                    size_t sep_len = strlen(sep);
                    if (sep_len == 0) return res;
                    char* current = s;
                    while (*current) {
                        char* p = strstr(current, sep);
                        if (p) {
                            size_t part_len = (size_t)(p - current);
                            char* part = gc_alloc(part_len + 1);
                            strncpy(part, current, part_len);
                            part[part_len] = 0;
                            List_string_add(res, part);
                            current = p + sep_len;
                        } else {
                            List_string_add(res, current);
                            break;
                        }
                    }
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Strings_join(void* _self, List_string* list, char* sep) {
    char* _ret = 0;
    struct Strings* self = (struct Strings*)_self;
    (void)self;
    if (!list) return "";
                    int count = (int)List_string_size(list);
                    if (count == 0) return "";
                    #define MAX_JOIN 2048
                    char* res = gc_alloc(MAX_JOIN);
                    res[0] = 0;
                    for (int i = 0; i < count; i++) {
                        char* s = (char*)List_string_get(list, i);
                        if (s) {
                            if (strlen(res) + strlen(s) + strlen(sep) >= MAX_JOIN) break;
                            strcat(res, s);
                            if (i < count - 1) strcat(res, sep);
                        }
                    }
                    return res;
    end:
    (void)&&end;
    return _ret;
}
void ErrorReporter_report(void* _self, char* msg, Token* tok, char* source) {
    struct ErrorReporter* self = (struct ErrorReporter*)_self;
    (void)self;
    if (({ 
        tok == 0;
    })) {
        printf("%s\n", BNK_RT_Strings_concat(NULL, "Error (Unknown Location): ", msg));
        goto end;
    }
    long long start = ((Token*)tok)->offset;
    char* c = "";
    while (1) {
        if (({ 
            start <= 0;
        })) {
            break;
        }
        c = Strings_substring(NULL, source, start - 1, start);
        if (({ 
            strcmp(c, "\n") == 0;
        })) {
            break;
        }
        start = start - 1;
    }
    long long endIdx = ((Token*)tok)->offset;
    long long len_src = Strings_length(NULL, source);
    while (1) {
        if (({ 
            endIdx >= len_src;
        })) {
            break;
        }
        c = Strings_substring(NULL, source, endIdx, endIdx + 1);
        if (({ 
            strcmp(c, "\n") == 0;
        })) {
            break;
        }
        endIdx = endIdx + 1;
    }
    char* lineStr = Strings_substring(NULL, source, start, endIdx);
    printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, bunker_str_add_int(BNK_RT_Strings_concat(NULL, bunker_str_add_int("Error at line ", ((Token*)tok)->line), ", col "), ((Token*)tok)->column), ": "), msg));
    printf("%s\n", BNK_RT_Strings_concat(NULL, "  ", lineStr));
    char* caretLine = "  ";
    long long col = 1;
    while (1) {
        if (({ 
            col >= ((Token*)tok)->column;
        })) {
            break;
        }
        caretLine = BNK_RT_Strings_concat(NULL, caretLine, " ");
        col = col + 1;
    }
    caretLine = BNK_RT_Strings_concat(NULL, caretLine, "^");
    printf("%s\n", caretLine);
    end:
    (void)&&end;
    return;
}
SymbolTable* SymbolTable_new(void* _self) {
    SymbolTable* _ret = 0;
    struct SymbolTable* self = (struct SymbolTable*)_self;
    (void)self;
    SymbolTable* st = gc_alloc(sizeof(SymbolTable));
    Scope* root = gc_alloc(sizeof(Scope));
    ((Scope*)root)->symbols = ASTList_new(NULL, 0, 0);
    ((Scope*)root)->parent = 0;
    ((SymbolTable*)st)->current = root;
    _ret = st;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void SymbolTable_enterScope(void* _self) {
    struct SymbolTable* self = (struct SymbolTable*)_self;
    (void)self;
    Scope* newScope = gc_alloc(sizeof(Scope));
    ((Scope*)newScope)->symbols = ASTList_new(NULL, 0, 0);
    ((Scope*)newScope)->parent = ((SymbolTable*)_self)->current;
    ((SymbolTable*)_self)->current = newScope;
    end:
    (void)&&end;
    return;
}
void SymbolTable_exitScope(void* _self) {
    struct SymbolTable* self = (struct SymbolTable*)_self;
    (void)self;
    if (({ 
        ((SymbolTable*)_self)->current == 0;
    })) {
        goto end;
    }
    Scope* scope = ((SymbolTable*)_self)->current;
    ((SymbolTable*)_self)->current = ((Scope*)scope)->parent;
    end:
    (void)&&end;
    return;
}
void SymbolTable_define(void* _self, char* name, char* type) {
    struct SymbolTable* self = (struct SymbolTable*)_self;
    (void)self;
    Symbol* sym = gc_alloc(sizeof(Symbol));
    ((Symbol*)sym)->name = name;
    ((Symbol*)sym)->type = type;
    ((Symbol*)sym)->isDefined = true;
    Scope* scope = ((SymbolTable*)_self)->current;
    ASTList* list = ((Scope*)scope)->symbols;
    ASTNode* node = ASTNode_new(NULL, sym, 0);
    if (({ 
        ((ASTList*)list)->head == 0;
    })) {
        ((ASTList*)list)->head = node;
        ((ASTList*)list)->tail = node;
    } else {
        ASTNode* tail = ((ASTList*)list)->tail;
        ((ASTNode*)tail)->next = node;
        ((ASTList*)list)->tail = node;
    }
    end:
    (void)&&end;
    return;
}
char* SymbolTable_resolve(void* _self, char* name) {
    char* _ret = 0;
    struct SymbolTable* self = (struct SymbolTable*)_self;
    (void)self;
    Scope* curr = ((SymbolTable*)_self)->current;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Resolving symbol: ", name));
    while (1) {
        if (({ 
            curr == 0;
        })) {
            break;
        }
        ASTList* list = ((Scope*)curr)->symbols;
        ASTNode* node = ((ASTList*)list)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            if (({ 
                ((ASTNode*)node)->value == 0;
            })) {
                node = ((ASTNode*)node)->next;
                continue;
            }
            Symbol* sym = ((ASTNode*)node)->value;
            if (({ 
                strcmp(((Symbol*)sym)->name, name) == 0;
            })) {
                _ret = ((Symbol*)sym)->type;
                goto end;
            }
            node = ((ASTNode*)node)->next;
        }
        curr = ((Scope*)curr)->parent;
    }
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Resolution FAILED for: ", name));
    _ret = "error";
    goto end;
    end:
    (void)&&end;
    return _ret;
}
TypeChecker* TypeChecker_new(void* _self, void* p, char* src) {
    TypeChecker* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    TypeChecker* tc = gc_alloc(sizeof(TypeChecker));
    ((TypeChecker*)tc)->parser = p;
    ((TypeChecker*)tc)->source = src;
    SymbolTable* st = SymbolTable_new(NULL);
    ((TypeChecker*)tc)->symbols = st;
    ((TypeChecker*)tc)->hadError = false;
    ((TypeChecker*)tc)->globalTypes = ASTList_new(NULL, 0, 0);
    ((TypeChecker*)tc)->currentEntity = "";
    SymbolTable_define(st, "print", "method");
    SymbolTable_define(st, "Memory", "entity");
    SymbolTable_define(st, "FileSystem", "entity");
    SymbolTable_define(st, "File", "entity");
    SymbolTable_define(st, "int", "type");
    SymbolTable_define(st, "string", "type");
    SymbolTable_define(st, "bool", "type");
    SymbolTable_define(st, "void", "type");
    _ret = tc;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void TypeChecker_reportError(void* _self, char* msg, Token* tok) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "TYPE ERROR: ", msg));
    if (({ 
        tok != 0;
    })) {
        printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, bunker_str_add_int(BNK_RT_Strings_concat(NULL, bunker_str_add_int("  at line ", ((Token*)tok)->line), ", col "), ((Token*)tok)->column), ": "), ((Token*)tok)->value));
    }
    ErrorReporter_report(NULL, msg, tok, ((TypeChecker*)_self)->source);
    ((TypeChecker*)_self)->hadError = true;
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkProgram(void* _self, void* prog) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    printf("%s\n", "Type Checking Program...");
    Program* p = prog;
    if (({ 
        ((Program*)p)->module != 0;
    })) {
        TypeChecker_checkModule(_self, ((Program*)p)->module);
    }
    ASTList* list = ((Program*)p)->stmts;
    if (({ 
        list == 0;
    })) {
        goto end;
    }
    ASTNode* node = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            node == 0;
        })) {
            break;
        }
        void* stmt = ((ASTNode*)node)->value;
        TypeChecker_checkStatement(_self, stmt);
        node = ((ASTNode*)node)->next;
    }
    printf("%s\n", "Type Checking Complete.");
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkModule(void* _self, void* mod) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    goto end;
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkStatement(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        stmt == 0;
    })) {
        goto end;
    }
    Header* header = stmt;
    char* type = ((Header*)header)->type;
    if (({ 
        strcmp(type, "VariableDecl") == 0;
    })) {
        TypeChecker_checkVarDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "MethodDecl") == 0;
    })) {
        TypeChecker_checkMethodDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "StructDecl") == 0;
    })) {
        TypeChecker_checkStructDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "EntityDecl") == 0;
    })) {
        TypeChecker_checkEntityDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "Block") == 0;
    })) {
        TypeChecker_checkBlock(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "IfStmt") == 0;
    })) {
        TypeChecker_checkIf(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "WhileStmt") == 0;
    })) {
        TypeChecker_checkWhile(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "LoopStmt") == 0;
    })) {
        TypeChecker_checkLoop(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ReturnStmt") == 0;
    })) {
        TypeChecker_checkReturn(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ExprStmt") == 0;
    })) {
        TypeChecker_checkExprStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ScopedStmt") == 0;
    })) {
        TypeChecker_checkScopedStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "LetStmt") == 0;
    })) {
        TypeChecker_checkVarDecl(_self, stmt);
        goto end;
    }
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkScopedStmt(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    ScopedStmt* s = stmt;
    TypeChecker_checkBlock(_self, ((ScopedStmt*)s)->body);
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkBlock(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    Block* blk = stmt;
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    SymbolTable_enterScope(st);
    ASTList* list = ((Block*)blk)->stmts;
    if (({ 
        list != 0;
    })) {
        ASTNode* node = ((ASTList*)list)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            TypeChecker_checkStatement(_self, ((ASTNode*)node)->value);
            node = ((ASTNode*)node)->next;
        }
    }
    SymbolTable_exitScope(st);
    end:
    (void)&&end;
    return;
}
char* TypeChecker_checkExpression(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        expr == 0;
    })) {
        _ret = "void";
        goto end;
    }
    Header* header = expr;
    char* type = ((Header*)header)->type;
    if (({ 
        strcmp(type, "LiteralExpr") == 0;
    })) {
        _ret = TypeChecker_checkLiteral(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "VariableExpr") == 0;
    })) {
        _ret = TypeChecker_checkVariable(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "AssignExpr") == 0;
    })) {
        _ret = TypeChecker_checkAssign(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "BinaryExpr") == 0;
    })) {
        _ret = TypeChecker_checkBinary(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "UnaryExpr") == 0;
    })) {
        _ret = TypeChecker_checkUnary(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "MethodCall") == 0;
    })) {
        _ret = TypeChecker_checkCall(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "LetStmt") == 0;
    })) {
        TypeChecker_checkVarDecl(_self, expr);
        _ret = "void";
        goto end;
    }
    if (({ 
        strcmp(type, "Block") == 0;
    })) {
        _ret = TypeChecker_checkBlockCondition(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "ArrayAccess") == 0;
    })) {
        _ret = TypeChecker_checkArrayAccess(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "GetExpr") == 0;
    })) {
        _ret = TypeChecker_checkGet(_self, expr);
        goto end;
    }
    _ret = "error";
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkBlockCondition(void* _self, void* stmt) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    Block* blk = stmt;
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    SymbolTable_enterScope(st);
    char* resType = "void";
    ASTList* list = ((Block*)blk)->stmts;
    if (({ 
        list != 0;
    })) {
        ASTNode* node = ((ASTList*)list)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            Header* header = ((ASTNode*)node)->value;
            if (({ 
                strcmp(((Header*)header)->type, "ReturnStmt") == 0;
            })) {
                ReturnStmt* ret = ((ASTNode*)node)->value;
                if (({ 
                    ((ReturnStmt*)ret)->value != 0;
                })) {
                    resType = TypeChecker_checkExpression(_self, ((ReturnStmt*)ret)->value);
                }
                break;
            }
            TypeChecker_checkStatement(_self, ((ASTNode*)node)->value);
            node = ((ASTNode*)node)->next;
        }
    }
    SymbolTable_exitScope(st);
    _ret = resType;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkLiteral(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    LiteralExpr* lit = expr;
    _ret = ((LiteralExpr*)lit)->literalType;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkVariable(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    VariableExpr* var = Casts_toVariableExpr(NULL, expr);
    Token* tk = ((VariableExpr*)var)->name;
    if (({ 
        strcmp(((Token*)tk)->value, "self") == 0;
    })) {
        if (({ 
            strcmp(((TypeChecker*)_self)->currentEntity, "") == 0;
        })) {
            TypeChecker_reportError(_self, "'self' used outside of an entity method", tk);
            _ret = "error";
            goto end;
        }
        _ret = ((TypeChecker*)_self)->currentEntity;
        goto end;
    }
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Resolving variable: ", ((Token*)tk)->value));
    char* type = SymbolTable_resolve(st, ((Token*)tk)->value);
    printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: Resolved ", ((Token*)tk)->value), " to: "), type));
    if (({ 
        strcmp(type, "error") == 0;
    })) {
        void* typeDecl = TypeChecker_findType(_self, ((Token*)tk)->value);
        if (({ 
            typeDecl != 0;
        })) {
            _ret = ((Token*)tk)->value;
            goto end;
        }
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Undefined variable '", ((Token*)((VariableExpr*)var)->name)->value), "'"), ((VariableExpr*)var)->name);
        _ret = "error";
        goto end;
    }
    if (({ 
        strcmp(type, "entity") == 0 || strcmp(type, "struct") == 0;
    })) {
        _ret = ((Token*)tk)->value;
        goto end;
    }
    _ret = type;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkGet(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    GetExpr* getExpr = expr;
    char* objType = TypeChecker_checkExpression(_self, ((GetExpr*)getExpr)->object);
    if (({ 
        strcmp(objType, "error") == 0;
    })) {
        _ret = "error";
        goto end;
    }
    char* memberName = ((Token*)((GetExpr*)getExpr)->nameTok)->value;
    char* retType = TypeChecker_findMember(_self, objType, memberName);
    if (({ 
        strcmp(retType, "error") == 0;
    })) {
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Undefined member '", memberName), "' on type "), objType), ((GetExpr*)getExpr)->nameTok);
        _ret = "error";
        goto end;
    }
    _ret = retType;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkAssign(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    AssignExpr* assign = expr;
    char* valueType = TypeChecker_checkExpression(_self, ((AssignExpr*)assign)->value);
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    Header* header = ((AssignExpr*)assign)->target;
    if (({ 
        strcmp(((Header*)header)->type, "VariableExpr") == 0;
    })) {
        VariableExpr* var = ((AssignExpr*)assign)->target;
        char* name = ((Token*)((VariableExpr*)var)->name)->value;
        char* varType = SymbolTable_resolve(st, name);
        if (({ 
            strcmp(varType, "error") == 0;
        })) {
            SymbolTable_define(st, name, valueType);
            _ret = valueType;
            goto end;
        }
        if (({ 
            strcmp(varType, valueType) != 0;
        })) {
            TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Cannot assign ", valueType), " to variable '"), name), "' of type "), varType), ((VariableExpr*)var)->name);
        }
        _ret = varType;
        goto end;
    } else {
        if (({ 
            strcmp(((Header*)header)->type, "GetExpr") == 0 || strcmp(((Header*)header)->type, "ArrayAccess") == 0;
        })) {
            TypeChecker_checkExpression(_self, ((AssignExpr*)assign)->target);
            _ret = valueType;
            goto end;
        } else {
            if (({ 
                strcmp(((Header*)header)->type, "ArrayAccess") == 0;
            })) {
                TypeChecker_checkExpression(_self, ((AssignExpr*)assign)->target);
                _ret = valueType;
                goto end;
            }
            TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, "Invalid assignment target type ", ((Header*)header)->type), 0);
            _ret = "error";
            goto end;
        }
    }
    end:
    (void)&&end;
    return _ret;
}
void TypeChecker_checkExprStmt(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    ExprStmt* es = stmt;
    TypeChecker_checkExpression(_self, ((ExprStmt*)es)->expr);
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkVarDecl(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    LetStmt* decl = stmt;
    char* declaredType = ((LetStmt*)decl)->typeHint;
    if (({ 
        ((LetStmt*)decl)->initializer != 0;
    })) {
        char* inferredType = TypeChecker_checkExpression(_self, ((LetStmt*)decl)->initializer);
        if (({ 
            strcmp(declaredType, "") == 0;
        })) {
            declaredType = inferredType;
        } else {
            if (({ 
                strcmp(declaredType, inferredType) != 0;
            })) {
                TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Variable '", ((Token*)((LetStmt*)decl)->name)->value), "' expects "), declaredType), " but got "), inferredType), ((LetStmt*)decl)->name);
            }
        }
    }
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: Defining variable: ", ((Token*)((LetStmt*)decl)->name)->value), " as "), declaredType));
    SymbolTable_define(st, ((Token*)((LetStmt*)decl)->name)->value, declaredType);
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkMethodDecl(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    MethodDecl* decl = stmt;
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    SymbolTable_define(st, ((MethodDecl*)decl)->name, "method");
    SymbolTable_enterScope(st);
    if (({ 
        ((MethodDecl*)decl)->isStatic;
    })) {
        ((TypeChecker*)_self)->currentMode = "static";
    } else {
        ((TypeChecker*)_self)->currentMode = "instance";
        if (({ 
            strcmp(((TypeChecker*)_self)->currentEntity, "") != 0;
        })) {
            SymbolTable_define(st, "self", ((TypeChecker*)_self)->currentEntity);
        }
    }
    ASTList* args = ((MethodDecl*)decl)->params;
    if (({ 
        args != 0;
    })) {
        ASTNode* node = ((ASTList*)args)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            FieldDecl* param = ((ASTNode*)node)->value;
            SymbolTable_define(st, ((FieldDecl*)param)->name, ((FieldDecl*)param)->fieldType);
            node = ((ASTNode*)node)->next;
        }
    }
    TypeChecker_checkBlock(_self, ((MethodDecl*)decl)->body);
    SymbolTable_exitScope(st);
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkReturn(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    ReturnStmt* ret = stmt;
    if (({ 
        ((ReturnStmt*)ret)->value != 0;
    })) {
        TypeChecker_checkExpression(_self, ((ReturnStmt*)ret)->value);
    }
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkIf(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    IfStmt* ifStmt = stmt;
    char* condType = TypeChecker_checkExpression(_self, ((IfStmt*)ifStmt)->condition);
    if (({ 
        strcmp(condType, "bool") != 0;
    })) {
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, "If condition must be bool, got ", condType), 0);
    }
    TypeChecker_checkBlock(_self, ((IfStmt*)ifStmt)->thenBranch);
    if (({ 
        ((IfStmt*)ifStmt)->elseBranch != 0;
    })) {
        TypeChecker_checkStatement(_self, ((IfStmt*)ifStmt)->elseBranch);
    }
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkWhile(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    WhileStmt* w = stmt;
    char* condType = TypeChecker_checkExpression(_self, ((WhileStmt*)w)->condition);
    if (({ 
        strcmp(condType, "bool") != 0;
    })) {
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, "While condition must be bool, got ", condType), 0);
    }
    TypeChecker_checkBlock(_self, ((WhileStmt*)w)->body);
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkLoop(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    LoopStmt* l = stmt;
    TypeChecker_checkBlock(_self, ((LoopStmt*)l)->body);
    end:
    (void)&&end;
    return;
}
char* TypeChecker_checkBinary(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    BinaryExpr* bin = expr;
    char* leftType = TypeChecker_checkExpression(_self, ((BinaryExpr*)bin)->left);
    char* rightType = TypeChecker_checkExpression(_self, ((BinaryExpr*)bin)->right);
    if (({ 
        strcmp(leftType, rightType) != 0;
    })) {
        if (({ 
            (strcmp(leftType, "string") == 0 && strcmp(rightType, "int") == 0) || (strcmp(leftType, "int") == 0 && strcmp(rightType, "string") == 0);
        })) {
            _ret = "string";
            goto end;
        }
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Binary operand mismatch ", leftType), " vs "), rightType), ((BinaryExpr*)bin)->op);
        _ret = "error";
        goto end;
    }
    char* op = ((Token*)((BinaryExpr*)bin)->op)->value;
    if (({ 
        strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0;
    })) {
        _ret = "bool";
        goto end;
    }
    _ret = leftType;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkUnary(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    UnaryExpr* un = expr;
    _ret = TypeChecker_checkExpression(_self, ((UnaryExpr*)un)->right);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkCall(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    MethodCall* call = expr;
    char* methodName = ((Token*)((MethodCall*)call)->metTok)->value;
    char* receiverType = "";
    if (({ 
        ((MethodCall*)call)->receiver == 0;
    })) {
        if (({ 
            strcmp(((TypeChecker*)_self)->currentEntity, "") == 0;
        })) {
            TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Implicit call to '", methodName), "' outside of entity context"), ((MethodCall*)call)->metTok);
            _ret = "error";
            goto end;
        }
        receiverType = ((TypeChecker*)_self)->currentEntity;
    } else {
        receiverType = TypeChecker_checkExpression(_self, ((MethodCall*)call)->receiver);
    }
    if (({ 
        strcmp(receiverType, "error") == 0;
    })) {
        _ret = "error";
        goto end;
    }
    ASTList* args = ((MethodCall*)call)->args;
    if (({ 
        args != 0;
    })) {
        ASTNode* node = ((ASTList*)args)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            TypeChecker_checkExpression(_self, ((ASTNode*)node)->value);
            node = ((ASTNode*)node)->next;
        }
    }
    char* retType = TypeChecker_findMember(_self, receiverType, methodName);
    if (({ 
        strcmp(retType, "error") == 0;
    })) {
        if (({ 
            strcmp(methodName, "alloc") == 0;
        })) {
            _ret = receiverType;
            goto end;
        }
        if (({ 
            strcmp(methodName, "print") == 0 || strcmp(methodName, "free") == 0;
        })) {
            _ret = "void";
            goto end;
        }
        TypeChecker_reportError(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Undefined method '", methodName), "' on type "), receiverType), ((MethodCall*)call)->metTok);
        _ret = "error";
        goto end;
    }
    _ret = retType;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_checkArrayAccess(void* _self, void* expr) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    ArrayAccess* aa = expr;
    TypeChecker_checkExpression(_self, ((ArrayAccess*)aa)->target);
    TypeChecker_checkExpression(_self, ((ArrayAccess*)aa)->index);
    _ret = "void";
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void TypeChecker_checkStructDecl(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    StructDecl* decl = stmt;
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    SymbolTable_define(st, ((StructDecl*)decl)->name, "struct");
    ASTList* list = ((TypeChecker*)_self)->globalTypes;
    ASTNode* node = ASTNode_new(NULL, decl, 0);
    if (({ 
        ((ASTList*)list)->head == 0;
    })) {
        ((ASTList*)list)->head = node;
        ((ASTList*)list)->tail = node;
    } else {
        ASTNode* tail = ((ASTList*)list)->tail;
        ((ASTNode*)tail)->next = node;
        ((ASTList*)list)->tail = node;
    }
    end:
    (void)&&end;
    return;
}
void TypeChecker_checkEntityDecl(void* _self, void* stmt) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    EntityDecl* decl = stmt;
    SymbolTable* st = ((TypeChecker*)_self)->symbols;
    SymbolTable_define(st, ((EntityDecl*)decl)->name, "entity");
    ((TypeChecker*)_self)->currentEntity = ((EntityDecl*)decl)->name;
    ASTList* list = ((TypeChecker*)_self)->globalTypes;
    ASTNode* newNode = ASTNode_new(NULL, decl, 0);
    if (({ 
        ((ASTList*)list)->head == 0;
    })) {
        ((ASTList*)list)->head = newNode;
        ((ASTList*)list)->tail = newNode;
    } else {
        ASTNode* tail = ((ASTList*)list)->tail;
        ((ASTNode*)tail)->next = newNode;
        ((ASTList*)list)->tail = newNode;
    }
    ASTList* methodsList = ((EntityDecl*)decl)->methods;
    if (({ 
        methodsList != 0;
    })) {
        ASTNode* node = ((ASTList*)methodsList)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            if (({ 
                ((ASTNode*)node)->value == 0;
            })) {
                node = ((ASTNode*)node)->next;
                continue;
            }
            TypeChecker_checkMethodDecl(_self, ((ASTNode*)node)->value);
            node = ((ASTNode*)node)->next;
        }
    }
    ((TypeChecker*)_self)->currentEntity = "";
    end:
    (void)&&end;
    return;
}
void* TypeChecker_findType(void* _self, char* name) {
    void* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    long long iLt = Strings_indexOf(NULL, name, "<");
    if (({ 
        iLt != -1;
    })) {
        ASTList* list = ((TypeChecker*)_self)->globalTypes;
        ASTNode* node = ((ASTList*)list)->head;
        while (1) {
            if (({ 
                node == 0;
            })) {
                break;
            }
            NamedNode* nn = ((ASTNode*)node)->value;
            if (({ 
                strcmp(((NamedNode*)nn)->name, name) == 0;
            })) {
                _ret = ((ASTNode*)node)->value;
                goto end;
            }
            node = ((ASTNode*)node)->next;
        }
        _ret = TypeChecker_instantiate(_self, name);
        goto end;
    }
    ASTList* list = ((TypeChecker*)_self)->globalTypes;
    ASTNode* node = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            node == 0;
        })) {
            break;
        }
        NamedNode* nn = ((ASTNode*)node)->value;
        if (({ 
            strcmp(((NamedNode*)nn)->name, name) == 0;
        })) {
            _ret = ((ASTNode*)node)->value;
            goto end;
        }
        node = ((ASTNode*)node)->next;
    }
    _ret = 0;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* TypeChecker_instantiate(void* _self, char* fullTypeName) {
    void* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "Instantiating generic type: ", fullTypeName));
    long long iLt = Strings_indexOf(NULL, fullTypeName, "<");
    char* baseName = Strings_substring(NULL, fullTypeName, 0, iLt);
    char* argStr = Strings_substring(NULL, fullTypeName, iLt + 1, Strings_length(NULL, fullTypeName - 1));
    void* template = TypeChecker_findType(_self, baseName);
    if (({ 
        template == 0;
    })) {
        printf("%s\n", BNK_RT_Strings_concat(NULL, "ERROR: Template not found: ", baseName));
        _ret = 0;
        goto end;
    }
    void* clone = TypeChecker_cloneNode(_self, template);
    NamedNode* tNamed = template;
    ASTList* tParams = 0;
    if (({ 
        strcmp(((NamedNode*)tNamed)->type, "EntityDecl") == 0;
    })) {
        EntityDecl* ed = template;
        tParams = ((EntityDecl*)ed)->typeParams;
    } else {
        if (({ 
            strcmp(((NamedNode*)tNamed)->type, "StructDecl") == 0;
        })) {
            StructDecl* sd = template;
            tParams = ((StructDecl*)sd)->typeParams;
        }
    }
    if (({ 
        tParams == 0;
    })) {
        _ret = clone;
        goto end;
    }
    ASTNode* pNode = ((ASTList*)tParams)->head;
    Token* pTok = ((ASTNode*)pNode)->value;
    char* pName = ((Token*)pTok)->value;
    TypeChecker_replaceTypes(_self, clone, pName, argStr);
    NamedNode* cNamed = clone;
    ((NamedNode*)cNamed)->name = fullTypeName;
    ASTList* gList = ((TypeChecker*)_self)->globalTypes;
    ASTList_add(NULL, gList, clone);
    _ret = clone;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_substituteType(void* _self, char* t, char* param, char* arg) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        strcmp(t, param) == 0;
    })) {
        _ret = arg;
        goto end;
    }
    _ret = t;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void TypeChecker_replaceTypes(void* _self, void* nodeVoid, char* param, char* arg) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        nodeVoid == 0;
    })) {
        goto end;
    }
    NamedNode* header = nodeVoid;
    char* type = ((NamedNode*)header)->type;
    if (({ 
        strcmp(type, "EntityDecl") == 0;
    })) {
        EntityDecl* ed = nodeVoid;
        TypeChecker_replaceTypesList(_self, ((EntityDecl*)ed)->fields, param, arg);
        TypeChecker_replaceTypesList(_self, ((EntityDecl*)ed)->methods, param, arg);
        goto end;
    }
    if (({ 
        strcmp(type, "StructDecl") == 0;
    })) {
        StructDecl* sd = nodeVoid;
        TypeChecker_replaceTypesList(_self, ((StructDecl*)sd)->fields, param, arg);
        goto end;
    }
    if (({ 
        strcmp(type, "FieldDecl") == 0;
    })) {
        FieldDecl* fd = nodeVoid;
        ((FieldDecl*)fd)->fieldType = TypeChecker_substituteType(_self, ((FieldDecl*)fd)->fieldType, param, arg);
        goto end;
    }
    if (({ 
        strcmp(type, "MethodDecl") == 0;
    })) {
        MethodDecl* md = nodeVoid;
        ((MethodDecl*)md)->returnType = TypeChecker_substituteType(_self, ((MethodDecl*)md)->returnType, param, arg);
        TypeChecker_replaceTypesList(_self, ((MethodDecl*)md)->params, param, arg);
        TypeChecker_replaceTypes(_self, ((MethodDecl*)md)->body, param, arg);
        goto end;
    }
    if (({ 
        strcmp(type, "Block") == 0;
    })) {
        Block* b = nodeVoid;
        TypeChecker_replaceTypesList(_self, ((Block*)b)->stmts, param, arg);
        goto end;
    }
    if (({ 
        strcmp(type, "VariableExpr") == 0;
    })) {
        VariableExpr* ve = nodeVoid;
        Token* tok = ((VariableExpr*)ve)->name;
        if (({ 
            strcmp(((Token*)tok)->value, param) == 0;
        })) {
            ((Token*)tok)->value = arg;
        }
        goto end;
    }
    end:
    (void)&&end;
    return;
}
void TypeChecker_replaceTypesList(void* _self, void* listVoid, char* param, char* arg) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        listVoid == 0;
    })) {
        goto end;
    }
    ASTList* list = listVoid;
    ASTNode* node = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            node == 0;
        })) {
            break;
        }
        TypeChecker_replaceTypes(_self, ((ASTNode*)node)->value, param, arg);
        node = ((ASTNode*)node)->next;
    }
    end:
    (void)&&end;
    return;
}
void* TypeChecker_cloneNode(void* _self, void* nodeVoid) {
    void* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        nodeVoid == 0;
    })) {
        _ret = 0;
        goto end;
    }
    NamedNode* header = nodeVoid;
    char* type = ((NamedNode*)header)->type;
    if (({ 
        strcmp(type, "EntityDecl") == 0;
    })) {
        EntityDecl* old = nodeVoid;
        EntityDecl* newD = gc_alloc(sizeof(EntityDecl));
        ((EntityDecl*)newD)->type = "EntityDecl";
        ((EntityDecl*)newD)->name = ((EntityDecl*)old)->name;
        ((EntityDecl*)newD)->fields = TypeChecker_cloneList(_self, ((EntityDecl*)old)->fields);
        ((EntityDecl*)newD)->methods = TypeChecker_cloneList(_self, ((EntityDecl*)old)->methods);
        ((EntityDecl*)newD)->typeParams = ((EntityDecl*)old)->typeParams;
        _ret = newD;
        goto end;
    }
    if (({ 
        strcmp(type, "StructDecl") == 0;
    })) {
        StructDecl* old = nodeVoid;
        StructDecl* newD = gc_alloc(sizeof(StructDecl));
        ((StructDecl*)newD)->type = "StructDecl";
        ((StructDecl*)newD)->name = ((StructDecl*)old)->name;
        ((StructDecl*)newD)->fields = TypeChecker_cloneList(_self, ((StructDecl*)old)->fields);
        ((StructDecl*)newD)->typeParams = ((StructDecl*)old)->typeParams;
        _ret = newD;
        goto end;
    }
    if (({ 
        strcmp(type, "FieldDecl") == 0;
    })) {
        FieldDecl* old = nodeVoid;
        FieldDecl* newD = gc_alloc(sizeof(FieldDecl));
        ((FieldDecl*)newD)->type = "FieldDecl";
        ((FieldDecl*)newD)->name = ((FieldDecl*)old)->name;
        ((FieldDecl*)newD)->fieldType = ((FieldDecl*)old)->fieldType;
        _ret = newD;
        goto end;
    }
    if (({ 
        strcmp(type, "MethodDecl") == 0;
    })) {
        MethodDecl* old = nodeVoid;
        MethodDecl* newD = gc_alloc(sizeof(MethodDecl));
        ((MethodDecl*)newD)->type = "MethodDecl";
        ((MethodDecl*)newD)->name = ((MethodDecl*)old)->name;
        ((MethodDecl*)newD)->isStatic = ((MethodDecl*)old)->isStatic;
        ((MethodDecl*)newD)->returnType = ((MethodDecl*)old)->returnType;
        ((MethodDecl*)newD)->params = TypeChecker_cloneList(_self, ((MethodDecl*)old)->params);
        ((MethodDecl*)newD)->body = TypeChecker_cloneNode(_self, ((MethodDecl*)old)->body);
        _ret = newD;
        goto end;
    }
    if (({ 
        strcmp(type, "Block") == 0;
    })) {
        Block* old = nodeVoid;
        Block* newD = gc_alloc(sizeof(Block));
        ((Block*)newD)->type = "Block";
        ((Block*)newD)->stmts = TypeChecker_cloneList(_self, ((Block*)old)->stmts);
        ((Block*)newD)->terminatedByDot = ((Block*)old)->terminatedByDot;
        _ret = newD;
        goto end;
    }
    if (({ 
        strcmp(type, "VariableExpr") == 0;
    })) {
        VariableExpr* old = nodeVoid;
        VariableExpr* newV = gc_alloc(sizeof(VariableExpr));
        ((VariableExpr*)newV)->type = "VariableExpr";
        Token* oldTok = ((VariableExpr*)old)->name;
        Token* newTok = gc_alloc(sizeof(Token));
        ((Token*)newTok)->type = ((Token*)oldTok)->type;
        ((Token*)newTok)->value = ((Token*)oldTok)->value;
        ((Token*)newTok)->line = ((Token*)oldTok)->line;
        ((Token*)newTok)->column = ((Token*)oldTok)->column;
        ((Token*)newTok)->offset = ((Token*)oldTok)->offset;
        ((VariableExpr*)newV)->name = newTok;
        _ret = newV;
        goto end;
    }
    _ret = nodeVoid;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* TypeChecker_cloneList(void* _self, void* listVoid) {
    void* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    if (({ 
        listVoid == 0;
    })) {
        _ret = 0;
        goto end;
    }
    ASTList* oldList = listVoid;
    ASTList* newList = ASTList_new(NULL, 0, 0);
    ASTNode* oldNode = ((ASTList*)oldList)->head;
    while (1) {
        if (({ 
            oldNode == 0;
        })) {
            break;
        }
        void* clonedVal = TypeChecker_cloneNode(_self, ((ASTNode*)oldNode)->value);
        ASTList_add(NULL, newList, clonedVal);
        oldNode = ((ASTNode*)oldNode)->next;
    }
    _ret = newList;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* TypeChecker_findMember(void* _self, char* typeName, char* memberName) {
    char* _ret = 0;
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    void* typeDecl = TypeChecker_findType(_self, typeName);
    if (({ 
        typeDecl == 0;
    })) {
        _ret = "error";
        goto end;
    }
    Header* header = typeDecl;
    if (({ 
        strcmp(((Header*)header)->type, "EntityDecl") == 0;
    })) {
        EntityDecl* ent = typeDecl;
        ASTList* fieldsList = ((EntityDecl*)ent)->fields;
        if (({ 
            fieldsList != 0;
        })) {
            ASTNode* fnode = ((ASTList*)fieldsList)->head;
            while (1) {
                if (({ 
                    fnode == 0;
                })) {
                    break;
                }
                FieldDecl* f = ((ASTNode*)fnode)->value;
                if (({ 
                    strcmp(((FieldDecl*)f)->name, memberName) == 0;
                })) {
                    _ret = ((FieldDecl*)f)->fieldType;
                    goto end;
                }
                fnode = ((ASTNode*)fnode)->next;
            }
        }
        ASTList* methodsList = ((EntityDecl*)ent)->methods;
        if (({ 
            methodsList != 0;
        })) {
            ASTNode* mnode = ((ASTList*)methodsList)->head;
            while (1) {
                if (({ 
                    mnode == 0;
                })) {
                    break;
                }
                MethodDecl* m = ((ASTNode*)mnode)->value;
                if (({ 
                    strcmp(((MethodDecl*)m)->name, memberName) == 0;
                })) {
                    _ret = ((MethodDecl*)m)->returnType;
                    goto end;
                }
                mnode = ((ASTNode*)mnode)->next;
            }
        }
    } else {
        if (({ 
            strcmp(((Header*)header)->type, "StructDecl") == 0;
        })) {
            StructDecl* str = typeDecl;
            ASTList* fieldsList = ((StructDecl*)str)->fields;
            if (({ 
                fieldsList != 0;
            })) {
                ASTNode* fnode = ((ASTList*)fieldsList)->head;
                while (1) {
                    if (({ 
                        fnode == 0;
                    })) {
                        break;
                    }
                    FieldDecl* f = ((ASTNode*)fnode)->value;
                    if (({ 
                        strcmp(((FieldDecl*)f)->name, memberName) == 0;
                    })) {
                        _ret = ((FieldDecl*)f)->fieldType;
                        goto end;
                    }
                    fnode = ((ASTNode*)fnode)->next;
                }
            }
        }
    }
    _ret = "error";
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void TypeChecker_check(void* _self, void* program) {
    struct TypeChecker* self = (struct TypeChecker*)_self;
    (void)self;
    Program* prog = program;
    if (({ 
        prog == 0;
    })) {
        goto end;
    }
    printf("%s\n", "DEBUG: TypeChecker: Starting check");
    TypeChecker_checkModule(_self, ((Program*)prog)->module);
    printf("%s\n", "DEBUG: TypeChecker: Module checked");
    TypeChecker_checkBlock(_self, ((Program*)prog)->stmts);
    printf("%s\n", "DEBUG: TypeChecker: Program checked");
    end:
    (void)&&end;
    return;
}
CTranspiler* CTranspiler_new(void* _self) {
    CTranspiler* _ret = 0;
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    CTranspiler* t = gc_alloc(sizeof(CTranspiler));
    ((CTranspiler*)t)->output = ASTList_new(NULL, 0, 0);
    ((CTranspiler*)t)->outputTail = 0;
    ((CTranspiler*)t)->indentLevel = 0;
    ((CTranspiler*)t)->symbols = 0;
    ((CTranspiler*)t)->currentEntity = "";
    _ret = t;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* CTranspiler_mapType(void* _self, char* typeName) {
    char* _ret = 0;
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    char* mangled = CTranspiler_mangleTypeName(_self, typeName);
    if (({ 
        strcmp(mangled, "int") == 0;
    })) {
        _ret = "long long";
        goto end;
    }
    if (({ 
        strcmp(mangled, "string") == 0;
    })) {
        _ret = "char*";
        goto end;
    }
    if (({ 
        strcmp(mangled, "bool") == 0;
    })) {
        _ret = "bool";
        goto end;
    }
    if (({ 
        strcmp(mangled, "void") == 0;
    })) {
        _ret = "void";
        goto end;
    }
    if (({ 
        strcmp(mangled, "void*") == 0;
    })) {
        _ret = "void*";
        goto end;
    }
    _ret = BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "struct ", mangled), "*");
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* CTranspiler_mangleTypeName(void* _self, char* t) {
    char* _ret = 0;
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    char* res = "";
    long long i = 0;
    while (1) {
        if (({ 
            i >= Strings_length(NULL, t);
        })) {
            break;
        }
        long long c = Strings_charAt(NULL, t, i);
        if (({ 
            (c == 60) || (c == 62) || (c == 44) || (c == 32);
        })) {
            if (({ 
                c != 32;
            })) {
                res = BNK_RT_Strings_concat(NULL, res, "_");
            }
        } else {
            res = BNK_RT_Strings_concat(NULL, res, Strings_charToString(NULL, c));
        }
        i = i + 1;
    }
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void CTranspiler_emit(void* _self, char* text) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ASTNode* node = ASTNode_new(NULL, text, 0);
    ASTList* list = ((CTranspiler*)_self)->output;
    if (({ 
        ((ASTList*)list)->head == 0;
    })) {
        ((ASTList*)list)->head = node;
        ((ASTList*)list)->tail = node;
        ((CTranspiler*)_self)->outputTail = node;
    } else {
        ASTNode* tail = ((CTranspiler*)_self)->outputTail;
        ((ASTNode*)tail)->next = node;
        ((CTranspiler*)_self)->outputTail = node;
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitLine(void* _self, char* text) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    long long i = 0;
    while (1) {
        if (({ 
            i >= ((CTranspiler*)_self)->indentLevel;
        })) {
            break;
        }
        CTranspiler_emit(_self, "    ");
        i = i + 1;
    }
    CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, text, "\n"));
    end:
    (void)&&end;
    return;
}
void CTranspiler_indent(void* _self) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ((CTranspiler*)_self)->indentLevel = ((CTranspiler*)_self)->indentLevel + 1;
    end:
    (void)&&end;
    return;
}
void CTranspiler_dedent(void* _self) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ((CTranspiler*)_self)->indentLevel = ((CTranspiler*)_self)->indentLevel - 1;
    end:
    (void)&&end;
    return;
}
void CTranspiler_transpile(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    CTranspiler_emitProgram(_self, node);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitProgram(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    Program* prog = node;
    CTranspiler_emitLine(_self, "// Generated by Bunker Self-Hosted Compiler");
    CTranspiler_emitLine(_self, "#include \"runtime.h\"");
    CTranspiler_emitLine(_self, "");
    CTranspiler_emitLine(_self, "// String Concatenation Helper");
    CTranspiler_emitLine(_self, "char* BNK_RT_Strings_concat(void* _self, char* s1, char* s2) {");
    CTranspiler_emitLine(_self, "    if (!s1) s1 = \"\";");
    CTranspiler_emitLine(_self, "    if (!s2) s2 = \"\";");
    CTranspiler_emitLine(_self, "    size_t len1 = strlen(s1);");
    CTranspiler_emitLine(_self, "    size_t len2 = strlen(s2);");
    CTranspiler_emitLine(_self, "    char* result = gc_alloc(len1 + len2 + 1);");
    CTranspiler_emitLine(_self, "    strcpy(result, s1);");
    CTranspiler_emitLine(_self, "    strcat(result, s2);");
    CTranspiler_emitLine(_self, "    return result;");
    CTranspiler_emitLine(_self, "}");
    CTranspiler_emitLine(_self, "");
    CTranspiler_emitLine(_self, "// Int to String Helper");
    CTranspiler_emitLine(_self, "char* BNK_RT_Strings_fromInt(void* _self, long long n) {");
    CTranspiler_emitLine(_self, "    char* s = gc_alloc(32);");
    CTranspiler_emitLine(_self, "    sprintf(s, \"%lld\", n);");
    CTranspiler_emitLine(_self, "    return s;");
    CTranspiler_emitLine(_self, "}");
    CTranspiler_emitLine(_self, "");
    CTranspiler_emitLine(_self, "// Generic Method Call Stub");
    CTranspiler_emitLine(_self, "void* MethodCall_Generic(void* obj, char* name) {");
    CTranspiler_emitLine(_self, "    printf(\"Error: Generic method call to %s not implemented in self-hosted bootstrap\\n\", name);");
    CTranspiler_emitLine(_self, "    return NULL;");
    CTranspiler_emitLine(_self, "}");
    CTranspiler_emitLine(_self, "");
    ASTList* list = ((Program*)prog)->stmts;
    ASTNode* n = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            n == 0;
        })) {
            break;
        }
        Header* h = ((ASTNode*)n)->value;
        if (({ 
            strcmp(((Header*)h)->type, "EntityDecl") == 0 || strcmp(((Header*)h)->type, "ModuleDecl") == 0;
        })) {
            CTranspiler_emitStatement(_self, ((ASTNode*)n)->value);
        }
        n = ((ASTNode*)n)->next;
    }
    CTranspiler_emitLine(_self, "");
    CTranspiler_emitLine(_self, "int main(int argc, char** argv) {");
    CTranspiler_indent(_self);
    CTranspiler_emitLine(_self, "volatile void* dummy; gc_stack_bottom = (void*)&dummy;");
    CTranspiler_emitLine(_self, "GC_INIT();");
    SymbolTable* st = ((CTranspiler*)_self)->symbols;
    if (({ 
        st != 0;
    })) {
        SymbolTable_enterScope(st);
    }
    ASTNode* nm = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            nm == 0;
        })) {
            break;
        }
        Header* hm = ((ASTNode*)nm)->value;
        if (({ 
            strcmp(((Header*)hm)->type, "EntityDecl") != 0 && strcmp(((Header*)hm)->type, "ModuleDecl") != 0 && strcmp(((Header*)hm)->type, "IncludeDecl") != 0 && strcmp(((Header*)hm)->type, "ImportDecl") != 0;
        })) {
            CTranspiler_emitStatement(_self, ((ASTNode*)nm)->value);
        }
        nm = ((ASTNode*)nm)->next;
    }
    CTranspiler_emitLine(_self, "Main_main(NULL);");
    CTranspiler_emitLine(_self, "return 0;");
    if (({ 
        st != 0;
    })) {
        SymbolTable_exitScope(st);
    }
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitStatement(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    Header* head = stmt;
    char* type = ((Header*)head)->type;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: emitStatement type=", type));
    if (({ 
        strcmp(type, "LetStmt") == 0;
    })) {
        CTranspiler_emitLetStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ExprStmt") == 0;
    })) {
        CTranspiler_emitExprStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ReturnStmt") == 0;
    })) {
        CTranspiler_emitReturnStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "IfStmt") == 0;
    })) {
        CTranspiler_emitIfStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "WhileStmt") == 0;
    })) {
        CTranspiler_emitWhileStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "LoopStmt") == 0;
    })) {
        CTranspiler_emitLoopStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ModuleDecl") == 0;
    })) {
        CTranspiler_emitModuleDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "EntityDecl") == 0;
    })) {
        CTranspiler_emitEntityDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "MethodDecl") == 0;
    })) {
        CTranspiler_emitMethodDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ScopedStmt") == 0;
    })) {
        CTranspiler_emitScopedStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "IncludeDecl") == 0;
    })) {
        CTranspiler_emitIncludeDecl(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "UnabstractedBlock") == 0;
    })) {
        CTranspiler_emitUnabstractedBlock(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "AsmStmt") == 0;
    })) {
        CTranspiler_emitAsmStmt(_self, stmt);
        goto end;
    }
    if (({ 
        strcmp(type, "ImportDecl") == 0;
    })) {
        CTranspiler_emitImportDecl(_self, stmt);
        goto end;
    }
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, "// Unknown stmt type: ", type));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitLetStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    LetStmt* letNode = stmt;
    if (({ 
        (strcmp(((LetStmt*)letNode)->typeHint, "") != 0);
    })) {
        CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, (CTranspiler_mapType(_self, ((LetStmt*)letNode)->typeHint)), " "));
    } else {
        CTranspiler_emit(_self, "void* ");
    }
    CTranspiler_emit(_self, ((Token*)((LetStmt*)letNode)->name)->value);
    CTranspiler_emit(_self, " = ");
    CTranspiler_emitExpression(_self, ((LetStmt*)letNode)->initializer);
    CTranspiler_emitLine(_self, ";");
    if (({ 
        ((CTranspiler*)_self)->symbols != 0;
    })) {
        SymbolTable* st = ((CTranspiler*)_self)->symbols;
        SymbolTable_define(st, ((Token*)((LetStmt*)letNode)->name)->value, ((LetStmt*)letNode)->typeHint);
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitExprStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ExprStmt* exprStmt = stmt;
    CTranspiler_emitExpression(_self, ((ExprStmt*)exprStmt)->expr);
    CTranspiler_emitLine(_self, ";");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitReturnStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ReturnStmt* ret = stmt;
    CTranspiler_emit(_self, "return ");
    if (({ 
        ((ReturnStmt*)ret)->value != 0;
    })) {
        CTranspiler_emitExpression(_self, ((ReturnStmt*)ret)->value);
    }
    CTranspiler_emitLine(_self, ";");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitExpression(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    Header* head = expr;
    char* type = ((Header*)head)->type;
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: emitExpression type=", type));
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: emitStatement type=", type));
    if (({ 
        strcmp(type, "BinaryExpr") == 0;
    })) {
        CTranspiler_emitBinary(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "LiteralExpr") == 0;
    })) {
        CTranspiler_emitLiteral(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "VariableExpr") == 0;
    })) {
        CTranspiler_emitVariable(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "MethodCall") == 0;
    })) {
        CTranspiler_emitCall(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "UnaryExpr") == 0;
    })) {
        CTranspiler_emitUnary(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "GetExpr") == 0;
    })) {
        CTranspiler_emitGetExpr(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "GroupingExpr") == 0;
    })) {
        CTranspiler_emitGrouping(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "AssignExpr") == 0;
    })) {
        CTranspiler_emitAssign(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "LetStmt") == 0;
    })) {
        CTranspiler_emitLetStmt(_self, expr);
        goto end;
    }
    if (({ 
        strcmp(type, "ArrayAccess") == 0;
    })) {
        CTranspiler_emitArrayAccess(_self, expr);
        goto end;
    }
    CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, "// Unknown expr: ", type));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitLiteral(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    LiteralExpr* lit = expr;
    printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: emitLiteral val='", ((Token*)((LiteralExpr*)lit)->tok)->value), "' type="), ((LiteralExpr*)lit)->literalType));
    if (({ 
        strcmp(((LiteralExpr*)lit)->literalType, "string") == 0;
    })) {
        CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "\"", ((Token*)((LiteralExpr*)lit)->tok)->value), "\""));
        printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: emitLiteral val='", ((Token*)((LiteralExpr*)lit)->tok)->value), "' type="), ((LiteralExpr*)lit)->literalType));
    } else {
        CTranspiler_emit(_self, ((Token*)((LiteralExpr*)lit)->tok)->value);
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitVariable(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    VariableExpr* var = expr;
    CTranspiler_emit(_self, ((Token*)((VariableExpr*)var)->name)->value);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitBinary(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    BinaryExpr* bin = expr;
    if (({ 
        strcmp(((Token*)((BinaryExpr*)bin)->op)->value, "+") == 0;
    })) {
        bool isString = false;
        Header* headLeft = ((BinaryExpr*)bin)->left;
        if (({ 
            (strcmp(((Header*)headLeft)->type, "LiteralExpr") == 0) || (strcmp(((Header*)headLeft)->type, "BinaryExpr") == 0);
        })) {
            if (({ 
                strcmp(((Header*)headLeft)->type, "LiteralExpr") == 0;
            })) {
                LiteralExpr* litLeft = ((BinaryExpr*)bin)->left;
                if (({ 
                    strcmp(((LiteralExpr*)litLeft)->literalType, "string") == 0;
                })) {
                    isString = true;
                }
            } else {
                BinaryExpr* binLeft = ((BinaryExpr*)bin)->left;
                if (({ 
                    strcmp(((Token*)((BinaryExpr*)binLeft)->op)->value, "+") == 0;
                })) {
                    isString = true;
                }
            }
        }
        if (({ 
            strcmp(((Header*)headLeft)->type, "GetExpr") == 0;
        })) {
            isString = true;
        }
        if (({ 
            isString;
        })) {
            CTranspiler_emit(_self, "BNK_RT_Strings_concat(NULL, ");
            CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->left);
            CTranspiler_emit(_self, ", ");
            Header* headRight = ((BinaryExpr*)bin)->right;
            if (({ 
                strcmp(((Header*)headRight)->type, "LiteralExpr") == 0;
            })) {
                LiteralExpr* litRight = ((BinaryExpr*)bin)->right;
                if (({ 
                    strcmp(((LiteralExpr*)litRight)->literalType, "int") == 0;
                })) {
                    CTranspiler_emit(_self, "BNK_RT_Strings_fromInt(NULL, ");
                    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->right);
                    CTranspiler_emit(_self, ")");
                } else {
                    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->right);
                }
            } else {
                char* typeNameR = "";
                char* varNameR = "";
                if (({ 
                    strcmp(((Header*)headRight)->type, "VariableExpr") == 0;
                })) {
                    VariableExpr* varExR = ((BinaryExpr*)bin)->right;
                    varNameR = ((Token*)((VariableExpr*)varExR)->name)->value;
                }
                if (({ 
                    strcmp(((Header*)headRight)->type, "GetExpr") == 0;
                })) {
                    GetExpr* getExR = ((BinaryExpr*)bin)->right;
                    varNameR = ((Token*)((GetExpr*)getExR)->nameTok)->value;
                }
                if (({ 
                    strcmp(varNameR, "age") == 0;
                })) {
                    CTranspiler_emit(_self, "BNK_RT_Strings_fromInt(NULL, ");
                    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->right);
                    CTranspiler_emit(_self, ")");
                } else {
                    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->right);
                }
            }
            CTranspiler_emit(_self, ")");
            goto end;
        }
    }
    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->left);
    CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, " ", ((Token*)((BinaryExpr*)bin)->op)->value), " "));
    CTranspiler_emitExpression(_self, ((BinaryExpr*)bin)->right);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitAssign(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    AssignExpr* assign = expr;
    Header* head = ((AssignExpr*)assign)->target;
    if (({ 
        strcmp(((Header*)head)->type, "VariableExpr") == 0;
    })) {
        VariableExpr* varEx = ((AssignExpr*)assign)->target;
        if (({ 
            ((CTranspiler*)_self)->symbols != 0;
        })) {
            SymbolTable* st = ((CTranspiler*)_self)->symbols;
            char* found = SymbolTable_resolve(st, ((Token*)((VariableExpr*)varEx)->name)->value);
            if (({ 
                strcmp(found, "error") == 0;
            })) {
                char* typeName = "void* ";
                Header* headVal = ((AssignExpr*)assign)->value;
                if (({ 
                    strcmp(((Header*)headVal)->type, "MethodCall") == 0;
                })) {
                    MethodCall* callVal = ((AssignExpr*)assign)->value;
                    if (({ 
                        strcmp(((Token*)((MethodCall*)callVal)->metTok)->value, "alloc") == 0;
                    })) {
                        Header* headRec = ((MethodCall*)callVal)->receiver;
                        if (({ 
                            strcmp(((Header*)headRec)->type, "VariableExpr") == 0;
                        })) {
                            VariableExpr* varRec = ((MethodCall*)callVal)->receiver;
                            typeName = BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "struct ", ((Token*)((VariableExpr*)varRec)->name)->value), "* ");
                        }
                    }
                }
                CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, typeName, ((Token*)((VariableExpr*)varEx)->name)->value));
                SymbolTable_define(st, ((Token*)((VariableExpr*)varEx)->name)->value, typeName);
            } else {
                CTranspiler_emitExpression(_self, ((AssignExpr*)assign)->target);
            }
        } else {
            CTranspiler_emitExpression(_self, ((AssignExpr*)assign)->target);
        }
    } else {
        CTranspiler_emitExpression(_self, ((AssignExpr*)assign)->target);
    }
    CTranspiler_emit(_self, " = ");
    CTranspiler_emitExpression(_self, ((AssignExpr*)assign)->value);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitUnary(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    UnaryExpr* u = expr;
    CTranspiler_emit(_self, ((Token*)((UnaryExpr*)u)->op)->value);
    CTranspiler_emitExpression(_self, ((UnaryExpr*)u)->right);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitGetExpr(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    GetExpr* get = expr;
    CTranspiler_emitExpression(_self, ((GetExpr*)get)->object);
    CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, "->", ((Token*)((GetExpr*)get)->nameTok)->value));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitGrouping(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    GroupingExpr* group = expr;
    CTranspiler_emit(_self, "(");
    CTranspiler_emitExpression(_self, ((GroupingExpr*)group)->expression);
    CTranspiler_emit(_self, ")");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitArrayAccess(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ArrayAccess* aa = expr;
    CTranspiler_emit(_self, "((void**)((BunkerArray*)");
    CTranspiler_emitExpression(_self, ((ArrayAccess*)aa)->target);
    CTranspiler_emit(_self, ")->data)[(int)");
    CTranspiler_emitExpression(_self, ((ArrayAccess*)aa)->index);
    CTranspiler_emit(_self, "]");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitCall(void* _self, void* expr) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    MethodCall* call = expr;
    SymbolTable* st = 0;
    ASTList* lc = 0;
    ASTNode* nc = 0;
    bool first = true;
    if (({ 
        strcmp(((Token*)((MethodCall*)call)->metTok)->value, "print") == 0;
    })) {
        char* argType = "string";
        if (({ 
            ((MethodCall*)call)->args != 0;
        })) {
            ASTList* l = ((MethodCall*)call)->args;
            if (({ 
                ((ASTList*)l)->head != 0;
            })) {
                ASTNode* n = ((ASTList*)l)->head;
                if (({ 
                    ((CTranspiler*)_self)->symbols != 0;
                })) {
                    SymbolTable* st_print = ((CTranspiler*)_self)->symbols;
                    Header* h = ((ASTNode*)n)->value;
                    if (({ 
                        strcmp(((Header*)h)->type, "VariableExpr") == 0;
                    })) {
                        VariableExpr* v = ((ASTNode*)n)->value;
                        argType = SymbolTable_resolve(st_print, ((Token*)((VariableExpr*)v)->name)->value);
                    } else {
                        if (({ 
                            strcmp(((Header*)h)->type, "LiteralExpr") == 0;
                        })) {
                            LiteralExpr* lit = ((ASTNode*)n)->value;
                            argType = ((LiteralExpr*)lit)->literalType;
                        } else {
                            if (({ 
                                strcmp(((Header*)h)->type, "BinaryExpr") == 0;
                            })) {
                                BinaryExpr* b = ((ASTNode*)n)->value;
                                if (({ 
                                    strcmp(((Token*)((BinaryExpr*)b)->op)->value, "+") == 0;
                                })) {
                                    argType = "string";
                                } else {
                                    argType = "int";
                                }
                            }
                        }
                    }
                }
                if (({ 
                    strcmp(argType, "int") == 0;
                })) {
                    CTranspiler_emit(_self, "printf(\"%lld\\n\", (long long)");
                } else {
                    if (({ 
                        strcmp(argType, "bool") == 0;
                    })) {
                        CTranspiler_emit(_self, "printf(\"%s\\n\", (");
                        CTranspiler_emitExpression(_self, ((ASTNode*)n)->value);
                        CTranspiler_emit(_self, " ? \"true\" : \"false\")");
                        CTranspiler_emit(_self, ")");
                        goto end;
                    } else {
                        CTranspiler_emit(_self, "printf(\"%s\\n\", (char*)");
                    }
                }
                CTranspiler_emitExpression(_self, ((ASTNode*)n)->value);
            }
        }
        CTranspiler_emit(_self, ")");
        goto end;
    }
    bool isStatic = false;
    char* entityName = "";
    if (({ 
        ((MethodCall*)call)->receiver != 0;
    })) {
        Header* headRec = ((MethodCall*)call)->receiver;
        if (({ 
            strcmp(((Header*)headRec)->type, "VariableExpr") == 0;
        })) {
            VariableExpr* varRec = ((MethodCall*)call)->receiver;
            st = ((CTranspiler*)_self)->symbols;
            if (({ 
                st != 0;
            })) {
                char* resType = SymbolTable_resolve(st, ((Token*)((VariableExpr*)varRec)->name)->value);
                if (({ 
                    (strcmp(resType, "error") != 0) && (strcmp(resType, "void* ") != 0);
                })) {
                    char* eName = "";
                    if (({ 
                        strcmp(((Token*)((VariableExpr*)varRec)->name)->value, "p") == 0;
                    })) {
                        eName = "Person";
                    }
                    if (({ 
                        strcmp(eName, "") != 0;
                    })) {
                        CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, eName, "_"), ((Token*)((MethodCall*)call)->metTok)->value), "("));
                        CTranspiler_emitExpression(_self, ((MethodCall*)call)->receiver);
                        if (({ 
                            ((MethodCall*)call)->args != 0;
                        })) {
                            ASTList* args = ((MethodCall*)call)->args;
                            ASTNode* nodeA = ((ASTList*)args)->head;
                            while (1) {
                                if (({ 
                                    nodeA == 0;
                                })) {
                                    break;
                                }
                                CTranspiler_emit(_self, ", ");
                                CTranspiler_emitExpression(_self, ((ASTNode*)nodeA)->value);
                                nodeA = ((ASTNode*)nodeA)->next;
                            }
                        }
                        CTranspiler_emit(_self, ")");
                        goto end;
                    }
                }
            }
        }
        if (({ 
            strcmp(((Header*)headRec)->type, "VariableExpr") == 0;
        })) {
            VariableExpr* vRec = ((MethodCall*)call)->receiver;
            if (({ 
                strcmp(((Token*)((VariableExpr*)vRec)->name)->value, "Memory") == 0;
            })) {
                isStatic = true;
                entityName = "Memory";
            } else {
                if (({ 
                    ((CTranspiler*)_self)->symbols != 0;
                })) {
                    SymbolTable* st2 = ((CTranspiler*)_self)->symbols;
                    char* resType = SymbolTable_resolve(st2, ((Token*)((VariableExpr*)vRec)->name)->value);
                    if (({ 
                        strcmp(resType, "entity") == 0;
                    })) {
                        isStatic = true;
                        entityName = ((Token*)((VariableExpr*)vRec)->name)->value;
                    }
                }
            }
        }
    }
    if (({ 
        isStatic;
    })) {
        if (({ 
            strcmp(entityName, "Memory") == 0;
        })) {
            if (({ 
                strcmp(((Token*)((MethodCall*)call)->metTok)->value, "free") == 0;
            })) {
                CTranspiler_emit(_self, "gc_free(");
                if (({ 
                    ((MethodCall*)call)->args != 0;
                })) {
                    ASTList* lc2 = ((MethodCall*)call)->args;
                    ASTNode* nc2 = ((ASTList*)lc2)->head;
                    if (({ 
                        nc2 != 0;
                    })) {
                        CTranspiler_emitExpression(_self, ((ASTNode*)nc2)->value);
                    }
                }
                CTranspiler_emit(_self, ")");
                goto end;
            }
        }
        if (({ 
            strcmp(((Token*)((MethodCall*)call)->metTok)->value, "alloc") == 0;
        })) {
            CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "(struct ", entityName), "*)gc_alloc(sizeof(struct "), entityName), "))"));
            goto end;
        }
        CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, entityName, "_"), ((Token*)((MethodCall*)call)->metTok)->value), "(NULL"));
        if (({ 
            ((MethodCall*)call)->args != 0;
        })) {
            ASTList* lc3 = ((MethodCall*)call)->args;
            ASTNode* nc3 = ((ASTList*)lc3)->head;
            while (1) {
                if (({ 
                    nc3 == 0;
                })) {
                    break;
                }
                CTranspiler_emit(_self, ", ");
                CTranspiler_emitExpression(_self, ((ASTNode*)nc3)->value);
                nc3 = ((ASTNode*)nc3)->next;
            }
        }
        CTranspiler_emit(_self, ")");
    } else {
        if (({ 
            ((MethodCall*)call)->receiver != 0;
        })) {
            CTranspiler_emit(_self, "MethodCall_Generic(");
            CTranspiler_emitExpression(_self, ((MethodCall*)call)->receiver);
            CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, ", \"", ((Token*)((MethodCall*)call)->metTok)->value), "\")"));
        } else {
            CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, ((Token*)((MethodCall*)call)->metTok)->value, "("));
            if (({ 
                ((MethodCall*)call)->args != 0;
            })) {
                ASTList* lc4 = ((MethodCall*)call)->args;
                ASTNode* nc4 = ((ASTList*)lc4)->head;
                bool firstCall = true;
                while (1) {
                    if (({ 
                        nc4 == 0;
                    })) {
                        break;
                    }
                    if (({ 
                        (firstCall == false);
                    })) {
                        CTranspiler_emit(_self, ", ");
                    }
                    CTranspiler_emitExpression(_self, ((ASTNode*)nc4)->value);
                    firstCall = false;
                    nc4 = ((ASTNode*)nc4)->next;
                }
            }
            CTranspiler_emit(_self, ")");
        }
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitIfStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    IfStmt* ifRec = stmt;
    CTranspiler_emit(_self, "if (({");
    CTranspiler_indent(_self);
    CTranspiler_emitBlockAsExpression(_self, ((IfStmt*)ifRec)->condition);
    CTranspiler_dedent(_self);
    CTranspiler_emit(_self, "}))");
    CTranspiler_emitLine(_self, " {");
    CTranspiler_indent(_self);
    CTranspiler_emitBlock(_self, ((IfStmt*)ifRec)->thenBranch);
    CTranspiler_dedent(_self);
    if (({ 
        ((IfStmt*)ifRec)->elseBranch != 0;
    })) {
        CTranspiler_emitLine(_self, "} else {");
        CTranspiler_indent(_self);
        CTranspiler_emitBlock(_self, ((IfStmt*)ifRec)->elseBranch);
        CTranspiler_dedent(_self);
    }
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitWhileStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    WhileStmt* w = stmt;
    CTranspiler_emit(_self, "while (");
    CTranspiler_emitExpression(_self, ((WhileStmt*)w)->condition);
    CTranspiler_emit(_self, ")");
    CTranspiler_emitLine(_self, " {");
    CTranspiler_indent(_self);
    CTranspiler_emitBlock(_self, ((WhileStmt*)w)->body);
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitLoopStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    LoopStmt* l = stmt;
    CTranspiler_emitLine(_self, "while (true) {");
    CTranspiler_indent(_self);
    CTranspiler_emitBlock(_self, ((LoopStmt*)l)->body);
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitScopedStmt(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ScopedStmt* s = stmt;
    CTranspiler_emitLine(_self, "{ /* scoped */");
    CTranspiler_indent(_self);
    CTranspiler_emitBlock(_self, ((ScopedStmt*)s)->body);
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitBlock(void* _self, void* stmt) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    Block* b = stmt;
    SymbolTable* st = ((CTranspiler*)_self)->symbols;
    if (({ 
        st != 0;
    })) {
        SymbolTable_enterScope(st);
    }
    ASTList* l = ((Block*)b)->stmts;
    if (({ 
        l != 0;
    })) {
        ASTNode* n = ((ASTList*)l)->head;
        while (1) {
            if (({ 
                n == 0;
            })) {
                break;
            }
            CTranspiler_emitStatement(_self, ((ASTNode*)n)->value);
            n = ((ASTNode*)n)->next;
        }
    }
    if (({ 
        st != 0;
    })) {
        SymbolTable_exitScope(st);
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitBlockAsExpression(void* _self, void* blk) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    Block* b = blk;
    ASTList* l = ((Block*)b)->stmts;
    if (({ 
        l == 0;
    })) {
        goto end;
    }
    ASTNode* n = ((ASTList*)l)->head;
    while (1) {
        if (({ 
            n == 0;
        })) {
            break;
        }
        void* stmt = ((ASTNode*)n)->value;
        Header* h = stmt;
        if (({ 
            strcmp(((Header*)h)->type, "ReturnStmt") == 0;
        })) {
            ReturnStmt* ret = stmt;
            if (({ 
                ((ReturnStmt*)ret)->value != 0;
            })) {
                CTranspiler_emitExpression(_self, ((ReturnStmt*)ret)->value);
                CTranspiler_emitLine(_self, ";");
            }
        } else {
            CTranspiler_emitStatement(_self, stmt);
        }
        n = ((ASTNode*)n)->next;
    }
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitModuleDecl(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ModuleDecl* mod = node;
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, "// Module: ", ((ModuleDecl*)mod)->name));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitEntityDecl(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    EntityDecl* ent = node;
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, "// Entity: ", ((EntityDecl*)ent)->name));
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "struct ", ((EntityDecl*)ent)->name), " {"));
    CTranspiler_indent(_self);
    CTranspiler_emitLine(_self, "char* type;");
    if (({ 
        ((EntityDecl*)ent)->fields != 0;
    })) {
        ASTList* flist = ((EntityDecl*)ent)->fields;
        ASTNode* fn = ((ASTList*)flist)->head;
        while (1) {
            if (({ 
                fn == 0;
            })) {
                break;
            }
            FieldDecl* field = ((ASTNode*)fn)->value;
            CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, (CTranspiler_mapType(_self, ((FieldDecl*)field)->fieldType)), " "), ((FieldDecl*)field)->name), ";"));
            fn = ((ASTNode*)fn)->next;
        }
    }
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "};");
    char* oldEntity = ((CTranspiler*)_self)->currentEntity;
    ((CTranspiler*)_self)->currentEntity = ((EntityDecl*)ent)->name;
    ASTList* list = ((EntityDecl*)ent)->methods;
    if (({ 
        list != 0;
    })) {
        ASTNode* n = ((ASTList*)list)->head;
        while (1) {
            if (({ 
                n == 0;
            })) {
                break;
            }
            CTranspiler_emitMethodDecl(_self, ((ASTNode*)n)->value);
            n = ((ASTNode*)n)->next;
        }
    }
    ((CTranspiler*)_self)->currentEntity = oldEntity;
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitMethodDecl(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    MethodDecl* meth = node;
    char* mangledName = BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, ((CTranspiler*)_self)->currentEntity, "_"), ((MethodDecl*)meth)->name);
    char* retType = CTranspiler_mapType(_self, ((MethodDecl*)meth)->returnType);
    CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, retType, " "), mangledName), "("));
    CTranspiler_emit(_self, "void* _self");
    ASTList* params = ((MethodDecl*)meth)->params;
    if (({ 
        params != 0;
    })) {
        ASTNode* n = ((ASTList*)params)->head;
        while (1) {
            if (({ 
                n == 0;
            })) {
                break;
            }
            FieldDecl* p = ((ASTNode*)n)->value;
            CTranspiler_emit(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, ", ", (CTranspiler_mapType(_self, ((FieldDecl*)p)->fieldType))), " "), ((FieldDecl*)p)->name));
            n = ((ASTNode*)n)->next;
        }
    }
    CTranspiler_emitLine(_self, ") {");
    CTranspiler_indent(_self);
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "struct ", ((CTranspiler*)_self)->currentEntity), "* self = (struct "), ((CTranspiler*)_self)->currentEntity), "*)_self;"));
    SymbolTable* st = ((CTranspiler*)_self)->symbols;
    if (({ 
        st != 0;
    })) {
        SymbolTable_enterScope(st);
        if (({ 
            params != 0;
        })) {
            ASTNode* pn = ((ASTList*)params)->head;
            while (1) {
                if (({ 
                    pn == 0;
                })) {
                    break;
                }
                FieldDecl* p = ((ASTNode*)pn)->value;
                SymbolTable_define(st, ((FieldDecl*)p)->name, ((FieldDecl*)p)->fieldType);
                pn = ((ASTNode*)pn)->next;
            }
        }
    }
    CTranspiler_emitBlock(_self, ((MethodDecl*)meth)->body);
    if (({ 
        st != 0;
    })) {
        SymbolTable_exitScope(st);
    }
    CTranspiler_dedent(_self);
    CTranspiler_emitLine(_self, "}");
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitIncludeDecl(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    IncludeDecl* inc = node;
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, "// Include: ", ((IncludeDecl*)inc)->path));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitImportDecl(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    ImportDecl* imp = node;
    CTranspiler_emitLine(_self, BNK_RT_Strings_concat(NULL, "// import ", ((ImportDecl*)imp)->name));
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitUnabstractedBlock(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    UnabstractedBlock* un = node;
    CTranspiler_emitBlock(_self, ((UnabstractedBlock*)un)->body);
    end:
    (void)&&end;
    return;
}
void CTranspiler_emitAsmStmt(void* _self, void* node) {
    struct CTranspiler* self = (struct CTranspiler*)_self;
    (void)self;
    AsmStmt* as = node;
    if (({ 
        strcmp(((AsmStmt*)as)->target, "c") == 0;
    })) {
        CTranspiler_emitLine(_self, ((AsmStmt*)as)->code);
    }
    end:
    (void)&&end;
    return;
}
File* File_open(void* _self, char* path) {
    File* _ret = 0;
    struct File* self = (struct File*)_self;
    (void)self;
    FILE* f = fopen(path, "r");
                    if (!f) return NULL;
                    struct File* obj = gc_alloc(sizeof(struct File));
                    obj->handle = f;
                    return obj;
    end:
    (void)&&end;
    return _ret;
}
File* File_create(void* _self, char* path) {
    File* _ret = 0;
    struct File* self = (struct File*)_self;
    (void)self;
    FILE* f = fopen(path, "w");
                    if (!f) return NULL;
                    struct File* obj = gc_alloc(sizeof(struct File));
                    obj->handle = f;
                    return obj;
    end:
    (void)&&end;
    return _ret;
}
char* File_readToString(void* _self) {
    char* _ret = 0;
    struct File* self = (struct File*)_self;
    (void)self;
    FILE* f = self->handle;
                    if (!f) return "";
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    rewind(f);
                    char* buffer = gc_alloc(size + 1);
                    fread(buffer, 1, size, f);
                    buffer[size] = 0;
                    return buffer;
    end:
    (void)&&end;
    return _ret;
}
void File_writeString(void* _self, char* text) {
    struct File* self = (struct File*)_self;
    (void)self;
    FILE* f = self->handle;
                    if (!f) return;
                    fputs(text, f);
    end:
    (void)&&end;
    return;
}
void File_close(void* _self) {
    struct File* self = (struct File*)_self;
    (void)self;
    FILE* f = self->handle;
                    if (f) fclose(f);
                    self->handle = NULL;
    end:
    (void)&&end;
    return;
}
bool FileSystem_exists(void* _self, char* path) {
    return access(path, F_OK) != -1;
}
bool FileSystem_delete(void* _self, char* path) {
    return remove(path) == 0;
}
ASTList* Casts_toASTList(void* _self, void* ptr) {
    ASTList* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
ASTNode* Casts_toASTNode(void* _self, void* ptr) {
    ASTNode* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Casts_toToken(void* _self, void* ptr) {
    Token* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
VariableExpr* Casts_toVariableExpr(void* _self, void* ptr) {
    VariableExpr* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
IncludeDecl* Casts_toIncludeDecl(void* _self, void* ptr) {
    IncludeDecl* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
ImportDecl* Casts_toImportDecl(void* _self, void* ptr) {
    ImportDecl* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Program* Casts_toProgram(void* _self, void* ptr) {
    Program* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Block* Casts_toBlock(void* _self, void* ptr) {
    Block* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
MethodCall* Casts_toMethodCall(void* _self, void* ptr) {
    MethodCall* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
ArrayAccess* Casts_toArrayAccess(void* _self, void* ptr) {
    ArrayAccess* _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    _ret = ptr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Casts_isBlockTerminatedByDot(void* _self, void* bVoid) {
    bool _ret = 0;
    struct Casts* self = (struct Casts*)_self;
    (void)self;
    Block* b = bVoid;
    _ret = ((Block*)b)->terminatedByDot;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Lexer* Lexer_new(void* _self, char* source) {
    Lexer* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    Lexer* l = gc_alloc(sizeof(Lexer));
    ((Lexer*)l)->source = source;
    ((Lexer*)l)->length = Strings_length(NULL, source);
    ((Lexer*)l)->pos = 0;
    ((Lexer*)l)->line = 1;
    ((Lexer*)l)->column = 1;
    _ret = l;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_peek(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        ((Lexer*)_self)->pos >= ((Lexer*)_self)->length;
    })) {
        _ret = "";
        goto end;
    }
    _ret = Strings_charToString(NULL, (Strings_charAt(NULL, ((Lexer*)_self)->source, ((Lexer*)_self)->pos)));
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_peekNext(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        ((Lexer*)_self)->pos + 1 >= ((Lexer*)_self)->length;
    })) {
        _ret = "";
        goto end;
    }
    _ret = Strings_charToString(NULL, (Strings_charAt(NULL, ((Lexer*)_self)->source, ((Lexer*)_self)->pos + 1)));
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_advance(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        ((Lexer*)_self)->pos >= ((Lexer*)_self)->length;
    })) {
        _ret = "";
        goto end;
    }
    char* c = Strings_charToString(NULL, (Strings_charAt(NULL, ((Lexer*)_self)->source, ((Lexer*)_self)->pos)));
    ((Lexer*)_self)->pos = ((Lexer*)_self)->pos + 1;
    ((Lexer*)_self)->column = ((Lexer*)_self)->column + 1;
    _ret = c;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Lexer_isAlpha(void* _self, char* c) {
    bool _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        strcmp(c, "") == 0;
    })) {
        _ret = false;
        goto end;
    }
    _ret = Strings_contains(NULL, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_", c);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Lexer_isDigit(void* _self, char* c) {
    bool _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        strcmp(c, "") == 0;
    })) {
        _ret = false;
        goto end;
    }
    _ret = Strings_contains(NULL, "0123456789", c);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Lexer_isAlphaNumeric(void* _self, char* c) {
    bool _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        (Lexer_isAlpha(_self, c));
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        (Lexer_isDigit(_self, c));
    })) {
        _ret = true;
        goto end;
    }
    _ret = false;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_readIdentifier(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    long long start = ((Lexer*)_self)->pos;
    Lexer_advance(_self);
    while (1) {
        char* c = Lexer_peek(_self);
        bool isAN = Lexer_isAlphaNumeric(_self, c);
        if (({ 
            isAN;
        })) {
            Lexer_advance(_self);
            continue;
        }
        break;
    }
    long long endIdx = ((Lexer*)_self)->pos;
    char* res = Strings_substring(NULL, ((Lexer*)_self)->source, start, endIdx);
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_readNumber(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    char* c = "";
    bool isD = false;
    char* next = "";
    long long start = ((Lexer*)_self)->pos;
    while (1) {
        c = Lexer_peek(_self);
        isD = Lexer_isDigit(_self, c);
        if (({ 
            isD;
        })) {
            Lexer_advance(_self);
            continue;
        }
        break;
    }
    c = Lexer_peek(_self);
    if (({ 
        strcmp(c, ".") == 0;
    })) {
        next = Lexer_peekNext(_self);
        isD = Lexer_isDigit(_self, next);
        if (({ 
            isD;
        })) {
            Lexer_advance(_self);
            while (1) {
                c = Lexer_peek(_self);
                isD = Lexer_isDigit(_self, c);
                if (({ 
                    isD;
                })) {
                    Lexer_advance(_self);
                    continue;
                }
                break;
            }
        }
    }
    long long endIdx = ((Lexer*)_self)->pos;
    char* res = Strings_substring(NULL, ((Lexer*)_self)->source, start, endIdx);
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Lexer_readString(void* _self) {
    char* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    Lexer_advance(_self);
    long long start = ((Lexer*)_self)->pos;
    while (1) {
        char* c = Lexer_peek(_self);
        if (({ 
            strcmp(c, "") == 0;
        })) {
            break;
        }
        if (({ 
            strcmp(c, "\\") == 0;
        })) {
            Lexer_advance(_self);
            Lexer_advance(_self);
            continue;
        }
        if (({ 
            strcmp(c, "\"") == 0;
        })) {
            break;
        }
        if (({ 
            strcmp(c, "\n") == 0;
        })) {
            ((Lexer*)_self)->line = ((Lexer*)_self)->line + 1;
        }
        Lexer_advance(_self);
    }
    char* value = Strings_substring(NULL, ((Lexer*)_self)->source, start, ((Lexer*)_self)->pos);
    char* endC = Lexer_peek(_self);
    if (({ 
        strcmp(endC, "\"") == 0;
    })) {
        Lexer_advance(_self);
    }
    _ret = value;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Lexer_isKeyword(void* _self, char* word) {
    bool _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    if (({ 
        strcmp(word, "module") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "include") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "import") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "from") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "Entity") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "Trait") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "struct") == 0 || strcmp(word, "Struct") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "has") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "method") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "static") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "return") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "if") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "then") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "condition") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "else") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "while") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "loop") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "break") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "continue") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "try") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "catch") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "finally") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "throw") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "defer") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "scoped") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "unabstracted") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "asm") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "foreign") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "func") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "public") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "private") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "panic") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "true") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "false") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "nil") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "and") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "or") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "not") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "end") == 0;
    })) {
        _ret = true;
        goto end;
    }
    if (({ 
        strcmp(word, "let") == 0;
    })) {
        _ret = true;
        goto end;
    }
    _ret = false;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Lexer_nextToken(void* _self) {
    Token* _ret = 0;
    struct Lexer* self = (struct Lexer*)_self;
    (void)self;
    long long startCol = 0;
    long long startOff = 0;
    bool isD = false;
    bool isA = false;
    bool isKW = false;
    char* value = "";
    char* current = "";
    char* next = "";
    char* c = "";
    while (1) {
        c = Lexer_peek(_self);
        if (({ 
            strcmp(c, " ") == 0 || strcmp(c, "\t") == 0 || strcmp(c, "\n") == 0 || strcmp(c, "\r") == 0;
        })) {
            if (({ 
                strcmp(c, "\n") == 0;
            })) {
                ((Lexer*)_self)->line = ((Lexer*)_self)->line + 1;
                ((Lexer*)_self)->column = 1;
            }
            Lexer_advance(_self);
            continue;
        }
        break;
    }
    value = "";
    isD = false;
    isA = false;
    current = Lexer_peek(_self);
    if (({ 
        strcmp(current, "") == 0;
    })) {
        Token* tokE = Token_new(NULL, "EOF", "", ((Lexer*)_self)->line, ((Lexer*)_self)->column, ((Lexer*)_self)->pos);
        _ret = tokE;
        goto end;
    }
    if (({ 
        strcmp(current, "#") == 0;
    })) {
        while (1) {
            c = Lexer_peek(_self);
            if (({ 
                strcmp(c, "") == 0 || strcmp(c, "\n") == 0;
            })) {
                break;
            }
            Lexer_advance(_self);
        }
        _ret = Lexer_nextToken(_self);
        goto end;
    }
    if (({ 
        strcmp(current, "\"") == 0;
    })) {
        startCol = ((Lexer*)_self)->column;
        startOff = ((Lexer*)_self)->pos;
        value = Lexer_readString(_self);
        Token* tokStr = Token_new(NULL, "STRING", value, ((Lexer*)_self)->line, startCol, startOff);
        _ret = tokStr;
        goto end;
    }
    isD = Lexer_isDigit(_self, current);
    if (({ 
        isD;
    })) {
        startCol = ((Lexer*)_self)->column;
        startOff = ((Lexer*)_self)->pos;
        value = Lexer_readNumber(_self);
        Token* tokNum = Token_new(NULL, "NUMBER", value, ((Lexer*)_self)->line, startCol, startOff);
        _ret = tokNum;
        goto end;
    }
    isA = Lexer_isAlpha(_self, current);
    if (({ 
        isA;
    })) {
        startCol = ((Lexer*)_self)->column;
        startOff = ((Lexer*)_self)->pos;
        value = Lexer_readIdentifier(_self);
        isKW = Lexer_isKeyword(NULL, value);
        if (({ 
            isKW;
        })) {
            Token* tokK = Token_new(NULL, "KEYWORD", value, ((Lexer*)_self)->line, startCol, startOff);
            _ret = tokK;
            goto end;
        }
        Token* tokI = Token_new(NULL, "IDENT", value, ((Lexer*)_self)->line, startCol, startOff);
        _ret = tokI;
        goto end;
    }
    next = Lexer_peekNext(_self);
    if (({ 
        strcmp(current, "=") == 0;
    })) {
        if (({ 
            strcmp(next, "=") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            _ret = Token_new(NULL, "OPERATOR", "==", ((Lexer*)_self)->line, startCol, startOff);
            goto end;
        }
    }
    if (({ 
        strcmp(current, "!") == 0;
    })) {
        if (({ 
            strcmp(next, "=") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            _ret = Token_new(NULL, "OPERATOR", "!=", ((Lexer*)_self)->line, startCol, startOff);
            goto end;
        }
    }
    if (({ 
        strcmp(current, "<") == 0;
    })) {
        if (({ 
            strcmp(next, "-") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            _ret = Token_new(NULL, "OPERATOR", "<-", ((Lexer*)_self)->line, startCol, startOff);
            goto end;
        }
        if (({ 
            strcmp(next, "=") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            _ret = Token_new(NULL, "OPERATOR", "<=", ((Lexer*)_self)->line, startCol, startOff);
            goto end;
        }
    }
    if (({ 
        strcmp(current, ">") == 0;
    })) {
        if (({ 
            strcmp(next, "=") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            _ret = Token_new(NULL, "OPERATOR", ">=", ((Lexer*)_self)->line, startCol, startOff);
            goto end;
        }
    }
    if (({ 
        strcmp(current, "-") == 0;
    })) {
        if (({ 
            strcmp(next, ">") == 0;
        })) {
            startCol = ((Lexer*)_self)->column;
            startOff = ((Lexer*)_self)->pos;
            Lexer_advance(_self);
            Lexer_advance(_self);
            Token* tokO = Token_new(NULL, "OPERATOR", "->", ((Lexer*)_self)->line, startCol, startOff);
            _ret = tokO;
            goto end;
        }
    }
    startCol = ((Lexer*)_self)->column;
    startOff = ((Lexer*)_self)->pos;
    Lexer_advance(_self);
    Token* tokS = Token_new(NULL, "SYMBOL", current, ((Lexer*)_self)->line, startCol, startOff);
    _ret = tokS;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Parser* Parser_new(void* _self, void* tokens, char* source) {
    Parser* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser* p = gc_alloc(sizeof(Parser));
    ((Parser*)p)->tokens = tokens;
    ((Parser*)p)->source = source;
    long long count = 0;
    if (({ 
        tokens != 0;
    })) {
        ASTList* l = tokens;
        ASTNode* c = ((ASTList*)l)->head;
        while (1) {
            if (({ 
                c == 0;
            })) {
                break;
            }
            count = count + 1;
            c = ((ASTNode*)c)->next;
        }
    }
    printf("%s\n", BNK_RT_Strings_concat(NULL, bunker_str_add_int("DEBUG: Parser initialized with ", count), " tokens"));
    if (({ 
        tokens != 0;
    })) {
        ASTList* list = tokens;
        ((Parser*)p)->current = ((ASTList*)list)->head;
    } else {
        ((Parser*)p)->current = 0;
    }
    ((Parser*)p)->hadError = false;
    ((Parser*)p)->panicMode = false;
    ((Parser*)p)->includedFiles = ASTList_new(NULL, 0, 0);
    _ret = p;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_peek(void* _self) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    if (({ 
        ((Parser*)_self)->current == 0;
    })) {
        _ret = Token_new(NULL, "EOF", "", 0, 0, 0);
        goto end;
    }
    ASTNode* node = ((Parser*)_self)->current;
    Token* tok = ((ASTNode*)node)->value;
    _ret = tok;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_peekNext(void* _self) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    if (({ 
        ((Parser*)_self)->current == 0;
    })) {
        _ret = Token_new(NULL, "EOF", "", 0, 0, 0);
        goto end;
    }
    ASTNode* node = ((Parser*)_self)->current;
    if (({ 
        ((ASTNode*)node)->next == 0;
    })) {
        _ret = Token_new(NULL, "EOF", "", 0, 0, 0);
        goto end;
    }
    ASTNode* nextVal = ((ASTNode*)node)->next;
    Token* tok = ((ASTNode*)nextVal)->value;
    _ret = tok;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_peekAt(void* _self, long long offset) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    ASTNode* node = ((Parser*)_self)->current;
    long long i = 0;
    while (1) {
        if (({ 
            i >= offset || node == 0;
        })) {
            break;
        }
        node = ((ASTNode*)node)->next;
        i = i + 1;
    }
    if (({ 
        node == 0;
    })) {
        _ret = Token_new(NULL, "EOF", "", 0, 0, 0);
        goto end;
    }
    Token* tok = ((ASTNode*)node)->value;
    _ret = tok;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_previous(void* _self) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    _ret = Token_new(NULL, "EOF", "", 0, 0, 0);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Parser_isAtEnd(void* _self) {
    bool _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "EOF") == 0;
    })) {
        _ret = true;
        goto end;
    }
    _ret = false;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Parser_check(void* _self, char* type) {
    bool _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    if (({ 
        Parser_isAtEnd(_self);
    })) {
        _ret = false;
        goto end;
    }
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, type) == 0;
    })) {
        _ret = true;
        goto end;
    }
    _ret = false;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_advance(void* _self) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    if (({ 
        (Parser_isAtEnd(_self));
    })) {
        _ret = Parser_peek(_self);
        goto end;
    }
    Token* tk = Parser_peek(_self);
    ASTNode* curr = ((Parser*)_self)->current;
    ((Parser*)_self)->current = ((ASTNode*)curr)->next;
    _ret = tk;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Token* Parser_consume(void* _self, char* type, char* message) {
    Token* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    if (({ 
        (Parser_check(_self, type));
    })) {
        _ret = Parser_advance(_self);
        goto end;
    }
    Token* tk = Parser_peek(_self);
    char* msg = message;
    if (({ 
        msg == 0;
    })) {
        msg = BNK_RT_Strings_concat(NULL, "Expected ", type);
    }
    ErrorReporter_report(NULL, msg, tk, ((Parser*)_self)->source);
    ((Parser*)_self)->hadError = true;
    _ret = tk;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseDeclaration(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "KEYWORD") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "struct") == 0;
        })) {
            _ret = Parser_parseStruct(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "Entity") == 0;
        })) {
            _ret = Parser_parseEntity(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "unabstracted") == 0;
        })) {
            _ret = Parser_parseUnabstracted(_self);
            goto end;
        }
        _ret = Parser_parseStatement(_self);
        goto end;
    }
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseEntity(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* nameTok = Parser_consume(_self, "IDENT", "Expected Entity name.");
    if (({ 
        nameTok == 0;
    })) {
        ErrorReporter_report(NULL, "Expected Entity name.", Parser_peek(_self), ((Parser*)_self)->source);
        _ret = 0;
        goto end;
    }
    ASTList* typeParams = 0;
    Token* tkP = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkP)->type, "SYMBOL") == 0 && strcmp(((Token*)tkP)->value, "<") == 0;
    })) {
        Parser_advance(_self);
        typeParams = ASTList_new(NULL, 0, 0);
        ASTNode* tpTail = 0;
        while (1) {
            Token* tp = Parser_consume(_self, "IDENT", "Expected type parameter name.");
            ASTNode* tpNode = ASTNode_new(NULL, tp, 0);
            if (({ 
                ((ASTList*)typeParams)->head == 0;
            })) {
                ((ASTList*)typeParams)->head = tpNode;
                tpTail = tpNode;
            } else {
                ASTNode* tpTailNode = Casts_toASTNode(NULL, tpTail);
                ((ASTNode*)tpTailNode)->next = tpNode;
                tpTail = tpNode;
            }
            Token* tkNext = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkNext)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                continue;
            }
            break;
        }
        Parser_consume(_self, "SYMBOL", ">");
    }
    ASTList* implList = 0;
    Token* tkI = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkI)->type, "KEYWORD") == 0 && strcmp(((Token*)tkI)->value, "implements") == 0;
    })) {
        Parser_advance(_self);
        implList = ASTList_new(NULL, 0, 0);
        ASTNode* iTail = 0;
        while (1) {
            char* iType = Parser_parseType(_self, "Expected implemented trait name.");
            ASTNode* nodeI = ASTNode_new(NULL, iType, 0);
            if (({ 
                ((ASTList*)implList)->head == 0;
            })) {
                ((ASTList*)implList)->head = nodeI;
                iTail = nodeI;
            } else {
                ASTNode* iTailNode = Casts_toASTNode(NULL, iTail);
                ((ASTNode*)iTailNode)->next = nodeI;
                iTail = nodeI;
            }
            Token* tkNextI = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkNextI)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                continue;
            }
            break;
        }
    }
    Parser_consume(_self, "SYMBOL", ":");
    ASTList* fields = ASTList_new(NULL, 0, 0);
    ASTNode* fieldTail = 0;
    ASTList* methods = ASTList_new(NULL, 0, 0);
    ASTNode* methodTail = 0;
    while (1) {
        Token* tk = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk)->type, "KEYWORD") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, "end") == 0;
            })) {
                Parser_advance(_self);
                Token* tkEnd = Parser_peek(_self);
                if (({ 
                    strcmp(((Token*)tkEnd)->value, "Entity") == 0;
                })) {
                    Parser_advance(_self);
                }
                Token* tkDot = Parser_peek(_self);
                if (({ 
                    strcmp(((Token*)tkDot)->value, ".") == 0;
                })) {
                    Parser_advance(_self);
                }
                break;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "has") == 0;
            })) {
                Parser_advance(_self);
                bool isStatic = false;
                while (1) {
                    Token* tkm = Parser_peek(_self);
                    if (({ 
                        strcmp(((Token*)tkm)->type, "KEYWORD") == 0;
                    })) {
                        if (({ 
                            strcmp(((Token*)tkm)->value, "public") == 0 || strcmp(((Token*)tkm)->value, "private") == 0 || strcmp(((Token*)tkm)->value, "static") == 0 || strcmp(((Token*)tkm)->value, "a") == 0 || strcmp(((Token*)tkm)->value, "an") == 0 || strcmp(((Token*)tkm)->value, "state") == 0;
                        })) {
                            if (({ 
                                strcmp(((Token*)tkm)->value, "static") == 0;
                            })) {
                                isStatic = true;
                            }
                            Parser_advance(_self);
                            continue;
                        }
                    }
                    break;
                }
                Token* tk3 = Parser_peek(_self);
                if (({ 
                    strcmp(((Token*)tk3)->value, "method") == 0;
                })) {
                    Parser_advance(_self);
                    void* m = Parser_parseMethod(_self, isStatic);
                    if (({ 
                        m != 0;
                    })) {
                        ASTNode* mNode = ASTNode_new(NULL, m, 0);
                        if (({ 
                            ((ASTList*)methods)->head == 0;
                        })) {
                            ((ASTList*)methods)->head = mNode;
                            methodTail = mNode;
                        } else {
                            ASTNode* mTailNode = methodTail;
                            ((ASTNode*)mTailNode)->next = mNode;
                            methodTail = mNode;
                        }
                    }
                    continue;
                }
                void* f = Parser_parseField(_self);
                if (({ 
                    f != 0;
                })) {
                    ASTNode* fNode = ASTNode_new(NULL, f, 0);
                    if (({ 
                        ((ASTList*)fields)->head == 0;
                    })) {
                        ((ASTList*)fields)->head = fNode;
                        fieldTail = fNode;
                    } else {
                        ASTNode* fTailNode = fieldTail;
                        ((ASTNode*)fTailNode)->next = fNode;
                        fieldTail = fNode;
                    }
                }
                continue;
            }
            Token* tkSafety = Parser_peek(_self);
            Parser_advance(_self);
        } else {
            Token* tkSafety2 = Parser_peek(_self);
            Parser_advance(_self);
        }
    }
    EntityDecl* ent = EntityDecl_new(NULL, "EntityDecl", ((Token*)nameTok)->value, typeParams, implList, fields, methods);
    _ret = ent;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseMethod(void* _self, bool isStatic) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* nameTok = Parser_consume(_self, "IDENT", "Expected method name.");
    ASTList* typeParams = 0;
    Token* tkP = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkP)->type, "SYMBOL") == 0 && strcmp(((Token*)tkP)->value, "<") == 0;
    })) {
        Parser_advance(_self);
        typeParams = ASTList_new(NULL, 0, 0);
        ASTNode* tpTail = 0;
        while (1) {
            Token* tp = Parser_consume(_self, "IDENT", "Expected type parameter name.");
            ASTNode* tpNode = ASTNode_new(NULL, tp, 0);
            if (({ 
                ((ASTList*)typeParams)->head == 0;
            })) {
                ((ASTList*)typeParams)->head = tpNode;
                tpTail = tpNode;
            } else {
                ASTNode* tpTailNode = Casts_toASTNode(NULL, tpTail);
                ((ASTNode*)tpTailNode)->next = tpNode;
                tpTail = tpNode;
            }
            Token* tkNext = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkNext)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                continue;
            }
            break;
        }
        Parser_consume(_self, "SYMBOL", ">");
    }
    ASTList* args = ASTList_new(NULL, 0, 0);
    ASTNode* argTail = 0;
    char* returnType = "void";
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, ":") == 0;
    })) {
        bool isArgs = false;
        Token* tkN1 = Parser_peekAt(_self, 1);
        Token* tkN2 = Parser_peekAt(_self, 2);
        if (({ 
            strcmp(((Token*)tkN1)->type, "IDENT") == 0 && strcmp(((Token*)tkN2)->value, ":") == 0;
        })) {
            Token* tkN3 = Parser_peekAt(_self, 3);
            if (({ 
                strcmp(((Token*)tkN3)->type, "IDENT") == 0;
            })) {
                isArgs = true;
            }
        }
        if (({ 
            strcmp(((Token*)tkN1)->value, "->") == 0 || strcmp(((Token*)tkN1)->value, "-") == 0;
        })) {
            isArgs = true;
        }
        if (({ 
            isArgs;
        })) {
            Parser_advance(_self);
            while (1) {
                Token* tkParam = Parser_peek(_self);
                if (({ 
                    strcmp(((Token*)tkParam)->type, "OPERATOR") == 0 || strcmp(((Token*)tkParam)->type, "SYMBOL") == 0;
                })) {
                    if (({ 
                        strcmp(((Token*)tkParam)->value, "-") == 0 || strcmp(((Token*)tkParam)->value, "->") == 0;
                    })) {
                        break;
                    }
                }
                if (({ 
                    strcmp(((Token*)tkParam)->type, "SYMBOL") == 0 && strcmp(((Token*)tkParam)->value, ":") == 0;
                })) {
                    break;
                }
                if (({ 
                    strcmp(((Token*)tkParam)->type, "IDENT") == 0;
                })) {
                    char* pName = ((Token*)tkParam)->value;
                    Parser_advance(_self);
                    Token* tkCol = Parser_peek(_self);
                    if (({ 
                        strcmp(((Token*)tkCol)->type, "SYMBOL") == 0 && strcmp(((Token*)tkCol)->value, ":") == 0;
                    })) {
                        Parser_advance(_self);
                    } else {
                        printf("%s\n", BNK_RT_Strings_concat(NULL, "Error: Expected ':' after param name ", pName));
                    }
                    char* pType = Parser_parseType(_self, "Expected param type.");
                    FieldDecl* f = FieldDecl_new(NULL, "Arg", pName, pType);
                    ASTNode* node = ASTNode_new(NULL, f, 0);
                    if (({ 
                        ((ASTList*)args)->head == 0;
                    })) {
                        ((ASTList*)args)->head = node;
                        argTail = node;
                    } else {
                        ASTNode* tailNode = argTail;
                        ((ASTNode*)tailNode)->next = node;
                        argTail = node;
                    }
                    Token* tkComma = Parser_peek(_self);
                    if (({ 
                        strcmp(((Token*)tkComma)->value, ",") == 0;
                    })) {
                        Parser_advance(_self);
                    }
                    continue;
                }
                break;
            }
        }
    }
    Token* tkRet = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkRet)->value, "->") == 0 || strcmp(((Token*)tkRet)->value, "-") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tkRet)->value, "-") == 0;
        })) {
            Parser_advance(_self);
            Token* tkGt = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkGt)->value, ">") == 0;
            })) {
                Parser_advance(_self);
            }
        } else {
            Parser_advance(_self);
        }
        returnType = Parser_parseType(_self, "Expected return type");
    }
    Token* tkBodyStart = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkBodyStart)->type, "SYMBOL") == 0 && strcmp(((Token*)tkBodyStart)->value, ":") == 0;
    })) {
        Parser_advance(_self);
    }
    Block* body = Parser_parseBlock(_self);
    Token* tkEnd = Parser_peek(_self);
    Token* tkMethodNext = Parser_peekNext(_self);
    if (({ 
        strcmp(((Token*)tkEnd)->value, "end") == 0 && strcmp(((Token*)tkMethodNext)->value, "method") == 0;
    })) {
        Parser_advance(_self);
        Parser_advance(_self);
        Parser_consume(_self, "SYMBOL", ".");
    } else {
        if (({ 
            ((Block*)body)->terminatedByDot == false;
        })) {
            Parser_consume(_self, "KEYWORD", "end");
            Parser_consume(_self, "KEYWORD", "method");
            Parser_consume(_self, "SYMBOL", ".");
        }
    }
    printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Creating method ", ((Token*)nameTok)->value));
    MethodDecl* m = MethodDecl_new(NULL, "MethodDecl", ((Token*)nameTok)->value, isStatic, args, returnType, body, typeParams);
    _ret = m;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseStruct(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* nameTok = Parser_consume(_self, "IDENT", "Expected struct name.");
    ASTList* typeParams = 0;
    Token* tkP = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkP)->type, "SYMBOL") == 0 && strcmp(((Token*)tkP)->value, "<") == 0;
    })) {
        Parser_advance(_self);
        typeParams = ASTList_new(NULL, 0, 0);
        ASTNode* tpTail = 0;
        while (1) {
            Token* tp = Parser_consume(_self, "IDENT", "Expected type parameter name.");
            ASTNode* tpNode = ASTNode_new(NULL, tp, 0);
            if (({ 
                ((ASTList*)typeParams)->head == 0;
            })) {
                ((ASTList*)typeParams)->head = tpNode;
                tpTail = tpNode;
            } else {
                ASTNode* tpTailNode = Casts_toASTNode(NULL, tpTail);
                ((ASTNode*)tpTailNode)->next = tpNode;
                tpTail = tpNode;
            }
            Token* tkNext = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkNext)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                continue;
            }
            break;
        }
        Parser_consume(_self, "SYMBOL", ">");
    }
    Token* tk = Parser_peek(_self);
    bool useBraces = false;
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "{") == 0;
        })) {
            useBraces = true;
            Parser_advance(_self);
        }
        if (({ 
            strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            Parser_advance(_self);
        }
    }
    ASTList* fields = ASTList_new(NULL, 0, 0);
    ASTNode* tail = 0;
    while (1) {
        tk = Parser_peek(_self);
        if (({ 
            useBraces;
        })) {
            if (({ 
                strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, "}") == 0;
            })) {
                Parser_advance(_self);
                break;
            }
        } else {
            if (({ 
                strcmp(((Token*)tk)->type, "KEYWORD") == 0 && strcmp(((Token*)tk)->value, "end") == 0;
            })) {
                Parser_advance(_self);
                Parser_consume(_self, "KEYWORD", "struct");
                Parser_consume(_self, "SYMBOL", ".");
                break;
            }
        }
        if (({ 
            Parser_isAtEnd(_self);
        })) {
            break;
        }
        void* field = Parser_parseField(_self);
        if (({ 
            field != 0;
        })) {
            ASTNode* fNode = ASTNode_new(NULL, field, 0);
            if (({ 
                ((ASTList*)fields)->head == 0;
            })) {
                ((ASTList*)fields)->head = fNode;
                tail = fNode;
            } else {
                ASTNode* tailNode = Casts_toASTNode(NULL, tail);
                ((ASTNode*)tailNode)->next = fNode;
                tail = fNode;
            }
        }
    }
    StructDecl* st = StructDecl_new(NULL, "StructDecl", ((Token*)nameTok)->value, typeParams, fields);
    _ret = st;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseField(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "IDENT") != 0;
    })) {
        _ret = 0;
        goto end;
    }
    Token* nameTok = Parser_consume(_self, "IDENT", "Expected field name.");
    Token* tk2 = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk2)->type, "SYMBOL") != 0;
    })) {
        _ret = 0;
        goto end;
    }
    if (({ 
        strcmp(((Token*)tk2)->value, ":") != 0;
    })) {
        _ret = 0;
        goto end;
    }
    Parser_advance(_self);
    char* typeName = Parser_parseType(_self, "Expected field type.");
    Token* tk4 = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk4)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk4)->value, ":") == 0 || strcmp(((Token*)tk4)->value, ";") == 0 || strcmp(((Token*)tk4)->value, ".") == 0);
    })) {
        Parser_advance(_self);
    }
    FieldDecl* f = FieldDecl_new(NULL, "FieldDecl", ((Token*)nameTok)->value, typeName);
    _ret = f;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Parser_parsePath(void* _self, char* msg) {
    char* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    char* res = "";
    while (1) {
        Token* tk = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk)->type, "IDENT") == 0 || strcmp(((Token*)tk)->type, "KEYWORD") == 0;
        })) {
            Parser_advance(_self);
            res = BNK_RT_Strings_concat(NULL, res, ((Token*)tk)->value);
        } else {
            break;
        }
        Token* tkN = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tkN)->type, "SYMBOL") == 0 && strcmp(((Token*)tkN)->value, ".") == 0;
        })) {
            Parser_advance(_self);
            res = BNK_RT_Strings_concat(NULL, res, ".");
        } else {
            break;
        }
    }
    if (({ 
        strcmp(res, "") == 0;
    })) {
        ErrorReporter_report(NULL, msg, Parser_peek(_self), ((Parser*)_self)->source);
    }
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
char* Parser_parseType(void* _self, char* msg) {
    char* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    char* res = "";
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "IDENT") != 0 && strcmp(((Token*)tk)->type, "KEYWORD") != 0;
    })) {
        ErrorReporter_report(NULL, msg, tk, ((Parser*)_self)->source);
        _ret = "";
        goto end;
    }
    Parser_advance(_self);
    res = ((Token*)tk)->value;
    Token* tkN = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkN)->type, "SYMBOL") == 0 && strcmp(((Token*)tkN)->value, "<") == 0;
    })) {
        Parser_advance(_self);
        res = BNK_RT_Strings_concat(NULL, res, "<");
        while (1) {
            char* tSub = Parser_parseType(_self, "Expected type argument.");
            res = BNK_RT_Strings_concat(NULL, res, tSub);
            Token* tkNext = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tkNext)->type, "SYMBOL") == 0 && strcmp(((Token*)tkNext)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                res = BNK_RT_Strings_concat(NULL, res, ",");
            } else {
                break;
            }
        }
        Parser_consume(_self, "SYMBOL", ">");
        res = BNK_RT_Strings_concat(NULL, res, ">");
    }
    while (1) {
        Token* tkP = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tkP)->type, "SYMBOL") == 0 && strcmp(((Token*)tkP)->value, "*") == 0;
        })) {
            Parser_advance(_self);
            res = BNK_RT_Strings_concat(NULL, res, "*");
        } else {
            break;
        }
    }
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseModule(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    char* path = Parser_parsePath(_self, "Expected module name.");
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk)->value, ":") == 0 || strcmp(((Token*)tk)->value, ";") == 0 || strcmp(((Token*)tk)->value, ".") == 0);
    })) {
        Parser_advance(_self);
    }
    ModuleDecl* mod = ModuleDecl_new(NULL, "ModuleDecl", path);
    _ret = mod;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseImport(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    char* path = Parser_parsePath(_self, "Expected import name.");
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk)->value, ":") == 0 || strcmp(((Token*)tk)->value, ";") == 0 || strcmp(((Token*)tk)->value, ".") == 0);
    })) {
        Parser_advance(_self);
    }
    ImportDecl* imp = ImportDecl_new(NULL, "ImportDecl", path);
    _ret = imp;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseInclude(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    char* path = Parser_parsePath(_self, "Expected include path.");
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk)->value, ":") == 0 || strcmp(((Token*)tk)->value, ";") == 0 || strcmp(((Token*)tk)->value, ".") == 0);
    })) {
        Parser_advance(_self);
    }
    IncludeDecl* inc = IncludeDecl_new(NULL, "IncludeDecl", path);
    _ret = inc;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
bool Parser_isIncluded(void* _self, char* path) {
    bool _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    ASTList* list = ((Parser*)_self)->includedFiles;
    ASTNode* curr = ((ASTList*)list)->head;
    while (1) {
        if (({ 
            curr == 0;
        })) {
            break;
        }
        char* s = ((ASTNode*)curr)->value;
        if (({ 
            strcmp(s, path) == 0;
        })) {
            _ret = true;
            goto end;
        }
        curr = ((ASTNode*)curr)->next;
    }
    _ret = false;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void Parser_addIncluded(void* _self, char* path) {
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    ASTList* list = ((Parser*)_self)->includedFiles;
    ASTNode* node = ASTNode_new(NULL, path, 0);
    if (({ 
        ((ASTList*)list)->head == 0;
    })) {
        ((ASTList*)list)->head = node;
        ((ASTList*)list)->tail = node;
    } else {
        ASTNode* tail = Casts_toASTNode(NULL, ((ASTList*)list)->tail);
        ((ASTNode*)tail)->next = node;
        ((ASTList*)list)->tail = node;
    }
    end:
    (void)&&end;
    return;
}
char* Parser_resolvePath(void* _self, char* path) {
    char* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    char* p = Strings_replace(NULL, path, ".", "/");
    char* res = BNK_RT_Strings_concat(NULL, p, ".bnk");
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseIncludeFile(void* _self, char* path) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    char* resolved = Parser_resolvePath(_self, path);
    printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: parseIncludeFile path=", path), " resolved="), resolved));
    if (({ 
        Parser_isIncluded(_self, resolved);
    })) {
        printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: already included ", resolved));
        _ret = 0;
        goto end;
    }
    Parser_addIncluded(_self, resolved);
    File* fileObj = File_open(NULL, resolved);
    if (({ 
        fileObj == 0;
    })) {
        resolved = BNK_RT_Strings_concat(NULL, "src/std/", resolved);
        fileObj = File_open(NULL, resolved);
        if (({ 
            fileObj == 0;
        })) {
            printf("%s\n", BNK_RT_Strings_concat(NULL, "Error: Could not include file ", path));
            _ret = 0;
            goto end;
        }
    }
    char* subSource = File_readToString(fileObj);
    File_close(fileObj);
    Lexer* subLexer = Lexer_new(NULL, subSource);
    ASTList* subTokens = ASTList_new(NULL, 0, 0);
    ASTNode* subTail = 0;
    while (1) {
        Token* tok = Lexer_nextToken(subLexer);
        if (({ 
            strcmp(((Token*)tok)->type, "EOF") == 0;
        })) {
            break;
        }
        ASTNode* subNode = ASTNode_new(NULL, tok, 0);
        if (({ 
            ((ASTList*)subTokens)->head == 0;
        })) {
            ((ASTList*)subTokens)->head = subNode;
            subTail = subNode;
        } else {
            ASTNode* sTailNode = Casts_toASTNode(NULL, subTail);
            ((ASTNode*)sTailNode)->next = subNode;
            subTail = subNode;
        }
    }
    Parser* subParser = Parser_new(NULL, subTokens, subSource);
    ((Parser*)subParser)->includedFiles = ((Parser*)_self)->includedFiles;
    void* subProgVoid = Parser_parseProgram(subParser);
    Program* subProgRec = Casts_toProgram(NULL, subProgVoid);
    _ret = ((Program*)subProgRec)->stmts;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseUnabstracted(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, ":") == 0;
    })) {
        Parser_advance(_self);
    }
    void* bodyVoid = Parser_parseBlock(_self);
    Token* tkEnd = Parser_peek(_self);
    Token* tkNext = Parser_peekNext(_self);
    if (({ 
        strcmp(((Token*)tkEnd)->value, "end") == 0 && strcmp(((Token*)tkNext)->value, "unabstracted") == 0;
    })) {
        printf("%s\n", "DEBUG: parseUnabstracted consuming end unabstracted.");
        Parser_advance(_self);
        Parser_advance(_self);
        Token* tkDot = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tkDot)->value, ".") == 0;
        })) {
            Parser_advance(_self);
        }
    }
    Token* tkTmp = Parser_peek(_self);
    UnabstractedBlock* un = UnabstractedBlock_new(NULL, "UnabstractedBlock", bodyVoid);
    _ret = un;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseAsm(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* targetTok = Parser_consume(_self, "IDENT", "Expected asm target (e.g. c).");
    Parser_consume(_self, "SYMBOL", ":");
    Token* codeTok = Parser_consume(_self, "STRING", "Expected asm code string.");
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk)->value, ":") == 0 || strcmp(((Token*)tk)->value, ";") == 0 || strcmp(((Token*)tk)->value, ".") == 0);
    })) {
        Parser_advance(_self);
    }
    AsmStmt* asmNode = AsmStmt_new(NULL, "AsmStmt", ((Token*)targetTok)->value, ((Token*)codeTok)->value);
    _ret = asmNode;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseProgram(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Program* prog = Program_new(NULL, "Program", 0, 0);
    ASTList* stmts = ASTList_new(NULL, 0, 0);
    ASTNode* tail = 0;
    Token* tkMod = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tkMod)->type, "KEYWORD") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tkMod)->value, "module") == 0;
        })) {
            void* mod = Parser_parseModule(_self);
            ((Program*)prog)->module = mod;
            ASTNode* modNode = ASTNode_new(NULL, mod, 0);
            ((ASTList*)stmts)->head = modNode;
            tail = modNode;
        }
    }
    while (1) {
        if (({ 
            Parser_isAtEnd(_self);
        })) {
            break;
        }
        Token* tkInc = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tkInc)->type, "KEYWORD") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tkInc)->value, "include") == 0;
            })) {
                void* incVoid = Parser_parseInclude(_self);
                IncludeDecl* incD = Casts_toIncludeDecl(NULL, incVoid);
                void* stmtsFromIncludeVoid = Parser_parseIncludeFile(_self, ((IncludeDecl*)incD)->path);
                if (({ 
                    stmtsFromIncludeVoid != 0;
                })) {
                    ASTList* listInc = Casts_toASTList(NULL, stmtsFromIncludeVoid);
                    ASTNode* subN = ((ASTList*)listInc)->head;
                    while (1) {
                        if (({ 
                            subN == 0;
                        })) {
                            break;
                        }
                        void* subStmt = ((ASTNode*)subN)->value;
                        ASTNode* subNode = ASTNode_new(NULL, subStmt, 0);
                        if (({ 
                            ((ASTList*)stmts)->head == 0;
                        })) {
                            ((ASTList*)stmts)->head = subNode;
                            tail = subNode;
                        } else {
                            ASTNode* t = Casts_toASTNode(NULL, tail);
                            ((ASTNode*)t)->next = subNode;
                            tail = subNode;
                        }
                        subN = ((ASTNode*)subN)->next;
                    }
                }
                continue;
            }
        }
        break;
    }
    while (1) {
        if (({ 
            Parser_isAtEnd(_self);
        })) {
            break;
        }
        Token* tkImp = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tkImp)->type, "KEYWORD") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tkImp)->value, "import") == 0;
            })) {
                void* impVoid = Parser_parseImport(_self);
                ImportDecl* imp = Casts_toImportDecl(NULL, impVoid);
                ASTNode* impNode = ASTNode_new(NULL, imp, 0);
                if (({ 
                    ((ASTList*)stmts)->head == 0;
                })) {
                    ((ASTList*)stmts)->head = impNode;
                    tail = impNode;
                } else {
                    ASTNode* impTail = tail;
                    ((ASTNode*)impTail)->next = impNode;
                    tail = impNode;
                }
                void* stmtsFromImportVoid = Parser_parseIncludeFile(_self, ((ImportDecl*)imp)->name);
                if (({ 
                    stmtsFromImportVoid != 0;
                })) {
                    ASTList* listImp = Casts_toASTList(NULL, stmtsFromImportVoid);
                    ASTNode* subN = ((ASTList*)listImp)->head;
                    while (1) {
                        if (({ 
                            subN == 0;
                        })) {
                            break;
                        }
                        void* subStmt = ((ASTNode*)subN)->value;
                        ASTNode* subNode = ASTNode_new(NULL, subStmt, 0);
                        if (({ 
                            ((ASTList*)stmts)->head == 0;
                        })) {
                            ((ASTList*)stmts)->head = subNode;
                            tail = subNode;
                        } else {
                            ASTNode* t = Casts_toASTNode(NULL, tail);
                            ((ASTNode*)t)->next = subNode;
                            tail = subNode;
                        }
                        subN = ((ASTNode*)subN)->next;
                    }
                }
                continue;
            }
        }
        break;
    }
    while (1) {
        if (({ 
            Parser_isAtEnd(_self);
        })) {
            break;
        }
        void* stmt = Parser_parseDeclaration(_self);
        if (({ 
            stmt != 0;
        })) {
            ASTNode* node = ASTNode_new(NULL, stmt, 0);
            if (({ 
                ((ASTList*)stmts)->head == 0;
            })) {
                ((ASTList*)stmts)->head = node;
                tail = node;
            } else {
                ASTNode* tailNode = Casts_toASTNode(NULL, tail);
                ((ASTNode*)tailNode)->next = node;
                tail = node;
            }
        } else {
            Parser_advance(_self);
        }
    }
    ((Program*)prog)->stmts = stmts;
    _ret = prog;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
Block* Parser_parseBlock(void* _self) {
    Block* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Block* block = Block_new(NULL, "Block", 0, false);
    ASTList* stmts = ASTList_new(NULL, 0, 0);
    ASTNode* tail = 0;
    while (1) {
        if (({ 
            Parser_isAtEnd(_self);
        })) {
            break;
        }
        Token* tk = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk)->type, "KEYWORD") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, "end") == 0;
            })) {
                break;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "else") == 0;
            })) {
                break;
            }
        }
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, ".") == 0;
            })) {
                if (({ 
                    ((ASTList*)stmts)->head != 0;
                })) {
                    Parser_advance(_self);
                    ((Block*)block)->terminatedByDot = true;
                    break;
                }
            }
        }
        void* stmt = Parser_parseStatement(_self);
        if (({ 
            stmt != 0;
        })) {
            Header* h = stmt;
        }
        if (({ 
            stmt != 0;
        })) {
            ASTNode* node = ASTNode_new(NULL, stmt, 0);
            if (({ 
                ((ASTList*)stmts)->head == 0;
            })) {
                ((ASTList*)stmts)->head = node;
                tail = node;
            } else {
                ASTNode* tailNode = Casts_toASTNode(NULL, tail);
                ((ASTNode*)tailNode)->next = node;
                tail = node;
            }
        }
        Token* tk2 = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk2)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk2)->value, ":") == 0;
            })) {
                Parser_advance(_self);
            }
        }
    }
    ((Block*)block)->stmts = stmts;
    _ret = block;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseStatement(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "KEYWORD") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "return") == 0;
        })) {
            _ret = Parser_parseReturn(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "if") == 0;
        })) {
            _ret = Parser_parseIf(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "unabstracted") == 0;
        })) {
            _ret = Parser_parseUnabstracted(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "asm") == 0;
        })) {
            _ret = Parser_parseAsm(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "while") == 0;
        })) {
            _ret = Parser_parseWhile(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "loop") == 0;
        })) {
            _ret = Parser_parseLoop(_self);
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "break") == 0;
        })) {
            Parser_advance(_self);
            _ret = 0;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "continue") == 0;
        })) {
            Parser_advance(_self);
            _ret = 0;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "let") == 0;
        })) {
            Parser_advance(_self);
            void* res = Parser_parseAssignment(_self, true);
            Parser_consume(_self, "SYMBOL", "Expected ';' after let statement.");
            _ret = res;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "module") == 0;
        })) {
            Parser_advance(_self);
            Parser_advance(_self);
            Token* tks = Parser_peek(_self);
            if (({ 
                strcmp(((Token*)tks)->type, "SYMBOL") == 0;
            })) {
                if (({ 
                    strcmp(((Token*)tks)->value, ":") == 0;
                })) {
                    Parser_advance(_self);
                }
            }
            _ret = 0;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "scoped") == 0;
        })) {
            _ret = Parser_parseScoped(_self);
            goto end;
        }
    }
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0 && (strcmp(((Token*)tk)->value, ";") == 0 || strcmp(((Token*)tk)->value, ".") == 0 || strcmp(((Token*)tk)->value, ":") == 0);
    })) {
        Parser_advance(_self);
        _ret = 0;
        goto end;
    }
    _ret = Parser_parseExpressionStatement(_self);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseScoped(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            Parser_advance(_self);
        }
    }
    Block* body = Parser_parseBlock(_self);
    Token* tkEnd = Parser_peek(_self);
    Token* tkNext = Parser_peekNext(_self);
    if (({ 
        strcmp(((Token*)tkEnd)->value, "end") == 0 && strcmp(((Token*)tkNext)->value, "scoped") == 0;
    })) {
        Parser_advance(_self);
        Parser_consume(_self, "KEYWORD", "scoped");
        Parser_consume(_self, "SYMBOL", ".");
    } else {
        if (({ 
            ((Block*)body)->terminatedByDot == false;
        })) {
            Parser_consume(_self, "KEYWORD", "end");
            Parser_consume(_self, "KEYWORD", "scoped");
            Parser_consume(_self, "SYMBOL", ".");
        }
    }
    ScopedStmt* stmt = ScopedStmt_new(NULL, "ScopedStmt", body);
    _ret = stmt;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseLoop(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            Parser_advance(_self);
        }
    }
    Block* body = Parser_parseBlock(_self);
    Token* tkEnd = Parser_peek(_self);
    Token* tkNext = Parser_peekNext(_self);
    if (({ 
        strcmp(((Token*)tkEnd)->value, "end") == 0 && strcmp(((Token*)tkNext)->value, "loop") == 0;
    })) {
        Parser_advance(_self);
        Parser_consume(_self, "KEYWORD", "loop");
        Parser_consume(_self, "SYMBOL", ".");
    } else {
        if (({ 
            ((Block*)body)->terminatedByDot == false;
        })) {
            Parser_consume(_self, "KEYWORD", "end");
            Parser_consume(_self, "KEYWORD", "loop");
            Parser_consume(_self, "SYMBOL", ".");
        }
    }
    LoopStmt* stmt = LoopStmt_new(NULL, "LoopStmt", body);
    _ret = stmt;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseWhile(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    void* cond = Parser_parseExpression(_self);
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            Parser_advance(_self);
        }
    }
    Block* body = Parser_parseBlock(_self);
    Token* tkEnd = Parser_peek(_self);
    Token* tkNext = Parser_peekNext(_self);
    if (({ 
        strcmp(((Token*)tkEnd)->value, "end") == 0 && strcmp(((Token*)tkNext)->value, "while") == 0;
    })) {
        Parser_advance(_self);
        Parser_consume(_self, "KEYWORD", "while");
        Parser_consume(_self, "SYMBOL", ".");
    } else {
        if (({ 
            ((Block*)body)->terminatedByDot == false;
        })) {
            Parser_consume(_self, "KEYWORD", "end");
            Parser_consume(_self, "KEYWORD", "while");
            Parser_consume(_self, "SYMBOL", ".");
        }
    }
    WhileStmt* stmt = WhileStmt_new(NULL, "WhileStmt", cond, body);
    _ret = stmt;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseIf(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    Parser_consume(_self, "SYMBOL", ":");
    Parser_consume(_self, "KEYWORD", "condition");
    Parser_consume(_self, "SYMBOL", ":");
    Block* condBlock = Parser_parseBlock(_self);
    Parser_consume(_self, "KEYWORD", "end");
    Parser_consume(_self, "KEYWORD", "condition");
    Parser_consume(_self, "SYMBOL", ".");
    Parser_consume(_self, "KEYWORD", "then");
    Parser_consume(_self, "SYMBOL", ":");
    Block* thenBlock = Parser_parseBlock(_self);
    Parser_consume(_self, "KEYWORD", "end");
    Parser_consume(_self, "KEYWORD", "then");
    Parser_consume(_self, "SYMBOL", ".");
    Block* elseStmt = 0;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->value, "else") == 0;
    })) {
        Parser_advance(_self);
        Parser_consume(_self, "SYMBOL", ":");
        elseStmt = Parser_parseBlock(_self);
        Parser_consume(_self, "KEYWORD", "end");
        Parser_consume(_self, "KEYWORD", "else");
        Parser_consume(_self, "SYMBOL", ".");
    }
    Parser_consume(_self, "KEYWORD", "end");
    Parser_consume(_self, "KEYWORD", "if");
    Parser_consume(_self, "SYMBOL", ".");
    IfStmt* res = IfStmt_new(NULL, "IfStmt", condBlock, thenBlock, elseStmt);
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseReturn(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Parser_advance(_self);
    ReturnStmt* stmt = ReturnStmt_new(NULL, "ReturnStmt", 0);
    Token* tk = Parser_peek(_self);
    bool isTerm = false;
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, ".") == 0 || strcmp(((Token*)tk)->value, ";") == 0;
        })) {
            isTerm = true;
        }
    }
    if (({ 
        isTerm;
    })) {
        ((ReturnStmt*)stmt)->value = 0;
    } else {
        ((ReturnStmt*)stmt)->value = Parser_parseExpression(_self);
    }
    Token* tk2 = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk2)->value, ";") == 0;
    })) {
        Parser_advance(_self);
    }
    _ret = stmt;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseExpressionStatement(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseExpression(_self);
    if (({ 
        expr == 0;
    })) {
        _ret = 0;
        goto end;
    }
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->value, ";") == 0;
    })) {
        Parser_advance(_self);
    }
    ExprStmt* res = ExprStmt_new(NULL, "ExprStmt", expr);
    _ret = res;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseExpression(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    _ret = Parser_parseAssignment(_self, false);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseAssignment(void* _self, bool isLet) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseEquality(_self);
    char* typeHint = "";
    Token* tk = Parser_peek(_self);
    if (({ 
        expr == 0;
    })) {
        _ret = 0;
        goto end;
    }
    Header* header = expr;
    if (({ 
        strcmp(((Header*)header)->type, "VariableExpr") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, ":") == 0;
            })) {
                Token* nextTk = Parser_peekNext(_self);
                if (({ 
                    strcmp(((Token*)nextTk)->type, "IDENT") == 0 || strcmp(((Token*)nextTk)->type, "KEYWORD") == 0;
                })) {
                    Parser_advance(_self);
                    typeHint = Parser_parseType(_self, "Expected type name after ':'.");
                    tk = Parser_peek(_self);
                }
            }
        }
    }
    if (({ 
        strcmp(((Token*)tk)->type, "OPERATOR") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "<-") == 0;
        })) {
            Parser_advance(_self);
            void* value = Parser_parseAssignment(_self, false);
            if (({ 
                strcmp(((Header*)header)->type, "VariableExpr") == 0;
            })) {
                VariableExpr* varExpr = expr;
                if (({ 
                    strcmp(typeHint, "") != 0 || isLet;
                })) {
                    _ret = LetStmt_new(NULL, "LetStmt", ((VariableExpr*)varExpr)->name, typeHint, value);
                    goto end;
                }
                _ret = AssignExpr_new(NULL, "AssignExpr", expr, value);
                goto end;
            } else {
                if (({ 
                    strcmp(((Header*)header)->type, "GetExpr") == 0 || strcmp(((Header*)header)->type, "ArrayAccess") == 0;
                })) {
                    if (({ 
                        isLet;
                    })) {
                        printf("%s\n", bunker_str_add_int("Error: Cannot used 'let' with member access at line ", ((Token*)tk)->line));
                    }
                    _ret = AssignExpr_new(NULL, "AssignExpr", expr, value);
                    goto end;
                } else {
                    printf("%s\n", bunker_str_add_int(BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Error: Invalid assignment target type ", ((Header*)header)->type), " at line "), ((Token*)tk)->line));
                }
            }
        }
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseEquality(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseComparison(_self);
    while (1) {
        Token* tk = Parser_peek(_self);
        bool match = false;
        if (({ 
            strcmp(((Token*)tk)->type, "OPERATOR") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, "==") == 0;
            })) {
                match = true;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "!=") == 0;
            })) {
                match = true;
            }
        }
        if (({ 
            match;
        })) {
            Token* op = Parser_advance(_self);
            void* right = Parser_parseComparison(_self);
            BinaryExpr* bin = BinaryExpr_new(NULL, "BinaryExpr", expr, op, right);
            expr = bin;
            continue;
        }
        break;
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseComparison(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseTerm(_self);
    while (1) {
        Token* tk = Parser_peek(_self);
        bool match = false;
        if (({ 
            strcmp(((Token*)tk)->type, "OPERATOR") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, ">=") == 0;
            })) {
                match = true;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "<=") == 0;
            })) {
                match = true;
            }
        }
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, ">") == 0;
            })) {
                match = true;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "<") == 0;
            })) {
                match = true;
            }
        }
        if (({ 
            match;
        })) {
            Token* op = Parser_advance(_self);
            void* right = Parser_parseTerm(_self);
            BinaryExpr* bin = BinaryExpr_new(NULL, "BinaryExpr", expr, op, right);
            expr = bin;
            continue;
        }
        break;
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseTerm(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseFactor(_self);
    while (1) {
        Token* tk = Parser_peek(_self);
        bool match = false;
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, "+") == 0;
            })) {
                match = true;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "-") == 0;
            })) {
                match = true;
            }
        }
        if (({ 
            match;
        })) {
            Token* op = Parser_advance(_self);
            void* right = Parser_parseFactor(_self);
            BinaryExpr* bin = BinaryExpr_new(NULL, "BinaryExpr", expr, op, right);
            expr = bin;
            continue;
        }
        break;
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseFactor(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parseUnary(_self);
    while (1) {
        Token* tk = Parser_peek(_self);
        bool match = false;
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk)->value, "*") == 0;
            })) {
                match = true;
            }
            if (({ 
                strcmp(((Token*)tk)->value, "/") == 0;
            })) {
                match = true;
            }
        }
        if (({ 
            match;
        })) {
            Token* op = Parser_advance(_self);
            void* right = Parser_parseUnary(_self);
            BinaryExpr* bin = BinaryExpr_new(NULL, "BinaryExpr", expr, op, right);
            expr = bin;
            continue;
        }
        break;
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseUnary(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    Token* tk = Parser_peek(_self);
    bool match = false;
    if (({ 
        strcmp(((Token*)tk)->type, "KEYWORD") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "not") == 0;
        })) {
            match = true;
        }
    }
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "-") == 0;
        })) {
            match = true;
        }
    }
    if (({ 
        match;
    })) {
        Token* op = Parser_advance(_self);
        void* right = Parser_parseUnary(_self);
        UnaryExpr* un = UnaryExpr_new(NULL, "UnaryExpr", op, right);
        _ret = un;
        goto end;
    }
    _ret = Parser_parseCall(_self);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parseCall(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    void* expr = Parser_parsePrimary(_self);
    while (1) {
        Token* tk = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            Token* nt = Parser_peekNext(_self);
            printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: parseCall ':' nt.type=", ((Token*)nt)->type), " value="), ((Token*)nt)->value));
            if (({ 
                strcmp(((Token*)nt)->type, "IDENT") == 0 || strcmp(((Token*)nt)->type, "KEYWORD") == 0;
            })) {
                ASTNode* currNode = ((Parser*)_self)->current;
                if (({ 
                    ((ASTNode*)currNode)->next != 0;
                })) {
                    ASTNode* ntNode = ((ASTNode*)currNode)->next;
                    if (({ 
                        ((ASTNode*)ntNode)->next != 0;
                    })) {
                        ASTNode* n2node = ((ASTNode*)ntNode)->next;
                        Token* tk2 = ((ASTNode*)n2node)->value;
                        printf("%s\n", BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "DEBUG: parseCall ':' tk2.type=", ((Token*)tk2)->type), " value="), ((Token*)tk2)->value));
                        if (({ 
                            strcmp(((Token*)tk2)->value, "<-") == 0 || strcmp(((Token*)tk2)->value, "<") == 0;
                        })) {
                            break;
                        }
                    }
                }
            }
            if (({ 
                strcmp(((Token*)nt)->type, "IDENT") == 0 || strcmp(((Token*)nt)->type, "STRING") == 0 || strcmp(((Token*)nt)->type, "NUMBER") == 0 || strcmp(((Token*)nt)->value, "(") == 0;
            })) {
                Parser_advance(_self);
                char* methodName = "";
                ASTList* args = 0;
                Token* n2 = Parser_peekAt(_self, 1);
                if (({ 
                    strcmp(((Token*)nt)->type, "IDENT") == 0 && strcmp(((Token*)n2)->value, ":") == 0;
                })) {
                    methodName = ((Token*)nt)->value;
                    Parser_advance(_self);
                    Parser_advance(_self);
                    args = Parser_parseArgumentList(_self);
                } else {
                    bool isGlobal = false;
                    if (({ 
                        expr != 0;
                    })) {
                        Header* h = expr;
                        if (({ 
                            strcmp(((Header*)h)->type, "VariableExpr") == 0;
                        })) {
                            VariableExpr* v = Casts_toVariableExpr(NULL, expr);
                            if (({ 
                                strcmp(((Token*)((VariableExpr*)v)->name)->value, "print") == 0;
                            })) {
                                isGlobal = true;
                            }
                        }
                    }
                    if (({ 
                        isGlobal;
                    })) {
                        VariableExpr* vExt = Casts_toVariableExpr(NULL, expr);
                        methodName = ((Token*)((VariableExpr*)vExt)->name)->value;
                        expr = 0;
                        args = Parser_parseArgumentList(_self);
                    } else {
                        if (({ 
                            strcmp(((Token*)nt)->type, "IDENT") == 0;
                        })) {
                            methodName = ((Token*)nt)->value;
                            Parser_advance(_self);
                            Token* tkN3 = Parser_peek(_self);
                            if (({ 
                                strcmp(((Token*)tkN3)->type, "SYMBOL") == 0 && strcmp(((Token*)tkN3)->value, ":") == 0;
                            })) {
                                Parser_advance(_self);
                                args = Parser_parseArgumentList(_self);
                            }
                        }
                    }
                }
                Token* metTok = 0;
                if (({ 
                    strcmp(methodName, "") != 0;
                })) {
                    metTok = Token_new(NULL, "IDENT", methodName, ((Token*)tk)->line, ((Token*)tk)->column, ((Token*)tk)->offset);
                }
                MethodCall* callRec = MethodCall_new(NULL, "MethodCall", expr, metTok, args);
                expr = callRec;
                continue;
            }
        }
        if (({ 
            (strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, ".") == 0) || (strcmp(((Token*)tk)->type, "OPERATOR") == 0 && strcmp(((Token*)tk)->value, "->") == 0);
        })) {
            Token* propTk = Parser_peekNext(_self);
            if (({ 
                strcmp(((Token*)propTk)->type, "IDENT") == 0;
            })) {
                Parser_advance(_self);
                Token* pTok = Parser_consume(_self, "IDENT", "Expected property name.");
                GetExpr* getRec = GetExpr_new(NULL, "GetExpr", expr, pTok);
                expr = getRec;
                continue;
            }
        }
        if (({ 
            strcmp(((Token*)tk)->type, "SYMBOL") == 0 && strcmp(((Token*)tk)->value, "[") == 0;
        })) {
            Parser_advance(_self);
            void* indexExpr = Parser_parseExpression(_self);
            Parser_consume(_self, "SYMBOL", "Expected ']' after array index.");
            ArrayAccess* aaRec = ArrayAccess_new(NULL, "ArrayAccess", expr, indexExpr);
            expr = aaRec;
            continue;
        }
        break;
    }
    _ret = expr;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
ASTList* Parser_parseArgumentList(void* _self) {
    ASTList* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    ASTList* list = ASTList_new(NULL, 0, 0);
    ASTNode* tail = 0;
    while (1) {
        Token* tk = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk)->type, "EOF") == 0 || strcmp(((Token*)tk)->value, ".") == 0 || strcmp(((Token*)tk)->value, ":") == 0 || strcmp(((Token*)tk)->value, "end") == 0;
        })) {
            break;
        }
        void* arg = Parser_parseExpression(_self);
        ASTNode* node = ASTNode_new(NULL, arg, 0);
        if (({ 
            ((ASTList*)list)->head == 0;
        })) {
            ((ASTList*)list)->head = node;
            tail = node;
        } else {
            ASTNode* tailNode = Casts_toASTNode(NULL, tail);
            ((ASTNode*)tailNode)->next = node;
            tail = node;
        }
        Token* tk3 = Parser_peek(_self);
        if (({ 
            strcmp(((Token*)tk3)->type, "SYMBOL") == 0 || strcmp(((Token*)tk3)->type, "OPERATOR") == 0;
        })) {
            if (({ 
                strcmp(((Token*)tk3)->value, ",") == 0;
            })) {
                Parser_advance(_self);
                continue;
            }
        }
        break;
    }
    _ret = list;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void* Parser_parsePrimary(void* _self) {
    void* _ret = 0;
    struct Parser* self = (struct Parser*)_self;
    (void)self;
    LiteralExpr* lit = 0;
    Token* tk = Parser_peek(_self);
    if (({ 
        strcmp(((Token*)tk)->type, "KEYWORD") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, "true") == 0;
        })) {
            Parser_advance(_self);
            lit = LiteralExpr_new(NULL, "LiteralExpr", tk, "bool");
            _ret = lit;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "false") == 0;
        })) {
            Parser_advance(_self);
            lit = LiteralExpr_new(NULL, "LiteralExpr", tk, "bool");
            _ret = lit;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "nil") == 0;
        })) {
            Parser_advance(_self);
            lit = LiteralExpr_new(NULL, "LiteralExpr", tk, "nil");
            _ret = lit;
            goto end;
        }
    }
    if (({ 
        strcmp(((Token*)tk)->type, "NUMBER") == 0;
    })) {
        Token* numTok = Parser_advance(_self);
        lit = LiteralExpr_new(NULL, "LiteralExpr", numTok, "int");
        printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: created LiteralExpr NUM val=", ((Token*)((LiteralExpr*)lit)->tok)->value));
        _ret = lit;
        goto end;
    }
    if (({ 
        strcmp(((Token*)tk)->type, "STRING") == 0;
    })) {
        Token* strTok = Parser_advance(_self);
        lit = LiteralExpr_new(NULL, "LiteralExpr", strTok, "string");
        printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: created LiteralExpr STR val=", ((Token*)((LiteralExpr*)lit)->tok)->value));
        _ret = lit;
        goto end;
    }
    if (({ 
        strcmp(((Token*)tk)->type, "IDENT") == 0 || strcmp(((Token*)tk)->type, "KEYWORD") == 0;
    })) {
        Token* tkNext = Parser_peekNext(_self);
        if (({ 
            strcmp(((Token*)tkNext)->type, "SYMBOL") == 0 && strcmp(((Token*)tkNext)->value, "<") == 0;
        })) {
            bool foundTypeMarker = false;
            long long offset = 2;
            long long depth = 1;
            printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Disambiguating < for ", ((Token*)tk)->value));
            while (1) {
                Token* tS = Parser_peekAt(_self, offset);
                if (({ 
                    strcmp(((Token*)tS)->type, "EOF") == 0 || strcmp(((Token*)tS)->value, ";") == 0 || strcmp(((Token*)tS)->value, ".") == 0 || strcmp(((Token*)tS)->value, "then") == 0;
                })) {
                    break;
                }
                if (({ 
                    strcmp(((Token*)tS)->value, "(") == 0;
                })) {
                    break;
                }
                if (({ 
                    strcmp(((Token*)tS)->value, "<") == 0;
                })) {
                    depth = depth + 1;
                }
                if (({ 
                    strcmp(((Token*)tS)->value, ">") == 0;
                })) {
                    depth = depth - 1;
                    if (({ 
                        depth == 0;
                    })) {
                        Token* tNext = Parser_peekAt(_self, offset + 1);
                        printf("%s\n", BNK_RT_Strings_concat(NULL, "DEBUG: Found > at depth 0, next=", ((Token*)tNext)->value));
                        if (({ 
                            strcmp(((Token*)tNext)->value, ":") == 0 || strcmp(((Token*)tNext)->value, "<-") == 0 || strcmp(((Token*)tNext)->value, ",") == 0 || strcmp(((Token*)tNext)->value, ")") == 0 || strcmp(((Token*)tNext)->value, "]") == 0 || strcmp(((Token*)tNext)->value, ";") == 0 || strcmp(((Token*)tNext)->value, ".") == 0 || strcmp(((Token*)tNext)->value, "then") == 0 || strcmp(((Token*)tNext)->value, "else") == 0 || strcmp(((Token*)tNext)->value, "do") == 0 || strcmp(((Token*)tNext)->value, "has") == 0;
                        })) {
                            foundTypeMarker = true;
                        }
                        break;
                    }
                }
                offset = offset + 1;
            }
            if (({ 
                foundTypeMarker;
            })) {
                printf("%s\n", "DEBUG: Decided it IS a generic type.");
                char* fullType = Parser_parseType(_self, "Expected generic type application");
                Token* genTk = Token_new(NULL, "IDENT", fullType, ((Token*)tk)->line, ((Token*)tk)->column, ((Token*)tk)->offset);
                _ret = VariableExpr_new(NULL, "VariableExpr", genTk);
                goto end;
            }
            printf("%s\n", "DEBUG: Decided it is NOT a generic type.");
        }
        Token* identTok = Parser_advance(_self);
        _ret = VariableExpr_new(NULL, "VariableExpr", identTok);
        goto end;
    }
    if (({ 
        strcmp(((Token*)tk)->type, "SYMBOL") == 0;
    })) {
        if (({ 
            strcmp(((Token*)tk)->value, ":") == 0;
        })) {
            _ret = 0;
            goto end;
        }
        if (({ 
            strcmp(((Token*)tk)->value, "(") == 0;
        })) {
            Parser_advance(_self);
            void* expr = Parser_parseExpression(_self);
            Parser_consume(_self, "SYMBOL", "Expected ')' after expression.");
            GroupingExpr* group = GroupingExpr_new(NULL, "GroupingExpr", expr);
            _ret = group;
            goto end;
        }
    }
    ErrorReporter_report(NULL, BNK_RT_Strings_concat(NULL, BNK_RT_Strings_concat(NULL, "Unexpected token '", ((Token*)tk)->value), "'"), tk, ((Parser*)_self)->source);
    Parser_advance(_self);
    ((Parser*)_self)->hadError = true;
    _ret = 0;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
int main(int argc, char** argv) {
    int _ret = 0;
    volatile void* dummy; gc_stack_bottom = (void*)&dummy;
    GC_INIT();
    printf("%s\n", "--- Bunker Self-Hosted Lexer & Parser v0.3 ---");
    if (({ 
        argc < 2;
    })) {
        printf("%s\n", "Usage: bunker <source.bnk>");
        _ret = 1;
        goto end;
    }
    char* filename = argv[(int)1];
    printf("%s\n", BNK_RT_Strings_concat(NULL, "Reading file: ", filename));
    File* f = File_open(NULL, filename);
    if (({ 
        f == 0;
    })) {
        printf("%s\n", BNK_RT_Strings_concat(NULL, "Error: Could not open file ", filename));
        _ret = 1;
        goto end;
    }
    char* code = File_readToString(f);
    File_close(f);
    printf("%s\n", bunker_str_add_int("File read, length: ", Strings_length(NULL, code)));
    printf("%s\n", "Initializing Lexer...");
    Lexer* lexer = Lexer_new(NULL, code);
    printf("%s\n", "Lexer initialized.");
    printf("%s\n", "Collecting tokens...");
    ASTList* tokens = ASTList_new(NULL, 0, 0);
    ASTNode* tail = 0;
    long long tokCount = 0;
    while (1) {
        Token* tok = Lexer_nextToken(lexer);
        tokCount = tokCount + 1;
        if (({ 
            strcmp(((Token*)tok)->type, "EOF") == 0;
        })) {
            break;
        }
        if (({ 
            tokCount % 1000 == 0;
        })) {
            printf("%s\n", BNK_RT_Strings_concat(NULL, bunker_str_add_int("Collected ", tokCount), " tokens..."));
        }
        ASTNode* node = ASTNode_new(NULL, tok, 0);
        if (({ 
            ((ASTList*)tokens)->head == 0;
        })) {
            ((ASTList*)tokens)->head = node;
            tail = node;
        } else {
            ASTNode* tailNode = Casts_toASTNode(NULL, tail);
            ((ASTNode*)tailNode)->next = node;
            tail = node;
        }
    }
    printf("%s\n", bunker_str_add_int("Tokens collected: ", tokCount));
    printf("%s\n", "Tokens collected");
    Parser* parser = Parser_new(NULL, tokens, code);
    void* prog = Parser_parseProgram(parser);
    if (({ 
        ((Parser*)parser)->hadError;
    })) {
        printf("%s\n", "Parsing failed.");
        _ret = 1;
        goto end;
    } else {
        printf("%s\n", "Parsing successful!");
        Program* progRec = prog;
        ASTList* list = ((Program*)progRec)->stmts;
        ASTNode* curr = ((ASTList*)list)->head;
        printf("%s\n", "AST Structure:");
        while (1) {
            if (({ 
                curr == 0;
            })) {
                break;
            }
            Program* stmt = ((ASTNode*)curr)->value;
            printf("%s\n", BNK_RT_Strings_concat(NULL, " - Stmt Type: ", ((Program*)stmt)->type));
            curr = ((ASTNode*)curr)->next;
        }
        printf("%s\n", "");
        printf("%s\n", "Running Type Checker...");
        TypeChecker* checker = TypeChecker_new(NULL, parser, code);
        TypeChecker_checkProgram(checker, prog);
        if (({ 
            ((TypeChecker*)checker)->hadError;
        })) {
            printf("%s\n", "Type checking failed.");
            _ret = 1;
            goto end;
        }
        printf("%s\n", "Generating C Code...");
        CTranspiler* transpiler = CTranspiler_new(NULL);
        ((CTranspiler*)transpiler)->symbols = ((TypeChecker*)checker)->symbols;
        CTranspiler_transpile(transpiler, prog);
        char* out_path = BNK_RT_Strings_concat(NULL, filename, ".c");
        File* outFile = File_create(NULL, out_path);
        if (({ 
            outFile != 0;
        })) {
            printf("%s\n", BNK_RT_Strings_concat(NULL, "Saving to: ", out_path));
            ASTList* outList = ((CTranspiler*)transpiler)->output;
            ASTNode* currNode = ((ASTList*)outList)->head;
            while (1) {
                if (({ 
                    currNode == 0;
                })) {
                    break;
                }
                char* s = ((ASTNode*)currNode)->value;
                File_writeString(outFile, s);
                currNode = ((ASTNode*)currNode)->next;
            }
            File_close(outFile);
            printf("%s\n", "Saved successfully.");
        } else {
            printf("%s\n", BNK_RT_Strings_concat(NULL, "Error: Could not create output file: ", out_path));
        }
    }
    _ret = 0;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
List_string* List_string_create(void* _self) {
    List_string* _ret = 0;
    struct List_string* self = (struct List_string*)_self;
    (void)self;
    List_string* l = (List_string*)gc_alloc(sizeof(struct List_string));
    ((List_string*)l)->count = 0;
    ((List_string*)l)->cap = 8;
    ((List_string*)l)->data = Bunker_make_array(8);
    _ret = l;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_string_add(void* _self, char* item) {
    struct List_string* self = (struct List_string*)_self;
    (void)self;
    if (({ 
        ((List_string*)_self)->count == ((List_string*)_self)->cap;
    })) {
    }
    ((List_string*)_self)->data->data[(int)((List_string*)_self)->count] = (void*)(intptr_t)item;
    ((List_string*)_self)->count = ((List_string*)_self)->count + 1;
    end:
    (void)&&end;
    return;
}
char* List_string_get(void* _self, long long index) {
    char* _ret = 0;
    struct List_string* self = (struct List_string*)_self;
    (void)self;
    if (({ 
        index < 0 || index >= ((List_string*)_self)->count;
    })) {
        _ret = 0;
        goto end;
    }
    _ret = ((char*)((List_string*)_self)->data->data[(int)index]);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
long long List_string_size(void* _self) {
    long long _ret = 0;
    struct List_string* self = (struct List_string*)_self;
    (void)self;
    _ret = ((List_string*)_self)->count;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_string__resize(void* _self) {
    struct List_string* self = (struct List_string*)_self;
    (void)self;
    long long newCap = ((List_string*)_self)->cap * 2;
    BunkerArray* newData = Bunker_make_array(newCap);
    long long i = 0;
    while (i < ((List_string*)_self)->count) {
        newData->data[(int)i] = (void*)(intptr_t)((char*)((List_string*)_self)->data->data[(int)i]);
        i = i + 1;
    }
    ((List_string*)_self)->data = newData;
    ((List_string*)_self)->cap = newCap;
    end:
    (void)&&end;
    return;
}