#!/usr/bin/env python3
"""Replace the live stacks by the restacked finals, so each target's live stack holds every good frame from every night.

    tools/consolidate.py [--data /mnt/fits-backup/claude] [--min-frames 10] [--apply]

Without --apply it prints the plan and changes nothing. Run tools/restack.py first (it rebuilds DATA/finals from the raw snapshots); this then:

  * names: each final goes to stacks/<the planner's key> (M33, NGC_7000_North_America_Nebula), the directory the run resumes. The final taken with the filter now fitted
    (~/.config/origin/filter) gets the plain name; the other filter's goes to <key>_<Filter> (M33_Clear), which the planner still counts for coverage but the live
    stacker never resumes, so a night through one filter does not archive the other filter's stack. Names the planner does not know (IC405_Flaming_Star) are kept.
  * a live stack with more exposure than its final (the final re-judges every frame against its own reference) is left in place
  * every other live stack of those targets (aliases, archived copies, pre-filter and "auto" fragments) is moved to stacks/_fragments_<YYYYMMDD>/; nothing is deleted
  * each new stack is dated by its last frame (frames.tsv), so the planner's "imaged N days ago" stays true
  * finals under --min-frames frames are skipped: a new stack starts fresh instead of from a token reference

The daemon must not be stacking while this runs (it would save over the stack it holds): run it by day, with the daemon's target on "auto".
"""
import argparse, calendar, glob, json, os, re, shutil, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from restack import safe

# the planner's non-Messier keys (Planner.cpp loadTargets extras) that have been imaged; Messier keys are the catalogue id itself
EXTRAS = ["NGC 7000 North America Nebula", "IC 5070 Pelican Nebula", "NGC 6992 Eastern Veil", "NGC 6960 Western Veil", "NGC 869/884 Double Cluster", "IC 1805 Heart Nebula",
          "IC 1848 Soul Nebula", "NGC 281 Pacman Nebula", "IC 1396 Elephant Trunk", "NGC 7380 Wizard Nebula", "NGC 1499 California Nebula", "NGC 2244 Rosette Nebula",
          "NGC 6888 Crescent Nebula", "NGC 6946 Fireworks Galaxy", "NGC 7822 Cepheus Nebula", "NGC 891", "NGC 7331 + Stephan Quintet", "NGC 7293 Helix Nebula"]
COPY = re.compile(r'_final_|backup|_\d{8}_\d{6}$', re.I)     # names the planner does not count (Planner.cpp applyNovelty)


def cid(s):
    """catalogue id as the planner forms it (Planner.cpp catalogId), plus the Rosette's old stack name"""
    if 'veil' in s.lower(): return 'VEIL'
    if s.lower().startswith('rosette'): return 'NGC2244'
    m = re.match(r'\s*(M|NGC|IC)[ _-]*(\d+)', s, re.I)
    return (m.group(1).upper() + m.group(2)) if m else None


def planner_key(name):
    c = cid(name)
    if c and re.fullmatch(r'M\d+', c): return c
    for e in EXTRAS:
        if cid(e) == c and (c != 'VEIL' or 'east' in name.lower() or '6992' in name): return e
    return name


def header(path):
    """(frames, total exposure s, reference filter) from a state.bin: LST1 magic w h frames exposure; LST2 adds the ISO before the exposure; LST3 the ISO and a
    length-prefixed filter (older stacks: filter '')"""
    try:
        with open(path, 'rb') as f: b = f.read(128)
        magic, w, h, n = struct.unpack('<Iiii', b[:16]); pos = 16; flt = ''
        if magic != 0x4C535431: pos += 4
        if magic == 0x4C535433: ln = struct.unpack('<i', b[pos:pos + 4])[0]; flt = b[pos + 4:pos + 4 + ln].decode('utf-8', 'replace'); pos += 4 + ln
        return n, struct.unpack('<d', b[pos:pos + 8])[0], flt
    except (OSError, struct.error, UnicodeDecodeError): return 0, 0.0, ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default='/mnt/fits-backup/claude'); ap.add_argument('--min-frames', type=int, default=10); ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    finals, stacks = os.path.join(a.data, 'finals'), os.path.join(a.data, 'stacks')
    fpath = os.path.expanduser('~/.config/origin/filter')
    fitted = (open(fpath).read().strip() if os.path.exists(fpath) else 'clear').capitalize()

    plan = {}
    for m in sorted(glob.glob(os.path.join(finals, '*', 'manifest.json'))):
        j = json.load(open(m)); s = j.get('stack') or {}
        if (s.get('frames') or 0) < a.min_frames: continue
        key = planner_key(j['name']); dest = safe(key) + ('' if j['filter'] == fitted else '_' + j['filter'])
        if dest in plan and plan[dest]['sec'] >= s['total_exposure_s']: continue
        plan[dest] = {'filter': j['filter'], 'src': os.path.dirname(m), 'frames': s['frames'], 'sec': s['total_exposure_s'], 'id': cid(key) or cid(j['name'])}
    live = {}
    for d in sorted(os.listdir(stacks)):
        p = os.path.join(stacks, d)
        if not os.path.isdir(p) or d.startswith('_'): continue
        n, sec, flt = header(os.path.join(p, 'state.bin')); live[d] = {'id': cid(d), 'frames': n, 'sec': sec, 'filter': flt, 'copy': bool(COPY.search(d))}

    # left in place: a live stack through the same filter with more exposure than its final, and one that already is its final (a re-run)
    keep = set()
    for d, v in list(plan.items()):
        l = live.get(d)
        if not l or l['copy'] or l['filter'].lower() != v['filter'].lower(): continue
        if l['frames'] == v['frames'] and abs(l['sec'] - v['sec']) < 1: keep.add(d); del plan[d]
        elif l['sec'] > v['sec']:
            print('keeping the live %s: %d frames, %.0f min, more than the final (%.0f min)' % (d, l['frames'], l['sec'] / 60, v['sec'] / 60)); keep.add(d); del plan[d]
    ids = {v['id'] for v in plan.values()}
    print('%-40s %-6s %6s %5s   %s' % ('live stack after', 'filter', 'frames', 'min', 'moved aside (frames/min; * = a copy the planner does not count)'))
    for dest, v in sorted(plan.items()):
        rep = [d for d, l in live.items() if l['id'] == v['id'] and d not in keep]
        print('%-40s %-6s %6d %5.0f   %s' % (dest, v['filter'], v['frames'], v['sec'] / 60,
                                             ', '.join('%s%s %d/%.0f' % ('*' if live[d]['copy'] else '', d, live[d]['frames'], live[d]['sec'] / 60) for d in rep)))
    print('not touched:', ', '.join(sorted(d for d, l in live.items() if l['id'] not in ids or d in keep)) or '-')
    if not plan: print('nothing to do'); return
    if not a.apply: print('(plan only: --apply to do it)'); return

    arch = os.path.join(stacks, '_fragments_' + time.strftime('%Y%m%d')); os.makedirs(arch, exist_ok=True)
    for d, l in live.items():
        if l['id'] in ids and d not in keep:
            to = os.path.join(arch, d)
            if os.path.exists(to): to += time.strftime('_%H%M%S')
            shutil.move(os.path.join(stacks, d), to)
    for dest, v in plan.items():
        p = os.path.join(stacks, dest); os.makedirs(p)
        for n in ('state.bin', 'preview.png'): shutil.copy2(os.path.join(v['src'], n), os.path.join(p, n))
        last = max(l.split('\t')[3] for l in open(os.path.join(v['src'], 'frames.tsv')) if l.strip())
        t = calendar.timegm(time.strptime(last, '%Y-%m-%dT%H:%M:%S'))
        for x in (os.path.join(p, 'state.bin'), os.path.join(p, 'preview.png'), p): os.utime(x, (t, t))
    print('%d live stacks written; the fragments are in %s' % (len(plan), arch))


if __name__ == '__main__':
    main()
