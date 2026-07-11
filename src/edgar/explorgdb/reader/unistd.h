#ifndef EXPLORGDB_WINDOWS_UNISTD_SHIM_H
#define EXPLORGDB_WINDOWS_UNISTD_SHIM_H

#ifdef _WIN32
#include "windows_posix_compat.h"
#else
#include_next <unistd.h>
#endif

#endif
