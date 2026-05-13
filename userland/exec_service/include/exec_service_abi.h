#ifndef CAPABILITYOS_EXEC_SERVICE_ABI_H
#define CAPABILITYOS_EXEC_SERVICE_ABI_H

#define EXEC_SERVICE_ABI_MAGIC 0x4558454353564331ULL
#define EXEC_SERVICE_ABI_VERSION 1
#define EXEC_SERVICE_ENDPOINT_ID 0x92ULL
#define EXEC_SERVICE_MAX_PATH_BYTES 128
#define EXEC_SERVICE_MAX_ARGV 8
#define EXEC_SERVICE_MAX_ENVP 16
#define EXEC_SERVICE_MAX_ARG_DATA_BYTES 2048

enum exec_service_opcode {
    EXEC_SERVICE_OP_SPAWN_LINUX = 1,
};

enum exec_service_status {
    EXEC_SERVICE_STATUS_OK = 0,
    EXEC_SERVICE_STATUS_INVALID = 1,
    EXEC_SERVICE_STATUS_NOT_FOUND = 2,
    EXEC_SERVICE_STATUS_IO = 3,
    EXEC_SERVICE_STATUS_SPAWN_FAILED = 4,
};

struct exec_service_request {
    unsigned long long magic;
    unsigned short version;
    unsigned short op;
    unsigned short path_bytes;
    unsigned short argv_count;
    unsigned short envp_count;
    unsigned short arg_data_bytes;
    unsigned long long response_token;
    unsigned long long client_process_slot;
    unsigned short argv_offsets[EXEC_SERVICE_MAX_ARGV];
    unsigned short argv_bytes[EXEC_SERVICE_MAX_ARGV];
    unsigned short envp_offsets[EXEC_SERVICE_MAX_ENVP];
    unsigned short envp_bytes[EXEC_SERVICE_MAX_ENVP];
    unsigned char arg_data[EXEC_SERVICE_MAX_ARG_DATA_BYTES];
};

struct exec_service_response {
    unsigned long long magic;
    unsigned short version;
    unsigned short op;
    unsigned int status;
    unsigned long long linux_abi_process_slot;
    unsigned long long exec_process_slot;
};

#endif
