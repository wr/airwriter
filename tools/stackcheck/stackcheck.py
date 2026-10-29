#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Worst-case stack of the tasks that make ESP-IDF's deep calls, by GCC's count.

    python3 tools/stackcheck/stackcheck.py [BUILD_DIR]     (after idf.py build)

Recompiles the project's sources, and ESP-IDF's on the image-check and flash
paths, with -fcallgraph-info=su, follows every call from each root below to its
deepest path, and fails if that leaves less than MARGIN of the task's stack.
GCC counts only what it compiled: ROM, newlib, the rest of ESP-IDF and calls
through pointers add nothing, so a pass is a floor, not a proof.

W-1108: checking an image's RSA-3072 signature takes at least 3.7 KB. The main
task did it at boot on 3,584 bytes whenever a slot was marked failed, overflowed
into the other core's IPC task, and the board crashed at every boot.
"""
import concurrent.futures, functools, json, os, re, shlex, subprocess, sys, tempfile

MARGIN = 1024

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
# ESP-IDF sources the roots reach for the calls that matter: OTA state and
# image checks, the signature check's RSA, and flash and partition access.
IDF = ('components/app_update/', 'components/bootloader_support/src/',
       'components/mbedtls/mbedtls/library/rsa.c', 'components/mbedtls/mbedtls/library/bignum',
       'components/mbedtls/port/bignum/', 'components/mbedtls/port/sha/',
       'components/esp_partition/', 'components/spi_flash/', 'components/log/')


def define(path, name):
    m = re.search(r'#define\s+%s\s+(\d+)' % name, open(path).read())
    if not m:
        sys.exit(f'stackcheck: no {name} in {path}')
    return int(m.group(1))


def roots(build):
    """(function, its task's stack in bytes, where that number comes from)."""
    comp = os.path.join(ROOT, 'firmware/components')
    return [
        ('app_main', define(os.path.join(build, 'config/sdkconfig.h'), 'CONFIG_ESP_MAIN_TASK_STACK_SIZE'),
         'the main task, CONFIG_ESP_MAIN_TASK_STACK_SIZE'),
        ('gate_task', define(os.path.join(comp, 'ota_health/ota_health.c'), 'GATE_STACK'),
         "ota_health's trial gate, GATE_STACK"),
        ('key_task', define(os.path.join(comp, 'fw_update/fw_update.c'), 'KEY_STACK'),
         "fw_update's look at the running image's key, KEY_STACK"),
    ]


def compile_all(build, out):
    cc = json.load(open(os.path.join(build, 'compile_commands.json')))
    fw = os.path.join(ROOT, 'firmware') + os.sep
    jobs = [e for e in cc if e['file'].endswith('.c') and
            ((e['file'].startswith(fw) and '/build/' not in e['file']) or any(k in e['file'] for k in IDF))]

    def one(i_e):
        i, e = i_e
        a = shlex.split(e['command'])
        a[a.index('-o') + 1] = os.path.join(out, f'{i}.o')
        r = subprocess.run(a + ['-fcallgraph-info=su'], cwd=e['directory'], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'stackcheck: {e["file"]} did not compile:\n{r.stderr[-800:]}')

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(one, enumerate(jobs)))
    return len(jobs)


def graph(out):
    frame, calls = {}, {}
    for name in os.listdir(out):
        if not name.endswith('.ci'):
            continue
        txt = open(os.path.join(out, name)).read()
        for m in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"', txt):
            s = re.search(r'(\d+) bytes', m.group(2))
            frame[m.group(1)] = max(frame.get(m.group(1)) or 0, int(s.group(1))) if s else frame.get(m.group(1))
        for m in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', txt):
            calls.setdefault(m.group(1), set()).add(m.group(2))
    return frame, calls


def deepest(frame, calls, fn):
    @functools.lru_cache(None)
    def walk(f, seen):
        best = (0, ())
        for g in calls.get(f, ()):
            if g not in seen:
                d = walk(g, seen | {f})
                if d[0] > best[0]:
                    best = d
        return ((frame.get(f) or 0) + best[0], (f,) + best[1])
    return walk(fn, frozenset())


def short(title):
    return title.rsplit(':', 1)[-1]     # a static function is "path:name"


def main():
    build = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'firmware/build'))
    with tempfile.TemporaryDirectory() as out:
        n = compile_all(build, out)
        frame, calls = graph(out)
    print(f'stackcheck: {n} sources, margin {MARGIN} bytes')
    ok = True
    for fn, stack, what in roots(build):
        title = fn if fn in frame else next((t for t in frame if t.endswith(':' + fn)), None)
        if title is None:
            sys.exit(f'stackcheck: {fn} is not in the build')
        depth, path = deepest(frame, calls, title)
        good = depth + MARGIN <= stack
        ok &= good
        print(f'{"ok  " if good else "FAIL"} {fn}: at least {depth} of {stack} bytes ({what})')
        if not good:
            print('     ' + ' -> '.join(f'{short(p)} {frame.get(p) or 0}' for p in path))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
