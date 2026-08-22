#ifndef CONN_ID_HPP
#define CONN_ID_HPP

#include <cstdint>

// Generation-guarded fd handle; stale handles fail validation after close/reuse.
struct ConnId {
    int fd = -1;
    uint64_t gen = 0;
};

#endif
