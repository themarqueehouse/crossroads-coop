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

    # --- items handed over after the giver is already spent ---------------
    #
    # giveitem can fail: a full pocket and the item is not added, and VAR_RESULT
    # comes back FALSE. A script that has ALREADY set its "you have had this"
    # flag, or removed the ball the item came out of, has nothing left to offer
    # on a second visit -- so the item is gone for the rest of the game.
    #
    # For a Great Ball that is a nuisance. For the Tea, the Silph Scope or the
    # Magma Emblem it is the end of the story: each one is the only key to a
    # place the plot goes next.
    #
    # The shared bag makes this likelier here than in a single-player game.
    # Two players fill one sixteen-slot ball pocket.
    # Checked by whoever calls them, not in their own body.
    CHECKED_BY_CALLER = {
        # Std_FindItem does checkitemspace and only calls this when there is
        # room. Every item ball in the game goes through it.
        'EventScript_PickUpItem',
        # The player's first Pokemon, handed over with an empty party.
        'PalletTown_ProfessorOaksLab_EventScript_ChoseStarter',
        # A debug script the game marks "functionally unused" and never calls.
        'LilycoveCity_ContestLobby_EventScript_SetDebug',
    }
    for path in glob.glob('data/maps/*/scripts.inc') + glob.glob('data/scripts/*.inc'):
        if path.endswith('debug.inc'):
            continue
        lines = open(path).read().split('\n')
        label, committed, guarded = None, None, False
        for i, line in enumerate(lines):
            text = line.strip()
            # One colon or two: a local label is still a new label, and reading
            # only `::` let a flag set at the end of one script look like it
            # belonged to the next.
            m = re.match(r'([A-Za-z_]\w*):{1,2}$', text)
            if m:
                label, committed, guarded = m.group(1), None, False
                continue
            # A space check before the handover is the correct pattern and
            # makes the order below harmless.
            if re.match(r'(checkitemspace|getpartysize|checkpcitem)\b', text):
                guarded = True
            if re.match(r'(removeobject|hideobject|setflag FLAG_(HIDE|ITEM|HIDDEN_ITEM|'
                        r'RECEIVED|GOT|CAN_USE))', text):
                committed = (i + 1, text)
            if re.match(r'(giveitem|additem|giveegg|givemon)\b', text) and committed \
               and not guarded and label not in CHECKED_BY_CALLER:
                # A one-shot scene cannot offer the item again, so the fix there
                # is the PC rather than a retry. Look ahead for it.
                if 'Common_EventScript_SendItemToPc' in '\n'.join(lines[i:i + 8]):
                    continue
                findings['item handed over after the giver is spent'].append(
                    '%s %s: line %d %s, then line %d %s'
                    % (path, label, committed[0], committed[1], i + 1, text))

    # --- trainers with nothing to send out -------------------------------
    sizes = trainer_parties()
    used = set()
    for path in glob.glob('data/maps/*/scripts.inc') + glob.glob('data/scripts/*.inc'):
        for m in re.finditer(r'trainerbattle\w*\s+(TRAINER_[A-Z0-9_]+)', open(path).read()):
            used.add(m.group(1))
    for trainer in sorted(used):
        if sizes.get(trainer, 1) == 0:
            findings['trainer with an empty party'].append(trainer)

    # --- story battles nobody wired for co-op ----------------------------
    #
    # The point of the hack. A gym leader, a rival, an admin or a champion is
    # meant to be one battle with both players in it; a script that was missed
    # when the wiring went in gives one player the fight and the other a room
    # where nothing happens. Juan's rematch was the one that got missed, and
    # there was no way to notice except by beating Juan twice.
    #
    # Checked per script rather than per line: the wiring sits at the top and
    # the battle can be forty lines of cutscene below it.
    important = re.compile(
        r'TRAINER_(BRAWLY|ROXANNE|WATTSON|FLANNERY|NORMAN|WINONA|TATE|LIZA|JUAN'
        r'|SIDNEY|PHOEBE|GLACIA|DRAKE|WALLACE|STEVEN|MAY_|BRENDAN_|WALLY|ARCHIE'
        r'|MAXIE|MATT|SHELLY|TABITHA|COURTNEY|BROCK|MISTY|LT_SURGE|ERIKA|KOGA'
        r'|SABRINA|BLAINE|GIOVANNI|LORELEI|BRUNO|AGATHA|LANCE|RIVAL|TERRY|GARY'
        r'|BLUE|RED|LEAF|CHAMPION)')
    wiring = re.compile(r'coopscene|coopnextbattle|coopgate|coop_require_partner')
    for path in glob.glob('data/maps/*/scripts.inc') + glob.glob('data/scripts/*.inc'):
        lines = open(path).read().split('\n')
        blocks, cur = [], None
        for i, line in enumerate(lines):
            m = re.match(r'([A-Za-z_]\w*):{1,2}$', line.strip())
            if m:
                cur = (m.group(1), [])
                blocks.append(cur)
            elif cur:
                cur[1].append((i + 1, line))
        for name, body in blocks:
            if wiring.search('\n'.join(l for _, l in body)):
                continue
            for lineno, line in body:
                m = re.match(r'\s*trainerbattle\w*\s+(.*)', line)
                if not m:
                    continue
                trainer = next((a.strip() for a in m.group(1).split(',')
                                if a.strip().startswith('TRAINER_')), '')
                if important.match(trainer):
                    findings['story battle not wired for co-op'].append(
                        '%s %s: line %d %s' % (path, name, lineno, trainer))

    # --- trigger scenes on a shared var, not wired for co-op --------------
    #
    # The other half of the co-op wiring problem, and the harder half to
    # notice. A trigger scene is one the game starts AT the player: an on-frame
    # table matching a story var. When that var is shared -- and the story vars
    # all are -- the console that gets there first runs the scene and advances
    # the var, and the trigger stops matching for the other player, who is left
    # standing on the tile with nothing going to happen and no way to make it.
    # Several of them warp as well, which separates the pair outright.
    #
    # Only scenes that say something, move the player or start a fight count. A
    # trigger that poses the camera or places an NPC is presentation, belongs to
    # each console, and is correctly unshared -- as is anything keyed on a
    # VAR_TEMP_*, which is per-map scratch rather than shared state.
    bodies = {}
    for path in sorted(glob.glob('data/maps/*/scripts.inc')
                       + glob.glob('data/scripts/*.inc')):
        cur = None
        for i, line in enumerate(open(path).read().split('\n')):
            m = re.match(r'([A-Za-z_]\w*):{1,2}$', line.strip())
            if m:
                cur = m.group(1)
                bodies[cur] = []
            elif cur is not None:
                bodies[cur].append(line)

    def script_text(label, seen=()):
        """A script plus whatever it hands straight off to."""
        if label not in bodies or label in seen or len(seen) > 4:
            return ''
        text = '\n'.join(bodies[label])
        for m in re.finditer(r'^\s*(?:goto|call)(?:_if\w*)?\s+(?:[A-Z_0-9]+,\s*)?'
                             r'([A-Za-z_]\w*)\s*$', text, re.M):
            text += '\n' + script_text(m.group(1), seen + (label,))
        return text

    does_something = re.compile(r'\b(msgbox|message|warp\w*|trainerbattle\w*|givemon|'
                                r'giveitem|fadescreenswapbuffers|playbgm)\b')

    # Scenes that are meant to belong to one console.
    PER_CONSOLE = {
        # Each player walks their own path across the ice and falls through
        # their own tile. Mirroring it would drop the partner through the floor
        # from wherever they happened to be standing.
        'SootopolisCity_Gym_1F_EventScript_FallThroughIce',
        # The roaming weather's own cleanup, which runs on whichever console
        # notices and is idempotent.
        'AbnormalWeather_EventScript_EndEventAndCleanup_1',
        # Side content, entered and played one at a time by design.
        'ContestHall_EventScript_Contest',
        'LilycoveCity_ContestLobby_EventScript_TryDoContestArtist',
        'LilycoveCity_ContestLobby_EventScript_TryDoLinkContestArtist',
        'BattleFrontier_ReceptionGate_EventScript_FirstTimeEntering',
        'FuchsiaCity_SafariZone_Entrance_EventScript_ExitWarpIn',
        'FuchsiaCity_SafariZone_Entrance_EventScript_ExitEarly',
        'FuchsiaCity_SafariZone_Entrance_EventScript_ExitWalkIn',
    }
    for path in glob.glob('data/maps/*/scripts.inc'):
        area = os.path.basename(os.path.dirname(path))
        for m in re.finditer(r'map_script_2\s+(VAR_[A-Z0-9_]+),\s*\d+,\s*([A-Za-z_]\w*)',
                             open(path).read()):
            var, label = m.group(1), m.group(2)
            if var.startswith('VAR_TEMP'):
                continue
            text = script_text(label)
            if label in PER_CONSOLE or wiring.search(text) \
               or not does_something.search(text):
                continue
            findings['trigger scene on a shared var, not wired for co-op'].append(
                '%s: %s keyed on %s' % (area, label, var))

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
