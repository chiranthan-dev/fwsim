/*
 * request.h - one host I/O as the simulator tracks it.
 */
#ifndef DISKSIM_REQUEST_H
#define DISKSIM_REQUEST_H

#include <stdint.h>
#include "disksim.h"

typedef struct {
    uint64_t  id;           /* arrival order, 0-based                     */
    double    arrival_ms;   /* when the host issued it                    */
    uint64_t  lba;
    uint32_t  count;        /* sectors                                    */
    disk_op_t op;

    /* filled in by the simulator */
    double        start_ms;   /* when the drive began working on it        */
    double        finish_ms;  /* completion                                */
    disk_timing_t timing;     /* seek / rotate / transfer breakdown        */
} io_request_t;

#endif
