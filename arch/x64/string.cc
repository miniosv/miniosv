/*
 * memcmp for x86-64.
 *
 */

#include <cstddef>
#include <cstdint>

namespace {

// Unaligned, and allowed to alias whatever the caller's buffer really holds.
typedef uint64_t u64u __attribute__((may_alias, aligned(1)));
typedef uint32_t u32u __attribute__((may_alias, aligned(1)));
typedef uint16_t u16u __attribute__((may_alias, aligned(1)));

typedef unsigned char v16 __attribute__((vector_size(16), may_alias, aligned(1)));
typedef char v16i __attribute__((vector_size(16)));

inline int diff_in_chunk(const unsigned char *a, const unsigned char *b)
{
    const v16 x = *reinterpret_cast<const v16 *>(a);
    const v16 y = *reinterpret_cast<const v16 *>(b);
    const unsigned mask =
        static_cast<unsigned>(__builtin_ia32_pmovmskb128(v16i(x == y)));

    if (mask == 0xffffu) {
        return 0;
    }
    const unsigned i = static_cast<unsigned>(__builtin_ctz(~mask & 0xffffu));
    return int(a[i]) - int(b[i]);
}

inline int cmp_be64(const unsigned char *a, const unsigned char *b)
{
    const uint64_t x = __builtin_bswap64(*reinterpret_cast<const u64u *>(a));
    const uint64_t y = __builtin_bswap64(*reinterpret_cast<const u64u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

inline int cmp_be32(const unsigned char *a, const unsigned char *b)
{
    const uint32_t x = __builtin_bswap32(*reinterpret_cast<const u32u *>(a));
    const uint32_t y = __builtin_bswap32(*reinterpret_cast<const u32u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

inline int cmp_be16(const unsigned char *a, const unsigned char *b)
{
    const uint16_t x = __builtin_bswap16(*reinterpret_cast<const u16u *>(a));
    const uint16_t y = __builtin_bswap16(*reinterpret_cast<const u16u *>(b));
    return x == y ? 0 : (x < y ? -1 : 1);
}

} // namespace

extern "C" int memcmp(const void *lhs, const void *rhs, size_t n)
{
    const auto *a = static_cast<const unsigned char *>(lhs);
    const auto *b = static_cast<const unsigned char *>(rhs);

    // Under 64 bytes, loading two xmm registers costs more than it saves
    if (n < 16) {
        if (n >= 8) {
            if (const int d = cmp_be64(a, b)) {
                return d;
            }
            return cmp_be64(a + n - 8, b + n - 8);
        }
        if (n >= 4) {
            if (const int d = cmp_be32(a, b)) {
                return d;
            }
            return cmp_be32(a + n - 4, b + n - 4);
        }
        if (n >= 2) {
            if (const int d = cmp_be16(a, b)) {
                return d;
            }
            return cmp_be16(a + n - 2, b + n - 2);
        }
        return n == 1 ? int(a[0]) - int(b[0]) : 0;
    }
    if (n <= 32) {
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        return diff_in_chunk(a + n - 16, b + n - 16);
    }
    if (n < 64) {
        // Check chunks at 0, 16, n-32 and n-16.
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        if (const int d = diff_in_chunk(a + 16, b + 16)) {
            return d;
        }
        if (const int d = diff_in_chunk(a + n - 32, b + n - 32)) {
            return d;
        }
        return diff_in_chunk(a + n - 16, b + n - 16);
    }

    while (n >= 64) {
        const v16 e0 = *reinterpret_cast<const v16 *>(a) ==
                       *reinterpret_cast<const v16 *>(b);
        const v16 e1 = *reinterpret_cast<const v16 *>(a + 16) ==
                       *reinterpret_cast<const v16 *>(b + 16);
        const v16 e2 = *reinterpret_cast<const v16 *>(a + 32) ==
                       *reinterpret_cast<const v16 *>(b + 32);
        const v16 e3 = *reinterpret_cast<const v16 *>(a + 48) ==
                       *reinterpret_cast<const v16 *>(b + 48);

        if (__builtin_ia32_pmovmskb128(v16i(e0 & e1 & e2 & e3)) !=
            static_cast<int>(0xffff)) {
            for (unsigned off = 0; off < 64; off += 16) {
                if (const int d = diff_in_chunk(a + off, b + off)) {
                    return d;
                }
            }
        }
        a += 64;
        b += 64;
        n -= 64;
    }

    while (n >= 16) {
        if (const int d = diff_in_chunk(a, b)) {
            return d;
        }
        a += 16;
        b += 16;
        n -= 16;
    }
    if (n == 0) {
        return 0;
    }
    return diff_in_chunk(a + n - 16, b + n - 16);
}
