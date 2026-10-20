#!/usr/bin/env python3
"""Enumerate foundation evidence; extracted candidates are NOT reviewed caps."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
from check_kernel_address_boundaries import code_only

ROOT = Path(__file__).resolve().parents[1]


def subsystem(path):
    if path.startswith('kernel/abi/'):
        return 'abi'
    if path.startswith('verified/scheduling/'):
        return 'verified-scheduling'
    if path.startswith('bootloader/') or '/boot/' in path:
        return 'boot'
    if '/arch/' in path:
        return 'architecture'
    if '/memory/' in path:
        return 'memory'
    if '/sync/' in path:
        return 'synchronization'
    if '/state/' in path:
        return 'state-' + Path(path).stem
    if '/syscall/' in path:
        return 'syscall-' + Path(path).stem
    name = Path(path).stem
    if name.endswith(('_test', '_test_runner')):
        return 'test-fixture'
    if name in ('cpu_set', 'cpu_topology', 'tlb_residency') or name.startswith(('scheduler', 'verified_sched', 'smp', 'futex')):
        return 'scheduler-smp'
    if name in ('user_copy', 'traps', 'interrupts', 'page_fault_log', 'syscalls'):
        return 'entry-exception-user-access'
    if name in ('acpi_dmar', 'acpi_ivrs', 'amd_iommu', 'iommu', 'iova', 'vtd', 'vtd_tables'):
        return 'dma-iommu'
    if name in ('acpi_tables', 'acpi_power', 'pci', 'hpet', 'lapic', 'rtc', 'serial'):
        return 'hardware'
    if name in ('clockevent', 'realtime_clock'):
        return 'time'
    if name == 'xstate_runtime':
        return 'thread-xstate-transition'
    if name in ('ipc_metric', 'kernel_log', 'log_util', 'halt', 'lock_profile', 'diagnostic_cpu_storage'):
        return 'diagnostic'
    if name in ('kernel', 'kernel_runtime', 'bootloader_api', 'elf_loader', 'capsule'):
        return 'composition-resources'
    return 'unclassified'


def thread_storage_references(text):
    """Lexical leads for the lifetime audit, not a lock/alias proof.

    Keep declarations and test/helper uses: dropping them syntactically can
    conceal a function pointer escape or a helper invoked by production.
    Assembly/string references are reported separately instead of discarded.
    """
    masked = code_only(text)
    lines = text.splitlines()
    pattern = re.compile(
        r'\b(?:threadContextMutable|threadContext|thread_contexts_ptr|'
        r'initial_thread_contexts|initial_context_pointers|threadDirectory|x_state_addr)\b|'
        r'\bthread_table\s*\.\s*(?:directory|contexts|hot_states|signal_timers)\b')
    rows = []
    for match in pattern.finditer(text):
        line = text.count('\n', 0, match.start()) + 1
        # Comments are not code, but unlike a call-count grep they remain
        # visible as non-code evidence, including assembly strings.
        rows.append(dict(line=line, reference=match[0],
                         kind='code' if masked[match.start():match.end()].strip() else 'non-code',
                         declaration=lines[line - 1].strip(),
                         review_status='candidate-unreviewed',
                         exclusion=None, borrow_end=None))
    return rows


def scan(path):
    text = path.read_text(errors='replace')
    relative = path.relative_to(ROOT).as_posix()
    imports = []
    limits = []
    arrays = []
    conversions = []
    locks = []
    for line, source in enumerate(text.splitlines(), 1):
        # Strip ordinary trailing comments, retaining declaration evidence.
        code = source.split('//', 1)[0]
        for value in re.findall(r'@import\("([^"\n]+)"\)', code):
            target = (path.parent / value).resolve()
            resolved = target.relative_to(ROOT).as_posix() if target.is_file() and target.is_relative_to(ROOT) else None
            imports.append(dict(line=line, import_name=value, resolved_file=resolved))
        declaration = re.search(r'\b(?:pub\s+)?const\s+(\w+)\s*(?::\s*([^=]+))?=\s*(.*)', code)
        if declaration and re.search(r'max_|_max|capacity|_limit|_count|_entries|_slots|_bytes|_ways|_banks|bootstrap_cpu_rows', declaration[1]):
            limits.append(dict(line=line, name=declaration[1], declaration=source.strip(),
                               expression=declaration[3].rstrip(';').strip(),
                               review_status='candidate-unreviewed', bound_reason=None,
                               id_representation=None, reference_lifetime=None,
                               growth_boundary=None, removal_stage=None))
        for extent in re.findall(r'\[([^\]\n]+)\]', code):
            if extent not in ('*', '*c', ':0') and not extent.startswith(('0..', 'index', 'slot')):
                arrays.append(dict(line=line, expression=extent, declaration=source.strip(), review_status='candidate-unreviewed'))
        if '@ptrFromInt' in code or '@intFromPtr' in code:
            conversions.append(dict(line=line, declaration=source.strip(), review_status='candidate-unreviewed'))
        if re.search(r'\b(?:lock|unlock|tryLock|lockState|unlockState)\s*\(', code) or 'lock_word' in code:
            locks.append(dict(line=line, declaration=source.strip(), review_status='candidate-unreviewed'))
    return dict(path=relative, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                subsystem=subsystem(relative), review_status='enumerated-not-fully-reviewed',
                imports=imports, bound_candidates=limits, array_candidates=arrays,
                address_conversions=conversions, lock_evidence=locks,
                thread_storage_references=thread_storage_references(text) if path.suffix == '.zig' else [])


def main():
    paths = set()
    for directory in ('kernel/src', 'kernel/abi', 'bootloader/limine', 'verified/scheduling'):
        for path in (ROOT / directory).rglob('*'):
            if path.is_file() and path.suffix in ('.zig', '.c', '.h', '.S', '.s', '.ld', '.v'):
                paths.add(path)
    records = [scan(path) for path in sorted(paths)]
    counts = {}
    for record in records:
        counts[record['subsystem']] = counts.get(record['subsystem'], 0) + 1
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / '.artifacts/kernel-refactor-stage1/foundation-inventory.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(dict(
        utc=datetime.now(timezone.utc).isoformat(),
        coverage='all source files in the four named roots; lexical candidates, NOT a complete semantic bound audit; generated resources/named build imports need separate review',
        subsystem_file_counts=counts, records=records,
    ), ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(output=str(output), files=len(records), groups=counts,
                         bound_candidates=sum(len(r['bound_candidates']) for r in records),
                         unclassified=[r['path'] for r in records if r['subsystem']=='unclassified']), ensure_ascii=False))


if __name__ == '__main__':
    main()
