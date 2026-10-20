"""Add exact-ELF SMP counter deltas at existing stopped profile boundaries."""
import json
import struct


def totals(data, cpus=None):
    if cpus is None:
        cpus = len(data) // (16 * 8)
    if cpus <= 0 or len(data) != cpus * 16 * 8:
        raise ValueError('unexpected smp_perf counter extent')
    words = struct.unpack('<' + str(cpus * 16) + 'Q', data)
    return [sum(words[cpu * 16 + column] for cpu in range(cpus)) for column in range(16)]


def attach(profile, output):
    if getattr(profile, '_tlb_attached', False):
        return
    original_reset, original_decode = profile.reset_stopped, profile.decode
    before = [None]
    sequence = [0]

    def read(label):
        return totals(profile.read('smp_perf_counters', label), profile.cpus if getattr(profile, 'dynamic', False) else None)

    def reset():
        original_reset()
        sequence[0] += 1
        before[0] = read('smp-begin-' + str(sequence[0]))

    def decode(raw):
        result = original_decode(raw)
        after = read('smp-end-' + str(sequence[0]))
        if before[0] is not None:
            delta = [a - b for a, b in zip(after, before[0])]
            if any(value < 0 for value in delta):
                raise ValueError('SMP counters moved backwards')
            result['smp_perf'] = delta
        return result

    # The first checkpoint includes boot and full, explicitly cumulative. It
    # does not pretend to be a full-window delta when attached after that run.
    profile.stop_clean()
    try:
        output.write_text(json.dumps(dict(scope='boot plus full cumulative', smp_perf=read('smp-after-full')), indent=2) + '\n')
    finally:
        profile.qmp.execute('cont')
    profile.reset_stopped, profile.decode = reset, decode
    profile._tlb_attached = True
