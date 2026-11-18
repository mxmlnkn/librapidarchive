/* Compile the vendored libsais with the prefix header included before its declarations. The wrapper
 * keeps the submodule unmodified while the definitions, declarations, and blocksort.c callers use
 * the same prefixed names. */

#include "libsais_prefix.h"
#include "libsais/src/libsais.c"
