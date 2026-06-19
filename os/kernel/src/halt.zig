const kernel_log = @import("kernel_log.zig");

const serialWrite = kernel_log.write;

pub fn haltLoop() noreturn {
    while (true) asm volatile ("hlt");
}

pub fn haltWithMessage(message: []const u8) noreturn {
    serialWrite(message);
    serialWrite("\n");
    haltLoop();
}

pub fn haltWithError(prefix: []const u8, err: anyerror) noreturn {
    serialWrite(prefix);
    serialWrite(@errorName(err));
    serialWrite("\n");
    haltLoop();
}

pub fn haltWithLabelMessage(label: []const u8, suffix: []const u8) noreturn {
    serialWrite(label);
    serialWrite(suffix);
    serialWrite("\n");
    haltLoop();
}

pub fn haltWithStepError(scope: []const u8, label: []const u8, step: []const u8, err: anyerror) noreturn {
    serialWrite(scope);
    serialWrite(label);
    serialWrite(" ");
    serialWrite(step);
    serialWrite(" failed: ");
    serialWrite(@errorName(err));
    serialWrite("\n");
    haltLoop();
}

pub fn haltWithRolePageError(role_label: []const u8, page_label: []const u8, action: []const u8, err: anyerror) noreturn {
    serialWrite(role_label);
    serialWrite(" ");
    serialWrite(page_label);
    serialWrite(" ");
    serialWrite(action);
    serialWrite(" failed: ");
    serialWrite(@errorName(err));
    serialWrite("\n");
    haltLoop();
}

pub fn haltWithRolePageMessage(role_label: []const u8, page_label: []const u8, suffix: []const u8) noreturn {
    serialWrite(role_label);
    serialWrite(" ");
    serialWrite(page_label);
    serialWrite(" ");
    serialWrite(suffix);
    serialWrite("\n");
    haltLoop();
}
