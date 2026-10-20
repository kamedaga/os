#!/usr/bin/env python3
"""Guard only migrated address boundaries; not a whole-kernel safety proof."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def code_only(source):
    # Preserve positions/newlines so function bodies can be delimited, while
    # explanatory comments and string/assembly text cannot satisfy a guard.
    # Character literals must precede multiline-ASM matching: '\\' inside
    # AML test data is not an ASM line, and '{' must not alter brace depth.
    pattern = r"//[^\n]*|'(?:\\.|[^'\\])*'|\\\\[^\n]*|\"(?:\\.|[^\"\\])*\""
    return re.sub(pattern, lambda match: ''.join('\n' if c == '\n' else ' ' for c in match[0]), source)


def function_body(source, name):
    match = re.search(r'\bfn\s+' + re.escape(name) + r'\s*\(', source)
    if not match:
        raise ValueError('missing migrated function ' + name)
    # Close the parameter list first; parameter/return types can themselves
    # contain braces (notably error{AlreadyInitialized}!void).
    cursor = source.index('(', match.start()) + 1
    parens = 1
    while cursor < len(source) and parens:
        parens += (source[cursor] == '(') - (source[cursor] == ')')
        cursor += 1
    if parens:
        raise ValueError('unterminated parameters ' + name)
    start = source.index('{', cursor)
    while re.search(r'\berror\s*$', source[cursor:start]):
        type_depth, cursor = 1, start + 1
        while cursor < len(source) and type_depth:
            type_depth += (source[cursor] == '{') - (source[cursor] == '}')
            cursor += 1
        if type_depth:
            raise ValueError('unterminated return error set ' + name)
        start = source.index('{', cursor)
    depth = 1
    end = start + 1
    while end < len(source) and depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if depth:
        raise ValueError('unterminated function ' + name)
    return source[start + 1:end - 1]


def without_tests(source):
    # Only test blocks may reset private boot configuration. Braces in strings
    # and comments have already been removed by code_only.
    while match := re.search(r'\btest\s+\{', source):
        start = source.index('{', match.start())
        depth, end = 1, start + 1
        while depth and end < len(source):
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        if depth:
            raise ValueError('unterminated test block')
        source = source[:match.start()] + ' ' * (end - match.start()) + source[end:]
    return source


# Reviewed physical access call graph, not a general Zig safety proof. Adding/renaming a
# callee or import requires review; this closes helper/alias escape routes in
# these functions without freezing unrelated locks or firmware/MMIO code.
PHYSICAL_ACCESS_CALLS = {'amd_iommu.zig:allocIovaBank': ['allocZeroPage',
                                 'borrowRamObject',
                                 'physical_layout.PhysicalAddress.fromInt'],
 'amd_iommu.zig:borrowRamObject': ['@call', '@import', 'physical.toInt', 'slice'],
 'amd_iommu.zig:completionWait': ['@as',
                                  '@intCast',
                                  '@truncate',
                                  'borrowRamObject',
                                  'fromInt',
                                  'mmioRead64',
                                  'observe',
                                  'submit',
                                  'volatile'],
 'amd_iommu.zig:deviceTableAt': ['@as',
                                 '@sizeOf',
                                 'borrowRamObject',
                                 'offset',
                                 'physical_layout.PhysicalAddress.fromInt'],
 'amd_iommu.zig:freeIovaBank': ['@fieldParentPtr', '@panic', 'free_list.appendPage', 'physical.toInt'],
 'amd_iommu.zig:pageAt': ['borrowRamObject', 'fromInt'],
 'amd_iommu.zig:pollEventsLocked': ['@as',
                                    '@intCast',
                                    '@truncate',
                                    'borrowRamObject',
                                    'fromInt',
                                    'log.writeFmt',
                                    'mmioRead64',
                                    'mmioWrite64',
                                    'or',
                                    'quarantineEvent',
                                    'quarantineUnit'],
 'amd_iommu.zig:refcounts': ['borrowRamObject', 'fromInt'],
 'amd_iommu.zig:submit': ['@intCast', 'borrowRamObject', 'fromInt', 'mmioRead64', 'mmioWrite64', 'volatile'],
 'arch/x86_64/physical_layout.zig:identitySlice': ['@alignOf', '@as', '@compileError', '@intCast', '@ptrFromInt', '@sizeOf', 'base.toInt', 'tableVirtual', 'std.math.mul'],
 'arch/x86_64/platform.zig:allocateApRuntimeStacks': ['@alignCast', '@intCast', 'paintUnusedStack',
                                                      '@memset',
                                                      'free_list.popContiguousBelow',
                                                      'fromInt',
                                                      'highKernelPdSlot',
                                                      'kernelHighMappingActive',
                                                      'kernelPtrPaddr',
                                                      'physical_layout.identitySlice'],
 # Boot-only painting borrows the already validated identity slice; neither
 # helper may manufacture a physical pointer or add an allocation/callback.
 'arch/x86_64/platform.zig:paintUnusedStack': ['@ptrCast', 'stackPaintWord'],
 'arch/x86_64/platform.zig:stackPaintWord': ['@as'],
 'arch/x86_64/platform.zig:bootMmioPageIsExplicitUc': ['@intCast',
                                                           'and',
                                                           'borrowIdentityTable',
                                                           'fromInt',
                                                           'return'],
 'arch/x86_64/platform.zig:borrowIdentityTable': ['borrowIdentityTableMut'],
 'arch/x86_64/platform.zig:borrowIdentityTableMut': ['@call', 'address.toInt'],
 'arch/x86_64/platform.zig:identityPtePointer': ['@intCast',
                                                 'borrowIdentityTableMut',
                                                 'fromInt',
                                                 'identityPdPointer',
                                                 'or'],
 'arch/x86_64/platform.zig:mapBootMmioRange': ['@intCast',
                                                       'and',
                                                       'borrowIdentityTableMut',
                                                       'fromInt',
                                                       'or',
                                                       'readCr3',
                                                       'readMsr',
                                                       'splitLargeKernelIdentityPage',
                                                       'writeCr3'],
 'arch/x86_64/platform.zig:mapKernelIdentityPage': ['@intCast',
                                                    'borrowIdentityTableMut',
                                                    'fromInt',
                                                    'splitLargeKernelIdentityPage'],
 'arch/x86_64/platform.zig:visitRamCacheIdentityAliases': ['@as',
                                                           '@atomicLoad',
                                                           '@import',
                                                           '@intCast',
                                                           'borrowIdentityTableMut',
                                                           'fromInt',
                                                           'identityPdPointer',
                                                           'kernelHighMappingActive',
                                                           'or',
                                                           'paging.IdentitySplit.validateSource',
                                                           'paging.validatePages',
                                                           'ramCacheOwnedIdentityPt',
                                                           'returnedBootScratchLeaf',
                                                           'std.math.add',
                                                           'visit'],
 'boot/process_factory.zig:installInitialUserStackOrHalt': ['@alignCast',
                                                            '@as',
                                                            '@import',
                                                            '@memcpy',
                                                            '@memset',
                                                            '@ptrCast',
                                                            'fromInt',
                                                            'halt.haltWithMessage',
                                                            'identitySlice',
                                                            'loaded.programHeaderVirtualAddress',
                                                            'pushStackBytes',
                                                            'stackVaFromOffset',
                                                            'state.fillRandomBytes'],
 'memory/ram_cache_storage.zig:allocateWith': ['@alignCast',
                                               '@alignOf',
                                               '@ptrCast',
                                               '@sizeOf',
                                               'backend.take',
                                               'std.math.add',
                                               'std.math.mul',
                                               'std.mem.alignForward'],
 'memory/ram_cache_storage.zig:give': ['@panic', 'address.toInt', 'self.free_list.appendContiguousRange'],
 'memory/ram_cache_storage.zig:overlapsTargets': ['paging.pagesIntersect', 'self.physical.toInt'],
 'memory/ram_cache_storage.zig:releaseWith': ['backend.give'],
 'memory/ram_cache_storage.zig:take': ['PhysicalAddress.fromInt',
                                       'physical_layout.identitySlice',
                                       'self.free_list.popContiguousBelow',
                                       'std.math.mul'],
 'page_fault_log.zig:dumpPageWalkForVa': ['@intCast',
                                          'fromInt',
                                          'getHooks',
                                          'h.write',
                                          'h.write_hex_raw',
                                          'platform.borrowIdentityTable'],
 'state/memory.zig:zeroAllocatedPage': ['@import', '@memset', 'fromInt', 'hook', 'identitySlice'],
 'vtd.zig:allocIovaBank': ['allocZeroPage', 'borrowRamObject', 'physical_layout.PhysicalAddress.fromInt'],
 'vtd.zig:allocTablePage': ['allocZeroPage', 'flushTableCache', 'tableAt'],
 'vtd.zig:borrowRamObject': ['@call', '@import', 'physical.toInt', 'slice'],
 'vtd.zig:ensureChildTable': ['@sizeOf',
                              'allocTablePage',
                              'flushTableCache',
                              'pageAddress',
                              'tables.setSecondLevelEntry'],
 'vtd.zig:ensureDomainRootChild': ['@as', '@sizeOf', 'allocTablePage', 'flushTableCache', 'pageAddress'],
 'vtd.zig:flushInitialTables': ['flushTableCache', 'tableAt'],
 'vtd.zig:flushTableCache': ['@alignCast', '@as', '@intCast', '@intFromPtr', '@ptrCast', 'volatile'],
 'vtd.zig:freeIovaBank': ['@fieldParentPtr', '@panic', 'free_list.appendPage', 'physical.toInt'],
 'vtd.zig:mapPage': ['@sizeOf',
                     'flushTableCache',
                     'refcountsAt',
                     'std.math.maxInt',
                     'tables.setSecondLevelEntry',
                     'walkToLeaf', 'iova_address.toInt'],
 'vtd.zig:refcountsAt': ['borrowRamObject', 'fromInt'],
 'vtd.zig:setDomainRootPermissions': ['@as', 'flushTableCache', 'pageAddress', 'tableAt'],
 'vtd.zig:tableAt': ['borrowRamObject', 'fromInt'],
 'vtd.zig:unmapPage': ['@sizeOf', 'flushTableCache', 'refcountsAt', 'walkToLeaf'],
 'vtd.zig:walkToLeaf': [],
 'vtd.zig:withdrawPage': ['@sizeOf', 'flushTableCache', 'refcountsAt', 'walkToLeaf']}
PHYSICAL_ACCESS_CALLS['arch/x86_64/physical_layout.zig:tableVirtual'] = ['active_image.physical_base.toInt', 'active_image.virtual_base.toInt', 'or']

PHYSICAL_ACCESS_ASM = {'amd_iommu.zig:completionWait': ['asm volatile ("mfence" ::: .{ .memory = true })',
                                  'asm volatile ("pause")'],
 'amd_iommu.zig:submit': ['asm volatile ("mfence" ::: .{ .memory = true })'],
 'vtd.zig:flushTableCache': ['asm volatile ("mfence" ::: .{ .memory = true })',
                             'asm volatile ("clflush (%%rax)" : : [address] "{rax}" (current), : .{ .memory '
                             '= true })',
                             'asm volatile ("mfence" ::: .{ .memory = true })']}


PHYSICAL_ACCESS_IMPORTS = {'amd_iommu.zig:borrowRamObject': ['memory/physical_access_test.zig'],
 'arch/x86_64/platform.zig:visitRamCacheIdentityAliases': ['../../memory/ram_cache_paging.zig'],
 'boot/process_factory.zig:installInitialUserStackOrHalt': ['../arch/x86_64/physical_layout.zig'],
 'state/memory.zig:zeroAllocatedPage': ['../arch/x86_64/physical_layout.zig'],
 'vtd.zig:borrowRamObject': ['memory/physical_access_test.zig']}

def raw_function_body(source, name):
    masked = code_only(source)
    body = function_body(masked, name)
    start = masked.index(body)
    return source[start:start + len(body)]


def call_names(body):
    return set(re.findall(r'(?<![\w@])(@?[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*\(', body)) - {'if', 'while', 'for', 'switch', 'catch', 'asm'}


def assembly_blocks(body):
    masked = code_only(body)
    result = []
    for match in re.finditer(r'\basm\s+(?:volatile\s*)?\(', masked):
        end, depth = match.end(), 1
        while depth and end < len(masked):
            depth += (masked[end] == '(') - (masked[end] == ')')
            end += 1
        if depth:
            raise ValueError('unterminated assembly')
        result.append(re.sub(r'\s+', ' ', body[match.start():end]).strip())
    return result


def check_physical_access(sources):
    errors = []
    for key, allowed in PHYSICAL_ACCESS_CALLS.items():
        path, name = key.split(':')
        try:
            raw = raw_function_body(sources[path], name)
        except ValueError as error:
            errors.append(key + ': ' + str(error))
            continue
        body = code_only(raw)
        if re.findall(r'@import\("([^"]+)"\)', raw) != PHYSICAL_ACCESS_IMPORTS.get(key, []):
            errors.append(key + ': unreviewed inline import')
        if call_names(body) != set(allowed):
            errors.append(key + ': unreviewed call/alias or missing boundary')
        if '@call' in allowed:
            # These three wrappers have one direct, compile-time mapping call.
            # An extra @call must not smuggle in a differently named helper.
            if body.count('@call') != 1 or not re.search(
                    r'@call\(\.always_inline, physical_layout\.identitySlice,', body):
                errors.append(key + ': unreviewed indirect/explicit helper call')
        if assembly_blocks(raw) != PHYSICAL_ACCESS_ASM.get(key, []):
            errors.append(key + ': unreviewed assembly memory access')
        # Rebinding an approved callee name must not evade the call census.
        for callee in allowed:
            root = callee.split('.')[0]
            if root.startswith('@') or ('.' in callee and root not in ('physical_layout', 'platform', 'std')):
                continue
            if re.search(r'\b(?:const|var)\s+' + re.escape(root) + r'\s*=\s*[\w.]+\s*;', body):
                errors.append(key + ': callable alias rebinding')
    contracts = {
        'arch/x86_64/physical_layout.zig:tablePointer': ['base.toInt() & 4095 != 0', 'tableVirtual(base.toInt(), 4096) orelse return null'],
        'arch/x86_64/physical_layout.zig:tableVirtual': ['bytes > direct_limit - physical', 'bytes > active_image.size_bytes - offset', 'bytes > image_pa - physical', 'bytes <= (@as(u64, 1) << 52) - physical'],
        'arch/x86_64/platform.zig:borrowIdentityTable': ['return borrowIdentityTableMut(address)'],
        'arch/x86_64/platform.zig:borrowIdentityTableMut': ['physical_layout.tablePointer(address)'],
        'arch/x86_64/physical_layout.zig:identitySlice': ['std.math.mul(usize, @sizeOf(T), count)', 'base.toInt() % @alignOf(T) != 0', 'bytes > limit - physical', 'count == 0 or base.toInt() == 0'],
        'vtd.zig:freeIovaBank': ['const physical = backing.physical', 'free_list.appendPage(physical.toInt())'],
        'amd_iommu.zig:freeIovaBank': ['const physical = backing.physical', 'free_list.appendPage(physical.toInt())'],
        'memory/ram_cache_storage.zig:releaseWith': ['const address = block.physical', 'backend.give(address, pages)'],
        'memory/ram_cache_storage.zig:overlapsTargets': ['paging.pagesIntersect(pages, self.physical.toInt(), self.pages * 4096)'],
        'memory/ram_cache_storage.zig:allocateWith': ['.physical = allocation.physical'],
    }
    for key, required in contracts.items():
        path, name = key.split(':')
        try:
            body = function_body(code_only(sources[path]), name)
        except ValueError:
            continue  # Reported by the closed function census above.
        for fragment in required:
            if fragment not in body:
                errors.append(key + ': missing address/grant contract ' + fragment)
    for path in ('vtd.zig', 'amd_iommu.zig'):
        raw = raw_function_body(sources[path], 'borrowRamObject')
        if not re.search(r'if \(builtin.is_test\)\s*@import\("memory/physical_access_test.zig"\)\.slice\(T, physical.toInt\(\), 1\)\s*else\s*@call\(\.always_inline, physical_layout.identitySlice, \.\{ T, physical, 1 \}\)', raw):
            errors.append(path + ': fixture must be confined to the host-only branch')
    if not re.search(r'fn flushTableCache\(pointer: \*const anyopaque, length: usize\)', sources['vtd.zig']):
        errors.append('vtd: flush must take a CPU pointer, never a physical scalar')
    # Approved aliases must still designate the reviewed module, including
    # inline imports; a same-named external raw helper is not an escape hatch.
    for path, spelling, target in (
        ('vtd.zig', 'physical_layout', 'arch/x86_64/physical_layout.zig'),
        ('amd_iommu.zig', 'physical_layout', 'arch/x86_64/physical_layout.zig'),
        ('arch/x86_64/platform.zig', 'physical_layout', 'physical_layout.zig'),
        ('page_fault_log.zig', 'platform', 'arch/x86_64/platform.zig'),
        ('memory/ram_cache_storage.zig', 'physical_layout', '../arch/x86_64/physical_layout.zig'),
    ):
        if f'const {spelling} = @import("{target}");' not in sources[path]:
            errors.append(path + ': mapping import alias changed')
    return errors


def self_test_u6a(sources):
    assert not check_physical_access(sources), check_physical_access(sources)
    path = 'page_fault_log.zig'
    for label, injection in (
        ('1 raw physical pointer', 'const bypass: *u64 = @ptrFromInt(cr3);'),
        ('2 raw helper', 'const bypass = rawBorrow(cr3);'),
        ('2 alias', 'const borrow = platform.borrowIdentityTable; const bypass = borrow(.fromInt(cr3));'),
        ('2 asm bypass', 'asm volatile ("mov (%[p]), %%rax" : : [p] "r" (cr3) : .{ .memory = true });'),
    ):
        changed = dict(sources)
        body = raw_function_body(changed[path], 'dumpPageWalkForVa')
        changed[path] = changed[path].replace(body, '\n' + injection + body, 1)
        assert check_physical_access(changed), label
        print('physical access injection PASS:', label)
    for label, path, old, new in (
        ('4 KVA return', 'vtd.zig', 'free_list.appendPage(physical.toInt())', 'free_list.appendPage(@intFromPtr(bank))'),
        ('5 physical flush', 'vtd.zig', 'flushTableCache(tableAt(paddr), page_size)', 'flushTableCache(paddr, page_size)'),
        ('6 table alignment', 'arch/x86_64/physical_layout.zig', 'base.toInt() & 4095 != 0', 'false'),
        ('6 complete extent', 'arch/x86_64/physical_layout.zig', 'bytes > limit - physical', 'false'),
        ('fixture runtime escape', 'amd_iommu.zig', 'const objects = if (builtin.is_test)', 'const objects = if (true)'),
        ('missing target', 'page_fault_log.zig', 'fn dumpPageWalkForVa(', 'fn renamedWalk('),
    ):
        changed = dict(sources)
        assert old in changed[path]
        changed[path] = changed[path].replace(old, new, 1)
        assert check_physical_access(changed), label
        print('physical access injection PASS:', label)
    changed = dict(sources)
    body = raw_function_body(changed['page_fault_log.zig'], 'dumpPageWalkForVa')
    changed['page_fault_log.zig'] = changed['page_fault_log.zig'].replace(body, '\n// @ptrFromInt(cr3)\nconst message = "@ptrFromInt(cr3)";\n' + body, 1)
    assert not check_physical_access(changed), check_physical_access(changed)
    print('physical access injection PASS: 8 comments and strings accepted')

# address domains: reviewed owner/accessor graph. Changes require explicit census review.
ADDRESS_DOMAIN_CALLS = {'../../bootloader/limine/resources.zig:collectMemoryStatsOrHalt': ['@as', '@intCast', '@min', '@sizeOf', 'allocation.borrowInto', 'allocation.publishPersistent', 'collector.addRegion', 'collector.finish', 'collector.reserveBootPages', 'dm.validateImage', 'entry.installPmmBootAccess', 'free_list.installRegions', 'halt.haltWithError', 'halt.haltWithMessage', 'hhdmOffsetOrHalt', 'kernel_api.boot_scratch.reservePersistent', 'loan.bytes', 'loan.end', 'owner.build', 'owner.handoffScratch', 'owner.installScratch', 'owner.metadata', 'pmm.MemoryCollector.init', 'pmm.prepareBootAccess', 'pmm.regionBoundary', 'pmm.sortBoundaries', 'pmmRegionKind', 'std.math.add', 'std.math.mul', 'std.mem.bytesAsSlice', 'storage.end.toInt', 'storage.start.toInt', 'userSpaceStoragePhysicalRange', 'x86_platform.directMapCpuFeatures', 'x86_platform.enableNxForCurrentCpu'],
 '../../bootloader/limine/resources.zig:hhdmPtrToPaddr': ['addresses.KernelVirtualAddress.fromPointer',
                                                          'fromInt',
                                                          'halt.haltWithMessage',
                                                          'hhdmOffsetOrHalt',
                                                          'virtual.toInt'],
 '../../bootloader/limine/resources.zig:userSpaceStoragePhysicalRange': [],
 'acpi_dmar.zig:findDmar': ['@import', 'findTable'],
 'acpi_tables.zig:checksumOk': [],
 'acpi_tables.zig:find': ['findTable', 'return'],
 'acpi_tables.zig:findTable': ['checksumOk',
                               'fromRoot',
                               'physicalBytes',
                               'read32',
                               'std.mem.eql',
                               'std.mem.readInt'],
 'acpi_tables.zig:fromRoot': ['read32', 'std.mem.readInt', 'tableResultAt'],
 'acpi_tables.zig:madtEntries': ['find'],
 'acpi_tables.zig:next': ['or'],
 'acpi_tables.zig:physicalBytes': ['@as', '@import', '@ptrFromInt', 'fixture.slice'],
 'acpi_tables.zig:tableAt': ['return', 'tableResultAt'],
 'acpi_tables.zig:tableResultAt': ['@max',
                                   'checksumOk',
                                   'fromInt',
                                   'physicalBytes',
                                   'read32',
                                   'std.mem.eql'],
 'amd_iommu.zig:freeIova': ['domain.allocator.free',
                            'domainForDevice',
                            'iova_address.toInt',
                            'isActive',
                            'lock',
                            'pollEventsLocked',
                            'unlock'],
 'amd_iommu.zig:mapPage': ['iova_address.toInt', 'pte', 'refcounts', 'std.math.maxInt', 'walk'],
 'amd_iommu.zig:mapPages': ['@as',
                            '@intCast',
                            '@mulWithOverflow',
                            'IovaAddress.fromInt',
                            'domainForDevice',
                            'invalidateDomain',
                            'iova_address.toInt',
                            'isActive',
                            'lock',
                            'mapPage',
                            'or',
                            'pollEventsLocked',
                            'unlock',
                            'validIovaRange',
                            'volatile',
                            'withdrawRange'],
 'amd_iommu.zig:mmioRead64': ['unit.registers.read'],
 'amd_iommu.zig:mmioWrite64': ['unit.registers.write'],
 'amd_iommu.zig:prepareUnit': ['@as',
                               '@errorName',
                               '@intCast',
                               '@max',
                               '@memset',
                               '@tagName',
                               'allocZeroPageForPreparation',
                               'deviceTableAt',
                               'fromInt',
                               'initialDteWords',
                               'list.popContiguousBelow',
                               'log.writeFmt',
                               'mmio.kernelAfterValidation',
                               'mmioRead64',
                               'pageAt',
                               'preparationFailure',
                               'smp.explicitUcMmioStatus',
                               'smp.mmioCacheEvidenceMasks',
                               'user_copy.zeroPhysicalPage'],
 'amd_iommu.zig:reserveIova': ['domain.allocator.reserve',
                               'domainForDevice',
                               'ensureAllocator',
                               'iova_address.toInt',
                               'isActive',
                               'lock',
                               'pollEventsLocked',
                               'unlock'],
 'amd_iommu.zig:unmapRangeForDevice': ['@intCast',
                                       'domainForDevice',
                                       'iova_address.toInt',
                                       'isActive',
                                       'lock',
                                       'pollEventsLocked',
                                       'unlock',
                                       'validIovaRange',
                                       'withdrawRange'],
 'hpet.zig:initialize': ['@as',
                         '@import',
                         '@intCast',
                         '@truncate',
                         'candidate.read',
                         'candidate.write',
                         'discover',
                         'fromInt',
                         'mmio.kernelAfterValidation',
                         'mmio_allowed',
                         'period',
                         'registerRange',
                         'volatile'],
 'hpet.zig:read': ['self.registers.read'],
 'hpet.zig:write': ['self.registers.write'],
 'iommu.zig:allocIova': ['amd.allocIova', 'amd.isActive', 'fromInt', 'isActive', 'vtd.allocIova'],
 'iommu.zig:freeIova': ['amd.freeIova', 'amd.isActive', 'vtd.freeIova'],
 'iommu.zig:mapPages': ['amd.isActive', 'amd.mapPages', 'isActive', 'vtd.mapPages'],
 'iommu.zig:reserveIova': ['amd.isActive', 'amd.reserveIova', 'isActive', 'vtd.reserveIova'],
 'iommu.zig:unmapRangeForDevice': ['amd.isActive',
                                   'amd.unmapRangeForDevice',
                                   'isActive',
                                   'vtd.unmapRangeForDevice'],
 'lapic.zig:enableLocalApic': ['fromInt',
                               'kvmX2ApicSupported',
                               'mmio.kernelAfterValidation',
                               'or',
                               'rdmsr',
                               'registerMsr',
                               'writeRegister',
                               'wrmsr'],
 'lapic.zig:readRegister': ['@truncate', 'rdmsr', 'registerMsr', 'registers.read'],
 'lapic.zig:writeRegister': ['registerMsr', 'registers.write', 'wrmsr'],
 'memory/mmio.zig:kernelAfterValidation': ['@ptrFromInt', 'physical.toInt'],
 'memory/mmio.zig:mappedAfterValidation': [],
 'memory/mmio.zig:pointer': ['@alignCast', '@ptrCast', '@sizeOf', 'std.debug.assert'],
 'memory/mmio.zig:read': ['self.pointer'],
 'memory/mmio.zig:write': ['self.pointer'],
 'pci.zig:ecamAddress': ['window.address'],
 'pci.zig:readConfig': ['@intCast',
                        'configAccessValid',
                        'ecamAddress',
                        'fromInt',
                        'lockConfig',
                        'mmio.kernelAfterValidation',
                        'readEcam',
                        'readLegacyConfig',
                        'unlockConfig'],
 'pci.zig:readEcam': ['borrow.read'],
 'pci.zig:writeConfig': ['@intCast',
                         'configAccessValid',
                         'ecamAddress',
                         'fromInt',
                         'lockConfig',
                         'mmio.kernelAfterValidation',
                         'unlockConfig',
                         'writeEcam',
                         'writeLegacyConfig'],
 'pci.zig:writeEcam': ['@truncate', 'borrow.write'],
 'smp.zig:buildTrampoline': ['@intCast',
                             '@memset',
                             '@truncate',
                             'emit16',
                             'emit32',
                             'emit64',
                             'emit8',
                             'fromInt',
                             'trampolineWriter',
                             'write64'],
 'smp.zig:collectMadtLapicIds': ['@import',
                                 '@intCast',
                                 'appendLapicId',
                                 'entries.next',
                                 'madtEntries',
                                 'std.math.maxInt',
                                 'std.mem.readInt'],
 'smp.zig:trampolineWriter': ['@ptrFromInt', 'physical.rangeFits', 'physical.toInt'],
 'user_copy.zig:mapPhysWindow': ['@as',
                                 '@import',
                                 '@intCast',
                                 'assertOneWayPerPage',
                                 'getHooks',
                                 'h.invlpg',
                                 'publishWindow',
                                 'ramCachePolicyForLockedPage',
                                 'wcPhysicalWindowLeaseBlocks',
                                 'windowSlotVa'],
 'user_copy.zig:physWindowAddr': ['@addWithOverflow',
                                  '@as',
                                  '@intCast',
                                  'getHooks',
                                  'mapPhysPageForKernelAccess'],
 'user_copy.zig:publishWindow': ['@ptrFromInt', 'windowSlotVa'],
 'user_copy.zig:readPhysU32': ['@alignCast',
                               '@ptrCast',
                               '@sizeOf',
                               'PhysCopyWindowGuard.lock',
                               'physWindowAddr',
                               'window.unlock'],
 'user_copy.zig:readPhysU64': ['@alignCast',
                               '@ptrCast',
                               '@sizeOf',
                               'PhysCopyWindowGuard.lock',
                               'physWindowAddr',
                               'window.unlock'],
 'user_copy.zig:readPhysU8': ['@alignCast',
                              '@ptrCast',
                              '@sizeOf',
                              'PhysCopyWindowGuard.lock',
                              'physWindowAddr',
                              'window.unlock'],
 'user_copy.zig:readPhysicalBytesInWindow': ['@memcpy', 'physWindowAddr'],
 'user_copy.zig:readUncachedMmioU32': ['PhysCopyWindowGuard.lock',
                                       'uncachedMmioWordAddress',
                                       'window.unlock',
                                       'window_addr.read'],
 'user_copy.zig:uncachedMmioWordAddress': ['@as',
                                           'mapPhysPageWithCache',
                                           'mmio.mappedAfterValidation',
                                           'smp.uncachedKernelMmioPageAllowed'],
 'user_copy.zig:writePhysU8': ['@alignCast',
                               '@ptrCast',
                               '@sizeOf',
                               'PhysCopyWindowGuard.lock',
                               'physWindowAddr',
                               'window.unlock'],
 'user_copy.zig:writePhysicalBytesInWindow': ['@memcpy', 'physWindowAddr'],
 'user_copy.zig:writeUncachedMmioU32': ['PhysCopyWindowGuard.lock',
                                        'uncachedMmioWordAddress',
                                        'window.unlock',
                                        'window_addr.write'],
 'vtd.zig:freeIova': ['domain.allocator.free',
                      'domainForDevice',
                      'iova_address.toInt',
                      'isActive',
                      'lock',
                      'unlock'],
 'vtd.zig:mapPage': ['@sizeOf',
                     'flushTableCache',
                     'iova_address.toInt',
                     'refcountsAt',
                     'std.math.maxInt',
                     'tables.setSecondLevelEntry',
                     'walkToLeaf'],
 'vtd.zig:mapPages': ['@as',
                      '@intCast',
                      'IovaAddress.fromInt',
                      'domainForDevice',
                      'dumpFaultsLocked',
                      'flushMapChanges',
                      'iova_address.toInt',
                      'isActive',
                      'kernel_log.writeFmt',
                      'lock',
                      'mapPage',
                      'or',
                      'unlock',
                      'validIovaRange',
                      'withdrawRange'],
 'vtd.zig:mmioRead32': ['driver_state.registers.read'],
 'vtd.zig:mmioRead64': ['mmioRead64At'],
 'vtd.zig:mmioRead64At': ['registers.read'],
 'vtd.zig:mmioWrite32': ['driver_state.registers.write'],
 'vtd.zig:mmioWrite64': ['driver_state.registers.write'],
 'vtd.zig:reserveIova': ['domain.allocator.reserve',
                         'domainForDevice',
                         'ensureDomainAllocator',
                         'iova_address.toInt',
                         'isActive',
                         'lock',
                         'unlock'],
 'vtd.zig:unmapRangeForDevice': ['@intCast',
                                 'domainForDevice',
                                 'dumpFaultsLocked',
                                 'iova_address.toInt',
                                 'isActive',
                                 'kernel_log.writeFmt',
                                 'lock',
                                 'unlock',
                                 'validIovaRange',
                                 'withdrawRange']}

ADDRESS_DOMAIN_ASM = {'amd_iommu.zig:mapPages': ['asm volatile ("mfence" ::: .{ .memory = true })'],
 'hpet.zig:initialize': ['asm volatile ("pause")']}

ADDRESS_DOMAIN_IMPORTS = {'../../bootloader/limine/resources.zig:collectMemoryStatsOrHalt': [],
 '../../bootloader/limine/resources.zig:hhdmPtrToPaddr': [],
 '../../bootloader/limine/resources.zig:userSpaceStoragePhysicalRange': [],
 'acpi_dmar.zig:findDmar': ['acpi_tables.zig'],
 'acpi_tables.zig:checksumOk': [],
 'acpi_tables.zig:find': [],
 'acpi_tables.zig:findTable': [],
 'acpi_tables.zig:fromRoot': [],
 'acpi_tables.zig:madtEntries': [],
 'acpi_tables.zig:next': [],
 'acpi_tables.zig:physicalBytes': ['builtin', 'memory/physical_access_test.zig'],
 'acpi_tables.zig:tableAt': [],
 'acpi_tables.zig:tableResultAt': [],
 'amd_iommu.zig:freeIova': [],
 'amd_iommu.zig:mapPage': [],
 'amd_iommu.zig:mapPages': [],
 'amd_iommu.zig:mmioRead64': [],
 'amd_iommu.zig:mmioWrite64': [],
 'amd_iommu.zig:prepareUnit': [],
 'amd_iommu.zig:reserveIova': [],
 'amd_iommu.zig:unmapRangeForDevice': [],
 'hpet.zig:initialize': ['scheduler_connection.zig'],
 'hpet.zig:read': [],
 'hpet.zig:write': [],
 'iommu.zig:allocIova': [],
 'iommu.zig:freeIova': [],
 'iommu.zig:mapPages': [],
 'iommu.zig:reserveIova': [],
 'iommu.zig:unmapRangeForDevice': [],
 'lapic.zig:enableLocalApic': [],
 'lapic.zig:readRegister': [],
 'lapic.zig:writeRegister': [],
 'memory/mmio.zig:kernelAfterValidation': [],
 'memory/mmio.zig:mappedAfterValidation': [],
 'memory/mmio.zig:pointer': [],
 'memory/mmio.zig:read': [],
 'memory/mmio.zig:write': [],
 'pci.zig:ecamAddress': [],
 'pci.zig:readConfig': [],
 'pci.zig:readEcam': [],
 'pci.zig:writeConfig': [],
 'pci.zig:writeEcam': [],
 'smp.zig:buildTrampoline': [],
 'smp.zig:collectMadtLapicIds': ['acpi_tables.zig'],
 'smp.zig:trampolineWriter': [],
 'user_copy.zig:mapPhysWindow': ['builtin'],
 'user_copy.zig:physWindowAddr': [],
 'user_copy.zig:publishWindow': [],
 'user_copy.zig:readPhysU32': [],
 'user_copy.zig:readPhysU64': [],
 'user_copy.zig:readPhysU8': [],
 'user_copy.zig:readPhysicalBytesInWindow': [],
 'user_copy.zig:readUncachedMmioU32': [],
 'user_copy.zig:uncachedMmioWordAddress': [],
 'user_copy.zig:writePhysU8': [],
 'user_copy.zig:writePhysicalBytesInWindow': [],
 'user_copy.zig:writeUncachedMmioU32': [],
 'vtd.zig:freeIova': [],
 'vtd.zig:mapPage': [],
 'vtd.zig:mapPages': [],
 'vtd.zig:mmioRead32': [],
 'vtd.zig:mmioRead64': [],
 'vtd.zig:mmioRead64At': [],
 'vtd.zig:mmioWrite32': [],
 'vtd.zig:mmioWrite64': [],
 'vtd.zig:reserveIova': [],
 'vtd.zig:unmapRangeForDevice': []}

ADDRESS_DOMAIN_BINDINGS = {'../../bootloader/limine/resources.zig:import:kernel_api': 'kernel_boot_api',
 '../../bootloader/limine/resources.zig:import:limine': 'protocol.zig',
 '../../bootloader/limine/resources.zig:import:std': 'std',
 'acpi_dmar.zig:import:iova': 'iova.zig',
 'acpi_dmar.zig:import:std': 'std',
 'acpi_tables.zig:import:fixture': 'memory/physical_access_test.zig',
 'acpi_tables.zig:import:physical_layout': 'arch/x86_64/physical_layout.zig',
 'acpi_tables.zig:import:std': 'std',
 'amd_iommu.zig:import:builtin': 'builtin',
 'amd_iommu.zig:import:fixture': 'memory/physical_access_test.zig',
 'amd_iommu.zig:import:iova': 'iova.zig',
 'amd_iommu.zig:import:ivrs': 'acpi_ivrs.zig',
 'amd_iommu.zig:import:kernel': 'kernel.zig',
 'amd_iommu.zig:import:lock_diagnostics': 'sync/lock_diagnostics.zig',
 'amd_iommu.zig:import:log': 'kernel_log.zig',
 'amd_iommu.zig:import:mmio': 'memory/mmio.zig',
 'amd_iommu.zig:import:mtrr': 'arch/x86_64/mtrr.zig',
 'amd_iommu.zig:import:pci': 'pci.zig',
 'amd_iommu.zig:import:physical_layout': 'arch/x86_64/physical_layout.zig',
 'amd_iommu.zig:import:smp': 'smp.zig',
 'amd_iommu.zig:import:std': 'std',
 'amd_iommu.zig:import:types': 'state/types.zig',
 'amd_iommu.zig:import:user_copy': 'user_copy.zig',
 'amd_iommu.zig:import:x86_platform': 'arch/x86_64/platform.zig',
 'hpet.zig:import:acpi': 'acpi_tables.zig',
 'hpet.zig:import:mmio': 'memory/mmio.zig',
 'hpet.zig:import:mtrr': 'arch/x86_64/mtrr.zig',
 'hpet.zig:import:physical_layout': 'arch/x86_64/physical_layout.zig',
 'hpet.zig:import:std': 'std',
 'iommu.zig:import:amd': 'amd_iommu.zig',
 'iommu.zig:import:ivrs': 'acpi_ivrs.zig',
 'iommu.zig:import:std': 'std',
 'iommu.zig:import:types': 'state/types.zig',
 'iommu.zig:import:vtd': 'vtd.zig',
 'lapic.zig:import:mmio': 'memory/mmio.zig',
 'lapic.zig:import:realtime_clock': 'realtime_clock.zig',
 'lapic.zig:import:std': 'std',
 'memory/mmio.zig:import:addresses': 'addresses.zig',
 'memory/mmio.zig:import:std': 'std',
 'pci.zig:import:acpi_tables': 'acpi_tables.zig',
 'pci.zig:import:boot_scratch': 'boot/boot_scratch.zig',
 'pci.zig:import:builtin': 'builtin',
 'pci.zig:import:lock_diagnostics': 'sync/lock_diagnostics.zig',
 'pci.zig:import:log': 'kernel_log.zig',
 'pci.zig:import:mmio': 'memory/mmio.zig',
 'pci.zig:import:std': 'std',
 'pci.zig:import:user_copy': 'user_copy.zig',
 'smp.zig:import:interrupts': 'interrupts.zig',
 'smp.zig:import:lapic': 'lapic.zig',
 'smp.zig:import:lock_diagnostics': 'sync/lock_diagnostics.zig',
 'smp.zig:import:mtrr': 'arch/x86_64/mtrr.zig',
 'smp.zig:import:scheduler_observer': 'scheduler_observer.zig',
 'smp.zig:import:std': 'std',
 'smp.zig:import:x86_platform': 'arch/x86_64/platform.zig',
 'user_copy.zig:import:address_types': 'memory/addresses.zig',
 'user_copy.zig:import:builtin': 'builtin',
 'user_copy.zig:import:kernel': 'kernel.zig',
 'user_copy.zig:import:kernel_log': 'kernel_log.zig',
 'user_copy.zig:import:lock_diagnostics': 'sync/lock_diagnostics.zig',
 'user_copy.zig:import:mmio': 'memory/mmio.zig',
 'user_copy.zig:import:perf': 'smp_perf.zig',
 'user_copy.zig:import:ram_cache': 'memory/ram_cache_policy.zig',
 'user_copy.zig:import:root': 'root',
 'user_copy.zig:import:scheduler': 'scheduler_connection.zig',
 'user_copy.zig:import:smp': 'smp.zig',
 'user_copy.zig:import:std': 'std',
 'user_copy.zig:import:user_vm': 'memory/user_vm.zig',
 'vtd.zig:import:acpi_dmar': 'acpi_dmar.zig',
 'vtd.zig:import:builtin': 'builtin',
 'vtd.zig:import:fixture': 'memory/physical_access_test.zig',
 'vtd.zig:import:iova_storage': 'iova.zig',
 'vtd.zig:import:kernel': 'kernel.zig',
 'vtd.zig:import:kernel_log': 'kernel_log.zig',
 'vtd.zig:import:lock_diagnostics': 'sync/lock_diagnostics.zig',
 'vtd.zig:import:mmio': 'memory/mmio.zig',
 'vtd.zig:import:pci': 'pci.zig',
 'vtd.zig:import:physical_layout': 'arch/x86_64/physical_layout.zig',
 'vtd.zig:import:smp': 'smp.zig',
 'vtd.zig:import:std': 'std',
 'vtd.zig:import:tables': 'vtd_tables.zig',
 'vtd.zig:import:types': 'state/types.zig',
 'vtd.zig:import:user_copy': 'user_copy.zig'}

ADDRESS_DOMAIN_INVARIANTS = {'acpi_tables.zig:physicalBytes': '466e03500d8494f6f521e1b940630c21aa223b162a84d38974335c02f9b5b8bd',
 'memory/mmio.zig:kernelAfterValidation': '52bdcb1b14b5079551fb5194c815c99b4508c2f48dca14eb2c7222a99eb7bc71',
 'memory/mmio.zig:mappedAfterValidation': '4ea614cfbacb3ad485ed426c1eb9c2e87b20658cec7838e38ccfd767b3e6b8e5',
 'user_copy.zig:mapPhysWindow': 'ab489ec395c09d1f86f513ad30d1cbfe826ea30294ed7e2be726ec86fc4416fd',
 'user_copy.zig:uncachedMmioWordAddress': '81c242e8f02023872e5d7484145aee4d14a276b9d712a908955825276a9f728f'}


ADDRESS_DOMAIN_CALLS.update({'pci.zig:initEcam': ['@as',
                      '@import',
                      'acpi_tables.find',
                      'log.write',
                      'log.writeFmt',
                      'mapBootMmioRange',
                      'parseMcfg'],
 'vtd.zig:init': ['@as',
                  '@errorName',
                  '@import',
                  '@intCast',
                  '@intFromBool',
                  '@max',
                  '@truncate',
                  'acpi_dmar.findDmar',
                  'acpi_dmar.parseDmar',
                  'allocTablePage',
                  'cpuid',
                  'domainCount',
                  'dumpFaults',
                  'flushInitialTables',
                  'fromInt',
                  'info.hostAddressWidthBits',
                  'installPciContexts',
                  'invalidateContextCacheGlobal',
                  'invalidateIotlbGlobal',
                  'kernel_log.write',
                  'kernel_log.writeFmt',
                  'lock_diagnostics.failure',
                  'logDeviceScopes',
                  'logInitFailure',
                  'mmio.kernelAfterValidation',
                  'mmioRead32',
                  'mmioRead64At',
                  'mmioWrite32',
                  'mmioWrite64',
                  'or',
                  'pci.bootDmaWindowEnd',
                  'pciFunctionCount',
                  'smp.ucMinusMmioAllowed',
                  'std.mem.alignForward',
                  'table.paddr.toInt',
                  'tableCacheLineBytes',
                  'volatile',
                  'wait32Set']})
ADDRESS_DOMAIN_IMPORTS.update({'vtd.zig:init': ['arch/x86_64/physical_layout.zig'], 'pci.zig:initEcam': ['kernel_log.zig', 'arch/x86_64/platform.zig']})
ADDRESS_DOMAIN_ASM.update({'vtd.zig:init': ['asm volatile ("mfence" ::: .{ .memory = true })'], 'pci.zig:initEcam': []})

# G1: validated firmware cursors replace inline output arrays. prepareUnit
# still constructs the same typed MMIO borrow after the unchanged UC proof.
# ECAM storage acquisition/publication is boot-only; runtime lookup only walks
# stable nodes. These additions do not permit raw physical pointer conversion.
ADDRESS_DOMAIN_CALLS['amd_iommu.zig:prepareUnit'] += [
    'definition.rawRules', 'definition.specials', 'rules.next', 'specials.next']
# Retry exclusion inspects only storage-owner identity; it cannot convert
# physical addresses, allocate, or publish a new hardware reference.
# DRHD ownership moves the unchanged MMIO extent/cache proof to prepareHardwareUnit.
# Keep both the boot dispatcher and extracted physical boundary under this gate.
ADDRESS_DOMAIN_CALLS['vtd.zig:init'] = ['@errorName', 'acpi_dmar.findDmar', 'acpi_dmar.parseDmar', 'allocTablePage', 'drhd.scopes', 'dumpFaults', 'enableHardwareUnit', 'info.hostAddressWidthBits', 'initializationRequiresHalt', 'installPciContexts', 'kernel_log.write', 'kernel_log.writeFmt', 'lock_diagnostics.failure', 'logInitFailure', 'pci.bootDmaWindowEnd', 'pciFunctionCount', 'poolInitializationStarted', 'prepareHardwareUnit', 'releaseDraftTables', 'resolveEndpointScope', 'scopes.next', 'selectUnits', 'table.paddr.toInt', 'unitAt']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:init'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:init'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:prepareHardwareUnit'] = ['@as', '@import', '@intCast', '@intFromBool', '@max', '@truncate', 'cpuid', 'domainCount', 'fromInt', 'kernel_log.writeFmt', 'logInitFailure', 'mmio.kernelAfterValidation', 'mmioRead32', 'mmioRead64At', 'or', 'smp.ucMinusMmioAllowed', 'std.mem.alignForward', 'tableCacheLineBytes', 'unitAt']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:prepareHardwareUnit'] = ['arch/x86_64/physical_layout.zig']
ADDRESS_DOMAIN_ASM['vtd.zig:prepareHardwareUnit'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:enableHardwareUnit'] = ['flushInitialTables', 'invalidateContextCacheGlobal', 'invalidateIotlbGlobal', 'mmioWrite32', 'mmioWrite64', 'volatile', 'wait32Set']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:enableHardwareUnit'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:enableHardwareUnit'] = ['asm volatile ("mfence" ::: .{ .memory = true })']
ADDRESS_DOMAIN_CALLS['vtd.zig:mmioRead32'] = ['unit.registers.read']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:mmioRead32'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:mmioRead32'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:mmioWrite32'] = ['unit.registers.write']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:mmioWrite32'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:mmioWrite32'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:mmioWrite64'] = ['@as', 'unit.registers.read', 'unit.registers.write']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:mmioWrite64'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:mmioWrite64'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:mapPages'] = ['@as', '@intCast', 'IovaAddress.fromInt', 'domainForDevice', 'dumpFaultsLocked', 'flushMapChanges', 'iova_address.toInt', 'isActive', 'kernel_log.writeFmt', 'lock', 'mapPage', 'or', 'quarantineDomain', 'unlock', 'validIovaRange', 'withdrawPage', 'withdrawRange']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:mapPages'] = []
ADDRESS_DOMAIN_ASM['vtd.zig:mapPages'] = []
ADDRESS_DOMAIN_CALLS['vtd.zig:poolInitializationStarted'] = []
ADDRESS_DOMAIN_IMPORTS['vtd.zig:poolInitializationStarted'] = []
ADDRESS_DOMAIN_CALLS['pci.zig:initEcam'] += [
    '@This', '@errorName', 'allocator', 'ecam_windows.install', 'map']
ADDRESS_DOMAIN_IMPORTS['pci.zig:initEcam'] += ['memory/identity_allocator.zig', 'kernel_runtime.zig']

# C: reviewed sparse high MMIO and direct-only RAM/table boundaries.
PHYSICAL_ACCESS_CALLS['arch/x86_64/physical_layout.zig:identitySlice'] = ['@alignOf', '@as', '@compileError', '@import', '@intCast', '@ptrFromInt', '@sizeOf', 'base.toInt', 'std.math.mul']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/physical_layout.zig:identitySlice'] = ['builtin', 'builtin']
PHYSICAL_ACCESS_CALLS['arch/x86_64/platform.zig:borrowIdentityTableMut'] = ['physical_layout.tablePointer']
PHYSICAL_ACCESS_CALLS['arch/x86_64/physical_layout.zig:tableVirtual'] = ['@as', '@import', 'active_image.physical_base.toInt', 'active_image.virtual_base.toInt', 'or']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/physical_layout.zig:tableVirtual'] = ['builtin']
PHYSICAL_ACCESS_CALLS['arch/x86_64/physical_layout.zig:tablePointer'] = ['@ptrFromInt', 'base.toInt', 'tableVirtual']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/physical_layout.zig:tablePointer'] = []
PHYSICAL_ACCESS_CALLS['arch/x86_64/platform.zig:kernelMmioBorrow'] = ['@import', '@ptrFromInt', 'mappedAfterValidation', 'window.lookup']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/platform.zig:kernelMmioBorrow'] = ['../../memory/mmio.zig']
ADDRESS_DOMAIN_CALLS['acpi_tables.zig:physicalBytes'] = ['@import', 'fixture.slice', 'mapKernelFirmwareBytes']
ADDRESS_DOMAIN_IMPORTS['acpi_tables.zig:physicalBytes'] = ['builtin', 'memory/physical_access_test.zig', 'builtin', 'arch/x86_64/platform.zig']
ADDRESS_DOMAIN_INVARIANTS['acpi_tables.zig:physicalBytes'] = '959fd9e71d66317688df3b15abd0d802f63e4d526000f3d5e9bb61cfde72b766'
ADDRESS_DOMAIN_CALLS['lapic.zig:enableLocalApic'] = ['@import', 'fromInt', 'kvmX2ApicSupported', 'mapBootMmioRange', 'mmio.kernelAfterValidation', 'or', 'rdmsr', 'registerMsr', 'writeRegister', 'wrmsr']
ADDRESS_DOMAIN_IMPORTS['lapic.zig:enableLocalApic'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_CALLS['memory/mmio.zig:kernelAfterValidation'] = ['@import', 'kernelMmioBorrow', 'physical.toInt', 'std.debug.assert']
ADDRESS_DOMAIN_IMPORTS['memory/mmio.zig:kernelAfterValidation'] = ['../arch/x86_64/platform.zig']
ADDRESS_DOMAIN_INVARIANTS['memory/mmio.zig:kernelAfterValidation'] = 'd9f570eacea404cffdae22771fd12e5097789a2a1cd2e47718c74e88b8e0d9b1'
ADDRESS_DOMAIN_CALLS['smp.zig:trampolineWriter'] = ['@import', 'identitySlice', 'physical.rangeFits', 'physical.toInt', 'return']
ADDRESS_DOMAIN_IMPORTS['smp.zig:trampolineWriter'] = ['arch/x86_64/physical_layout.zig']
ADDRESS_DOMAIN_CALLS['pci.zig:initEcam'] = ['@This', '@as', '@errorName', '@import', 'acpi_tables.find', 'allocator', 'ecam_windows.install', 'log.write', 'log.writeFmt', 'map', 'mapBootMmioRange', 'parseMcfg']
ADDRESS_DOMAIN_CALLS['vtd.zig:prepareHardwareUnit'] = ['@as', '@import', '@intCast', '@intFromBool', '@max', '@truncate', 'cpuid', 'domainCount', 'fromInt', 'kernelMmioRangeFits', 'kernel_log.writeFmt', 'logInitFailure', 'mmio.kernelAfterValidation', 'mmioRead32', 'mmioRead64At', 'or', 'smp.ucMinusMmioAllowed', 'std.mem.alignForward', 'tableCacheLineBytes', 'unitAt']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:prepareHardwareUnit'] = ['arch/x86_64/platform.zig', 'arch/x86_64/platform.zig']

for _retired in ('identityPtePointer', 'mapKernelIdentityPage', 'visitRamCacheIdentityAliases'):
    PHYSICAL_ACCESS_CALLS.pop('arch/x86_64/platform.zig:' + _retired, None)

PHYSICAL_ACCESS_CALLS['arch/x86_64/platform.zig:bootMmioPageIsExplicitUc'] = ['kernelPtrPaddr', 'window.lookup', 'window.validate']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/platform.zig:bootMmioPageIsExplicitUc'] = []
PHYSICAL_ACCESS_CALLS['arch/x86_64/platform.zig:mapBootMmioRange'] = ['nonRamWindowRange', 'readMsr', 'window.map']
PHYSICAL_ACCESS_IMPORTS['arch/x86_64/platform.zig:mapBootMmioRange'] = []

ADDRESS_DOMAIN_CALLS.pop('memory/mmio.zig:kernelAfterValidation', None)

ADDRESS_DOMAIN_CALLS['amd_iommu.zig:prepareUnit'] = ['@as', '@errorName', '@import', '@intCast', '@max', '@memset', '@tagName', 'allocZeroPageForPreparation', 'definition.rawRules', 'definition.specials', 'deviceTableAt', 'initialDteWords', 'kernelMmioBorrow', 'list.popContiguousBelow', 'log.writeFmt', 'mmioRead64', 'pageAt', 'preparationFailure', 'rules.next', 'smp.explicitUcMmioStatus', 'smp.mmioCacheEvidenceMasks', 'specials.next', 'user_copy.zeroPhysicalPage']
ADDRESS_DOMAIN_IMPORTS['amd_iommu.zig:prepareUnit'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_CALLS['hpet.zig:initialize'] = ['@as', '@import', '@intCast', '@truncate', 'candidate.read', 'candidate.write', 'discover', 'kernelMmioBorrow', 'mmio_allowed', 'period', 'registerRange', 'volatile']
ADDRESS_DOMAIN_IMPORTS['hpet.zig:initialize'] = ['arch/x86_64/platform.zig', 'scheduler_connection.zig']
ADDRESS_DOMAIN_CALLS['lapic.zig:enableLocalApic'] = ['@import', 'kernelMmioBorrow', 'kvmX2ApicSupported', 'mapBootMmioRange', 'or', 'rdmsr', 'registerMsr', 'writeRegister', 'wrmsr']
ADDRESS_DOMAIN_IMPORTS['lapic.zig:enableLocalApic'] = ['arch/x86_64/platform.zig', 'arch/x86_64/platform.zig']
ADDRESS_DOMAIN_CALLS['pci.zig:readConfig'] = ['@import', '@intCast', 'configAccessValid', 'ecamAddress', 'kernelMmioBorrow', 'lockConfig', 'readEcam', 'readLegacyConfig', 'unlockConfig']
ADDRESS_DOMAIN_IMPORTS['pci.zig:readConfig'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_CALLS['pci.zig:writeConfig'] = ['@import', '@intCast', 'configAccessValid', 'ecamAddress', 'kernelMmioBorrow', 'lockConfig', 'unlockConfig', 'writeEcam', 'writeLegacyConfig']
ADDRESS_DOMAIN_IMPORTS['pci.zig:writeConfig'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_CALLS['vtd.zig:prepareHardwareUnit'] = ['@as', '@import', '@intCast', '@intFromBool', '@max', '@truncate', 'cpuid', 'domainCount', 'kernelMmioBorrow', 'kernelMmioRangeFits', 'kernel_log.writeFmt', 'logInitFailure', 'mapBootMmioRange', 'mmioRead32', 'mmioRead64At', 'or', 'smp.ucMinusMmioAllowed', 'std.mem.alignForward', 'tableCacheLineBytes', 'unitAt']
ADDRESS_DOMAIN_IMPORTS['vtd.zig:prepareHardwareUnit'] = ['arch/x86_64/platform.zig'] * 6

# Stage 8 B: topology/runtime storage owners; body guards remain pinned.
del ADDRESS_DOMAIN_CALLS['smp.zig:collectMadtLapicIds']
del ADDRESS_DOMAIN_IMPORTS['smp.zig:collectMadtLapicIds']
ADDRESS_DOMAIN_CALLS['smp.zig:prepareBootInfo'] = ['@import', 'appendLapicId', 'entries.next', 'lapic.localApicId', 'lapic.usesX2Apic', 'madtEntries', 'runtime.alloc', 'std.math.add', 'topology.append', 'topology.decode']
ADDRESS_DOMAIN_IMPORTS['smp.zig:prepareBootInfo'] = ['acpi_tables.zig', 'acpi_tables.zig']
ADDRESS_DOMAIN_CALLS['user_copy.zig:mapPhysWindow'] = ['@as', '@import', '@intCast', 'assertOneWayPerPage', 'getHooks', 'h.invlpg', 'publishWindow', 'ramCachePolicyForLockedPage', 'wcPhysicalWindowLeaseBlocks', 'windowEntry', 'windowSlotVa']
ADDRESS_DOMAIN_IMPORTS['user_copy.zig:mapPhysWindow'] = ['builtin']
ADDRESS_DOMAIN_INVARIANTS['user_copy.zig:mapPhysWindow'] = '3b22e7aeaf60d038a01d12df9559574fbe77460b46cce3d4053b9764755091f6'
ADDRESS_DOMAIN_CALLS['user_copy.zig:windowEntry'] = ['@import', 'getHooks', 'perCpuWindowEntry']
ADDRESS_DOMAIN_IMPORTS['user_copy.zig:windowEntry'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_INVARIANTS['user_copy.zig:windowEntry'] = '78d1472bba3b8d482667dc28ad767603ed7fa48d1fccda6ac711fbc428f82a64'
ADDRESS_DOMAIN_CALLS['user_copy.zig:windowSlotVa'] = ['@as', '@import', '@intCast', 'getHooks', 'perCpuWindowVa']
ADDRESS_DOMAIN_IMPORTS['user_copy.zig:windowSlotVa'] = ['arch/x86_64/platform.zig']
ADDRESS_DOMAIN_INVARIANTS['user_copy.zig:windowSlotVa'] = 'aaf7ef228de552be3c27f6e794b7245c35b316caef1b6d943af64205c269e6d0'
PHYSICAL_ACCESS_CALLS['arch/x86_64/platform.zig:allocateApRuntimeStacks'] = ['@alignCast', '@memset', 'cpu_runtime.map', 'cpu_stacks.stackBase', 'free_list.popContiguousBelow', 'fromInt', 'kernelHighMappingActive', 'paintUnusedStack', 'physical_layout.identitySlice']
ADDRESS_DOMAIN_CALLS['amd_iommu.zig:prepareUnit'] = [name.replace('smp.mmioCacheEvidenceMasks', 'smp.mmioCacheEvidenceCounts') for name in ADDRESS_DOMAIN_CALLS['amd_iommu.zig:prepareUnit']]
ADDRESS_DOMAIN_BINDINGS['smp.zig:import:runtime'] = 'arch/x86_64/cpu_runtime.zig'
ADDRESS_DOMAIN_BINDINGS['smp.zig:import:topology'] = 'cpu_topology.zig'

# Runtime-island translation is an architecture-owned page walk, not linear PA arithmetic.
ADDRESS_DOMAIN_CALLS['arch/x86_64/cpu_runtime.zig:physicalWith'] = ['@as', '@atomicLoad', 'Reader.pdp', 'Reader.rootLinked', 'Reader.table', 'return']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/cpu_runtime.zig:physicalWith'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/cpu_runtime.zig:physicalWith'] = '3e7c884ff5d74a820d0776ed7a9dbf134172cfc8bc42a88e0bd30a3821dcca0d'
ADDRESS_DOMAIN_CALLS['arch/x86_64/cpu_runtime.zig:physicalAddress'] = ['address.toInt', 'fromInt', 'physicalWith']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/cpu_runtime.zig:physicalAddress'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/cpu_runtime.zig:physicalAddress'] = 'c00ceda2f2174bfa96ec376de1d67aaa3c3c2f2ff17712a6ca70b6a5d522c0ef'
ADDRESS_DOMAIN_CALLS['arch/x86_64/cpu_runtime.zig:rootLinked'] = ['@as', '@atomicLoad', '@intFromPtr', 'fromInt', 'linked.toInt', 'physical.kernelAddressToPhysical', 'platform.runtimePdp', 'platform.runtimeRoot', 'return']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/cpu_runtime.zig:rootLinked'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/cpu_runtime.zig:rootLinked'] = '25c5e336a4900e72b4000b27cfa73b389391d0a63624c833a06b615894724233'
ADDRESS_DOMAIN_CALLS['arch/x86_64/cpu_runtime.zig:pdp'] = ['platform.runtimePdp']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/cpu_runtime.zig:pdp'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/cpu_runtime.zig:pdp'] = 'be781a8d77e98529b80c4e0d7b7e2728711b3dfa78ee7d9b9936681bc7d58312'
ADDRESS_DOMAIN_CALLS['arch/x86_64/cpu_runtime.zig:table'] = ['fromInt', 'physical.tablePointer']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/cpu_runtime.zig:table'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/cpu_runtime.zig:table'] = 'c9ac59d3a3c803f281a128eaf9f71f2b6bcdca43c4e36d6f46b80199f02532cd'
ADDRESS_DOMAIN_CALLS['arch/x86_64/platform.zig:kernelAddressToPhysical'] = ['address.toInt', 'boot_direct_map.scratchPhysical', 'cpu_runtime.physicalAddress', 'fromInt', 'physical_layout.kernelAddressToPhysical']
ADDRESS_DOMAIN_IMPORTS['arch/x86_64/platform.zig:kernelAddressToPhysical'] = []
ADDRESS_DOMAIN_INVARIANTS['arch/x86_64/platform.zig:kernelAddressToPhysical'] = '0b63d1f42cc22b7dfaadeb5b31152f93e4d84d5cee953752ce6574099c3c173f'
ADDRESS_DOMAIN_BINDINGS['arch/x86_64/cpu_runtime.zig:import:physical'] = 'physical_layout.zig'
ADDRESS_DOMAIN_BINDINGS['arch/x86_64/cpu_runtime.zig:import:platform'] = 'platform.zig'
ADDRESS_DOMAIN_BINDINGS['arch/x86_64/cpu_runtime.zig:import:image'] = '../../boot/image_range.zig'

# B2: observation only; preserve the PTE/cache proof and track its two timing calls.
ADDRESS_DOMAIN_CALLS['user_copy.zig:mapPhysWindow'] += ['copy_profile.copyTimestamp', 'copy_profile.copyEnd']
ADDRESS_DOMAIN_BINDINGS['user_copy.zig:import:copy_profile'] = 'lock_profile.zig'
ADDRESS_DOMAIN_INVARIANTS['user_copy.zig:mapPhysWindow'] = 'd8c30561c3ff0eea39539e3e7da903843dceb793719e696c33b8679cc5cb4c8c'

def check_address_domains(sources):
    import hashlib
    errors = []
    for key, allowed in ADDRESS_DOMAIN_CALLS.items():
        path, name = key.split(':')
        try:
            raw = raw_function_body(sources[path], name)
        except ValueError as error:
            errors.append(key + ': ' + str(error))
            continue
        body = code_only(raw)
        if call_names(body) != set(allowed):
            errors.append(key + ': unreviewed call/alias or missing boundary')
        if assembly_blocks(raw) != ADDRESS_DOMAIN_ASM.get(key, []):
            errors.append(key + ': unreviewed assembly')
        if re.findall(r'@import\("([^"]+)"\)', raw) != ADDRESS_DOMAIN_IMPORTS[key]:
            errors.append(key + ': unreviewed inline import')
        if key in ADDRESS_DOMAIN_INVARIANTS:
            # Pin the existing window proof and PTE/hit/miss protocol, and the
            # tiny conversion owners. Comments/whitespace are not contracts.
            digest = hashlib.sha256(re.sub(r'\s+', '', body).encode()).hexdigest()
            if digest != ADDRESS_DOMAIN_INVARIANTS[key]:
                errors.append(key + ': changed mapping/fixture invariant')
        for callee in allowed:
            root = callee.split('.')[0]
            if root.startswith('@'): continue
            if re.search(r'\b(?:const|var)\s+' + re.escape(root) + r'\s*=\s*[\w.]+\s*;', body):
                errors.append(key + ': callable alias rebinding')
    for key, target in ADDRESS_DOMAIN_BINDINGS.items():
        path, _, alias = key.split(':')
        if f'const {alias} = @import("{target}");' not in sources[path]:
            errors.append(key + ': owner import rebound')
    # Register publication boundary: cache evidence alone does not install a VA.
    vtd = code_only(raw_function_body(sources['vtd.zig'], 'prepareHardwareUnit'))
    for extent in ('page_size', 'register_bytes'):
        mapping = 'mapBootMmioRange(drhd.register_base, ' + extent + ')'
        borrow = 'kernelMmioBorrow(drhd.register_base, ' + extent + ')'
        if mapping not in vtd or borrow not in vtd or vtd.index(mapping) > vtd.index(borrow):
            errors.append('vtd: register borrow precedes high mapping publication')
    for path in ('iommu.zig', 'vtd.zig', 'amd_iommu.zig'):
        for name in ('mapPages', 'reserveIova', 'freeIova', 'unmapRangeForDevice'):
            declaration = re.search(r'fn ' + name + r'\([^)]*\)', sources[path], re.S)
            if not declaration or 'IovaAddress' not in declaration[0]:
                errors.append(path + ':' + name + ': missing IOVA type')
    for name in ('readEcam', 'writeEcam'):
        if not re.search(r'fn ' + name + r'\(borrow: \*const mmio.Borrow,', sources['pci.zig']):
            errors.append('pci:' + name + ': missing MMIO borrow type')
    smp = without_tests(code_only(sources['smp.zig']))
    for forbidden in ('findMadtFromRoot', 'findMadt', 'tableLength', 'bytesEqual'):
        if re.search(r'\b' + forbidden + r'\s*\(', smp):
            errors.append('smp: raw firmware traversal reintroduced')
    stack = code_only(raw_function_body(sources['arch/x86_64/platform.zig'], 'allocateApRuntimeStacks'))
    for required in ('cpu_slot >= tss_tables.len', 'kernelHighMappingActive()',
                     'free_list.popContiguousBelow(cpu_stacks.backing_bytes / 4096, physical_layout.allocation_limit)',
                     'physical_layout.identitySlice(u8, .fromInt(backing), cpu_stacks.backing_bytes)',
                     'for (cpu_stacks.sizes, 0..)', 'cpu_stacks.stackBase(cpu_slot, stack)',
                     'while (off < bytes)', 'cpu_runtime.map(va + off, phys) catch return false'):
        if required not in stack:
            errors.append('runtime stacks: owned backing/usable-range boundary missing: ' + required)
    return errors


def self_test_u6b(sources):
    assert not check_address_domains(sources), check_address_domains(sources)
    mutations = [
        ('runtime converter integration omitted', 'arch/x86_64/platform.zig', 'if (cpu_runtime.physicalAddress(address)) |backing| return backing;', ''),
        ('runtime converter accepts foreign root', 'arch/x86_64/cpu_runtime.zig', 'or !Reader.rootLinked()', ''),
        ('runtime converter accepts unknown PDP', 'arch/x86_64/cpu_runtime.zig', 'root & ~@as(u64, 32) != owned_roots[root_index]', 'false'),
        ('runtime converter accepts user leaf', 'arch/x86_64/cpu_runtime.zig', 'or entry & 4 != 0', ''),
        ('runtime converter assumes contiguous backing', 'arch/x86_64/cpu_runtime.zig', 'return (entry & address_mask) | (va & 4095);', 'return (entry & address_mask) + (va - low);'),
        ('runtime stack physical bound', 'arch/x86_64/platform.zig', 'free_list.popContiguousBelow(cpu_stacks.backing_bytes / 4096, physical_layout.allocation_limit)', 'free_list.popContiguousBelow(cpu_stacks.backing_bytes / 4096, 0xffffffffffffffff)'),
        ('runtime stack guard hole', 'arch/x86_64/platform.zig', 'while (off < bytes)', 'while (off < cpu_stacks.stride)'),
        ('runtime copy window fixture escape', 'user_copy.zig', 'if (!builtin.is_test) return @import("arch/x86_64/platform.zig").perCpuWindowEntry', 'if (false) return @import("arch/x86_64/platform.zig").perCpuWindowEntry'),
        ('runtime copy window CPU bound', 'user_copy.zig', 'if (cpu_slot >= window_victim.len) return null;', ''),
        ('runtime copy window type policy', 'user_copy.zig', 'if (cache != .device and wcPhysicalWindowLeaseBlocks(page_base)) return null;', ''),
        ('3 IOVA type exchange', 'iommu.zig', 'iova: IovaAddress', 'iova: u64'),
        ('7 fixture runtime escape', 'acpi_tables.zig', 'if (@import("builtin").is_test)', 'if (true)'),
        ('7 HHDM bypass', 'user_copy.zig', 'return page + offset;', 'return hhdmSlice(addr, offset);'),
        ('MMIO accessor RAM physical', 'pci.zig', 'borrow: *const mmio.Borrow', 'borrow: mmio.PhysicalAddress'),
        ('firmware raw traversal', 'smp.zig', 'var entries = @import("acpi_tables.zig").madtEntries(rsdp)', 'var entries = findMadtFromRoot(rsdp, true)'),
        ('window raw helper', 'user_copy.zig', 'return page + offset;', 'return rawPointer(addr);'),
        ('window alias', 'user_copy.zig', 'return page + offset;', 'const alias = rawPointer; return alias(addr);'),
        ('missing target', 'user_copy.zig', 'fn physWindowAddr(', 'fn removedWindowAddr('),
        ('MMIO raw conversion', 'vtd.zig', 'return unit.registers.read(u32, offset);', 'return @as(*volatile u32, @ptrFromInt(offset)).*;'),
        ('MMIO mapping publication omitted', 'vtd.zig', 'mapBootMmioRange(drhd.register_base, register_bytes)', 'mapBootMmioRange(drhd.register_base, page_size)'),
    ]
    for label, path, old, new in mutations:
        assert old in sources[path], label
        changed = dict(sources)
        changed[path] = changed[path].replace(old, new, 1)
        assert check_address_domains(changed), label
        print('address domains injection PASS:', label)

def check(sources):
    errors = []
    for path in ('state/process.zig', 'scheduler_connection.zig'):
        body = function_body(code_only(sources[path]), 'allocKernelSlice')
        if '@ptrFromInt' in body or 'identitySlice(' not in body:
            errors.append(path + ': allocation must use the physical mapping boundary')
    address_space = without_tests(code_only(sources['memory/address_space.zig']))
    if 'allocKernelSlice' in address_space or 'popContiguous' in address_space:
        errors.append('address_space: owned identity allocator bypassed')
    for name in ('ensureCapacity', 'ensureSlot'):
        body = function_body(address_space, name)
        if 'identity_allocator.allocator(free_list)' not in body or 'transaction.reserve(' not in body:
            errors.append('address_space.' + name + ': owned identity allocator missing')
    vm = code_only(sources['memory/user_vm.zig'])
    # Host allocators use synthetic physical grants backed by host pointers.
    # Only a lexical test block may perform that reverse mapping; runtime
    # helpers outside those blocks remain covered by this prohibition.
    if '@ptrFromInt' in without_tests(vm):
        errors.append('user_vm: raw physical pointer conversion bypasses the table borrow boundary')
    # The masked strings above include imports; only signatures/code count.
    for signature in (
        r'kernel_pointer_paddr:\s*\*const fn \(addresses.KernelVirtualAddress\) \?addresses.PhysicalAddress',
    ):
        if not re.search(signature, vm):
            errors.append('user_vm: typed table hook contract missing')
    runtime_vm = without_tests(vm)
    configure = function_body(runtime_vm, 'configureHooks')
    initialize = function_body(runtime_vm, 'init')
    lookup = function_body(runtime_vm, 'lookupUserPhysicalPageWithAddressSpaceLocked')
    if ('updateUserSpaces' in runtime_vm or
        not re.search(r'if\s*\(!allow_fixture_replacement and hooks != null\)', configure) or
        'error.AlreadyInitialized' not in configure or
        'configureHooks(new_hooks, builtin.is_test)' not in initialize or
        not re.search(r'const h:\s*\*const Hooks', lookup)):
        errors.append('user_vm: boot-only immutable configuration borrow contract missing')
    outside_install = runtime_vm.replace(configure, '')
    if re.search(r'\bhooks\s*=', outside_install) or re.search(r'if\s*\(hooks\)\s*\|\*', outside_install.replace(lookup, '')):
        errors.append('user_vm: configuration writer bypasses boot initialization')
    copy = code_only(sources['user_copy.zig'])
    if 'lookupUserMappedPaddrForAccessWithAddressSpaceLocked' in copy:
        errors.append('user_copy: permission lookup must use the typed user/physical boundary')
    contracts = {
        'copyFromUser': 'profileResolve',
        'copyToUser': 'profileResolve',
        'copyUserWordBytesProfiled': 'profileResolve',
        'profileResolve': 'ensureUserPageMappedForCopyWithAddressSpaceLocked',
        'ensureUserPageMappedForCopyWithAddressSpaceLocked': 'resolveCopyPhysicalPageWithAddressSpaceLocked',
        'resolveCopyPhysicalPageWithAddressSpaceLocked': 'lookupUserPhysicalPageWithAddressSpaceLocked',
    }
    for name, required in contracts.items():
        body = function_body(copy, name)
        if '@ptrFromInt' in body or required not in body:
            errors.append('user_copy.' + name + ': typed lookup/window borrow boundary bypassed')
    return errors + check_physical_access(sources) + check_address_domains(sources)


def self_test(sources):
    assert not without_tests(code_only(r'''test "literal" { const chars = [_]u8{ '\\', '{', '}' }; }''')).strip()
    assert function_body('fn sample(x: error{Input}!void) error{Failure}!void { return; }', 'sample').strip() == 'return;'
    assert not check(sources)
    for path in ('state/process.zig', 'scheduler_connection.zig'):
        mutated = dict(sources)
        mutated[path] = mutated[path].replace('const raw =', 'const bypass: [*]u8 = @ptrFromInt(paddr);\n    const raw =', 1)
        assert check(mutated), path
    for replacement in ('raw_allocator(free_list)', 'allocKernelSlice(free_list)'):
        mutated = dict(sources)
        mutated['memory/address_space.zig'] = mutated['memory/address_space.zig'].replace('identity_allocator.allocator(free_list)', replacement)
        assert check(mutated)
    mutated = dict(sources)
    mutated['memory/user_vm.zig'] += '\nfn bypass(p: u64) *u64 { return @ptrFromInt(p); }\n'
    assert check(mutated)
    for bypass in (
        '\nfn replace() void { hooks = null; }\n',
        '\nfn replace() void { if (hooks) |*h| h.user_spaces = undefined; }\n',
    ):
        mutated = dict(sources)
        mutated['memory/user_vm.zig'] += bypass
        assert check(mutated)
    mutated = dict(sources)
    mutated['memory/user_vm.zig'] = mutated['memory/user_vm.zig'].replace(
        '!allow_fixture_replacement and hooks != null', 'false')
    assert check(mutated)
    mutated = dict(sources)
    mutated['memory/user_vm.zig'] = mutated['memory/user_vm.zig'].replace(
        'fn (addresses.KernelVirtualAddress) ?addresses.PhysicalAddress', 'fn (usize) ?u64')
    assert check(mutated)
    mutated = dict(sources)
    mutated['user_copy.zig'] += '\nfn bypass() void { lookupUserMappedPaddrForAccessWithAddressSpaceLocked(); }\n'
    assert check(mutated)
    for name in ('copyFromUser', 'copyToUser', 'copyUserWordBytesProfiled',
                 'ensureUserPageMappedForCopyWithAddressSpaceLocked',
                 'resolveCopyPhysicalPageWithAddressSpaceLocked'):
        mutated = dict(sources)
        text = mutated['user_copy.zig']
        match = re.search(r'\bfn\s+' + name + r'\s*\(', text)
        start = text.index('{', match.end()) + 1
        mutated['user_copy.zig'] = text[:start] + '\nconst bypass: [*]u8 = @ptrFromInt(4096);\n' + text[start:]
        assert check(mutated), name
    # An explanation mentioning a forbidden builtin must not cause rejection.
    commented = dict(sources)
    commented['memory/user_vm.zig'] += '\n// @ptrFromInt is forbidden here.\n'
    assert not check(commented)


def compile_fixture(zig, fixture):
    return subprocess.run([
        zig, 'build-obj', '-fno-emit-bin', '--dep', 'addresses',
        '-Mroot=' + str(ROOT / 'tests' / fixture),
        '-Maddresses=' + str(ROOT / 'kernel/src/memory/addresses.zig'),
    ], cwd=ROOT / 'kernel', capture_output=True, text=True)


def main():
    names = ('state/process.zig', 'memory/address_space.zig', 'scheduler_connection.zig',
             'memory/user_vm.zig', 'user_copy.zig',
             *sorted({key.split(':')[0] for key in PHYSICAL_ACCESS_CALLS}),
             *sorted({key.split(':')[0] for key in ADDRESS_DOMAIN_CALLS}))
    sources = {name: (ROOT / 'kernel/src' / name).read_text() for name in names}
    self_test(sources)
    self_test_u6a(sources)
    self_test_u6b(sources)
    errors = check(sources)
    if errors:
        raise SystemExit('\n'.join(errors))
    zig = sys.argv[1] if len(sys.argv) > 1 else 'zig'
    accepted = compile_fixture(zig, 'address_domain_type_acceptance.zig')
    if accepted.returncode:
        raise SystemExit(accepted.stderr)
    rejected = compile_fixture(zig, 'address_domain_type_rejection.zig')
    if rejected.returncode == 0:
        raise SystemExit('address-domain misuse unexpectedly compiled')
    for marker in ("found 'addresses.Address(.user_virtual)'",
                   "found 'addresses.Address(.kernel_virtual)'",
                   "found 'addresses.Address(.physical)'",
                   'only kernel virtual addresses',
                   "found 'addresses.Address(.iova)'"):
        if marker not in rejected.stderr:
            raise SystemExit('negative fixture failed for the wrong reason: ' + rejected.stderr)
    for fixture, should_pass in (('mmio_borrow_type_acceptance.zig', True), ('mmio_borrow_type_rejection.zig', False)):
        result = subprocess.run([
            zig, 'build-obj', '-fno-emit-bin', '--dep', 'mmio',
            '-Mroot=' + str(ROOT / 'tests' / fixture),
            '-Mmmio=' + str(ROOT / 'kernel/src/memory/mmio.zig'),
        ], cwd=ROOT / 'kernel', capture_output=True, text=True)
        if should_pass != (result.returncode == 0):
            raise SystemExit('MMIO type fixture failed: ' + result.stderr)
        if not should_pass and ('expected type \'*const mmio.Borrow\'' not in result.stderr or '.physical)' not in result.stderr):
            raise SystemExit('MMIO rejection failed for wrong reason: ' + result.stderr)
    print('address domains compiler PASS: IOVA/physical exchange and RAM physical as MMIO rejected; borrowed access accepted')
    print('kernel address boundaries: migrated paths, bypass self-tests, positive/negative compiler fixtures PASS')


if __name__ == '__main__':
    main()
