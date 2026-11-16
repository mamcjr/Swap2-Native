#!/usr/bin/env python3
# A repeatable benchmark of a game on an Android phone over adb (the Mac host's is scripts/bench.py): the test app (com.swap2native.test by default) starts the game from its
# save with scripted presses (DM_KEYS, the same on every run), the host writes its per-frame timing rows (DM_NVN_TSV), and a fixed window after the scene is loaded gives
# frame-time percentiles, the frames that miss a 30 fps target, GPU and recording time, render passes and page faults, with the phone's temperatures and available memory at
# the window's start and end and the replay's own statistics lines. Variants (sets of environment switches) run in turn (A B A B ...), with a cool-down between runs, so heat
# drift falls on every variant alike.
# Usage: python dm/tools/android_bench.py --game "<the game's path as the launcher shows it>" [--variant 'label:KEY=VALUE,KEY=VALUE' ...] [--runs N] [--from S] [--secs S]
#   [--keys DM_KEYS] [--cool S] [--serial X] [--package P] [--out folder] [--title ID]
#   Without --variant a single variant 'base'. Results go to output/bench/<date-time>-<tag>/ (per run log.txt, frames.tsv; results.md and results.json for all).
#   The phone is shared: register the use in POCO-FILA.md first. The env.txt this writes is removed at the end, and so are the placed-map kill records.
# Defaults are Zelda: Ocarina of Time's (0400486024DEC000): six A presses from 35 s load the test save's Kokiri Forest, the window starts at 100 s.
import argparse, datetime, json, math, re, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent


def adb_bytes(a, *cmd, check=True, timeout=120):
    r = subprocess.run(['adb', '-s', a.serial, *cmd], capture_output=True, timeout=timeout)
    if check and r.returncode: sys.exit(f'error: adb {" ".join(cmd)}: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def adb(a, *cmd, **kw): return adb_bytes(a, *cmd, **kw).decode(errors='replace')


def sh(a, cmd, **kw): return adb(a, 'shell', cmd, **kw)


def pct(xs, p):   # nearest-rank percentile
    if not xs: return None
    s = sorted(xs); return s[min(len(s) - 1, max(0, math.ceil(p / 100 * len(s)) - 1))]


def thermal(a):
    """The phone's hottest CPU, GPU and skin temperatures (dumpsys thermalservice's current values; types 0 CPU, 1 GPU, 3 skin)"""
    out, best = sh(a, 'dumpsys thermalservice', check=False), {}
    cur = out.split('Current temperatures from HAL:', 1)[-1].split('Current cooling devices', 1)[0]
    for m in re.finditer(r'mValue=([-\d.]+), mType=(\d+), mName=([^,]+)', cur):
        v, t = float(m.group(1)), {'0': 'cpu', '1': 'gpu', '3': 'skin'}.get(m.group(2))
        if t and v > best.get(t, -1e9): best[t] = v
    return best


def mem_available(a):
    m = re.search(r'MemAvailable:\s+(\d+)', sh(a, 'cat /proc/meminfo', check=False))
    return int(m.group(1)) // 1024 if m else None


def frames(tsv, t0, t1):
    rows = {}
    for ln in tsv.splitlines():
        c = ln.split('\t')
        if len(c) != 3 or not c[1].startswith('@'): continue
        try: rows.setdefault(int(c[0]), {})[c[1]] = float(c[2])
        except ValueError: continue
    return [r for f, r in sorted(rows.items()) if '@t_s' in r and t0 <= r['@t_s'] < t1]


def summary(sel, secs):
    d = {'frames': len(sel), 'fps': round(len(sel) / secs, 1)}
    col = lambda k, ok=lambda x: x >= 0: [r[k] for r in sel if k in r and ok(r[k])]
    ft = col('@frame_ms', lambda x: x > 0)
    for name, xs in (('frame', ft), ('shown', col('@presented_ms', lambda x: x > 0)), ('gpu', col('@gpu_ms')), ('rec', col('@rec_ms')), ('cpu', col('@cpu_ms')),
                     ('proc_cpu', col('@proc_cpu_ms')), ('wait', col('@wait_ms')), ('passes', col('@passes')), ('draws', col('@draws'))):
        if not xs: continue
        for p in (50, 95, 99):
            q = pct(xs, p)
            if q is not None: d[f'{name}_p{p}'] = round(q, 2)   # (xs is not empty: never None)
        d[f'{name}_max'] = round(max(xs), 2)
    d['over_40ms'] = sum(1 for x in ft if x > 40)     # a frame a 30 fps target sees as late
    d['over_50ms'] = sum(1 for x in ft if x > 50)     # a hitch
    d['over_100ms'] = sum(1 for x in ft if x > 100)
    d['in_30'] = round(100 * sum(1 for x in ft if 31.3 <= x <= 35.3) / len(ft), 1) if ft else None   # the share of frames within 2 ms of 33.3
    for k in ('@faults', '@pso_new', '@reuploads', '@mprotect_calls'):
        xs = col(k)
        if xs: d[k[1:] + '_sum'] = int(sum(xs))
    return d


LOG_LINES = ('[vk] replay up to frame', '[vk] command encoder per frame', '[vk] descriptors:', '[vk] inline uniform updates per frame',
             '[vk] game barriers inside a draw pass per frame', '[vk] dispatches per frame', '[vk] pass ends per frame', '[vk] memory:', '[vk] guest memory:',
             '[nvn] frame interval up to frame', 'resolution', '[vk] fps:')


def log_digest(log):
    last = {}
    for ln in log.splitlines():
        for k in LOG_LINES:
            if k in ln: last[k] = ln.split('] ', 1)[-1][:600]
    reds = {}
    for ln in log.splitlines():
        if '[RED]' in ln: k = re.sub(r'0x[0-9a-f]+|\d+', '#', ln.split('[RED]', 1)[1])[:90]; reds[k] = reds.get(k, 0) + 1
    return last, reds


def run_one(a, label, env, n, out):
    files = f'/sdcard/Android/data/{a.package}/files'; temp = f'{files}/temp/{a.title}'
    tsv_dev = f'/storage/emulated/0/Android/data/{a.package}/files/bench-frames.tsv'
    sh(a, f'am force-stop {a.package}; rm -f {files}/env.txt {temp}/vk_backing-*.txt {tsv_dev}; : > {files}/ctl.txt')
    if a.save_ref:   # the save as it was: the game saves while it runs (autosave), so without this each run starts where the last one left Link
        sv = f'{a.save}/{a.title}'
        sh(a, f'[ -d {a.save_ref}/main ] && [ -d {a.save_ref}/work ] && rm -rf {sv}/* {sv}.work/* && cp -r {a.save_ref}/main/. {sv}/ && cp -r {a.save_ref}/work/. {sv}.work/')
    lines = [f'DM_NVN_TSV={tsv_dev}', 'DM_NVN_TSV_CALLS=0', f'DM_KEYS={a.keys}'] + [f'{k}={v}' for k, v in env.items()]
    sh(a, f"printf '%s\\n' {' '.join(repr(x) for x in lines)} > {files}/env.txt")
    t_start = time.time()
    pk = f" --es pack '{a.pack}'" if a.pack else ''   # a pack the app does not find beside the game (another app's, copied to shared storage)
    sh(a, f"am start -n {a.package}/dev.nxnative.android.MainActivity --es game '{a.game}'{pk} --ez run true")
    for _ in range(30):
        if sh(a, f'pidof {a.package}:game', check=False).strip(): break
        time.sleep(1)
    else: sys.exit(f'error: {label} run {n}: the game did not start')
    def at(s):   # wait until s seconds after the start
        d = t_start + s - time.time()
        if d > 0: time.sleep(d)
    at(a.start); t0, m0 = thermal(a), mem_available(a)
    for off, line in sorted(a.ctl):   # live control lines (DM_CTL: <app files>/ctl.txt), seconds after the window's start
        at(a.start + off); sh(a, f"echo '{line}' >> {files}/ctl.txt", check=False)
    at(a.start + a.secs); t1, m1 = thermal(a), mem_available(a)
    alive = bool(sh(a, f'pidof {a.package}:game', check=False).strip())
    shot = adb_bytes(a, 'exec-out', 'screencap -p', check=False)
    sh(a, f'am force-stop {a.package}')
    time.sleep(2)
    rd = out / f'{label}-{n}'; rd.mkdir(parents=True, exist_ok=True)
    tsv = adb(a, 'exec-out', f'cat {tsv_dev}', check=False)
    logp = sh(a, f'ls -t {files}/logs/{a.title}-*.log | head -1').strip()
    log = adb(a, 'exec-out', f'cat {logp}')
    (rd / 'frames.tsv').write_text(tsv, encoding='utf-8'); (rd / 'log.txt').write_text(log, encoding='utf-8'); (rd / 'end.png').write_bytes(shot)
    sh(a, f'rm -f {files}/env.txt {temp}/vk_backing-*.txt {tsv_dev}')
    s = digest(a, tsv, log)
    s.update(alive=alive, temp_start=t0, temp_end=t1, mem_avail_start_mb=m0, mem_avail_end_mb=m1)
    return s


FIRST_FRAME_S = 4   # the game's first frame, seconds after the intent (the POCO F7 Ultra: 3 to 5)


def digest(a, tsv, log):
    """The window's summary: the TSV's @t_s has a base of its own (not the host's start), so the window counts from the first frame's @t_s"""
    ts = [float(c[2]) for c in (ln.split('\t') for ln in tsv.splitlines()) if len(c) == 3 and c[1] == '@t_s']
    s: dict = {'frames': 0}
    if ts:
        base = min(ts) - FIRST_FRAME_S
        sel = frames(tsv, base + a.start, base + a.start + a.secs)
        if sel: s = summary(sel, a.secs)
    last, reds = log_digest(log)
    s.update(log=last, red=reds)
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', default='/storage/emulated/0/Switch2/Games/The Legend of Zelda Ocarina of Time.nup')
    ap.add_argument('--title', default='0400486024DEC000')
    ap.add_argument('--package', default='com.swap2native.test')
    ap.add_argument('--serial', default='98d66058')
    ap.add_argument('--variant', action='append', default=[])
    ap.add_argument('--runs', type=int, default=2)
    ap.add_argument('--keys', default='35:A*6/4', help='DM_KEYS that reach the scene (default: the test save into Kokiri Forest)')
    ap.add_argument('--start', type=float, default=100, help='window start, seconds after the intent')
    ap.add_argument('--secs', type=float, default=60, help='window length')
    ap.add_argument('--cool', type=float, default=90, help='idle seconds between runs')
    ap.add_argument('--ctl', action='append', default=[], type=lambda v: (float(v.split(':', 1)[0]), v.split(':', 1)[1]),
                    help='S:LINE: a live control line (for example 20:env DM_VK_SYNCLOG=40) S seconds into the window')
    ap.add_argument('--pack', help='the prepared pack folder of the game on the phone, when the app has none beside the game (MainActivity extra "pack")')
    ap.add_argument('--save', default='/sdcard/Switch2/saves-test', help='the save folder the test app plays from (its <title ID> and <title ID>.work)')
    ap.add_argument('--save-ref', help='a copy on the phone (main/ and work/ of the save) put back before every run, e.g. /data/local/tmp/s2bench/0400486024DEC000-mido')
    ap.add_argument('--tag', default='bench')
    ap.add_argument('--out')
    ap.add_argument('--resummarize', help='a results folder of an earlier invocation: its runs summarized again (with this --start / --secs), results.md rewritten')
    a = ap.parse_args()
    if a.save_ref and not a.save_ref.startswith('/data/'):   # (Git Bash turns a /data/... argument into C:/Program Files/Git/data/...: run with MSYS_NO_PATHCONV=1)
        sys.exit(f'error: --save-ref must be a folder on the phone under /data/, not {a.save_ref!r}')
    if a.resummarize:
        out = Path(a.resummarize); old = json.loads((out / 'results.json').read_text())
        for k in ('keys', 'runs', 'cool', 'tag'): setattr(a, k, old['args'][k])
        a.save_ref = old['args'].get('save_ref')
        vars_ = list(old['variants'].items()); res = old['results']
        for label, rs in res.items():
            for i, r in enumerate(rs, 1):
                rd = out / f'{label}-{i}'
                d = digest(a, (rd / 'frames.tsv').read_text(encoding='utf-8'), (rd / 'log.txt').read_text(encoding='utf-8'))
                for k in ('alive', 'temp_start', 'temp_end', 'mem_avail_start_mb', 'mem_avail_end_mb'): d[k] = r.get(k)
                rs[i - 1] = d
        write_results(a, vars_, res, out); return
    vars_ = []
    for v in a.variant or ['base:']:
        label, _, kvs = v.partition(':')
        vars_.append((label, dict(kv.split('=', 1) for kv in kvs.split(',') if kv)))
    out = Path(a.out) if a.out else REPO / 'output/bench' / f'{datetime.datetime.now():%Y%m%d-%H%M}-{a.tag}'
    out.mkdir(parents=True, exist_ok=True)
    res = {label: [] for label, _ in vars_}
    first = True
    for n in range(1, a.runs + 1):
        for label, env in vars_:
            if not first: time.sleep(a.cool)
            first = False
            print(f'{datetime.datetime.now():%H:%M:%S} {label} run {n} ...', flush=True)
            try: s = run_one(a, label, env, n, out); res[label].append(s)
            finally:   # whatever happened, the phone is left as it was: the game stopped, no env.txt, no kill records
                sh(a, f'am force-stop {a.package}; rm -f /sdcard/Android/data/{a.package}/files/env.txt /sdcard/Android/data/{a.package}/files/temp/{a.title}/vk_backing-*.txt; : > /sdcard/Android/data/{a.package}/files/ctl.txt', check=False)
            print(f'  fps {s.get("fps")} frame p50 {s.get("frame_p50")} p99 {s.get("frame_p99")} max {s.get("frame_max")} in30 {s.get("in_30")}% gpu p50 {s.get("gpu_p50")} '
                  f'rec p50 {s.get("rec_p50")} temp {s["temp_end"]} avail {s["mem_avail_end_mb"]} alive {s["alive"]}', flush=True)
    if a.save_ref:   # and the save is left as the reference has it
        sv = f'{a.save}/{a.title}'; sh(a, f'[ -d {a.save_ref}/main ] && [ -d {a.save_ref}/work ] && rm -rf {sv}/* {sv}.work/* && cp -r {a.save_ref}/main/. {sv}/ && cp -r {a.save_ref}/work/. {sv}.work/', check=False)
    write_results(a, vars_, res, out)


def write_results(a, vars_, res, out):
    (out / 'results.json').write_text(json.dumps({'args': vars(a), 'variants': {l: e for l, e in vars_}, 'results': res}, indent=1))
    keys = ['fps', 'frame_p50', 'frame_p95', 'frame_p99', 'frame_max', 'in_30', 'over_40ms', 'over_50ms', 'over_100ms', 'gpu_p50', 'gpu_p95', 'rec_p50', 'rec_p95',
            'cpu_p50', 'proc_cpu_p50', 'passes_p50', 'draws_p50', 'faults_sum', 'pso_new_sum']
    md = [f'# {a.tag} ({datetime.datetime.now():%Y-%m-%d %H:%M})', '', f'window {a.start:.0f}+{a.secs:.0f} s, keys `{a.keys}`, runs {a.runs}, cool {a.cool:.0f} s', '']
    md.append('| variant | run | ' + ' | '.join(keys) + ' | temp end (cpu/gpu/skin) | avail MB | alive |')
    md.append('|' + '---|' * (len(keys) + 5))
    for label, rs in res.items():
        for i, s in enumerate(rs, 1):
            t = s['temp_end']; md.append(f'| {label} | {i} | ' + ' | '.join(str(s.get(k, '')) for k in keys) +
                                          f' | {t.get("cpu", "")}/{t.get("gpu", "")}/{t.get("skin", "")} | {s["mem_avail_end_mb"]} | {s["alive"]} |')
    md += ['', '## Variants', ''] + [f'- `{l}`: ' + (', '.join(f'`{k}={v}`' for k, v in e.items()) or 'defaults') for l, e in vars_]
    md += ['', '## Last replay lines (run 1 of each variant)', '']
    for label, rs in res.items():
        md.append(f'### {label}'); md += [f'- {v}' for v in rs[0]['log'].values()]
        if rs[0]['red']: md.append('- [RED] lines: ' + '; '.join(f'{k.strip()} ×{c}' for k, c in sorted(rs[0]['red'].items(), key=lambda x: -x[1])[:8]))
        md.append('')
    (out / 'results.md').write_text('\n'.join(md), encoding='utf-8')
    print(out / 'results.md')


if __name__ == '__main__':
    main()
