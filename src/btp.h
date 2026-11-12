/* btp.h - constants of BTP/1, the ByOS sync protocol (docs/SYNC_PROTOCOL.md).
 * Shared by src/hibysync.c (device daemon) and tests/btp/btp_test.c. The numbers here are
 * normative: they must match the spec and tests/btp/vectors.json. */
#ifndef BTP_H
#define BTP_H

#define BTP_VERSION      1
#define BTP_HDR          12            /* cleartext frame header */
#define BTP_MAX_PAYLOAD  65544         /* 8-byte DATA offset + 64 KiB */
#define BTP_MAX_DATA     65536
#define BTP_TAG          16            /* Poly1305 tag */
#define BTP_REC_HDR      (4 + BTP_TAG) /* box_len + tag */
#define BTP_MAX_FRAME    (BTP_HDR + BTP_MAX_PAYLOAD)
#define BTP_MAX_RECORD   (BTP_TAG + BTP_MAX_FRAME)   /* 65572: largest box_len */
#define BTP_PORT         7340
#define BTP_ANNOUNCE_PORT 7341
#define BTP_FLAG_END     0x01

enum btp_type {
    T_HELLO = 0x01, T_ERROR = 0x02, T_OK = 0x03, T_PING = 0x04, T_PONG = 0x05, T_BYE = 0x06, T_CANCEL = 0x07,
    T_PAIR_COMMIT = 0x10, T_PAIR_DEVICE = 0x11, T_PAIR_REVEAL = 0x12, T_PAIR_CONFIRM = 0x13, T_PAIR_RESULT = 0x14,
    T_AUTH_INIT = 0x18, T_AUTH_REPLY = 0x19, T_AUTH_FINISH = 0x1A, T_AUTH_OK = 0x1B,
    T_STAT = 0x20, T_STAT_R = 0x21, T_LIST = 0x22, T_LIST_R = 0x23, T_DF = 0x24, T_DF_R = 0x25,
    T_MKDIR = 0x26, T_MOVE = 0x27, T_DELETE = 0x28,
    T_PUT_BEGIN = 0x30, T_PUT_READY = 0x31, T_DATA = 0x32, T_PUT_END = 0x33, T_PUT_DONE = 0x34,
    T_GET = 0x38, T_GET_R = 0x39, T_GET_END = 0x3A,
    T_DB_SNAPSHOT = 0x40, T_DB_STAGE = 0x41, T_DB_STATUS = 0x42, T_DB_STATUS_R = 0x43, T_DB_UNSTAGE = 0x44, T_LIB_RESCAN = 0x45,
    T_UPDATE_STAGE = 0x50, T_UPDATE_STAGE_R = 0x51, T_UPDATE_INSTALL = 0x52, T_UPDATE_STATUS = 0x53, T_UPDATE_STATUS_R = 0x54,
    T_STATUS = 0x60, T_STATUS_R = 0x61,
};

enum btp_err {
    E_PROTO = 1, E_VERSION = 2, E_UNKNOWN_TYPE = 3, E_AUTH_REQUIRED = 4, E_AUTH_FAILED = 5, E_PAIR_CLOSED = 6,
    E_PAIR_REJECTED = 7, E_BUSY = 8, E_TOO_LARGE = 9, E_RATE_LIMIT = 10, E_TIMEOUT = 11, E_CANCELLED = 12,
    E_UNSUPPORTED = 13, E_INTERNAL = 14,
    E_BAD_PATH = 32, E_NOT_FOUND = 33, E_EXISTS = 34, E_NOT_DIR = 35, E_IS_DIR = 36, E_NOT_EMPTY = 37,
    E_NO_SPACE = 38, E_IO = 39, E_SD_UNAVAILABLE = 40, E_HASH_MISMATCH = 41, E_OFFSET = 42, E_FILE_TOO_BIG = 43,
    E_NOT_ALLOWED = 44,
    E_SIGNATURE = 64, E_UPDATE_INVALID = 65, E_DOWNGRADE = 66, E_BATTERY = 67, E_NEEDS_FIRMWARE = 68, E_NOT_STAGED = 69,
    E_DB_INVALID = 80, E_DB_STALE = 81,
};

enum btp_caps {
    CAP_FILES = 1 << 0, CAP_RESUME = 1 << 1, CAP_DB_SNAPSHOT = 1 << 2, CAP_DB_STAGE = 1 << 3, CAP_LIB_RESCAN = 1 << 4,
    CAP_UPDATE = 1 << 5, CAP_PAIRING = 1 << 6, CAP_ENCRYPTION = 1 << 7, CAP_STATUS = 1 << 8,
};
#define CAP_ALL 0x1ff

enum btp_hello_flags { HF_LOCAL = 1, HF_PAIRING_OPEN = 2, HF_SD_MOUNTED = 4, HF_SHELL_ATTACHED = 8 };
enum btp_tlv { TLV_SOFTWARE = 1, TLV_NAME = 2, TLV_DEVICE_ID = 3, TLV_BYOS_VERSION = 4, TLV_FIRMWARE = 5, TLV_MODEL = 6 };
enum btp_ns { NS_SD = 0, NS_UPDATE = 1, NS_DB = 2 };
enum btp_put_mode { PM_CREATE = 0, PM_REPLACE = 1, PM_SKIP_IDENTICAL = 2 };
enum btp_kind { K_FILE = 0, K_DIR = 1, K_OTHER = 2 };

/* path rule violations (section 9.2), in check order; names as in the vectors */
enum btp_path_reason {
    PR_OK = 0, PR_EMPTY, PR_TOO_LONG, PR_BAD_UTF8, PR_ABSOLUTE, PR_TOO_DEEP, PR_EMPTY_COMPONENT, PR_DOT_COMPONENT,
    PR_HIDDEN, PR_CONTROL_CHAR, PR_FORBIDDEN_CHAR, PR_TRAILING_DOT_SPACE, PR_COMPONENT_TOO_LONG,
};
static const char *const BTP_PATH_REASON[] = {
    "ok", "empty", "too_long", "bad_utf8", "absolute", "too_deep", "empty_component", "dot_component",
    "hidden", "control_char", "forbidden_char", "trailing_dot_space", "component_too_long",
};

#define BTP_PATH_MAX       1024
#define BTP_COMP_MAX       255
#define BTP_DEPTH_MAX      32
#define BTP_NAME_MAX       64
#define BTP_RESUME_BLOCK   65536
#define BTP_NS_UPDATE_JSON_MAX (64 * 1024)
#define BTP_NS_UPDATE_TAR_MAX  (64LL * 1024 * 1024)
#define BTP_NS_DB_MAX      (64LL * 1024 * 1024)

#endif
