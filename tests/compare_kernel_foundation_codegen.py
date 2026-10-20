#!/usr/bin/env python3
"""Compare selected real ELF bodies, normalizing relocated address operands."""
import difflib
import json
from pathlib import Path
import re
import subprocess
import sys


def bodies(path):
    sections = subprocess.check_output(['objdump', '-h', str(path)], text=True)
    data_ranges = []
    for line in sections.splitlines():
        record = re.match(r'\s*\d+\s+(\.(?:rodata|data|bss)\S*)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if record:
            begin = int(record[3], 16)
            data_ranges.append((begin, begin + int(record[2], 16)))
    output = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(path)], text=True)
    result = {}
    current = None
    start = 0
    for line in output.splitlines():
        header = re.match(r'^([0-9a-f]+) <(.+)>:$', line)
        if header:
            start = int(header[1], 16)
            current = []
            result[header[2]] = current
            continue
        instruction = re.match(r'^\s*([0-9a-f]+):\s*(.*)', line)
        if current is None or not instruction:
            continue
        body = instruction[2]
        body = re.sub(r'__anon_\d+', '', body)
        # Symbol-target relocations and PC-relative data addresses shift when
        # unrelated functions grow. Keep symbol names and intra-body offsets.
        body = re.sub(r'\b[0-9a-f]+ <([^>]+)>', lambda m: '<' + m[1] + '>', body)
        body = re.sub(r'[+-]?0x[0-9a-f]+\(%rip\)', 'RELOC(%rip)', body)
        body = re.sub(r'#\s*[0-9a-f]+', '# RELOC', body)
        def data_operand(match):
            value = int(match[1], 16)
            if any(begin <= value < end for begin, end in data_ranges):
                return '$DATA_RELOC'
            return match[0]
        body = re.sub(r'\$0x([0-9a-f]+)', data_operand, body)
        # Unsymbolized local branches still need their relative destinations.
        branch = re.match(r'(j\w+\s+)([0-9a-f]+)$', body)
        if branch:
            body = branch[1] + 'BODY+' + hex(int(branch[2], 16) - start)
        current.append(body)
    return result


def find(records, name):
    matches = [(key, value) for key, value in records.items() if key == name or key.startswith(name + '__anon_')]
    if len(matches) != 1:
        raise ValueError(f'{name}: expected one body, got {[key for key, _ in matches]}')
    return matches[0]


def main():
    arguments = list(sys.argv[1:])
    same_symbols = '--same-symbols' in arguments
    if same_symbols:
        arguments.remove('--same-symbols')
    extra_symbols = []
    while '--extra-symbol' in arguments:
        position = arguments.index('--extra-symbol')
        extra_symbols.append(arguments[position+1])
        del arguments[position:position+2]
    before, after, destination = map(Path, arguments)
    old, new = bodies(before), bodies(after)
    names = [
        ('scheduler_connection.switchToGenerationLockedWithRegisters',) * 2,
        ('scheduler_connection.handoffToReadyThreadGenerationWithRax',) * 2,
        ('user_copy.copyUserBytesFromVa',) * 2,
        ('user_copy.copyBytesToUserVa',) * 2,
        ('user_copy.ensureUserPageMappedForCopyWithAddressSpaceLocked',) * 2,
        ('user_copy.userPageCopyCache',) * 2,
        ('memory.user_vm.userPageIndexForVa',) * 2,
        ('memory.user_vm.lookupUserMappedPaddrForAccessWithAddressSpaceLocked',
         'memory.user_vm.lookupUserPhysicalPageWithAddressSpaceLocked'),
    ]
    if same_symbols:
        names[-1] = (names[-1][1],) * 2
    names.extend((symbol, symbol) for symbol in extra_symbols)
    rows = []
    for old_name, new_name in names:
        old_key, old_body = find(old, old_name)
        new_key, new_body = find(new, new_name)
        # Renamed typed implementation does not itself add a machine call.
        old_body = [value.replace(names[-1][0], names[-1][1]) for value in old_body]
        rows.append(dict(before_symbol=old_key, after_symbol=new_key,
                         before_instructions=len(old_body), after_instructions=len(new_body),
                         identical_normalized=old_body == new_body,
                         before_calls=[v for v in old_body if v.startswith('call')],
                         after_calls=[v for v in new_body if v.startswith('call')],
                         diff=list(difflib.unified_diff(old_body, new_body, fromfile=old_key, tofile=new_key))))
    destination.write_text(json.dumps(dict(before=str(before), after=str(after),
        caveat='Normalized assembly comparison, not a latency measurement; symbol relocations ignored, actual branches/calls retained.',
        functions=rows), indent=2) + '\n')
    for row in rows:
        print(row['after_symbol'], row['before_instructions'], row['after_instructions'],
              'same' if row['identical_normalized'] else 'changed')


if __name__ == '__main__':
    main()
