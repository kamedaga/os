"""Validate a paused, sealed boot arena; not a concurrent-memory validator."""


def sealed_records(used, capacity, header_bytes, persistent_value, read_header):
    if not 0 <= used <= capacity or header_bytes <= 0:
        raise ValueError('scratch extent outside buffer')
    records = []
    cursor, previous, last_id = 0, 2**64 - 1, 0
    while cursor < used:
        if cursor % 8 or cursor + header_bytes > used:
            raise ValueError('scratch header outside live extent')
        header = read_header(cursor)
        end, length = header['end'], header['length']
        if (end <= cursor + header_bytes or end > used or end % 8 or
                not 0 < length <= end - cursor - header_bytes or
                header['previous'] != previous or header['id'] <= last_id):
            raise ValueError('invalid scratch record chain')
        if (header['lifetime'] != persistent_value or header['published'] != 1 or
                header['borrowed'] != 0):
            raise ValueError('sealed scratch retains temporary/draft/borrow')
        records.append(dict(offset=cursor, **header))
        previous, last_id, cursor = cursor, header['id'], end
    return records
