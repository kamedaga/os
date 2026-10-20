#!/usr/bin/env python3
"""Scoped boot-staging migration tripwires, not a Zig borrow/lifetime proof."""
from collections import Counter
from pathlib import Path
import re
from check_kernel_address_boundaries import code_only, function_body, without_tests

ROOT = Path(__file__).resolve().parents[1]


def check(files):
    errors = []
    scoped = {'kernel/src/boot/boot_scratch.zig', 'kernel/src/boot/entry.zig',
              'kernel/src/boot/elf_load.zig', 'kernel/src/pci.zig',
              'kernel/src/boot/resource_pages.zig', 'kernel/src/boot/init_setup.zig',
              'kernel/src/arch/x86_64/physical_layout.zig',
              'bootloader/limine/kernel_entry.zig'}
    sources = {path: (without_tests(code_only(text)) if path in scoped or
                     re.search(r'^\s*const\s+\w+\s*=\s*@import\("[^"\n]*boot_scratch\.zig"\)', text, re.M)
                     else code_only(text)) for path, text in files.items()}
    arena = sources['kernel/src/boot/boot_scratch.zig']
    if re.search(r'pub fn (allocate|free)\s*\(', arena):
        errors.append('scratch raw allocate/free bypass restored')
    if 'if (self.installed) return error.AlreadyInstalled;' not in function_body(arena, 'install'):
        errors.append('scratch live reset protection removed')
    seal = function_body(arena, 'seal')
    if any(token not in seal for token in ('while (cursor)', 'header.borrowed', 'header.published', 'self.sealed = true')):
        errors.append('scratch seal does not check the entire chain')
    expected = {
        'kernel/src/boot/entry.zig': Counter(reserveTemporary=2, seal=1, reclaim=1, statistics=1,
                                          kernelStaticStorageStartAddr=2, kernelStaticStorageEndAddr=2),
        'kernel/src/boot/elf_load.zig': Counter(reserveTemporary=1),
        'kernel/src/arch/x86_64/boot_direct_map.zig': Counter(reservePersistent=1, installGrowing=1),
    }
    actual = {}
    for path, source in sources.items():
        if path == 'kernel/src/boot/boot_scratch.zig':
            continue
        # Discover import aliases, rather than assuming the name boot_scratch.
        aliases = re.findall(r'^\s*const\s+(\w+)\s*=\s*@import\("[^"\n]*boot_scratch\.zig"\)', files[path], re.M)
        calls = Counter()
        for alias in aliases:
            calls.update(re.findall(r'\b'+re.escape(alias)+r'\.(\w+)\s*\(', source))
            if re.search(r'\b'+re.escape(alias)+r'\.(?:arena|Arena)\b', source):
                errors.append(path+': direct arena access outside the ownership boundary')
        if calls:
            actual[path] = calls
    if actual != expected:
        errors.append('scratch callsite census changed; audit boot/IRQ/lifetime: '+str(actual))
    # Constrain references, not just a particular import alias or direct call:
    # assigning a boot-only function to another name must trigger review too.
    wrappers = ('captureBootFunctions', 'captureBootFunctionsWith', 'loadUserElfIntoProcessPages',
                'loadUserElfIntoProcessPagesOrHalt', 'persistBootResourcesOrHalt',
                'bootWithResources')
    wrapper_expected = {
        'kernel/src/boot/entry.zig': Counter(captureBootFunctions=1,
                                          loadUserElfIntoProcessPages=1,
                                          bootWithResources=1),
        'kernel/src/boot/elf_load.zig': Counter(loadUserElfIntoProcessPages=1),
        'kernel/src/pci.zig': Counter(captureBootFunctionsWith=1),
        'bootloader/limine/kernel_entry.zig': Counter(persistBootResourcesOrHalt=1,
                                                     bootWithResources=1),
    }
    wrapper_actual = {}
    for path, source in sources.items():
        references = Counter()
        for wrapper in wrappers:
            count = len(re.findall(r'\b'+wrapper+r'\b', source))
            count -= len(re.findall(r'\bfn\s+'+wrapper+r'\b', source))
            if count:
                references[wrapper] = count
        if references:
            wrapper_actual[path] = references
    if wrapper_actual != wrapper_expected:
        errors.append('boot-only wrapper reference census changed; audit serial/pre-AP lifetime: '+str(wrapper_actual))
    entry = sources['kernel/src/boot/entry.zig']
    if not re.search(r'bootWithResources\(resources:\s*PreparedBootResources', entry):
        errors.append('boot entry accepts unprepared borrowed module slices')
    construct = function_body(entry, 'constructBootProcesses')
    required = ('bootfs_loan.end()', 'res.bootfs_staging.release()',
                'elf_load.loadUserElfIntoProcessPages(', 'init_loan.end()',
                'res.init_staging.release()', 'load_result orelse',
                'boot_scratch.seal()', 'scheduler.enableApUserDispatch()')
    if any(token not in construct for token in required) or 'loadUserElfIntoProcessPagesOrHalt' in construct:
        errors.append('boot staging close/halt/dispatch boundary bypassed')
    elif not (construct.index(required[0]) < construct.index(required[1]) < construct.index(required[2]) <
              construct.index(required[3]) < construct.index(required[4]) < construct.index(required[5]) <
              construct.index(required[6]) < construct.index(required[7])):
        errors.append('boot staging close/seal order changed')
    # Bound only this migrated staging path. These tripwires do not prove
    # general alias/borrow safety; runtime identity tests cover record copies.
    prepare = function_body(entry, 'persistBootResourcesOrHalt')
    setup = function_body(sources['kernel/src/boot/init_setup.zig'], 'mapBootFsImageIntoProcessOrHalt')
    pages = sources['kernel/src/boot/resource_pages.zig']
    if ('@ptrFromInt' in prepare or '@ptrFromInt' in setup or
            'bytesAsSlice(u64' in prepare or 'bytesAsSlice(u64' in construct or
            'appendPage(' in construct or re.search(r'pub fn allocate\b', pages)):
        errors.append('bootfs raw physical/region-zero staging bypass restored')
    if (prepare.count('for (pages) |*slot| slot.* = .{};') != 1 or
            'for (pages, 0..) |*slot, index|' not in prepare or
            'slot.acquireMapped(' not in prepare or
            'for (staged_pages) |*slot|' not in construct or 'slot.release()' not in construct):
        errors.append('bootfs one-time original-slot ownership boundary bypassed')
    elif construct.index('slot.release()') > construct.index('bootfs_loan.end()'):
        errors.append('bootfs metadata loan ended before all page returns')
    if ('source_loan.identityBytes()' not in setup or 'source_loan.end()' not in setup or
            setup.index('source_loan.end()') < setup.index('writePhysicalBytes(')):
        errors.append('bootfs source page view escaped its copy loan')
    acquire = function_body(pages, 'acquire')
    release = function_body(pages, 'release')
    if ('popContiguousOwnedInRange(1, 4096, layout.allocation_limit)' not in acquire or
            'self.bound_address != null' not in acquire or
            'self.issuer = list' not in acquire or
            'self.issuer.?.appendContiguousRange(grant.physical.toInt(), grant.pages)' not in release or
            'self.borrower != null' not in release):
        errors.append('bootfs grant/issuer/null/borrowed-return boundary bypassed')
    # Restrict pre-CR3 HHDM use even through a renamed import/function alias.
    references = {}
    for path, source in sources.items():
        counts = Counter()
        for name in ('hhdmSlice', 'hhdmBytes', 'identityBytes'):
            count = len(re.findall(r'\b'+name+r'\b', source)) - len(re.findall(r'\bfn\s+'+name+r'\b', source))
            if count:
                counts[name] = count
        if counts:
            references[path] = counts
    if references != {
            'kernel/src/boot/resource_pages.zig': Counter(hhdmSlice=1),
            'kernel/src/memory/free_page_list.zig': Counter(hhdmSlice=1),
            'kernel/src/boot/entry.zig': Counter(hhdmBytes=1),
            'kernel/src/boot/init_setup.zig': Counter(identityBytes=1)}:
        errors.append('bootfs HHDM/identity view reference census changed: '+str(references))
    load = function_body(sources['kernel/src/boot/elf_load.zig'], 'loadUserElfIntoProcessPages')
    if 'defer' not in load or 'loan.end()' not in load or 'staging.release()' not in load or load.index('loan.end()') > load.index('staging.release()'):
        errors.append('ELF window borrow/end/rollback boundary bypassed')
    pci_source = sources['kernel/src/pci.zig']
    wrapper = function_body(pci_source, 'captureBootFunctions')
    if 'return captureBootFunctionsWith(NativeBootCapture, boot_scratch);' not in wrapper:
        errors.append('PCI boot wrapper lost static hardware/scratch selection')
    if (re.search(r'pub\s+fn\s+captureBootFunctionsWith\b', pci_source) or
            not re.search(r'fn captureBootFunctionsWith\(comptime Reader: type, scratch: anytype\)', pci_source)):
        errors.append('PCI capture helper must remain private and compile-time selected')
    pci = function_body(pci_source, 'captureBootFunctionsWith')
    if (pci.count('scratch.reservePersistent(') != 1 or
            'allocation.publishPersistent()' not in pci or 'Reader.capture(draft,' not in pci or
            'allocation.release()' not in pci or re.search(r'device_apertures\s*=\s*(draft|entries)', pci)):
        errors.append('PCI catalog draft publication/rollback bypassed')
    loader = sources['bootloader/limine/kernel_entry.zig']
    if (len(re.findall(r'entry\.persistBootResourcesOrHalt\s*\(', loader)) != 1 or
            len(re.findall(r'entry\.bootWithResources\s*\(resources,', loader)) != 1):
        errors.append('Limine preparation-to-entry boundary changed')
    resources = sources['bootloader/limine/resources.zig']
    catalog = function_body(resources, 'collectMemoryStatsOrHalt')
    platform = without_tests(sources['kernel/src/arch/x86_64/platform.zig'])
    backend = sources['kernel/src/arch/x86_64/boot_direct_map.zig']
    boot_order = ('owner.installScratch(', 'owner.metadata(', 'owner.build(',
                  'free_list.installRegions(', 'pmm.prepareBootAccess(',
                  'owner.handoffScratch(', 'collector.reserveBootPages(', 'collector.addRegion(')
    if any(t not in catalog for t in boot_order) or [catalog.index(t) for t in boot_order] != sorted(catalog.index(t) for t in boot_order):
        errors.append('scratch boot reservation/publication/PMM handoff order changed')
    install = function_body(backend, 'installScratch')
    if any(t not in install for t in ('view.contains(pdp_pa, 4096)', 'for (pdp[0..first_image_slot])', 'entry != 0', 'error.OccupiedRoot')):
        errors.append('scratch HHDM proof or existing root collision check missing')
    grow = function_body(backend, 'growScratch')
    if 'const flags: u64 = (@as(u64, 1) << 63) | 3' not in grow:
        errors.append('scratch leaf requires supervisor RW/NX/WB')
    if 'x86_platform.enableNxForCurrentCpu()' not in catalog or catalog.index('x86_platform.enableNxForCurrentCpu()') > catalog.index('owner.installScratch('):
        errors.append('scratch NXE must precede NX leaf publication')
    install = function_body(backend, 'installScratch')
    parent_order = ('view.contains(pdp_pa, 4096)', 'root[511] &= ~@as(u64, 4)', 'asm volatile (', 'scratch.installGrowing(')
    if '"mov %[root], %%cr3"' not in function_body(files['kernel/src/arch/x86_64/boot_direct_map.zig'], 'installScratch') or any(token not in install for token in parent_order) or [install.index(t) for t in parent_order] != sorted(install.index(t) for t in parent_order):
        errors.append('scratch supervisor parent must precede TLB reset and child publication')
    if 'boot_direct_map.installScratchRoot(' not in function_body(platform, 'installHighKernelMapping'):
        errors.append('scratch stable VA root handoff missing')
    if 'boot_direct_map.scratchRootMatches(' not in function_body(platform, 'validateRamCacheKernelRoots'):
        errors.append('scratch root ownership validation missing')
    retire = function_body(arena, 'reclaim')
    ordered = ('self.reclaim_started = true', 'Retirer.physical(', 'Retirer.withdraw(',
               'Retirer.acknowledge(', 'Retirer.release(', 'self.reclaimed = true')
    if any(token not in retire for token in ordered) or [retire.index(t) for t in ordered] != sorted(retire.index(t) for t in ordered):
        errors.append('scratch retirement must withdraw then ACK before PMM publication')
    if 'if (!self.sealed or self.reclaim_started)' not in retire:
        errors.append('scratch repeated/unsealed retirement allowed')
    if 'self.buffer = self.buffer[0..retained]' not in seal:
        errors.append('scratch sealed arena still exposes returned pages')
    if any(t not in construct for t in ('reclaimBootScratchOrHalt();', 'boot_scratch.seal()', 'scheduler.enableApUserDispatch()')) or not (construct.index('boot_scratch.seal()') <
            construct.index('reclaimBootScratchOrHalt();') < construct.index('scheduler.enableApUserDispatch()')):
        errors.append('scratch reclaim/dispatch order bypassed')
    retire_adapter = entry[entry.index('const ScratchRetirer = struct'):entry.index('fn reclaimBootScratchOrHalt')]
    if any(token not in retire_adapter for token in ('backing.prepareScratchRetirement(',
            'backing.withdrawScratch(', 'user_copy.flushTlbForCr3Range(', 'backing.releaseScratch(')):
        errors.append('scratch scattered owner/ACK/PMM adapter bypassed')
    ack = function_body(retire_adapter, 'acknowledge')
    if ('user_copy.flushTlbForCr3Range(' not in ack or 'backing.acknowledgeScratch(' not in ack or
            ack.index('user_copy.flushTlbForCr3Range(') > ack.index('backing.acknowledgeScratch(')):
        errors.append('scratch ACK publication precedes rendezvous')
    give_back = function_body(backend, 'releaseScratch')
    publication = ('!scratch_acknowledged', 'try pmm.appendPage(pa)', 'leaf.* = 0')
    if any(t not in give_back for t in publication) or [give_back.index(t) for t in publication] != sorted(give_back.index(t) for t in publication):
        errors.append('scratch PFN ownership must survive until ACK and successful PMM return')
    # Exhaustion is terminal at the owning boot boundary; a nullable ELF
    # loader is allowed only because construct checks it before dispatch.
    copy_boot = function_body(entry, 'copyBootBytesToScratchOrHalt')
    if not re.search(r'reserveTemporary\(bytes.len\) catch \{[^}]*halt.haltWithMessage\(', copy_boot):
        errors.append('scratch copy exhaustion no longer halts boot')
    if not re.search(r'reserveTemporary\(metadata_bytes\) catch\s+halt.haltWithMessage\(', prepare):
        errors.append('scratch metadata exhaustion no longer halts boot')
    if not re.search(r'reservePersistent\(bytes\) catch\s+halt.haltWithMessage\(', catalog):
        errors.append('scratch region table exhaustion no longer halts boot')
    if 'halt.haltLoop()' not in construct or 'if (!pci.captureBootFunctions())' not in construct:
        errors.append('scratch nullable capture failure can reach dispatch')
    if 'if (poison) @memset(self.buffer[retained..], 0xa5)' not in seal:
        errors.append('scratch diagnostic poison lost')
    # No extra caller may forge a suffix or rebuild the image mapping. Test
    # declarations are excluded, but aliases remain references to audit.
    expected_refs = {
        'prepareScratchRetirement': {'kernel/src/boot/entry.zig': 1},
        'withdrawScratch': {'kernel/src/boot/entry.zig': 1},
        'acknowledgeScratch': {'kernel/src/boot/entry.zig': 1},
        'releaseScratch': {'kernel/src/boot/entry.zig': 1},
        'reclaimBootScratchOrHalt': {'kernel/src/boot/entry.zig': 1},
    }
    for name, expected_paths in expected_refs.items():
        found = {}
        for path, text in sources.items():
            text = without_tests(text)
            count = len(re.findall(r'\b'+name+r'\b', text)) - len(re.findall(r'\bfn\s+'+name+r'\b', text))
            if count: found[path] = count
        if found != expected_paths:
            errors.append('scratch boot-only reference census changed: '+name+' '+str(found))
    return errors


def main():
    files = {str(path.relative_to(ROOT)): path.read_text()
             for directory in ('kernel/src', 'bootloader/limine')
             for path in (ROOT/directory).rglob('*.zig')}
    errors = check(files)
    if errors:
        raise SystemExit('\n'.join(errors))
    mutations = (
        ('kernel/src/arch/x86_64/boot_direct_map.zig', 'root[511] &= ~@as(u64, 4)', 'root[511] |= 4'),
        ('kernel/src/arch/x86_64/boot_direct_map.zig', 'mov %[root], %%cr3', 'nop'),
        ('kernel/src/arch/x86_64/boot_direct_map.zig', 'const flags: u64 = (@as(u64, 1) << 63) | 3', 'const flags: u64 = 3'),
        ('kernel/src/arch/x86_64/boot_direct_map.zig', '!scratch_acknowledged', 'false'),
        ('kernel/src/arch/x86_64/boot_direct_map.zig', 'view.contains(pdp_pa, 4096)', 'true'),
        ('bootloader/limine/resources.zig', 'owner.handoffScratch(', 'owner.skipHandoff('),
        ('kernel/src/arch/x86_64/platform.zig', 'boot_direct_map.installScratchRoot(', 'boot_direct_map.skipRoot('),
        ('kernel/src/arch/x86_64/platform.zig', 'boot_direct_map.scratchRootMatches(', 'boot_direct_map.skipOwnership('),
        ('kernel/src/boot/boot_scratch.zig', 'if (poison) @memset(self.buffer[retained..], 0xa5)', 'if (poison) {}'),
        ('kernel/src/boot/entry.zig', 'halt.haltWithMessage("boot resource copy failed")', 'ignoreFailure()'),
        ('kernel/src/boot/entry.zig', 'halt.haltWithMessage("bootfs staging metadata unavailable")', 'ignoreFailure()'),
        ('bootloader/limine/resources.zig', 'halt.haltWithMessage("PMM region table allocation failed")', 'ignoreFailure()'),


        ('kernel/src/boot/boot_scratch.zig', 'try Retirer.acknowledge(', 'try Retirer.skipAck('),
        ('kernel/src/boot/boot_scratch.zig', 'self.buffer = self.buffer[0..retained]', 'self.buffer = self.buffer'),
        ('kernel/src/boot/boot_scratch.zig', 'if (!self.sealed or self.reclaim_started)', 'if (!self.sealed)'),
        ('kernel/src/boot/entry.zig', 'reclaimBootScratchOrHalt();', 'skipScratchReclaim();'),
        ('kernel/src/boot/entry.zig', 'user_copy.flushTlbForCr3Range(', 'user_copy.skipScratchAck('),
        ('kernel/src/boot/boot_scratch.zig', 'if (self.installed) return error.AlreadyInstalled;', ''),
        ('kernel/src/boot/boot_scratch.zig', 'header.borrowed) return error.Borrowed', 'false) return error.Borrowed'),
        ('kernel/src/boot/entry.zig', 'res.bootfs_staging.release()', 'skip_metadata_release()'),
        ('kernel/src/boot/entry.zig', 'res.init_staging.release()', 'skip_init_release()'),
        ('kernel/src/boot/entry.zig', 'boot_scratch.seal()', 'skip_seal()'),
        ('kernel/src/boot/elf_load.zig', 'staging.release()', 'skip_window_release()'),
        ('kernel/src/pci.zig', 'allocation.publishPersistent()', 'skip_catalog_publication()'),
        ('bootloader/limine/kernel_entry.zig', 'entry.persistBootResourcesOrHalt(', 'entry.skipPreparation('),
        ('kernel/src/boot/entry.zig', 'slot.release()', 'skip_page_release()'),
        ('kernel/src/boot/entry.zig', 'for (staged_pages) |*slot|', 'for (staged_pages) |slot|'),
        ('kernel/src/boot/entry.zig', 'for (pages) |*slot| slot.* = .{};', ''),
        ('kernel/src/boot/init_setup.zig', 'source_loan.end()', 'skip_page_loan_end()'),
        ('kernel/src/boot/resource_pages.zig', 'grant.physical.toInt(), grant.pages', '0, grant.physical.toInt(), grant.pages'),
        ('kernel/src/boot/resource_pages.zig', 'popContiguousOwnedInRange(1, 4096,', 'popContiguousOwnedInRange(1, 0,'),
    )
    for path, old, new in mutations:
        assert old in files[path], (path, old)
        changed = dict(files)
        changed[path] = changed[path].replace(old, new, 1)
        assert check(changed), (path, old)
    changed = dict(files)
    changed['kernel/src/late_scratch_use.zig'] = 'const late = @import("boot/boot_scratch.zig"); fn bad() void { _ = late.reserveTemporary(1); }'
    assert check(changed)
    for name in ('hhdmSlice', 'hhdmBytes', 'identityBytes'):
        changed = dict(files)
        changed['kernel/src/late_boot_page_view.zig'] = f'const indirect = alias.{name};'
        assert check(changed), (name, 'late alias')
    for wrapper, module in (('captureBootFunctions', 'pci.zig'),
                            ('captureBootFunctionsWith', 'pci.zig'),
                            ('loadUserElfIntoProcessPages', 'boot/elf_load.zig'),
                            ('loadUserElfIntoProcessPagesOrHalt', 'boot/elf_load.zig'),
                            ('persistBootResourcesOrHalt', 'boot/entry.zig'),
                            ('bootWithResources', 'boot/entry.zig')):
        changed = dict(files)
        changed['kernel/src/late_boot_wrapper.zig'] = (
            f'const alias = @import("{module}"); fn late() void {{ _ = alias.{wrapper}(); }}')
        assert check(changed), wrapper
        changed['kernel/src/late_boot_wrapper.zig'] = (
            f'const alias = @import("{module}"); const indirect = alias.{wrapper};')
        assert check(changed), (wrapper, 'indirect')
    commented = dict(files)
    commented['kernel/src/pci.zig'] += '\n// boot_scratch.allocate(8); boot_scratch.free(buf);\n'
    assert not check(commented)
    print('boot scratch boundaries: actual callers, Limine census and negative bypass tests PASS')


if __name__ == '__main__':
    main()
