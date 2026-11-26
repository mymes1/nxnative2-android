// Darwin-only interfaces the shared sources call that have no Android counterpart yet; each says what is missing.
#include "dm.h"
#include <stdlib.h>
#include <string.h>
// thread_selfcounts (libSystem private): the calling thread's {instructions, cycles}. Android would need perf_event_open, which apps may not have: zeros
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
// mergesort (BSD libc): stable, O(n log n). Android's bionic has no mergesort, so the shared sources' call needs one here.
int mergesort(void *base, size_t nel, size_t size, int (*compar)(const void *, const void *)) {
    if (nel < 2 || size == 0) return 0;
    uint8_t *q = malloc(nel * size);
    if (!q) return -1;
    uint8_t *src = base, *dst = q;
    size_t len = 1;
    while (len < nel) {
        for (size_t lo = 0; lo < nel; lo += 2 * len) {
            size_t mid = lo + len; if (mid > nel) mid = nel;
            size_t hi = lo + 2 * len; if (hi > nel) hi = nel;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (compar(src + i * size, src + j * size) <= 0) memcpy(dst + k * size, src + i * size, size), i++;
                else memcpy(dst + k * size, src + j * size, size), j++;
                k++;
            }
            while (i < mid) memcpy(dst + k++ * size, src + i++ * size, size);
            while (j < hi) memcpy(dst + k++ * size, src + j++ * size, size);
        }
        uint8_t *t = src; src = dst; dst = t;
        len *= 2;
    }
    if (src != base) memcpy(base, src, nel * size);
    free(q);
    return 0;
}
// dm_cheats_init (hle_cheats.c): the Atmosphère cheat-file loader. hle_cheats.c is test-only and
// is excluded from the Android host build (its DM_CHEAT_* macros and dm_guest_heap_* are not defined
// for a host build, and nothing on Android calls its interface). main.c still calls dm_cheats_init
// unconditionally, so provide a no-op here: no cheats on Android.
void dm_cheats_init(void) {}
