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

typedef struct List List;
typedef struct App App;
typedef struct List_int List_int;
typedef struct List_str List_str;
struct List {
    BunkerArray* data;
    long long count;
    long long cap;
};
struct App {
};
struct List_int {
    BunkerArray* data;
    long long count;
    long long cap;
};
struct List_str {
    BunkerArray* data;
    long long count;
    long long cap;
};
List_int* List_int_create(void* _self);
void List_int_append(void* _self, long long item);
long long List_int_get(void* _self, long long index);
long long List_int_size(void* _self);
void List_int__resize(void* _self);
List_str* List_str_create(void* _self);
void List_str_append(void* _self, char* item);
char* List_str_get(void* _self, long long index);
long long List_str_size(void* _self);
void List_str__resize(void* _self);
int main(int argc, char** argv) {
    int _ret = 0;
    volatile void* dummy; gc_stack_bottom = (void*)&dummy;
    GC_INIT();
    printf("%s\n", "=== Generics Test ===");
    List_int* list = List_create(NULL);
    List_int_append(list, 10);
    List_int_append(list, 20);
    long long val = List_int_get(list, 0);
    printf("%lld\n", (long long)val);
    long long val2 = List_int_get(list, 1);
    printf("%lld\n", (long long)val2);
    long long size = List_int_size(list);
    printf("%lld\n", (long long)size);
    List_str* slist = List_create(NULL);
    List_str_append(slist, "Hello");
    List_str_append(slist, "World");
    char* s1 = List_str_get(slist, 0);
    printf("%s\n", s1);
    char* s2 = List_str_get(slist, 1);
    printf("%s\n", s2);
    printf("%s\n", "=== Done ===");
    end:
    (void)&&end;
    return _ret;
}
List_int* List_int_create(void* _self) {
    List_int* _ret = 0;
    struct List_int* self = (struct List_int*)_self;
    (void)self;
    List_int* l = (List_int*)gc_alloc(sizeof(struct List_int));
    ((List_int*)l)->count = 0;
    ((List_int*)l)->cap = 8;
    ((List_int*)l)->data = Bunker_make_array(8);
    _ret = l;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_int_append(void* _self, long long item) {
    struct List_int* self = (struct List_int*)_self;
    (void)self;
    if (({ 
        ((List_int*)_self)->count == ((List_int*)_self)->cap;
    })) {
    }
    ((List_int*)_self)->data->data[(int)((List_int*)_self)->count] = (void*)(intptr_t)item;
    ((List_int*)_self)->count = ((List_int*)_self)->count + 1;
    end:
    (void)&&end;
    return;
}
long long List_int_get(void* _self, long long index) {
    long long _ret = 0;
    struct List_int* self = (struct List_int*)_self;
    (void)self;
    if (({ 
        index < 0 || index >= ((List_int*)_self)->count;
    })) {
        _ret = 0;
        goto end;
    }
    _ret = ((long long)(intptr_t)((List_int*)_self)->data->data[(int)index]);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
long long List_int_size(void* _self) {
    long long _ret = 0;
    struct List_int* self = (struct List_int*)_self;
    (void)self;
    _ret = ((List_int*)_self)->count;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_int__resize(void* _self) {
    struct List_int* self = (struct List_int*)_self;
    (void)self;
    long long newCap = ((List_int*)_self)->cap * 2;
    BunkerArray* newData = Bunker_make_array(newCap);
    long long i = 0;
    while (i < ((List_int*)_self)->count) {
        newData->data[(int)i] = (void*)(intptr_t)((long long)(intptr_t)((List_int*)_self)->data->data[(int)i]);
        i = i + 1;
    }
    ((List_int*)_self)->data = newData;
    ((List_int*)_self)->cap = newCap;
    end:
    (void)&&end;
    return;
}
List_str* List_str_create(void* _self) {
    List_str* _ret = 0;
    struct List_str* self = (struct List_str*)_self;
    (void)self;
    List_str* l = (List_str*)gc_alloc(sizeof(struct List_str));
    ((List_str*)l)->count = 0;
    ((List_str*)l)->cap = 8;
    ((List_str*)l)->data = Bunker_make_array(8);
    _ret = l;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_str_append(void* _self, char* item) {
    struct List_str* self = (struct List_str*)_self;
    (void)self;
    if (({ 
        ((List_str*)_self)->count == ((List_str*)_self)->cap;
    })) {
    }
    ((List_str*)_self)->data->data[(int)((List_str*)_self)->count] = (void*)(intptr_t)item;
    ((List_str*)_self)->count = ((List_str*)_self)->count + 1;
    end:
    (void)&&end;
    return;
}
char* List_str_get(void* _self, long long index) {
    char* _ret = 0;
    struct List_str* self = (struct List_str*)_self;
    (void)self;
    if (({ 
        index < 0 || index >= ((List_str*)_self)->count;
    })) {
        _ret = 0;
        goto end;
    }
    _ret = ((char*)((List_str*)_self)->data->data[(int)index]);
    goto end;
    end:
    (void)&&end;
    return _ret;
}
long long List_str_size(void* _self) {
    long long _ret = 0;
    struct List_str* self = (struct List_str*)_self;
    (void)self;
    _ret = ((List_str*)_self)->count;
    goto end;
    end:
    (void)&&end;
    return _ret;
}
void List_str__resize(void* _self) {
    struct List_str* self = (struct List_str*)_self;
    (void)self;
    long long newCap = ((List_str*)_self)->cap * 2;
    BunkerArray* newData = Bunker_make_array(newCap);
    long long i = 0;
    while (i < ((List_str*)_self)->count) {
        newData->data[(int)i] = (void*)(intptr_t)((char*)((List_str*)_self)->data->data[(int)i]);
        i = i + 1;
    }
    ((List_str*)_self)->data = newData;
    ((List_str*)_self)->cap = newCap;
    end:
    (void)&&end;
    return;
}