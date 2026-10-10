#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation controls against real code; bad outputs stay under work/, never ship."""
import argparse, json, os, pathlib, re, shutil, subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', required=True, type=pathlib.Path, help='Positive A+C build')
p.add_argument('--shared-build', required=True, type=pathlib.Path, help='Positive B build')
p.add_argument('--headers', required=True, type=pathlib.Path)
p.add_argument('--output', required=True, type=pathlib.Path)
a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parent.parent
build = a.build.resolve(); shared = a.shared_build.resolve(); headers = a.headers.resolve(); out = a.output.resolve()
version = re.search(r'VERSION (\d+\.\d+\.\d+)', (root / 'CMakeLists.txt').read_text()).group(1)
common = ['c++', '-std=c++17', '-O2', f'-DPVB_VERSION="{version}"', '-I'+str(headers)]
results = {}
def run(command, name, expected_failure=False, env=None):
    r = subprocess.run(list(map(str, command)), cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    (out / (name+'.log')).write_bytes(r.stdout)
    assert (r.returncode != 0) == expected_failure, (name, r.returncode)
    return r.stdout.decode(errors='replace')
for name, old, new in [
    ('arena-alias', 'input_offset=aligned(arena->input_used); output_offset=aligned(arena->output_used);', 'input_offset=0; output_offset=0;'),
    ('history-range', 'shadow==last.layout && stages==last.stages && offset==last.offset && size==last.data.size()', 'size==last.data.size()'),
]:
    d = out / name; shutil.copytree(root/'src', d/'src', dirs_exist_ok=True); (d/'tests').mkdir(exist_ok=True)
    shutil.copy(root/'tests/optimization_ownership.cpp', d/'tests/optimization_ownership.cpp')
    f = d/'src/wrapper.cpp'; s = f.read_text(); assert s.count(old) == 1; f.write_text(s.replace(old, new))
    run(common + ['-I'+str(build), d/'tests/optimization_ownership.cpp', '-ldl', '-pthread', '-o', d/'control'], name+'-build')
    text = run([d/'control'], name, True); assert 'FAIL:' in text
    results[name] = re.search(r'OPTIMIZATION_OWNERSHIP_RESULT: .*', text).group(0)
sync = out/'sync'; sync.mkdir(exist_ok=True)
shutil.copy(root/'tests/probe.cpp', sync/'probe.cpp')
s = (root/'tests/transfer_probe.inc').read_text()
old = '    transfer_barrier(a,r.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,sync2);'
assert s.count(old) == 1; (sync/'transfer_probe.inc').write_text(s.replace(old, '    // Intentional dependency removal, negative control only.'))
includes = ['-I'+str(root/'tests'), '-I'+str(root/'src'), '-I'+str(build)]
run(common+includes+[sync/'probe.cpp', '-ldl', '-o', sync/'control'], 'sync-build')
env = dict(os.environ, VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
text = run([sync/'control', build/'libvulkan_pixel_bc.so', '--validation'], 'sync', True, env)
results['removed_dependency_read_after_write_hazards'] = text.count('READ_AFTER_WRITE hazard detected')
assert results['removed_dependency_read_after_write_hazards'] > 0
shader = out/'shader'; shader.mkdir(exist_ok=True)
s = (root/'src/bc_decode_shared.comp').read_text(); old = 'sharedBc7[groupBlock]=bc7Parse(uvec4(b,src.words[base+2u],src.words[base+3u]));'
assert s.count(old) == 1; (shader/'bad.comp').write_text(s.replace(old, 'sharedBc7[groupBlock]=bc7Parse(uvec4(0u));'))
run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-DPVB_SHARED_DECODE=1', '-S', 'comp', '-o', shader/'bc_decode.spv', shader/'bad.comp'], 'shader-compile')
run(['spirv-val', '--target-env', 'vulkan1.1', shader/'bc_decode.spv'], 'shader-spirv')
run(['python3', root/'scripts/embed_spirv.py', shader/'bc_decode.spv', shader/'bc_decode_spv.h'], 'shader-embed')
run(common+['-I'+str(shader), '-I'+str(shared)]+includes+[root/'tests/bc7_probe.cpp', '-ldl', '-o', shader/'control'], 'shader-build')
text = run([shader/'control', shared/'libvulkan_pixel_bc.so', '--validation'], 'shader', True)
results['corrupted_shared_bc7_parse'] = re.search(r'RESULT: .*', text).group(0)
assert 'FAIL:' in text
(out/'result.json').write_text(json.dumps(results, indent=2)+'\n')
print(json.dumps(results, indent=2))
