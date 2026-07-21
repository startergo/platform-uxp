/* Compatibility shims for Mac OS X 10.6.
 *
 * Some BSD functions (arc4random, arc4random_buf) exist in 10.7+ libSystem
 * but not in 10.6's. UXP's AC_CHECK_FUNCS finds them on the build host
 * (modern macOS) and the JS shell's configure doesn't honor top-level
 * mozconfig cache overrides, so call sites in js/src/jsmath.cpp and
 * xpcom/base/nsUUIDGenerator.cpp emit references to _arc4random that dyld
 * can't resolve at runtime on 10.6.
 *
 * Provide small implementations using /dev/urandom. They are linked into
 * libmozglue.dylib, which is loaded by every other dylib in the bundle,
 * so the symbols resolve uniformly.
 *
 * The use of __attribute__((visibility("default"))) is required because
 * libmozglue is built with -fvisibility=hidden; without it the symbols
 * wouldn't be visible to XUL/NSS/etc.
 */
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED < 1070

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>

static void
read_from_urandom(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Should never happen on a working macOS system, but if it does
         * there's nothing safe we can do. Zero the buffer rather than
         * returning uninitialized memory. */
        for (size_t i = 0; i < n; i++) ((char *)buf)[i] = 0;
        return;
    }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
}

extern "C" __attribute__((visibility("default")))
unsigned int
arc4random(void)
{
    unsigned int v;
    read_from_urandom(&v, sizeof(v));
    return v;
}

extern "C" __attribute__((visibility("default")))
void
arc4random_buf(void *buf, size_t n)
{
    read_from_urandom(buf, n);
}

#endif /* __APPLE__ && MIN_REQUIRED < 1070 */
