/* SPDX-License-Identifier: MIT */
/*
 * UEFI entry point used only by PachaOS installation media.
 *
 * A removable device is discovered through EFI/BOOT/BOOTX64.EFI, while a
 * fixed disk is expected to have a Boot#### load option.  PachaOS cannot set
 * that variable after ExitBootServices, so the installer leaves a marker on
 * the target ESP.  On the next firmware boot this small application consumes
 * the marker, installs the fixed-disk load option and starts that loader.  In
 * every other case it starts Limine from the installation medium unchanged.
 */
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;

#define EFI_SUCCESS UINT64_C(0)
#define EFI_ERROR_BIT (UINT64_C(1) << 63)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_OUT_OF_RESOURCES (EFI_ERROR_BIT | 9)
#define EFI_LOAD_ERROR (EFI_ERROR_BIT | 1)
#define EFI_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)
#define EFI_OPEN_READ UINT64_C(1)
#define EFI_OPEN_WRITE UINT64_C(2)
#define EFI_VARIABLE_NON_VOLATILE UINT32_C(1)
#define EFI_VARIABLE_BOOTSERVICE_ACCESS UINT32_C(2)
#define EFI_VARIABLE_RUNTIME_ACCESS UINT32_C(4)
#define LOAD_OPTION_ACTIVE UINT32_C(1)
#define BY_PROTOCOL 2
#define LOADER_DATA 4

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint8_t Type;
    uint8_t SubType;
    uint8_t Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **,
        CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *);
    void *Read;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    void *GetInfo;
    void *SetInfo;
    void *Flush;
};

typedef struct {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
        CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute, *ClearScreen;
    void *SetCursorPosition, *EnableCursor;
    void *Mode;
};

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL, *AllocatePages, *FreePages, *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(uint32_t, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent;
    void *CheckEvent, *InstallProtocolInterface, *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath;
    void *InstallConfigurationTable;
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE,
        EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN, EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    void *Exit, *UnloadImage, *ExitBootServices, *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN);
    void *SetWatchdogTimer;
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE,
        EFI_HANDLE *, EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN);
    void *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(uint32_t, EFI_GUID *, void *,
        UINTN *, EFI_HANDLE **);
    void *LocateProtocol, *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces, *CalculateCrc32;
    void *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *, EFI_GUID *, uint32_t *,
        UINTN *, void *);
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *, EFI_GUID *, uint32_t,
        UINTN, void *);
    void *GetNextHighMonotonicCount, *ResetSystem, *UpdateCapsule;
    void *QueryCapsuleCapabilities, *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

static const EFI_GUID loaded_image_guid = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID simple_fs_guid = {
    0x964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID device_path_guid = {
    0x09576e91, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID block_io_guid = {
    0x964e5b21, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID global_variable_guid = {
    0x8be4df61, 0x93ca, 0x11d2,
    {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};

static CHAR16 limine_path[] = {
    '\\','E','F','I','\\','P','a','c','h','a','O','S','\\',
    'L','I','M','I','N','E','.','E','F','I',0};
static CHAR16 pending_path[] = {
    '\\','E','F','I','\\','P','a','c','h','a','O','S','\\',
    'I','N','S','T','A','L','L','.','P','E','N','D','I','N','G',0};
static CHAR16 boot_order_name[] = {'B','o','o','t','O','r','d','e','r',0};
static CHAR16 description[] = {'P','a','c','h','a','O','S',0};

static void bytes_copy(void *destination, const void *source, UINTN bytes) {
    uint8_t *to = destination;
    const uint8_t *from = source;
    while (bytes--) *to++ = *from++;
}

static void bytes_zero(void *destination, UINTN bytes) {
    uint8_t *to = destination;
    while (bytes--) *to++ = 0;
}

static int bytes_equal(const void *a, const void *b, UINTN bytes) {
    const uint8_t *left = a, *right = b;
    while (bytes--) if (*left++ != *right++) return 0;
    return 1;
}

static UINTN string_bytes(const CHAR16 *text) {
    UINTN count = 0;
    while (text[count]) ++count;
    return (count + 1) * sizeof(CHAR16);
}

static void console_ascii(EFI_SYSTEM_TABLE *st, const char *text) {
    CHAR16 buffer[192];
    if (!st || !st->ConOut || !st->ConOut->OutputString || !text) return;
    while (*text) {
        UINTN used = 0;
        while (*text && used + 1 < sizeof(buffer) / sizeof(buffer[0])) {
            if (*text == '\n' && used + 2 < sizeof(buffer) / sizeof(buffer[0]))
                buffer[used++] = '\r';
            buffer[used++] = (uint8_t)*text++;
        }
        buffer[used] = 0;
        (void)st->ConOut->OutputString(st->ConOut, buffer);
    }
}

static void console_hex(EFI_SYSTEM_TABLE *st, uint64_t value,
    unsigned digits) {
    static const char hex[] = "0123456789ABCDEF";
    char buffer[19];
    if (digits > 16) digits = 16;
    buffer[0] = '0'; buffer[1] = 'x';
    for (unsigned i = 0; i < digits; ++i)
        buffer[2 + i] = hex[(value >> (4 * (digits - i - 1))) & 15];
    buffer[2 + digits] = 0;
    console_ascii(st, buffer);
}

static void boot_name(CHAR16 name[9], uint16_t number) {
    static const CHAR16 hex[] = {
        '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
    name[0] = 'B'; name[1] = 'o'; name[2] = 'o'; name[3] = 't';
    name[4] = hex[number >> 12]; name[5] = hex[(number >> 8) & 15];
    name[6] = hex[(number >> 4) & 15]; name[7] = hex[number & 15];
    name[8] = 0;
}

static unsigned report_device_path(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle) {
    EFI_DEVICE_PATH_PROTOCOL *path = NULL;
    EFI_STATUS status = st->BootServices->HandleProtocol(handle,
        (EFI_GUID *)&device_path_guid, (void **)&path);
    if (EFI_ERROR(status)) {
        console_ascii(st, " path-error="); console_hex(st, status, 16);
        return 0;
    }
    unsigned nvme = 0;
    UINTN offset = 0;
    for (unsigned i = 0; i < 128; ++i) {
        const uint8_t *node = (const uint8_t *)path + offset;
        UINTN length = node[2] | (UINTN)node[3] << 8;
        if (length < 4 || offset + length > 4096) break;
        if (node[0] == 0x7f) break;
        console_ascii(st, " /");
        console_hex(st, node[0], 2); console_ascii(st, ":");
        console_hex(st, node[1], 2);
        if (node[0] == 3 && node[1] == 0x17) {
            nvme = 1;
            console_ascii(st, "(NVMe)");
        }
        if (node[0] == 3 && node[1] == 5)
            console_ascii(st, "(USB)");
        if (node[0] == 4 && node[1] == 1 && length >= 42) {
            uint32_t number = 0;
            bytes_copy(&number, node + 4, sizeof(number));
            console_ascii(st, "(partition="); console_hex(st, number, 4);
            uint32_t signature_prefix = 0;
            bytes_copy(&signature_prefix, node + 24, sizeof(signature_prefix));
            console_ascii(st, " id-prefix=");
            console_hex(st, signature_prefix, 8);
            console_ascii(st, ")");
        }
        offset += length;
    }
    return nvme;
}

static unsigned report_block_handles(EFI_SYSTEM_TABLE *st) {
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(BY_PROTOCOL,
        (EFI_GUID *)&block_io_guid, NULL, &count, &handles);
    console_ascii(st, "\nBlock devices: count="); console_hex(st, count, 4);
    console_ascii(st, " status="); console_hex(st, status, 16);
    console_ascii(st, "\n");
    if (EFI_ERROR(status)) return 0;
    unsigned nvme = 0;
    for (UINTN i = 0; i < count; ++i) {
        void *fs = NULL;
        status = st->BootServices->HandleProtocol(handles[i],
            (EFI_GUID *)&simple_fs_guid, &fs);
        console_ascii(st, "block "); console_hex(st, i, 4);
        nvme += report_device_path(st, handles[i]);
        console_ascii(st, "\n  filesystem-status="); console_hex(st, status, 16);
        console_ascii(st, "\n");
    }
    st->BootServices->FreePool(handles);
    return nvme;
}

static EFI_STATUS open_root(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle,
    EFI_FILE_PROTOCOL **root) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_STATUS status = bs->HandleProtocol(handle, (EFI_GUID *)&simple_fs_guid,
        (void **)&fs);
    if (EFI_ERROR(status)) return status;
    return fs->OpenVolume(fs, root);
}

static EFI_STATUS file_exists(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = root->Open(root, &file, path, EFI_OPEN_READ, 0);
    if (!EFI_ERROR(status)) status = file->Close(file);
    return status;
}

static EFI_STATUS make_file_device_path(EFI_BOOT_SERVICES *bs,
    EFI_HANDLE device, CHAR16 *path, EFI_DEVICE_PATH_PROTOCOL **result,
    UINTN *result_bytes) {
    EFI_DEVICE_PATH_PROTOCOL *base = NULL;
    EFI_STATUS status = bs->HandleProtocol(device,
        (EFI_GUID *)&device_path_guid, (void **)&base);
    if (EFI_ERROR(status)) return status;
    UINTN base_bytes = 0;
    int found_end = 0;
    for (unsigned nodes = 0; nodes < 128; ++nodes) {
        EFI_DEVICE_PATH_PROTOCOL *node =
            (EFI_DEVICE_PATH_PROTOCOL *)((uint8_t *)base + base_bytes);
        const UINTN length = node->Length[0] | (UINTN)node->Length[1] << 8;
        if (length < 4 || base_bytes + length > 4096) return EFI_LOAD_ERROR;
        if (node->Type == 0x7f && node->SubType == 0xff) {
            found_end = 1;
            break;
        }
        base_bytes += length;
    }
    if (!found_end) return EFI_LOAD_ERROR;
    const UINTN path_bytes = string_bytes(path);
    const UINTN file_node_bytes = 4 + path_bytes;
    const UINTN total = base_bytes + file_node_bytes + 4;
    uint8_t *joined = NULL;
    status = bs->AllocatePool(LOADER_DATA, total, (void **)&joined);
    if (EFI_ERROR(status)) return status;
    bytes_copy(joined, base, base_bytes);
    joined[base_bytes] = 4;
    joined[base_bytes + 1] = 4;
    joined[base_bytes + 2] = (uint8_t)file_node_bytes;
    joined[base_bytes + 3] = (uint8_t)(file_node_bytes >> 8);
    bytes_copy(joined + base_bytes + 4, path, path_bytes);
    joined[base_bytes + file_node_bytes] = 0x7f;
    joined[base_bytes + file_node_bytes + 1] = 0xff;
    joined[base_bytes + file_node_bytes + 2] = 4;
    joined[base_bytes + file_node_bytes + 3] = 0;
    *result = (EFI_DEVICE_PATH_PROTOCOL *)joined;
    *result_bytes = total;
    return EFI_SUCCESS;
}

static EFI_STATUS get_variable(EFI_SYSTEM_TABLE *st, CHAR16 *name,
    void **data, UINTN *bytes) {
    uint32_t attributes = 0;
    UINTN needed = 0;
    EFI_STATUS status = st->RuntimeServices->GetVariable(name,
        (EFI_GUID *)&global_variable_guid, &attributes, &needed, NULL);
    if (status == EFI_NOT_FOUND) { *data = NULL; *bytes = 0; return status; }
    if (status != EFI_BUFFER_TOO_SMALL || !needed) return status;
    status = st->BootServices->AllocatePool(LOADER_DATA, needed, data);
    if (EFI_ERROR(status)) return status;
    *bytes = needed;
    status = st->RuntimeServices->GetVariable(name,
        (EFI_GUID *)&global_variable_guid, &attributes, bytes, *data);
    if (EFI_ERROR(status)) {
        st->BootServices->FreePool(*data); *data = NULL; *bytes = 0;
    }
    return status;
}

static int option_is_pacha(const uint8_t *data, UINTN bytes) {
    if (bytes < 6 + string_bytes(description)) return 0;
    return bytes_equal(data + 6, description, string_bytes(description));
}

static EFI_STATUS register_boot_option(EFI_SYSTEM_TABLE *st,
    EFI_DEVICE_PATH_PROTOCOL *path, UINTN path_bytes, uint16_t *number_out,
    unsigned *stage) {
    if (stage) *stage = 1;
    void *order_data = NULL;
    UINTN order_bytes = 0;
    EFI_STATUS status = get_variable(st, boot_order_name,
        &order_data, &order_bytes);
    if (status == EFI_NOT_FOUND) { order_data = NULL; order_bytes = 0; }
    else if (EFI_ERROR(status) || order_bytes % 2) return status;

    if (stage) *stage = 2;
    uint16_t number = UINT16_MAX;
    uint16_t *order = order_data;
    for (UINTN i = 0; i < order_bytes / 2; ++i) {
        CHAR16 name[9]; void *value = NULL; UINTN value_bytes = 0;
        boot_name(name, order[i]);
        EFI_STATUS got = get_variable(st, name, &value, &value_bytes);
        if (!EFI_ERROR(got) && option_is_pacha(value, value_bytes))
            number = order[i];
        if (value) st->BootServices->FreePool(value);
        if (number != UINT16_MAX) break;
    }
    if (number == UINT16_MAX) {
        for (uint32_t candidate = 0; candidate <= UINT16_MAX; ++candidate) {
            CHAR16 name[9]; UINTN size = 0; uint32_t attributes = 0;
            boot_name(name, (uint16_t)candidate);
            status = st->RuntimeServices->GetVariable(name,
                (EFI_GUID *)&global_variable_guid, &attributes, &size, NULL);
            if (status == EFI_NOT_FOUND) { number = (uint16_t)candidate; break; }
            if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status)) {
                if (order_data) st->BootServices->FreePool(order_data);
                return status;
            }
        }
    }
    if (number == UINT16_MAX) {
        if (order_data) st->BootServices->FreePool(order_data);
        return EFI_OUT_OF_RESOURCES;
    }

    if (stage) *stage = 3;
    const UINTN desc_bytes = string_bytes(description);
    const UINTN option_bytes = 6 + desc_bytes + path_bytes;
    uint8_t *option = NULL;
    status = st->BootServices->AllocatePool(LOADER_DATA, option_bytes,
        (void **)&option);
    if (EFI_ERROR(status)) goto out_order;
    bytes_zero(option, option_bytes);
    option[0] = LOAD_OPTION_ACTIVE;
    option[4] = (uint8_t)path_bytes;
    option[5] = (uint8_t)(path_bytes >> 8);
    bytes_copy(option + 6, description, desc_bytes);
    bytes_copy(option + 6 + desc_bytes, path, path_bytes);
    CHAR16 option_name[9];
    boot_name(option_name, number);
    const uint32_t attributes = EFI_VARIABLE_NON_VOLATILE |
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    if (stage) *stage = 4;
    status = st->RuntimeServices->SetVariable(option_name,
        (EFI_GUID *)&global_variable_guid, attributes, option_bytes, option);
    if (EFI_ERROR(status)) goto out_order;

    if (stage) *stage = 5;
    void *option_readback = NULL;
    UINTN option_readback_bytes = 0;
    status = get_variable(st, option_name, &option_readback,
        &option_readback_bytes);
    if (!EFI_ERROR(status) && (option_readback_bytes != option_bytes ||
        !bytes_equal(option_readback, option, option_bytes)))
        status = EFI_LOAD_ERROR;
    if (option_readback) st->BootServices->FreePool(option_readback);
    if (EFI_ERROR(status)) goto out_order;

    if (stage) *stage = 6;
    uint16_t *new_order = NULL;
    status = st->BootServices->AllocatePool(LOADER_DATA, order_bytes + 2,
        (void **)&new_order);
    if (EFI_ERROR(status)) goto out_order;
    new_order[0] = number;
    UINTN used = 1;
    for (UINTN i = 0; i < order_bytes / 2; ++i)
        if (order[i] != number) new_order[used++] = order[i];
    if (stage) *stage = 7;
    status = st->RuntimeServices->SetVariable(boot_order_name,
        (EFI_GUID *)&global_variable_guid, attributes, used * 2, new_order);
    st->BootServices->FreePool(new_order);
    if (EFI_ERROR(status)) goto out_order;

    if (stage) *stage = 8;
    void *order_readback = NULL;
    UINTN order_readback_bytes = 0;
    status = get_variable(st, boot_order_name, &order_readback,
        &order_readback_bytes);
    if (!EFI_ERROR(status) && (order_readback_bytes < 2 ||
        ((const uint16_t *)order_readback)[0] != number))
        status = EFI_LOAD_ERROR;
    if (order_readback) st->BootServices->FreePool(order_readback);
    if (!EFI_ERROR(status) && number_out) *number_out = number;
out_order:
    if (option) st->BootServices->FreePool(option);
    if (order_data) st->BootServices->FreePool(order_data);
    return status;
}

static EFI_STATUS start_loader(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
    EFI_HANDLE device, CHAR16 *loader_path) {
    EFI_DEVICE_PATH_PROTOCOL *path = NULL;
    UINTN path_bytes = 0;
    EFI_STATUS status = make_file_device_path(st->BootServices, device,
        loader_path, &path, &path_bytes);
    if (EFI_ERROR(status)) return status;
    EFI_HANDLE child = NULL;
    status = st->BootServices->LoadImage(0, image, path, NULL, 0, &child);
    st->BootServices->FreePool(path);
    if (EFI_ERROR(status)) return status;
    return st->BootServices->StartImage(child, NULL, NULL);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_STATUS status = st->BootServices->HandleProtocol(image,
        (EFI_GUID *)&loaded_image_guid, (void **)&loaded);
    if (EFI_ERROR(status)) return status;

    /* Some firmware connects only devices already named by BootOrder.  That
     * is exactly the state this application repairs, so recursively connect
     * every controller before asking for Simple File System handles. */
    UINTN previous_count = 0;
    for (unsigned pass = 0; pass < 8; ++pass) {
        UINTN controller_count = 0;
        EFI_HANDLE *controllers = NULL;
        status = st->BootServices->LocateHandleBuffer(0, NULL, NULL,
            &controller_count, &controllers);
        if (EFI_ERROR(status)) break;
        /* Binding a storage driver creates child handles. Re-enumerate so
         * partition and filesystem drivers also see those new children. */
        for (UINTN i = 0; i < controller_count; ++i)
            (void)st->BootServices->ConnectController(controllers[i],
                NULL, NULL, 1);
        st->BootServices->FreePool(controllers);
        if (controller_count == previous_count) break;
        previous_count = controller_count;
    }
    unsigned nvme_blocks = report_block_handles(st);
    unsigned nvme_filesystems = 0;

    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    status = st->BootServices->LocateHandleBuffer(BY_PROTOCOL,
        (EFI_GUID *)&simple_fs_guid, NULL, &count, &handles);
    if (!EFI_ERROR(status)) {
        for (UINTN i = 0; i < count; ++i) {
            console_ascii(st, "filesystem "); console_hex(st, i, 4);
            nvme_filesystems += report_device_path(st, handles[i]);
            if (handles[i] == loaded->DeviceHandle) {
                console_ascii(st, " (installer source)\n"); continue;
            }
            EFI_FILE_PROTOCOL *root = NULL, *pending = NULL;
            status = open_root(st->BootServices, handles[i], &root);
            console_ascii(st, "\n  open-root="); console_hex(st, status, 16);
            if (EFI_ERROR(status)) { console_ascii(st, "\n"); continue; }
            status = root->Open(root, &pending, pending_path,
                EFI_OPEN_READ | EFI_OPEN_WRITE, 0);
            EFI_STATUS loader_status = file_exists(root, limine_path);
            console_ascii(st, "\n  pending="); console_hex(st, status, 16);
            console_ascii(st, " limine="); console_hex(st, loader_status, 16);
            console_ascii(st, "\n");
            if (EFI_ERROR(status) || EFI_ERROR(loader_status)) {
                if (pending) pending->Close(pending);
                root->Close(root);
                continue;
            }
            EFI_DEVICE_PATH_PROTOCOL *path = NULL;
            UINTN path_bytes = 0;
            uint16_t boot_number = UINT16_MAX;
            unsigned registration_stage = 0;
            console_ascii(st,
                "\nPachaOS installer: found pending fixed-disk boot.\n");
            status = make_file_device_path(st->BootServices, handles[i],
                limine_path, &path, &path_bytes);
            if (!EFI_ERROR(status))
                status = register_boot_option(st, path, path_bytes,
                    &boot_number, &registration_stage);
            if (path) st->BootServices->FreePool(path);
            if (!EFI_ERROR(status)) {
                registration_stage = 9;
                status = pending->Delete(pending);
            }
            else pending->Close(pending);
            root->Close(root);
            EFI_HANDLE target = handles[i];
            st->BootServices->FreePool(handles);
            if (!EFI_ERROR(status)) {
                console_ascii(st, "Verified Boot");
                console_hex(st, boot_number, 4);
                console_ascii(st,
                    " and BootOrder; starting installed PachaOS.\n");
                (void)st->BootServices->Stall(UINT64_C(1500000));
            } else {
                console_ascii(st, "UEFI registration failed at stage ");
                console_hex(st, registration_stage, 2);
                console_ascii(st, ", status ");
                console_hex(st, status, 16);
                console_ascii(st,
                    ". Marker retained; starting installed PachaOS.\n");
                (void)st->BootServices->Stall(UINT64_C(8000000));
            }
            return start_loader(image, st, target, limine_path);
        }
        st->BootServices->FreePool(handles);
    }
    console_ascii(st, "\nPachaOS installer: no pending fixed-disk ESP was found (filesystem handles ");
    console_hex(st, count, 4);
    console_ascii(st, ").\nNVMe block handles=");
    console_hex(st, nvme_blocks, 4);
    console_ascii(st, " NVMe filesystem handles=");
    console_hex(st, nvme_filesystems, 4);
    console_ascii(st, "\nStarting the USB live medium.\n");
    (void)st->BootServices->Stall(UINT64_C(8000000));
    return start_loader(image, st, loaded->DeviceHandle, limine_path);
}
