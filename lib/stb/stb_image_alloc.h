/* First-party allocation admission for the pinned PNG decoder, not upstream
 * stb code. Positive requests retain the system allocator's behavior. */
#ifndef MDKR_STB_IMAGE_ALLOC_H
#define MDKR_STB_IMAGE_ALLOC_H

#include <stdlib.h>

static inline void *mdkr_stbi_malloc(size_t size) {
    return size != 0u ? malloc(size) : NULL;
}

static inline void *mdkr_stbi_realloc(void *pointer, size_t size) {
    /* A refused resize must retain the original allocation for the decoder's
     * normal failure cleanup. Do not delegate realloc(pointer, 0), whose
     * ownership behavior varies between C runtimes. */
    return size != 0u ? realloc(pointer, size) : NULL;
}

#endif
