#!/usr/bin/env python3
"""Evaluate recorded GTA2 simulation intervals; never generate measurement data.

CSV must contain timestamp_ms or delta_ms. Source/provenance are supplied by the
operator and recorded, not independently certified. Percentiles use linear
interpolation between adjacent sorted samples (position = (N-1) * quantile).
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import sys

TARGET_TICKRATE_HZ = 1000.0 / 33.0
TOLERANCE_PCT = 2.0


def finite_number(value, label, *, positive=False):
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{label} is not a number") from None
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError(f"{label} must be finite and {'positive' if positive else 'nonnegative'}")
    return result


def read_tick_csv(path, column=None):
    with Path(path).open('r', encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        if len(fields) != len(set(fields)):
            raise ValueError('CSV has duplicate column names')
        available = [name for name in ('timestamp_ms', 'delta_ms') if name in fields]
        if column is None:
            if len(available) != 1:
                raise ValueError('CSV must contain exactly one timing column, or specify --column')
            column = available[0]
        if column not in available:
            raise ValueError(f'CSV is missing {column}')
        values = []
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f'CSV row {reader.line_num} does not match the header')
            values.append(finite_number(row[column], f'CSV row {reader.line_num} {column}',
                                        positive=column == 'delta_ms'))
    if column == 'timestamp_ms':
        if len(values) < 2:
            raise ValueError('At least two timestamps are required')
        deltas = [later - earlier for earlier, later in zip(values, values[1:])]
        if any(delta <= 0 or not math.isfinite(delta) for delta in deltas):
            raise ValueError('Timestamps must increase strictly without wraparound or resets')
    else:
        deltas = values
    if not deltas:
        raise ValueError('No tick samples provided')
    return deltas, column, len(values)


def percentile(sorted_values, quantile):
    position = (len(sorted_values) - 1) * quantile
    lower = int(position)
    upper = min(lower + 1, len(sorted_values) - 1)
    return sorted_values[lower] + (sorted_values[upper] - sorted_values[lower]) * (position - lower)


def evaluate_tick_log(tick_deltas_ms, target_hz=TARGET_TICKRATE_HZ,
                      tolerance_percent=TOLERANCE_PCT, min_duration_seconds=0.0):
    target_hz = finite_number(target_hz, 'target_hz', positive=True)
    tolerance_percent = finite_number(tolerance_percent, 'tolerance_percent')
    min_duration_seconds = finite_number(min_duration_seconds, 'min_duration_seconds')
    samples = [finite_number(value, f'delta[{index}]', positive=True)
               for index, value in enumerate(tick_deltas_ms)]
    if not samples:
        raise ValueError('No tick samples provided')
    try:
        duration_ms = math.fsum(samples)
    except OverflowError:
        raise ValueError('Total sample duration overflowed') from None
    if not math.isfinite(duration_ms):
        raise ValueError('Total sample duration is not finite')
    mean_ms = duration_ms / len(samples)
    measured_hz = 1000.0 / mean_ms
    deviation = abs(measured_hz / target_hz - 1.0) * 100.0
    if not math.isfinite(measured_hz) or not math.isfinite(deviation):
        raise ValueError('Derived tick rate or deviation is not finite')
    ordered = sorted(samples)
    within_tolerance = deviation <= tolerance_percent
    sufficient_duration = duration_ms / 1000.0 >= min_duration_seconds
    return {
        'valid': True,
        'scope': 'Recorded simulation intervals only; not renderer FPS or a Quest/G0 acceptance.',
        'target_tickrate_hz': target_hz,
        'measured_tickrate_hz': measured_hz,
        'average_tick_delta_ms': mean_ms,
        'p50_tick_delta_ms': percentile(ordered, 0.50),
        'p95_tick_delta_ms': percentile(ordered, 0.95),
        'p99_tick_delta_ms': percentile(ordered, 0.99),
        'min_tick_delta_ms': ordered[0],
        'max_tick_delta_ms': ordered[-1],
        'percent_deviation': deviation,
        'tolerance_percent': tolerance_percent,
        'within_tolerance': within_tolerance,
        'minimum_duration_seconds': min_duration_seconds,
        'sufficient_duration': sufficient_duration,
        'check_passed': within_tolerance and sufficient_duration,
        'duration_seconds': duration_ms / 1000.0,
        'sample_count': len(samples),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input_csv', type=Path)
    parser.add_argument('--source', required=True, help='What event/hook produced each tick')
    parser.add_argument('--provenance', required=True,
                        help='Capture command, device/runtime, build identity and session reference')
    parser.add_argument('--column', choices=('timestamp_ms', 'delta_ms'))
    parser.add_argument('--target-hz', type=float, default=TARGET_TICKRATE_HZ)
    parser.add_argument('--tolerance-percent', type=float, default=TOLERANCE_PCT)
    parser.add_argument('--min-duration-seconds', type=float, default=0.0)
    parser.add_argument('--check', action='store_true', help='Exit 1 if rate/duration checks fail')
    args = parser.parse_args(argv)
    try:
        if not args.source.strip() or not args.provenance.strip():
            raise ValueError('Source and provenance must be nonempty')
        samples, column, rows = read_tick_csv(args.input_csv, args.column)
        report = evaluate_tick_log(samples, args.target_hz, args.tolerance_percent,
                                   args.min_duration_seconds)
        report.update({
            'input_csv': str(args.input_csv.resolve()),
            'input_sha256': hashlib.sha256(args.input_csv.read_bytes()).hexdigest(),
            'input_column': column, 'input_row_count': rows,
            'source': args.source, 'provenance': args.provenance,
            'provenance_verification': 'operator_supplied_not_independently_verified',
        })
    except (OSError, ValueError, csv.Error) as error:
        print(json.dumps({'valid': False, 'check_passed': False, 'error': str(error)}, indent=2))
        return 2
    print(json.dumps(report, indent=2, allow_nan=False))
    return 1 if args.check and not report['check_passed'] else 0


if __name__ == '__main__':
    sys.exit(main())
