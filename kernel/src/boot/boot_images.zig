pub const DiskFile = struct {
    uefi_path: [*:0]const u16,
    log_path: []const u8,
};

pub const boot_log_console = DiskFile{
    .uefi_path = &[_:0]u16{
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'S', 'H', 'E', 'L', 'L', '.', 'E', 'L', 'F',
    },
    .log_path = "\\EFI\\BOOT\\SHELL.ELF",
};

pub const init_app = DiskFile{
    .uefi_path = &[_:0]u16{
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'I', 'N', 'I', 'T', 'A', 'P', 'P', '.', 'E', 'L', 'F',
    },
    .log_path = "\\EFI\\BOOT\\INITAPP.ELF",
};

pub const bootfs_image = DiskFile{
    .uefi_path = &[_:0]u16{
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'B', 'O', 'O', 'T', 'F', 'S', '.', 'I', 'M', 'G',
    },
    .log_path = "\\EFI\\BOOT\\BOOTFS.IMG",
};
