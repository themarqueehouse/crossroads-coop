#!/usr/bin/env python3
"""Put Pokemon from every generation into the wild encounter tables.

The hack defines a thousand species and lets you catch 235 of them. Everything
from Unova onwards exists in the ROM and appears nowhere in the world, which
from inside the game is indistinguishable from not being there at all.

What this does NOT do is rewrite the tables. Every species an area had, it
keeps: the first two slots of each are left exactly as they were, so Route 101
is still Zigzagoon and Poochyena and Wurmple, and the levels, the encounter
rates and the shape of the game's difficulty curve are untouched. What it
replaces is the THIRD and later copies of the same species in the same table --
Route 101 lists Wurmple four times and Poochyena four times -- and there are
four thousand of those. Those repeats are where the rest of the dex goes.

So the common Pokemon of an area stay common and stay its own; the rarer slots
behind them become somebody from another region. A player who walks into the
first patch of grass still meets Hoenn, and a player who stays a while meets
the world.

Placement rules, in the order they matter:

  terrain  Water tables take Water-types and nothing else. Land tables take
           anything that is not a pure Water-type. Rock Smash prefers the
           things that live under rocks.
  level    The slot keeps its own levels, so a species may only go somewhere
           its stage suits: a first-stage Pokemon anywhere, something that has
           already evolved once no earlier than level 16, twice no earlier
           than 32. Single-stage Pokemon with the stats of a final form (a
           Tauros, a Lapras) wait until 25.
  spread   Every species is placed somewhere before any species is placed
           twice, so nothing is left uncatchable in order to put a fourth
           Zubat on a route.

Legendaries, mythicals, Ultra Beasts, Paradox forms, Megas, Gigantamax forms,
Totems and the regional variants are all left out: a legendary is a story
event, not grass, and the variant forms want their own handling rather than a
slot in a table.

Run from the repository root; it rewrites src/data/wild_encounters.json in
place. It is a one-way transformation -- once the repeats are gone there is
nothing left for a second run to replace -- so it is kept here for the record
of how the tables were built rather than as part of the build.

    python3 tools/coop/spread_species.py [--dry-run]
"""

import collections
import glob
import json
import random
import re
import sys

ENCOUNTERS = 'src/data/wild_encounters.json'
SPECIES_INFO = 'src/data/pokemon/species_info/gen_*_families.h'

# How many copies of a species an area is allowed to keep before the rest of
# its slots are given away. Two, so a route still has a signature Pokemon you
# meet more than once, and so the common slots stay common.
KEEP_COPIES = 2

# Not grass. A legendary that can be run into on a route is not a legendary,
# and the alternate forms need their own handling rather than a slot.
BANNED_FLAGS = {
    'isMegaEvolution', 'isGigantamax', 'isTotem', 'isPrimalReversion',
    'isSubLegendary', 'isRestrictedLegendary', 'isMythical', 'isUltraBeast',
    'isParadox', 'isAlolanForm', 'isGalarianForm', 'isHisuianForm',
    'isPaldeanForm',
}

ENTRY = re.compile(r'\[(SPECIES_[A-Z0-9_]+)\]\s*=\s*\{', re.M)
STATS = ('baseHP', 'baseAttack', 'baseDefense', 'baseSpeed', 'baseSpAttack',
         'baseSpDefense')

# Things that live in caves, roughly. Only a preference: a cave that cannot be
# filled from this list is filled from anything that fits.
CAVE_TYPES = {'TYPE_ROCK', 'TYPE_GROUND', 'TYPE_DARK', 'TYPE_POISON',
              'TYPE_STEEL', 'TYPE_GHOST', 'TYPE_DRAGON', 'TYPE_BUG'}
CAVE_WORDS = ('CAVE', 'MT_', 'TUNNEL', 'VICTORY_ROAD', 'DESERT', 'GRANITE',
              'METEOR', 'SEAFLOOR', 'SHOAL', 'SKY_PILLAR', 'MAGMA', 'AQUA')
ROCK_SMASH_TYPES = {'TYPE_ROCK', 'TYPE_GROUND', 'TYPE_STEEL', 'TYPE_BUG'}


def balanced(text, open_paren):
    depth = 0
    for i in range(open_paren, len(text)):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
    return ''


def read_species():
    """Every species in the ROM, with what is needed to place it."""
    info, evolves_from = {}, {}

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
                continue  # not a real species entry

            types = re.search(r'\.types\s*=\s*MON_TYPES\(([^)]*)\)', body)
            dex = re.search(r'\.natDexNum\s*=\s*NATIONAL_DEX_([A-Z0-9_]+)', body)

            info[name] = {
                'bst': sum(stats),
                'types': [t.strip() for t in types.group(1).split(',')] if types else [],
                'flags': set(re.findall(r'\.(is[A-Za-z]+)\s*=\s*TRUE', body)),
                'dex': dex.group(1) if dex else None,
            }

            m = re.search(r'\.evolutions\s*=\s*EVOLUTION\s*\(', body)
            if m:
                for into in set(re.findall(r'SPECIES_[A-Z0-9_]+',
                                           balanced(body, m.end() - 1))):
                    evolves_from.setdefault(into, set()).add(name)

    def stage(species, seen=()):
        before = [p for p in evolves_from.get(species, ()) if p not in seen]
        if not before:
            return 0
        return 1 + max(stage(p, seen + (species,)) for p in before)

    for name in info:
        info[name]['stage'] = stage(name)

    return info


def earliest_level(species):
    """The lowest level this species has any business turning up at."""
    stage, bst = species['stage'], species['bst']
    if stage >= 3:
        return 40
    if stage == 2:
        return 32
    if stage == 1:
        return 16
    # Single-stage. Most are fine anywhere; the ones built like a final
    # evolution are not.
    if bst > 450:
        return 25
    if bst > 350:
        return 10
    return 1


def usable(info):
    """One species per dex number, minus everything that is not wild game."""
    by_dex = {}
    for name, sp in info.items():
        if not sp['dex'] or sp['flags'] & BANNED_FLAGS:
            continue
        by_dex.setdefault(sp['dex'], name)   # the base form comes first
    return set(by_dex.values())


def fits(species, info, table_kind, map_name, level):
    sp = info[species]
    types = set(sp['types'])

    if earliest_level(sp) > level:
        return False

    if table_kind in ('water_mons', 'fishing_mons'):
        return 'TYPE_WATER' in types
    if table_kind == 'rock_smash_mons':
        return bool(types & ROCK_SMASH_TYPES)
    # Land. A pure Water-type does not belong in grass.
    return types != {'TYPE_WATER'}


def preferred(species, info, map_name):
    """A soft nudge, so caves feel like caves."""
    if any(word in map_name for word in CAVE_WORDS):
        return bool(set(info[species]['types']) & CAVE_TYPES)
    return True


def main():
    dry_run = '--dry-run' in sys.argv
    rng = random.Random(20261010)   # fixed, so two runs agree

    info = read_species()
    pool = usable(info)

    data = json.load(open(ENCOUNTERS))
    encounters = data['wild_encounter_groups'][0]['encounters']

    # What is already catchable, and which slots are repeats.
    present = set()
    openings = []            # (map, kind, mon dict, level)
    for area in encounters:
        map_name = area.get('map', '')
        for kind, table in area.items():
            if not isinstance(table, dict) or 'mons' not in table:
                continue
            seen = collections.Counter()
            for mon in table['mons']:
                present.add(mon['species'])
                seen[mon['species']] += 1
                if seen[mon['species']] > KEEP_COPIES:
                    openings.append((map_name, kind, mon, mon['min_level']))

    wanted = sorted(pool - present)
    rng.shuffle(wanted)
    rng.shuffle(openings)

    print('%d species in the ROM, %d of them wild game' % (len(info), len(pool)))
    print('%d already catchable, %d not' % (len(present & pool), len(wanted)))
    print('%d repeated slots to give away' % len(openings))

    # Round by round: everybody gets a first home before anybody gets a second.
    placed = collections.Counter()
    filled = 0
    rounds = 0
    while openings and rounds < 6:
        rounds += 1
        queue = [s for s in wanted if placed[s] == rounds - 1]
        if not queue:
            break
        rng.shuffle(queue)
        left = []
        for map_name, kind, mon, level in openings:
            choice = None
            # Two passes: first the ones that suit the place, then any that fit.
            for want_flavour in (True, False):
                for species in queue:
                    if placed[species] >= rounds:
                        continue
                    if not fits(species, info, kind, map_name, level):
                        continue
                    if want_flavour and not preferred(species, info, map_name):
                        continue
                    choice = species
                    break
                if choice:
                    break
            if not choice:
                left.append((map_name, kind, mon, level))
                continue
            mon['species'] = choice
            placed[choice] += 1
            filled += 1
            queue.remove(choice)
            queue.append(choice)   # back of the line for the next round
        openings = left
        print('  round %d: %d slots filled, %d species still homeless'
              % (rounds, filled, sum(1 for s in wanted if not placed[s])))

    homeless = [s for s in wanted if not placed[s]]
    print('%d slots rewritten; %d species now catchable; %d could not be placed'
          % (filled, sum(1 for s in wanted if placed[s]), len(homeless)))
    if homeless:
        print('  not placed: ' + ', '.join(h[8:].title() for h in homeless[:40])
              + (' ...' if len(homeless) > 40 else ''))

    if dry_run:
        return

    with open(ENCOUNTERS, 'w') as f:
        json.dump(data, f, indent=2)
        f.write('\n')
    print('wrote ' + ENCOUNTERS)


if __name__ == '__main__':
    main()
