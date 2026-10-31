#pragma once

#if defined(__linux__)
    #include <features.h>
    #if defined(__GLIBC__)
        #include <malloc.h>
        #define RAPIDGZIP_HAS_MALLOC_TRIM
    #endif
#elif defined(_WIN32)
    #include <malloc.h>
    #define RAPIDGZIP_HAS_HEAPMIN
#elif defined(__APPLE__)
    #include <malloc/malloc.h>
    #define RAPIDGZIP_HAS_MALLOC_ZONE_PRESSURE_RELIEF
#endif

#ifdef LIBRAPIDARCHIVE_WITH_RPMALLOC
    #include <rpmalloc.h>
#endif


namespace rapidgzip
{
/**
 * Returns free heap pages to the OS. Allocators keep freed pages for reuse, which makes long-running processes
 * appear to retain the peak memory usage of a large decompression even after all readers have been closed.
 * This only releases pages that are not in use anymore and is therefore safe to call with other readers alive.
 */
inline void
releaseUnusedMemory()
{
#ifdef LIBRAPIDARCHIVE_WITH_RPMALLOC
    rpmalloc_thread_collect();
#endif
#if defined(RAPIDGZIP_HAS_MALLOC_TRIM)
    malloc_trim( 0 );
#elif defined(RAPIDGZIP_HAS_HEAPMIN)
    _heapmin();
#elif defined(RAPIDGZIP_HAS_MALLOC_ZONE_PRESSURE_RELIEF)
    malloc_zone_pressure_relief( nullptr, 0 );
#endif
}
}  // namespace rapidgzip
