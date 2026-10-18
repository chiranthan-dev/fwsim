/*
 * status.c - status codes -> strings and SCSI-style sense data.
 *
 * WHY sense data and not just an enum: a host driver needs to know not only
 * THAT a command failed but how to react. The sense KEY is the coarse
 * category that drives the reaction (ILLEGAL REQUEST: host bug, do not
 * retry; MEDIUM ERROR: data lost at err_lba, maybe rewrite it; ABORTED
 * COMMAND: retry is reasonable). ASC/ASCQ narrow it down for logging.
 * The values below are the real SPC/SBC codes.
 */
#include "fwsim.h"

#include "squeue.h"

#define SK_NO_SENSE        0x0
#define SK_MEDIUM_ERROR    0x3
#define SK_HARDWARE_ERROR  0x4
#define SK_ILLEGAL_REQUEST 0x5
#define SK_ABORTED_COMMAND 0xB

static const struct {
    const char *name;
    fw_sense_t  sense;
} status_table[FW_STATUS_COUNT] = {
    [FW_OK]                 = { "OK",             { SK_NO_SENSE,        0x00, 0x00 } },
    [FW_ERR_INVALID_OPCODE] = { "INVALID_OPCODE", { SK_ILLEGAL_REQUEST, 0x20, 0x00 } },
    [FW_ERR_INVALID_FIELD]  = { "INVALID_FIELD",  { SK_ILLEGAL_REQUEST, 0x24, 0x00 } },
    [FW_ERR_INVALID_LBA]    = { "INVALID_LBA",    { SK_ILLEGAL_REQUEST, 0x21, 0x00 } },
    [FW_ERR_MEDIUM]         = { "MEDIUM_ERROR",   { SK_MEDIUM_ERROR,    0x11, 0x00 } },
    [FW_ERR_WRITE_FAULT]    = { "WRITE_FAULT",    { SK_MEDIUM_ERROR,    0x0C, 0x00 } },
    [FW_ERR_TAG_IN_USE]     = { "TAG_IN_USE",     { SK_ABORTED_COMMAND, 0x4E, 0x00 } },
    [FW_ERR_ABORTED]        = { "ABORTED",        { SK_ABORTED_COMMAND, 0x00, 0x00 } },
    [FW_ERR_HARDWARE]       = { "HARDWARE_ERROR", { SK_HARDWARE_ERROR,  0x44, 0x00 } },
};

const char *fw_status_str(fw_status_t st)
{
    if ((unsigned)st >= FW_STATUS_COUNT)
        return "UNKNOWN";
    return status_table[st].name;
}

fw_sense_t fw_status_sense(fw_status_t st)
{
    if ((unsigned)st >= FW_STATUS_COUNT)
        return status_table[FW_ERR_HARDWARE].sense;
    return status_table[st].sense;
}

const char *fw_opcode_str(uint8_t op)
{
    static const char *names[FW_OP_COUNT] = {
        [FW_OP_IDENTIFY] = "IDENTIFY", [FW_OP_READ] = "READ",
        [FW_OP_WRITE] = "WRITE",       [FW_OP_FLUSH] = "FLUSH",
        [FW_OP_SET_FEATURES] = "SET_FEATURES",
    };
    return op < FW_OP_COUNT ? names[op] : "INVALID";
}

const char *fw_queue_impl(void)
{
    return SQUEUE_IMPL;
}
