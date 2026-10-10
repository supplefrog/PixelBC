#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare explicit immutable baseline and separately built variants, sequentially."""
import argparse, hashlib, json, os, pathlib, statistics, subprocess

p = argparse.ArgumentParser()
p.add_argument('--benchmark', required=True)
p.add_argument('--library', action='append', required=True, help='label=explicit-library-path')
p.add_argument('--output', required=True)
p.add_argument('--rounds', type=int, default=3)
p.add_argument('--repeats', type=int, default=15)
a = p.parse_args()
assert 1 <= a.rounds <= 5 and 3 <= a.repeats <= 60
out = pathlib.Path(a.output); out.mkdir(parents=True, exist_ok=True)
libraries = dict(s.split('=', 1) for s in a.library)
hashes = {label: hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() for label, path in libraries.items()}
records = []
workloads = [(64, 8, 'all'), (256, 1, 'all'), (256, 8, 'all'), (256, 1, 'bc1'), (256, 1, 'bc3'), (256, 1, 'bc7')]
labels = list(libraries)
for round_number in range(a.rounds):
    order = labels[round_number % len(labels):] + labels[:round_number % len(labels)]
    if round_number % 2: order.reverse()
    for size, history, fmt in workloads:
        for label in order:
            command = [a.benchmark, libraries[label], '--warmup', '5', '--repeats', str(a.repeats), '--size', str(size), '--history', str(history), '--format', fmt]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            name = f'{round_number}-{label}-{size}-{history}-{fmt}.log'
            (out / name).write_text(result.stdout)
            if result.returncode: raise RuntimeError(f'{name}: exit {result.returncode}')
            checked_count = 0
            for line in result.stdout.splitlines():
                if line.startswith('BENCH_SAMPLE '):
                    sample = json.loads(line.removeprefix('BENCH_SAMPLE '))
                    if not sample['warmup']: records.append(dict(label=label, round=round_number, size=size, history=history, format=fmt, **sample))
                if line.startswith('BENCH_RESULT '):
                    checked = json.loads(line.removeprefix('BENCH_RESULT ')); assert checked['failed'] == checked['validation_errors'] == 0
                    checked_count += 1
            assert checked_count == 1, name
            print(f'{name}: passed', flush=True)
summary = []
for size, history, fmt in workloads:
    for label in labels:
        samples = [s for s in records if (s['size'], s['history'], s['format'], s['label']) == (size, history, fmt, label)]
        assert len(samples) == a.rounds * a.repeats
        row = dict(label=label, size=size, history=history, format=fmt, repetitions=len(samples))
        for key in ['cpu_reset_ms', 'cpu_record_ms', 'gpu_upload_decode_transfer_ms']:
            values = [s[key] for s in samples if s[key] is not None]
            row[key] = None if not values else dict(median=statistics.median(values), minimum=min(values), maximum=max(values), quartiles=statistics.quantiles(values, n=4))
        for key in ['buffer_create_calls', 'memory_allocate_calls', 'descriptor_pool_create_calls', 'requested_buffer_bytes', 'replayed_binds', 'replayed_pushes']:
            values = [s[key] for s in samples if key in s]
            row[key] = None if not values else dict(minimum=min(values), maximum=max(values))
        summary.append(row)
receipt = dict(library_sha256=hashes, benchmark_sha256=hashlib.sha256(pathlib.Path(a.benchmark).read_bytes()).hexdigest(), backend='llvmpipe CPU Vulkan; not Pixel/Mali or gameplay FPS', gpu_window='upload + BC compute + RGBA image transfer; pure kernel not isolated', warmed_repetitions=True, sequential_rotated_process_order=True, rows=summary, samples=records)
receipt['LP_NUM_THREADS'] = os.environ.get('LP_NUM_THREADS')
quota = pathlib.Path('/sys/fs/cgroup/cpu.max')
receipt['cpu_quota'] = quota.read_text().strip() if quota.exists() else None
(out / 'summary.json').write_text(json.dumps(receipt, indent=2) + '\n')
