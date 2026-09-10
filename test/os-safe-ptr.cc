/*
 * safe_load() / safe_store() width test.
 *
 * These access an address that may not be mapped and report failure instead of
 * faulting. The access must be exactly sizeof(T) wide: a wider load can fault
 * on a page the caller never asked about, and a wider store runs past the
 * destination into whatever follows it.
 *
 * Checks the value round-trips for every supported width, that a store writes
 * only its own bytes, and that a load of a value at the end of a mapped page
 * succeeds even when the next page is not mapped.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include <safe-ptr.hh>

static int g_failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            g_failures++; \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

template <typename T>
static void check_roundtrip(const char* name, T value)
{
    struct { T before; T target; T after; } box;
    box.before = (T)~(uint64_t)0;
    box.after = (T)~(uint64_t)0;
    box.target = 0;

    CHECK(safe_store(&box.target, value), "%s: safe_store reported failure", name);
    CHECK(box.target == value, "%s: stored 0x%llx, read back 0x%llx", name,
          (unsigned long long)value, (unsigned long long)box.target);
    CHECK(box.before == (T)~(uint64_t)0, "%s: store wrote before the target", name);
    CHECK(box.after == (T)~(uint64_t)0,
          "%s: store ran past the target into the next %zu bytes",
          name, sizeof(T));

    // The load destination gets guards too: safe_load must write sizeof(T)
    // into it, not the width of the register it loaded through.
    struct { T before; T out; T after; } dst;
    dst.before = (T)~(uint64_t)0;
    dst.after = (T)~(uint64_t)0;
    dst.out = 0;

    CHECK(safe_load(&box.target, dst.out), "%s: safe_load reported failure", name);
    CHECK(dst.out == value, "%s: loaded 0x%llx, want 0x%llx", name,
          (unsigned long long)dst.out, (unsigned long long)value);
    CHECK(dst.before == (T)~(uint64_t)0, "%s: load wrote before its output", name);
    CHECK(dst.after == (T)~(uint64_t)0,
          "%s: load ran past its output into the next %zu bytes",
          name, sizeof(T));
}

// A value at the very end of a mapped page, with the next page unmapped. A load
// wider than the value reaches into the unmapped page and fails.
template <typename T>
static void check_page_edge(const char* name)
{
    const size_t pagesize = 4096;
    char* region = (char*)mmap(nullptr, 2 * pagesize, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        printf("    SKIP %s: mmap failed\n", name);
        return;
    }
    if (munmap(region + pagesize, pagesize) != 0) {
        printf("    SKIP %s: munmap of the trailing page failed\n", name);
        munmap(region, 2 * pagesize);
        return;
    }

    T* edge = (T*)(region + pagesize - sizeof(T));
    T value = (T)0x1122334455667788ull;
    memcpy(edge, &value, sizeof(T));

    T out = 0;
    CHECK(safe_load(edge, out),
          "%s: load of the last %zu bytes of a page failed -- it read wider "
          "than the value and faulted on the unmapped page after it",
          name, sizeof(T));
    CHECK(out == value, "%s: loaded 0x%llx, want 0x%llx", name,
          (unsigned long long)out, (unsigned long long)value);

    munmap(region, pagesize);
}

int os_safe_ptr_main()
{
    printf("---- safe_load / safe_store test ----\n");
    g_failures = 0;

    printf("  round-trip and no write outside the target\n");
    check_roundtrip<uint8_t>("uint8_t", 0xa5);
    check_roundtrip<uint16_t>("uint16_t", 0xa55a);
    check_roundtrip<uint32_t>("uint32_t", 0xdeadbeef);
    check_roundtrip<uint64_t>("uint64_t", 0x0123456789abcdefull);

    printf("  load at the end of a mapped page\n");
    check_page_edge<uint8_t>("uint8_t");
    check_page_edge<uint16_t>("uint16_t");
    check_page_edge<uint32_t>("uint32_t");
    check_page_edge<uint64_t>("uint64_t");

    printf("---- safe_load / safe_store: %s ----\n",
           g_failures ? "FAILURE" : "all checks passed");
    return g_failures ? 1 : 0;
}
