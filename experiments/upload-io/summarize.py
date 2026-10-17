#!/usr/bin/env python3
"""Summarize a complete AMULE_UPLOAD_IO_PROFILE CSV capture."""
import argparse
import csv
import math

FIELDS = ['kind', 'elapsed_ns', 'duration_ns', 'bytes', 'success', 'part', 'mmap']
FORMAT = ['amule-upload-io', '2']
MAX_SAMPLES_PER_LOOP = 4096


def complete_lines(stream):
    for line in stream:
        if not line.endswith('\n'):
            raise ValueError('truncated CSV line; capture is incomplete')
        yield line


def summarize(stream):
    reader = csv.reader(complete_lines(stream), strict=True)
    try:
        if next(reader, None) != FORMAT:
            raise ValueError('unsupported capture format version; recollect with the current profiler')
        if next(reader, None) != FIELDS:
            raise ValueError('unexpected CSV header')
        durations = []
        read_ns = active_ns = byte_count = failed = loops = previous_loop = 0
        pending_time = pending_elapsed = 0
        pending_reads = 0
        modes = set()
        ended = False
        wall_ns = 0
        for row in reader:
            if ended:
                raise ValueError('records after completion marker')
            if len(row) != len(FIELDS):
                raise ValueError('incorrect CSV field count')
            kind = row[0]
            # int() also accepts whitespace, signs, underscores and Unicode digits,
            # none of which the collector emits. Reject damaged fields rather than
            # silently converting them into plausible measurements.
            if any(not field or not field.isascii() or not field.isdecimal()
                   for field in row[1:]):
                raise ValueError('invalid unsigned decimal field')
            elapsed, duration, size, success, part, mmap = map(int, row[1:])
            if not (0 <= elapsed <= 2**63 - 1 and 0 <= duration <= 2**63 - 1
                    and 0 <= size <= 2**64 - 1):
                raise ValueError('time or byte count outside collector integer range')
            if success not in (0, 1) or part not in (0, 1) or mmap not in (0, 1):
                raise ValueError('invalid boolean flag')
            if kind == 'read':
                pending_reads += 1
                if pending_reads > MAX_SAMPLES_PER_LOOP:
                    raise ValueError('read sample limit exceeded in one loop')
                if elapsed <= previous_loop or (pending_elapsed and elapsed != pending_elapsed):
                    raise ValueError('read timestamp does not match its loop')
                if duration > elapsed - previous_loop:
                    raise ValueError('read duration exceeds wall interval')
                pending_elapsed = elapsed
                pending_time += duration
                modes.add(mmap)
                if success:
                    durations.append(duration)
                    byte_count += size
                else:
                    failed += 1
            elif kind == 'loop':
                if size or success or part or mmap:
                    raise ValueError('nonzero loop placeholder')
                if elapsed <= previous_loop or duration > elapsed - previous_loop:
                    raise ValueError('invalid loop wall interval')
                if pending_elapsed and pending_elapsed != elapsed:
                    raise ValueError('read timestamp does not match its loop')
                if pending_time > duration:
                    raise ValueError('read time exceeds active loop time')
                active_ns += duration
                read_ns += pending_time
                pending_time = pending_elapsed = 0
                pending_reads = 0
                previous_loop = elapsed
                loops += 1
            elif kind == 'end':
                if duration or size or success or part or mmap:
                    raise ValueError('nonzero completion placeholder')
                if pending_elapsed or elapsed < previous_loop:
                    raise ValueError('incomplete final loop or invalid completion timestamp')
                wall_ns = elapsed
                ended = True
            else:
                raise ValueError('unknown record kind')
    except csv.Error as error:
        raise ValueError(f'invalid CSV: {error}') from error
    if not ended:
        raise ValueError('missing completion marker; capture failed or aMule did not stop normally')
    if len(modes) > 1:
        raise ValueError('keep mmap mode fixed throughout each baseline run')
    if not loops or not wall_ns or not durations:
        raise ValueError('no successful reads in completed loops; collect an upload workload first')
    durations.sort()
    seconds = wall_ns / 1e9
    return {
        'mmap_enabled': bool(next(iter(modes))),
        'successful_reads': len(durations),
        'failed_or_deferred_reads': failed,
        'wall_seconds': seconds,
        'p50_ms': durations[math.ceil(len(durations) * .50) - 1] / 1e6,
        'p90_ms': durations[math.ceil(len(durations) * .90) - 1] / 1e6,
        'p95_ms': durations[math.ceil(len(durations) * .95) - 1] / 1e6,
        'p99_ms': durations[math.ceil(len(durations) * .99) - 1] / 1e6,
        'reads_per_second': len(durations) / seconds,
        'bytes_per_second': byte_count / seconds,
        'read_share_active_percent': 100 * read_ns / active_ns if active_ns else 0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv_path')
    args = parser.parse_args()
    try:
        with open(args.csv_path, newline='') as stream:
            result = summarize(stream)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    for key, value in result.items():
        print(f'{key}: {value:.3f}' if isinstance(value, float) else f'{key}: {value}')


if __name__ == '__main__':
    main()
