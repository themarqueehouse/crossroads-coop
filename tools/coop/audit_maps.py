#!/usr/bin/env python3
"""Walk the whole game looking for the faults that strand a player.

Not a style check. Every one of these is a thing that, hit in play, looks like
the game breaking: a door that puts you nowhere, an NPC standing where you
cannot talk to them, two objects sharing an id so a script hides the wrong one,
a trainer with nothing to send out.

The build already catches undefined labels and bad constants -- the assembler
will not link without them -- so none of that is repeated here. What is here is
everything the build is happy to compile and the player is not.

    python3 tools/coop/audit_maps.py [--quiet]

Exit status is 1 if anything was found, so it can gate a build.
"""

import collections
import glob
import json
import os
import re
import struct
import sys

LAYOUTS = 'data/layouts/layouts.json'

# Maps that no door opens onto are not always bugs -- several are leftovers
# the hack never deleted -- so they are reported separately and quietly.
KNOWN_UNUSED = ('UNUSED', 'UNKNOWN', 'TEST', 'DEBUG')


def read_maps():
    maps = {}
    for path in sorted(glob.glob('data/maps/*/map.json')):
        data = json.load(open(path))
        data['_path'] = path
        data['_dir'] = os.path.dirname(path)
        maps[data['id']] = data
    return maps


def read_layouts():
    return {l['id']: l for l in json.load(open(LAYOUTS))['layouts']}


def collision_reader(layout):
    """passable(x, y) for a layout, or None if its blockdata is missing."""
    path = layout.get('blockdata_filepath')
    if not path or not os.path.exists(path):
        return None
    w, h = layout['width'], layout['height']
    raw = open(path, 'rb').read()
    if len(raw) < w * h * 2:
        return None
    blocks = struct.unpack('<%dH' % (w * h), raw[:w * h * 2])

    def passable(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return None          # off the map: the border, not a wall
        return ((blocks[y * w + x] >> 10) & 3) == 0

    return passable


SCRIPT_WARP = re.compile(
    r'\b(?:warp|warpsilent|warphole|warpteleport|warpdoor|warpmossdeepgym|'
    r'setwarp|setdynamicwarp|setescapewarp|setholewarp)\s+(MAP_[A-Z0-9_]+)')


def reachable(maps):
    """Everywhere you can get to, through doors AND through scripts.

    Counting only warp events said a quarter of the game was unreachable,
    which is nonsense: a great many doors in this game are a script running a
    warp command, not a warp event on the tile.
    """
    graph = collections.defaultdict(set)
    for map_id, area in maps.items():
        path = os.path.join(area['_dir'], 'scripts.inc')
        if os.path.exists(path):
            for m in SCRIPT_WARP.finditer(open(path).read()):
                graph[map_id].add(m.group(1))
    for map_id, area in maps.items():
        for warp in (area.get('warp_events') or []):
            dest = warp.get('dest_map')
            if dest and dest != 'MAP_NONE':
                graph[map_id].add(dest)
        for conn in (area.get('connections') or []):
            if conn.get('map'):
                graph[map_id].add(conn['map'])
                graph[conn['map']].add(map_id)
    seen, queue = set(), ['MAP_LITTLEROOT_TOWN']
    while queue:
        here = queue.pop()
        if here in seen or here not in maps:
            continue
        seen.add(here)
        queue.extend(graph[here])
    return seen


def trainer_parties():
    """Trainer id -> how many Pokemon it brings.

    Read from the generated trainers.h, where every trainer carries an
    explicit .partySize. A trainer with none is a battle that starts and then
    has nothing to send out.
    """
    sizes = {}
    for path in ('src/data/trainers.h', 'src/data/trainers_frlg.h'):
        if not os.path.exists(path):
            continue
        text = open(path).read()
        for m in re.finditer(
                r'\[(TRAINER_[A-Z0-9_]+)\]\s*=\s*\{(.*?)\n    \},', text, re.S):
            size = re.search(r'\.partySize\s*=\s*(\d+)', m.group(2))
            if size:
                sizes[m.group(1)] = int(size.group(1))
    return sizes


def main():
    quiet = '--quiet' in sys.argv
    maps = read_maps()
    lays = read_layouts()
    walkable = reachable(maps)

    findings = collections.defaultdict(list)

    # --- warps ----------------------------------------------------------
    #
    # A warp whose destination map does not exist, or whose destination warp
    # id is not a warp on that map, drops the player at the top-left corner of
    # wherever it lands -- usually inside a wall. This is what "Pewter City
    # warp" and "hard lock SS Anne" look like from the data.
    for map_id, area in maps.items():
        warps = area.get('warp_events') or []
        for i, warp in enumerate(warps):
            dest = warp.get('dest_map')
            if not dest or dest in ('MAP_NONE', 'MAP_DYNAMIC', 'MAP_UNDEFINED'):
                continue   # MAP_DYNAMIC is "wherever the game last set", not a map
            if dest not in maps:
                findings['warp to a map that does not exist'].append(
                    '%s warp %d -> %s' % (map_id, i, dest))
                continue
            try:
                want = int(str(warp.get('dest_warp_id')), 0)
            except (TypeError, ValueError):
                continue
            there = len(maps[dest].get('warp_events') or [])
            if want >= there:
                findings['warp to a warp id that is not there'].append(
                    '%s warp %d -> %s warp %d, which has %d'
                    % (map_id, i, dest, want, there))

    # --- local ids ------------------------------------------------------
    #
    # Scripts move, hide and talk to objects by local id. Two objects sharing
    # one means every one of those commands hits whichever comes first, and
    # the other is unreachable -- an NPC who will not move out of a doorway,
    # or a trainer who never walks up to you.
    for map_id, area in maps.items():
        seen = collections.Counter()
        for ev in (area.get('object_events') or []):
            if 'local_id' in ev and ev['local_id']:
                seen[str(ev['local_id'])] += 1
        for local_id, n in seen.items():
            if n > 1:
                findings['two objects sharing a local id'].append(
                    '%s: %s used %d times' % (map_id, local_id, n))

    # --- objects you cannot reach ---------------------------------------
    for map_id, area in maps.items():
        layout = lays.get(area.get('layout'))
        passable = collision_reader(layout) if layout else None
        if passable is None:
            continue
        warp_tiles = {(w['x'], w['y']) for w in (area.get('warp_events') or [])}
        for ev in (area.get('object_events') or []):
            x, y = ev['x'], ev['y']
            if ev.get('script') in (None, '0x0', '0') and not ev.get('trainer_type', '').endswith(
                    ('SEE_ALL_DIRECTIONS', 'NORMAL')):
                continue   # scenery: nobody needs to talk to it
            sides = [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
            # None means off the edge of the map, which is the border rather
            # than a wall -- an object on the edge of a gate is reachable from
            # the map next door.
            if any(passable(*s) is None for s in sides):
                continue
            # Two tiles out as well: a shopkeeper or a nurse stands behind a
            # counter, which is impassable and which you talk across. Without
            # this every Pokemon Centre in the game is a finding.
            near = [(x + dx, y + dy) for dx in range(-2, 3) for dy in range(-2, 3)
                    if abs(dx) + abs(dy) <= 2]
            if not any(passable(*s) for s in sides) and \
               not any(passable(*n) for n in near):
                findings['object nobody can stand next to'].append(
                    '%s: %s at %d,%d' % (map_id, ev.get('script', '?'), x, y))
            if (x, y) in warp_tiles:
                findings['object standing on a warp'].append(
                    '%s: %s at %d,%d' % (map_id, ev.get('script', '?'), x, y))

    # --- maps nothing opens onto ----------------------------------------
    for map_id in sorted(set(maps) - walkable):
        if any(word in map_id for word in KNOWN_UNUSED):
            continue
        if maps[map_id].get('map_type') == 'MAP_TYPE_SECRET_BASE':
            continue
        findings['no door opens onto this map'].append(map_id)

    # --- trainers with nothing to send out -------------------------------
    sizes = trainer_parties()
    used = set()
    for path in glob.glob('data/maps/*/scripts.inc') + glob.glob('data/scripts/*.inc'):
        for m in re.finditer(r'trainerbattle\w*\s+(TRAINER_[A-Z0-9_]+)', open(path).read()):
            used.add(m.group(1))
    for trainer in sorted(used):
        if sizes.get(trainer, 1) == 0:
            findings['trainer with an empty party'].append(trainer)

    # --- report ---------------------------------------------------------
    total = sum(len(v) for v in findings.values())
    if not total:
        print('nothing found')
        return 0

    for kind in sorted(findings, key=lambda k: -len(findings[k])):
        rows = findings[kind]
        print('\n%s (%d)' % (kind, len(rows)))
        show = rows if not quiet else rows[:5]
        for row in show:
            print('    ' + row)
        if len(rows) > len(show):
            print('    ... and %d more' % (len(rows) - len(show)))

    print('\n%d findings' % total)
    return 1


if __name__ == '__main__':
    sys.exit(main())
