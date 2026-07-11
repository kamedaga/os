# Userland Service ABI

## Policy

All producers and consumers of a service boundary are built together. Protocol
headers and identifiers therefore have no generation suffix and no compatibility
aliases. Operation numbers are local to one boundary, contiguous, and start at
zero.

## Common envelope

`userland/libipc/include/pacha/service_abi.h` is the single definition of the
64-byte page envelope: `pacha_service_envelope_t`. Request and reply views share
the prefix and use a union for the direction-specific tail, preserving the
existing wire offsets.

| Offset | Size | Request | Reply |
|---:|---:|---|---|
| 0 | 8 | `magic = PACHA_SERVICE_REQUEST_MAGIC` | `magic = PACHA_SERVICE_REPLY_MAGIC` |
| 8 | 4 | `abi_version` | `abi_version` |
| 12 | 4 | `service_id` | `service_id` |
| 16 | 4 | `op` | `op` |
| 20 | 4 | `flags` | `flags` |
| 24 | 8 | `request_id` (sequence/correlation id) | echoed `request_id` |
| 32 | 8 | `trace_id` | echoed `trace_id` |
| 40 | 8 | `payload_size`, `fd_count` | `status` |
| 48 | 8 | `reserved0` | `error_domain`, `reply_payload_size` |
| 56 | 8 | `reserved1` | `result` |

`status` is zero or a negative Linux errno. `payload_size` and
`reply_payload_size` exclude the 64-byte envelope. The envelope size is fixed by
`PACHA_SERVICE_HEADER_BYTES`; the normal page size is
`PACHA_SERVICE_PAGE_BYTES`.

Compact `pacha_ipc_msg` boundaries use the same request/reply magic constants.
Their first four words retain the layout `magic`, `op` or `status`, payload or
result, and sequence. This keeps their transport layout unchanged while removing
per-service magic/version definitions.

## Protocol headers

- `filed/ipc_protocol.h` and `filed/payload.h`: filed operations and payloads.
- `koboxd/control_protocol.h`: kobox control and block endpoint operations.
- `koboxd/storage_protocol.h`: storage backend object operations.
- `netd/ipc_protocol.h`: socket operations.
- `termd/ipc_protocol.h`: TTY operations.
- `lpr_supervisor/ipc_protocol.h`: Linux process supervision operations.
- `personality/coordinator_protocol.h`: coordinator request/response layout.
- `personality/lpr_client_abi.h`: the aggregate ABI visible to the LPR runtime.

## Operation numbers

### filed

| Op | Number | Op | Number |
|---|---:|---|---:|
| `FILED_OP_HELLO` | 0 | `FILED_OP_SESSION_OPEN` | 1 |
| `FILED_OP_SESSION_CLOSE` | 2 | `FILED_OP_SESSION_DOORBELL` | 3 |
| `FILED_OP_VFS_ROOT_STAT` | 4 | `FILED_OP_VFS_ROOT_GETDENTS` | 5 |
| `FILED_OP_VFS_OPENAT` | 6 | `FILED_OP_VFS_VALIDATE_OPEN_CACHE` | 7 |
| `FILED_OP_VFS_STAT` | 8 | `FILED_OP_VFS_READ` | 9 |
| `FILED_OP_VFS_PREAD` | 10 | `FILED_OP_VFS_WRITE` | 11 |
| `FILED_OP_VFS_PWRITE` | 12 | `FILED_OP_VFS_WRITE_BATCH` | 13 |
| `FILED_OP_VFS_PWRITE_BATCH` | 14 | `FILED_OP_VFS_GETDENTS` | 15 |
| `FILED_OP_VFS_SEEK` | 16 | `FILED_OP_VFS_CLOSE` | 17 |
| `FILED_OP_VFS_DUP` | 18 | `FILED_OP_VFS_GET_FLAGS` | 19 |
| `FILED_OP_VFS_SET_FLAGS` | 20 | `FILED_OP_VFS_FSYNC` | 21 |
| `FILED_OP_VFS_TRUNCATE` | 22 | `FILED_OP_VFS_UNLINK` | 23 |
| `FILED_OP_VFS_RENAME` | 24 | `FILED_OP_VFS_MKDIR` | 25 |
| `FILED_OP_VFS_RMDIR` | 26 | `FILED_OP_VFS_SYMLINK` | 27 |
| `FILED_OP_VFS_READLINK` | 28 | `FILED_OP_VFS_LINK` | 29 |
| `FILED_OP_VFS_FILE_VMO` | 30 | `FILED_OP_VFS_PREAD_TO_VMO` | 31 |
| `FILED_OP_VFS_SYNC_ALL` | 32 | `FILED_OP_VFS_UTIMENS` | 33 |
| `FILED_OP_VFS_CHMOD` | 34 | `FILED_OP_VFS_SHARED_FILE_VMO` | 35 |
| `FILED_OP_VFS_MEMFD_CREATE` | 36 | `FILED_OP_EXEC_PATH` | 37 |
| `FILED_OP_EXEC_SELF` | 38 | `FILED_OP_SERVICE_SET_NETD_SOCKET` | 39 |
| `FILED_OP_SERVICE_SET_TERMD_TTY` | 40 | `FILED_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR` | 41 |
| `FILED_OP_DIAG_PING` | 42 | `FILED_OP_DIAG_DUMP` | 43 |
| `FILED_OP_DIAG_ERROR_GET` | 44 | `FILED_OP_DIAG_DUMP_METRICS` | 45 |
| `FILED_OP_DIAG_SET_CACHE_SLOTS` | 46 | | |

### storage

| Op | Number | Op | Number |
|---|---:|---|---:|
| `STORAGE_OP_HELLO` | 0 | `STORAGE_OP_MOUNT_ROOT` | 1 |
| `STORAGE_OP_LOOKUP` | 2 | `STORAGE_OP_STATX` | 3 |
| `STORAGE_OP_GETDENTS` | 4 | `STORAGE_OP_PREAD` | 5 |
| `STORAGE_OP_PWRITE` | 6 | `STORAGE_OP_FSYNC` | 7 |
| `STORAGE_OP_CREATE` | 8 | `STORAGE_OP_TRUNCATE` | 9 |
| `STORAGE_OP_UTIMENS` | 10 | `STORAGE_OP_CHMOD` | 11 |
| `STORAGE_OP_UNLINK` | 12 | `STORAGE_OP_RENAME` | 13 |
| `STORAGE_OP_MKDIR` | 14 | `STORAGE_OP_RMDIR` | 15 |
| `STORAGE_OP_RELEASE_OBJECT` | 16 | `STORAGE_OP_SYNC_ALL` | 17 |
| `STORAGE_OP_DIAG_DUMP` | 18 | | |

### termd

| Op | Number | Op | Number |
|---|---:|---|---:|
| `TERMD_OP_HELLO` | 0 | `TERMD_OP_OPEN_PTMX` | 1 |
| `TERMD_OP_OPEN_PTS` | 2 | `TERMD_OP_OPEN_CTTY` | 3 |
| `TERMD_OP_OPEN_HVC` | 4 | `TERMD_OP_HANDLE_CLOSE` | 5 |
| `TERMD_OP_HANDLE_DUP` | 6 | `TERMD_OP_HANDLE_READ` | 7 |
| `TERMD_OP_HANDLE_WRITE` | 8 | `TERMD_OP_HANDLE_IOCTL` | 9 |
| `TERMD_OP_HANDLE_POLL` | 10 | `TERMD_OP_SIGNAL_TAKE` | 11 |
| `TERMD_OP_SIGNAL_REGISTER_SUPERVISOR` | 12 | `TERMD_OP_DIAG_DUMP` | 13 |
| `TERMD_OP_DIAG_ERROR_GET` | 14 | | |

### lpr-supervisor

| Op | Number | Op | Number |
|---|---:|---|---:|
| `LPRS_OP_HELLO` | 0 | `LPRS_OP_PROCESS_REGISTER_EXEC` | 1 |
| `LPRS_OP_PROCESS_REGISTER_FD` | 2 | `LPRS_OP_PROCESS_GET_STATE` | 3 |
| `LPRS_OP_PROCESS_FORK_BEGIN` | 4 | `LPRS_OP_PROCESS_FORK_PARENT_REGISTER` | 5 |
| `LPRS_OP_PROCESS_FORK_CHILD_READY` | 6 | `LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN` | 7 |
| `LPRS_OP_PROCESS_EXEC_COMMIT_DONE` | 8 | `LPRS_OP_PROCESS_WAIT4` | 9 |
| `LPRS_OP_PROCESS_SETPGID` | 10 | `LPRS_OP_PROCESS_SETSID` | 11 |
| `LPRS_OP_PROCESS_GETPGID` | 12 | `LPRS_OP_PROCESS_GETSID` | 13 |
| `LPRS_OP_SIGNAL_KILL` | 14 | `LPRS_OP_SIGNAL_DELIVER_TTY` | 15 |
| `LPRS_OP_CWD_GET` | 16 | `LPRS_OP_CWD_SET` | 17 |
| `LPRS_OP_DIAG_DUMP` | 18 | `LPRS_OP_DIAG_ERROR_GET` | 19 |

### netd

| Op | Number |
|---|---:|
| `NETD_OP_HELLO` | 0 |
| `NETD_OP_SOCKET` | 1 |
| `NETD_OP_CONNECT` | 2 |
| `NETD_OP_CLOSE` | 3 |
| `NETD_OP_SEND` | 4 |
| `NETD_OP_RECV` | 5 |
| `NETD_OP_POLL` | 6 |

### kobox endpoints

The control and block endpoints are separate operation-number boundaries, so
each begins at zero. The filesystem backend uses the storage operation table.

| Boundary | Op | Number |
|---|---|---:|
| control | `KOBOXD_CONTROL_GET_ENDPOINT` | 0 |
| block | `KOBOXD_BLOCK_IDENTIFY` | 0 |

### personality coordinator

| Op | Number |
|---|---:|
| `LPR_COORD_OP_REGISTER_PROCESS` | 0 |
| `LPR_COORD_OP_UNREGISTER_PROCESS` | 1 |
| `LPR_COORD_OP_ALLOC_TID` | 2 |
| `LPR_COORD_OP_EXIT` | 3 |
| `LPR_COORD_OP_WAIT` | 4 |
| `LPR_COORD_OP_SIGNAL` | 5 |
| `LPR_COORD_OP_FORK_FD_TABLE` | 6 |
| `LPR_COORD_OP_SHARE_FD_TABLE` | 7 |

### kobox extended endpoints

`koboxd/src/ipc_service.h` also defines an extended inline-payload transport.
Its request and reply layouts remain 96 and 80 bytes respectively, and use the
common service request/reply magic and ABI version. Each endpoint has its own
zero-based operation space.

| Boundary | Operations in numeric order |
|---|---|
| control (0–5) | `HELLO`, `GET_ENDPOINT`, `SETUP_PKEY_DATA_PLANE`, `CANCEL`, `GET_METRICS`, `DEBUG_DUMP` |
| block (0–3) | `IDENTIFY`, `READ`, `WRITE`, `FLUSH` |
| filesystem (0–10) | `MOUNT_ROOT`, `LOOKUP`, `STATX`, `GETDENTS`, `PREAD`, `PWRITE`, `FSYNC`, `CREATE`, `TRUNCATE`, `UNLINK`, `RENAME` |
| event (0–2) | `SUBSCRIBE`, `UNSUBSCRIBE`, `NEXT` |
