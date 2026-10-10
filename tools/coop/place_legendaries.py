#!/usr/bin/env python3
"""Scatter every legendary across the world, one to a place.

The hack has ten legendaries standing in the world -- the Hoenn three, the
Regis, the Kanto birds, Mewtwo -- and about eighty more that exist in the ROM
and are nowhere. This puts each of the missing ones somewhere of its own:
a cave, an island, a ruin, the top of something. No two share a map, and each
is caught or driven off once, for good, like Rayquaza is.

How a spot is chosen:

  lateness  Every map is scored by the levels of the wild Pokemon on it,
            plus what its name says about how far off the path it is -- a
            depths, a summit, a chamber, a B3F. The heavier the legendary,
            the later the map it gets, so nothing enormous is standing
            around on Route 102.
  biome     Then by what suits it. Ice legendaries want an ice cave, fire
            ones a volcano, psychics the ruins, dragons somewhere high. It
            is a preference, not a rule: a legendary with nowhere ideal left
            takes the best remaining spot rather than going unplaced.
  the tile  Read out of the map's own collision data. The spot has to be
            standable, have a standable tile beside it to be talked to
            from, and sit clear of doorways, triggers and anything else
            already on the map.

Each one gets a flag of its own, set when it is beaten or caught, which also
hides the object -- so it does not come back, and running away leaves it where
it was. Flags are allocated from the free space above the hack's own, which
has room for four hundred and does not reach the end of the array.

One consequence worth knowing: the two players share a world, so they share
these. Whoever catches Mew has caught the only Mew.

    python3 tools/coop/place_legendaries.py [--dry-run]

Writes: include/constants/coop_legendaries.h, data/scripts/legendaries.inc,
and an object event into each chosen map.json.
"""

import collections
import glob
import json
import os
import re
import struct
import sys

LAYOUTS = 'data/layouts/layouts.json'
ENCOUNTERS = 'src/data/wild_encounters.json'
SPECIES_INFO = 'src/data/pokemon/species_info/gen_*_families.h'
FLAGS_OUT = 'include/constants/coop_legendaries.h'
SCRIPTS_OUT = 'data/scripts/legendaries.inc'

# Free flag space. The hack's own flags stop at 0xAAD and the array holds 395
# bytes -- 3160 flags -- so everything from here up is ours and well short of
# the end. See the note in the module docstring.
FLAG_BASE = 0xB00

LEGENDARY_FLAGS = {'isSubLegendary', 'isRestrictedLegendary', 'isMythical',
                   'isUltraBeast', 'isParadox'}
FORM_FLAGS = {'isMegaEvolution', 'isGigantamax', 'isTotem', 'isPrimalReversion',
              'isAlolanForm', 'isGalarianForm', 'isHisuianForm', 'isPaldeanForm'}

# What each tier turns up at.
LEVELS = {'isMythical': 50, 'isSubLegendary': 55, 'isUltraBeast': 55,
          'isParadox': 60, 'isRestrictedLegendary': 65}

OUTDOOR_TYPES = {'MAP_TYPE_ROUTE', 'MAP_TYPE_UNDERGROUND', 'MAP_TYPE_UNDERWATER',
                 'MAP_TYPE_OCEAN_ROUTE'}

# Name fragments that say "this is off the beaten path", and what they are
# worth. A legendary at the back of a cave reads very differently from one
# standing on a city route.
REMOTE_WORDS = {
    'DEPTHS': 14, 'SUMMIT': 14, 'INNER': 12, 'CHAMBER': 12, 'RUINS': 12,
    'CRATER': 12, 'SEAFLOOR': 14, 'SKY_PILLAR': 14, 'CAVERN': 12, 'ABYSS': 14,
    'CAVE': 8, 'TUNNEL': 7, 'ISLAND': 7, 'UNDERWATER': 10, 'TOP': 8,
    'B3F': 10, 'B2F': 8, 'B1F': 5, 'FOREST': 5, 'WOODS': 4, 'DESERT': 8,
    'MT_': 8, 'VOLCANO': 12, 'MANSION': 6, 'TOWER': 6, 'HIDEOUT': 6,
    'ENTRANCE': -6, 'OUTSIDE': -3, 'ROUTE1': -4,
}

# Where each type feels at home.
BIOMES = {
    'TYPE_ICE':      ('ICEFALL', 'SHOAL', 'ICE', 'GLACIER', 'SNOW', 'SEAFOAM'),
    'TYPE_FIRE':     ('CHIMNEY', 'MAGMA', 'EMBER', 'VOLCANO', 'MANSION', 'CRATER'),
    'TYPE_WATER':    ('SEAFLOOR', 'UNDERWATER', 'ISLAND', 'CAVERN', 'SHOAL', 'ABANDONED_SHIP'),
    'TYPE_ROCK':     ('CAVE', 'TUNNEL', 'GRANITE', 'DESERT', 'QUARRY', 'MT_'),
    'TYPE_GROUND':   ('CAVE', 'TUNNEL', 'DESERT', 'UNDERGROUND', 'HOLE'),
    'TYPE_STEEL':    ('CAVE', 'MAUVILLE', 'MAGMA', 'TUNNEL'),
    'TYPE_PSYCHIC':  ('RUINS', 'CHAMBER', 'TOWER', 'MONEAN', 'TANOBY', 'DESERT'),
    'TYPE_GHOST':    ('TOWER', 'LOST', 'RUINS', 'CHAMBER', 'MANSION'),
    'TYPE_GRASS':    ('FOREST', 'WOODS', 'BERRY', 'MEADOW', 'GARDEN'),
    'TYPE_BUG':      ('FOREST', 'WOODS', 'PATTERN_BUSH'),
    'TYPE_FLYING':   ('SKY', 'PILLAR', 'TOP', 'SUMMIT', 'METEOR'),
    'TYPE_DRAGON':   ('SKY', 'PILLAR', 'METEOR', 'CAVERN', 'SUMMIT'),
    'TYPE_ELECTRIC': ('POWER_PLANT', 'MAUVILLE', 'GENERATOR'),
    'TYPE_DARK':     ('HIDEOUT', 'LOST', 'HOLE', 'DEPTHS', 'MANSION'),
    'TYPE_POISON':   ('HIDEOUT', 'HOLE', 'SEWER', 'MANSION'),
    'TYPE_FAIRY':    ('MEADOW', 'GARDEN', 'FOREST', 'ISLAND'),
    'TYPE_FIGHTING': ('VICTORY_ROAD', 'TUNNEL', 'CAVE', 'DOJO'),
}

ENTRY = re.compile(r'\[(SPECIES_[A-Z0-9_]+)\]\s*=\s*\{', re.M)
STATS = ('baseHP', 'baseAttack', 'baseDefense', 'baseSpeed', 'baseSpAttack',
         'baseSpDefense')


def read_species():
    out = {}
    for path in sorted(glob.glob(SPECIES_INFO)):
        text = open(path).read()
        starts = [(m.group(1), m.start(), m.end()) for m in ENTRY.finditer(text)]
        for i, (name, start, body_at) in enumerate(starts):
            end = starts[i + 1][1] if i + 1 < len(starts) else len(text)
            body = text[body_at:end]
            stats = []
            for field in STATS:
                m = re.search(r'\.%s\s*=\s*(\d+)' % field, body)
                stats.append(int(m.group(1)) if m else None)
            if any(s is None for s in stats):
                continue
            types = re.search(r'\.types\s*=\s*MON_TYPES\(([^)]*)\)', body)
            dex = re.search(r'\.natDexNum\s*=\s*NATIONAL_DEX_([A-Z0-9_]+)', body)
            nm = re.search(r'\.speciesName\s*=\s*_\("([^"]*)"\)', body)
            out[name] = {
                'bst': sum(stats),
                'types': [t.strip() for t in types.group(1).split(',')] if types else [],
                'flags': set(re.findall(r'\.(is[A-Za-z]+)\s*=\s*TRUE', body)),
                'dex': dex.group(1) if dex else None,
                'name': nm.group(1) if nm else name[8:].title(),
            }
    return out


def already_placed():
    placed = set()
    for path in glob.glob('data/maps/*/scripts.inc') + glob.glob('data/scripts/*.inc'):
        for m in re.finditer(r'setwildbattle\s+(SPECIES_[A-Z0-9_]+)', open(path).read()):
            placed.add(m.group(1))
    return placed


def read_maps():
    maps = {}
    for path in sorted(glob.glob('data/maps/*/map.json')):
        d = json.load(open(path))
        d['_path'] = path
        maps[d['id']] = d
    return maps


def map_levels():
    """Mean wild level per map, as a stand-in for how late in the game it is."""
    out = {}
    data = json.load(open(ENCOUNTERS))['wild_encounter_groups'][0]['encounters']
    for area in data:
        levels = []
        for key, table in area.items():
            if isinstance(table, dict) and 'mons' in table:
                levels += [m['min_level'] for m in table['mons']]
        if levels:
            out.setdefault(area['map'], []).extend(levels)
    return {k: sum(v) / len(v) for k, v in out.items()}


def remoteness(map_id, levels):
    score = levels.get(map_id, 20.0)
    for word, worth in REMOTE_WORDS.items():
        if word in map_id:
            score += worth
    return score


# A dungeon's rooms and floors are one place, not eight. Without this the
# heaviest legendaries all land in whichever cave scores highest -- the first
# run put five box legendaries in five rooms of Seafloor Cavern, which is one
# cave with a queue in it rather than five places in the world.
FAMILY_SUFFIXES = (r'_ROOM\d+$', r'_B?\d+F$', r'_PATH_B?\d+F$', r'_PATH_\d+$',
                   r'_ENTRANCE$', r'_EXIT$', r'_INNER$', r'_OUTSIDE$',
                   r'_STAIRS$', r'_CORRIDORS?$', r'_SUMMIT$', r'_TOP$',
                   r'_[A-Z]+_CHAMBER$', r'_CHAMBER$',
                   r'_1$', r'_2$', r'_3$')


def family(map_id):
    out = map_id
    for pattern in FAMILY_SUFFIXES:
        out = re.sub(pattern, '', out)
    return out


def biome_fit(map_id, types):
    for t in types:
        for word in BIOMES.get(t, ()):
            if word in map_id:
                return True
    return False


def reachable_maps(maps):
    """Every map you can actually walk to, from the first town outwards.

    Walked over the warps and the map connections, which is what the game
    itself walks over. The first pass without this put legendaries in
    CAVE_OF_ORIGIN_UNUSED_RUBY_SAPPHIRE_MAP3 and two of the leftover Aqua
    Hideout maps -- rooms that exist in the data and that no door in the game
    opens onto. A legendary nobody can reach is the exact complaint this is
    meant to answer.

    Ticket islands fall out of this for free: the boat to Navel Rock is a
    script, not a door, so nothing warps there and it is never reached.
    """
    graph = collections.defaultdict(set)
    for map_id, area in maps.items():
        for warp in (area.get('warp_events') or []):
            if warp.get('dest_map') and warp['dest_map'] != 'MAP_NONE':
                graph[map_id].add(warp['dest_map'])
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


def layouts():
    return {l['id']: l for l in json.load(open(LAYOUTS))['layouts']}


def find_spot(area, layout):
    """A standable tile with a standable neighbour, clear of everything else.

    Tried with a wide berth around the warps first and a narrower one after:
    a two-tile ring is right for a cave floor and impossible in a corridor,
    and a corridor with a legendary at the end of it is better than one with
    nothing in it.
    """
    for clearance in (2, 1, 0):
        spot = find_spot_with(area, layout, clearance)
        if spot:
            return spot
    return None


def find_spot_with(area, layout, clearance):
    path = layout['blockdata_filepath']
    if not os.path.exists(path):
        return None
    w, h = layout['width'], layout['height']
    raw = open(path, 'rb').read()
    if len(raw) < w * h * 2:
        return None
    blocks = struct.unpack('<%dH' % (w * h), raw[:w * h * 2])

    def at(x, y):
        return blocks[y * w + x]

    def passable(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return False
        return ((at(x, y) >> 10) & 3) == 0

    # Everything already claiming a tile, with a ring around the warps so a
    # legendary never ends up standing in a doorway.
    taken = set()
    for ev in (area.get('object_events') or []):
        taken.add((ev['x'], ev['y']))
    for ev in (area.get('coord_events') or []):
        taken.add((ev['x'], ev['y']))
    for ev in (area.get('bg_events') or []):
        taken.add((ev['x'], ev['y']))
    for ev in (area.get('warp_events') or []):
        for dx in range(-clearance, clearance + 1):
            for dy in range(-clearance, clearance + 1):
                taken.add((ev['x'] + dx, ev['y'] + dy))

    best = None
    cx, cy = w / 2.0, h / 2.0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            if (x, y) in taken or not passable(x, y):
                continue
            neighbours = [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
            free = [n for n in neighbours if passable(*n) and n not in taken]
            if len(free) < 2 or blocks_the_way(passable, x, y, free):
                continue
            # Out in the open, not wedged in a gap. A legendary standing in a
            # one-tile corridor is a locked door: you cannot walk past it, and
            # if you cannot beat it either you are stuck. The check above is
            # the real guard -- the tile must not be the only way through --
            # and this just prefers somewhere it can be seen and walked round.
            score = len(free) - abs(x - cx) / w - abs(y - cy) / h
            if best is None or score > best[0]:
                best = (score, x, y, (at(x, y) >> 12) & 15)
    if best is None:
        return None
    return {'x': best[1], 'y': best[2], 'elevation': best[3]}


def blocks_the_way(passable, x, y, free):
    """True if standing here cuts the map in two.

    Flood fill from one free neighbour with this tile treated as solid, and
    see whether the others are still reachable. Bounded, because a few of
    these maps are a hundred tiles across and the answer is always found in
    the first room.
    """
    start = free[0]
    seen = {start}
    queue = [start]
    want = set(free[1:])
    steps = 0
    while queue and want and steps < 6000:
        steps += 1
        cx, cy = queue.pop()
        want.discard((cx, cy))
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if (nx, ny) == (x, y) or (nx, ny) in seen or not passable(nx, ny):
                continue
            seen.add((nx, ny))
            queue.append((nx, ny))
    return bool(want)


SCRIPT = """
{label}::
\tlockall
\twaitse
\tplaymoncry {species}, CRY_MODE_ENCOUNTER
\tdelay 40
\twaitmoncry
\tsetwildbattle {species}, {level}
\tsetflag FLAG_SYS_CTRL_OBJ_DELETE
\tspecial BattleSetup_StartLegendaryBattle
\twaitstate
\tclearflag FLAG_SYS_CTRL_OBJ_DELETE
\tspecialvar VAR_RESULT, GetBattleOutcome
\tgoto_if_eq VAR_RESULT, B_OUTCOME_RAN, Legendary_EventScript_Leave
\tgoto_if_eq VAR_RESULT, B_OUTCOME_PLAYER_TELEPORTED, Legendary_EventScript_Leave
\tsetflag {flag}
\treleaseall
\tend
"""

PREAMBLE = """@ Legendary encounters, generated by tools/coop/place_legendaries.py.
@
@ One per map, each with a flag of its own. The flag is set when the Pokemon
@ is beaten or caught and is also the object's hide flag, so it goes away and
@ stays away -- while running from it, or losing to it, leaves it standing
@ exactly where it was.
@
@ Do not edit by hand: re-run the tool.

@ Ran away, or teleported out. Nothing is set, so it is still there.
Legendary_EventScript_Leave::
\treleaseall
\tend
"""


def main():
    dry_run = '--dry-run' in sys.argv

    info = read_species()
    placed = already_placed()
    maps = read_maps()
    lays = layouts()
    levels = map_levels()

    # One species per dex number; legendaries only; no alternate forms.
    by_dex = {}
    for name, sp in sorted(info.items()):
        if not sp['dex'] or sp['flags'] & FORM_FLAGS:
            continue
        if not (sp['flags'] & LEGENDARY_FLAGS):
            continue
        by_dex.setdefault(sp['dex'], name)
    wanted = [n for n in by_dex.values() if n not in placed]

    def tier(name):
        flags = info[name]['flags']
        for key in ('isRestrictedLegendary', 'isParadox', 'isUltraBeast',
                    'isSubLegendary', 'isMythical'):
            if key in flags:
                return key
        return 'isSubLegendary'

    wanted.sort(key=lambda n: (info[n]['bst'], n))

    # Candidate maps: outdoors and caves, with a layout, not already hosting a
    # legendary, ranked by how far off the path they feel.
    hosting = set()
    for area in maps.values():
        text_path = os.path.join(os.path.dirname(area['_path']), 'scripts.inc')
        if os.path.exists(text_path) and 'setwildbattle' in open(text_path).read():
            hosting.add(area['id'])

    walkable = reachable_maps(maps)
    candidates = []
    for map_id, area in maps.items():
        if area.get('map_type') not in OUTDOOR_TYPES or map_id in hosting:
            continue
        if area['layout'] not in lays or map_id not in walkable:
            continue
        if 'UNUSED' in map_id:
            continue
        candidates.append(map_id)
    candidates.sort(key=lambda m: (-remoteness(m, levels), m))

    print('%d legendaries in the ROM, %d already standing somewhere, %d to place'
          % (len(by_dex), len(by_dex) - len(wanted), len(wanted)))
    print('%d candidate maps' % len(candidates))

    # Strongest to the most remote, with biome as the tie-break: walk the
    # legendaries from heaviest down, and give each the best map left that
    # suits it, falling back to the best map left at all.
    assignments = []
    used = set()
    families = collections.Counter()

    def choose(name, family_limit):
        sp = info[name]
        for want_biome in (True, False):
            for map_id in candidates:
                if map_id in used or families[family(map_id)] >= family_limit:
                    continue
                if want_biome and not biome_fit(map_id, sp['types']):
                    continue
                return map_id
        return None

    for name in reversed(wanted):
        # One to a dungeon first. Only once every cave in Hoenn and Kanto has
        # one does a second go anywhere, and even then not in the same room.
        pick = choose(name, 1) or choose(name, 2) or choose(name, 99)
        if pick is None:
            print('  no map left for ' + info[name]['name'])
            continue
        used.add(pick)
        families[family(pick)] += 1
        assignments.append((name, pick))

    # A tile on each, dropping any map that turns out to have nowhere to stand.
    final = []
    for name, map_id in assignments:
        spot = find_spot(maps[map_id], lays[maps[map_id]['layout']])
        if spot is None:
            print('  nowhere to stand on %s (%s)' % (map_id, info[name]['name']))
            continue
        final.append((name, map_id, spot))

    print('%d placed' % len(final))
    for i, (name, map_id, spot) in enumerate(final[:12]):
        print('    %-12s L%-3d %-42s (%d,%d)'
              % (info[name]['name'], LEVELS[tier(name)], map_id, spot['x'], spot['y']))
    if len(final) > 12:
        print('    ... and %d more' % (len(final) - 12))

    if dry_run:
        return

    # --- flags ----------------------------------------------------------
    lines = ['#ifndef GUARD_CONSTANTS_COOP_LEGENDARIES_H',
             '#define GUARD_CONSTANTS_COOP_LEGENDARIES_H',
             '',
             '// One flag per scattered legendary, generated by',
             '// tools/coop/place_legendaries.py. Set when it is beaten or caught,',
             '// and used as the object\'s hide flag so it does not come back.',
             '//',
             '// Allocated from 0x%X. The hack\'s own flags stop at 0xAAD and the' % FLAG_BASE,
             '// array holds 3160, so this range is free and well short of the end.',
             '']
    for i, (name, map_id, spot) in enumerate(final):
        lines.append('#define FLAG_LEGENDARY_%-22s 0x%X' % (name[8:], FLAG_BASE + i))
    lines += ['', '#endif // GUARD_CONSTANTS_COOP_LEGENDARIES_H', '']
    open(FLAGS_OUT, 'w').write('\n'.join(lines))

    # --- scripts --------------------------------------------------------
    out = [PREAMBLE]
    for name, map_id, spot in final:
        out.append(SCRIPT.format(label='Legendary_EventScript_' + name[8:].title(),
                                 species=name, level=LEVELS[tier(name)],
                                 flag='FLAG_LEGENDARY_' + name[8:]))
    open(SCRIPTS_OUT, 'w').write('\n'.join(out))

    # --- the object events ----------------------------------------------
    for name, map_id, spot in final:
        area = maps[map_id]
        area.setdefault('object_events', [])
        area['object_events'].append({
            'graphics_id': 'OBJ_EVENT_GFX_SPECIES(%s)' % name[8:],
            'x': spot['x'],
            'y': spot['y'],
            'elevation': spot['elevation'],
            'movement_type': 'MOVEMENT_TYPE_LOOK_AROUND',
            'movement_range_x': 1,
            'movement_range_y': 1,
            'trainer_type': 'TRAINER_TYPE_NONE',
            'trainer_sight_or_berry_tree_id': '0',
            'script': 'Legendary_EventScript_' + name[8:].title(),
            'flag': 'FLAG_LEGENDARY_' + name[8:],
        })
        path = area.pop('_path')
        with open(path, 'w') as f:
            json.dump(area, f, indent=2)
            f.write('\n')
        area['_path'] = path

    print('wrote %s, %s, and %d map files' % (FLAGS_OUT, SCRIPTS_OUT, len(final)))


if __name__ == '__main__':
    main()
