// PCI host tests reach the real x86 assembly through transitive imports.
// LLVM can encode it; these guards satisfy entries that a host test must
// never execute, while the verified scheduler object supplies its real model.
test {
    _ = @import("scheduler_test_trap_guards.zig");
    _ = @import("pci.zig");
}
