"""Generate emulator/src/core/bbsc_messages.inc: BBSC's replacement texts for the co-op messages.

Every replaced message is checked against the game's own English text (msg/engus/menu.msgbnd):
the id must exist and must say what we expect before it is replaced, and boss messages keep the
game's own boss-name tag (<?npcNameId@N?>) so each one still names its boss.

Only messages that are wrong in seamless co-op are replaced (the guest is not sent home, the
session goes on after deaths and bosses). Messages about really leaving - the Silencing Blank,
a lost connection, an adversary - are left exactly as the game has them.
"""
import os
import re
import sys

import bbfiles

sys.stdout.reconfigure(encoding='utf-8')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'emulator', 'src', 'core', 'bbsc_messages.inc')

EVENT_TEXT = 30  # イベントテキスト.fmg

files = bbfiles.read_bnd4(bbfiles.read_dcx(bbfiles.GAME + r'\msg\engus\menu.msgbnd.dcx'))
by_category = {fid: bbfiles.read_fmg(data) for _, (fid, data) in files.items()}
event_text = by_category[EVENT_TEXT]

entries = []  # (category, id, new text, original text)


def replace(msg_id, expect, text, category=EVENT_TEXT):
    original = by_category[category].get(msg_id)
    if original is None:
        raise SystemExit('message %d/%d does not exist' % (category, msg_id))
    if not re.search(expect, original):
        raise SystemExit('message %d/%d is %r, expected /%s/' % (category, msg_id, original, expect))
    entries.append((category, msg_id, text, original))


# 2026-10-07 (the user): every co-op line short and plain, saying what really happens.
# "The host" / "a player" instead of beckoner / cooperator / resonance.

# Someone joins (host's screen).
replace(102001, r'bell\'s resonance has beckoned <\?joinName\?>', '<?joinName?> joined your game.')
for i in (102003, 102004, 102005):
    replace(i, r'^<\?joinName\?> has arrived', '<?joinName?> joined your game.')
for i in (102006, 102007, 102056, 102057):
    replace(i, r'^Confederate <\?joinName\?> has arrived', '<?joinName?> joined your game.')
for i in range(102051, 102056):
    replace(i, r'^Old Hunter <\?joinName\?> has arrived', '<?joinName?> joined your game.')

# Joining someone (guest's screen).
for i in (102100, 102200, 102600, 102700):
    replace(i, r'<\?hostName\?>', "You joined <?hostName?>'s game.")

# Deaths: nobody is sent home any more.
for i in (103100, 103200, 103300, 103400, 103500):
    replace(i, r'^You died\.\s*(\n)?Returning to your world\.$', 'You died.')
replace(103101, r'^Your beckoner has died\.', 'The host died.\nYou stay in their game.')
replace(103001, r'^Cooperator <\?deadName\?> has died\.$', '<?deadName?> died.\nThey will respawn.')
for i in (103006, 103007, 103056, 103057):
    replace(i, r'^Confederate <\?deadName\?> has died\.$', '<?deadName?> died.\nThey will respawn.')
for i in range(103051, 103056):
    replace(i, r'^Old Hunter <\?deadName\?> has died\.$', '<?deadName?> died.\nThey will respawn.')
replace(106200, r'^Beckoner <\?hostName\?> has fallen\.', 'The host <?hostName?> died.\nYou stay in their game.')

# Boss battles. The battle lines keep the game's own boss-name tag. The two generic "final battle"
# lines (105199 / 105299) have no tag; bloodborne_re.cpp (TickBbscBossLine) swaps in a copy with the
# boss's name once it knows the boss, this text is what shows until then.
battle_start = [i for i, t in sorted(event_text.items())
                if t and re.fullmatch(r'Battle with <\?npcNameId@\d+\?> has commenced\.\s*\n?Returning to your world\.', t)]
assert len(battle_start) >= 40, battle_start
for i in battle_start:
    tag = re.search(r'(<\?npcNameId@\d+\?>)', event_text[i]).group(1)
    replace(i, r'has commenced', 'The host started the battle against\n' + tag + '.')
replace(105100, r'^Beckoner has challenged', 'The host started the battle against\n<?npcNameId@500000?>.')
replace(105199, r'^The beckoner has begun the final battle\.$', 'The host started a boss battle.')
replace(105299, r'^The beckoner has begun the final battle\.', 'The host started a boss battle.')
replace(105399, r'final battle', 'The host started a boss battle.')
replace(105499, r'final battle', 'The host started a boss battle.')
replace(105599, r'final battle', 'The host started a boss battle.')
boss_farewell = [i for i, t in sorted(event_text.items())
                 if t and re.fullmatch(r'<\?npcNameId@\d+\?> was defeated,\s*\nending the hunt\. Now, say farewell\.', t)]
assert 106100 in boss_farewell and len(boss_farewell) >= 30, boss_farewell
for i in boss_farewell:
    tag = re.match(r'(<\?npcNameId@\d+\?>)', event_text[i]).group(1)
    replace(i, r'say farewell', tag + ' defeated.')
replace(106199, r'^The final battle has ended\.', 'Boss defeated.')

# Invasions (host's and cooperators' screens).
replace(102002, r'^Adversary <\?joinName\?> has arrived\.$', 'Invader <?joinName?> entered your world!')
replace(109000, r'bell-ringing woman has appeared somewhere\.$',
        'A bell-ringing woman appeared.\nAn invader may come.')
replace(103002, r'^Adversary <\?deadName\?> has died\.$', 'Invader <?deadName?> was killed.')
replace(103003, r'^Vileblood <\?deadName\?> has died\.$', 'Invader <?deadName?> was killed.')
replace(103004, r'^Executioner <\?deadName\?> has died\.$', 'Invader <?deadName?> was killed.')
replace(103005, r'^Hunter of Hunters, <\?deadName\?>, has died\.$', 'Invader <?deadName?> was killed.')
replace(106300, r'^Executioner <\?hostName\?> has fallen\.', 'The host <?hostName?> died.\nYou stay in their game.')
replace(106400, r'^Vileblood <\?hostName\?> has fallen\.', 'The host <?hostName?> died.\nYou stay in their game.')
replace(106500, r'^Blood-addled hunter <\?hostName\?> has fallen\.', 'The host <?hostName?> died.\nYou stay in their game.')
for ids in ((20000407, 20000408), (20000409, 20000410), (20000411, 20000412), (20000413, 20000414)):
    for i in ids:
        replace(i, r'<\?leaveName\?>\s*\nreturned to their own world\.$',
                'Invader <?leaveName?> left\nand went back to their world.')

# Invading (the invader's own screen).
for i in (102300, 102400, 102500):
    replace(i, r'^You are now in the world of the beckoner,', "You invaded <?hostName?>'s world.")

# Bells answered.
for i in (101001, 101003, 101004, 101005):
    replace(i, r'^Discovered the chime of a Small Resonant Bell\.$', 'A player answered your bell.\nJoining...')
replace(101100, r'^Discovered the chime of a Beckoning Bell\.$', 'Found a host.\nJoining their game...')

# The BBSC hunter's kit replaces the vanilla bells (bloodborne_re.cpp "The BBSC hunter's kit"): the
# bell prompts name the kit, and nothing costs Insight any more.
replace(14000, r'^Expend Insight', 'Ring the Hunt Bell\nto call other players?')
replace(14003, r'^Silence the Beckoning Bell$', 'Stop the Hunt Bell')
replace(14004, r'^Silence the Small Resonant Bell$', 'Stop the Kin Bell')
replace(14005, r'^Silence the Sinister Resonant Bell$', 'Stop the Bloodlust Bell')
replace(100001, r'^Searching for the chime of a Small Resonant Bell\.\.\.$',
        'Hunt Bell rung.\nWaiting for players to join...')
replace(100100, r'^Searching for the chime of a Beckoning Bell\.\.\.$',
        'Kin Bell rung.\nLooking for a host to join...')
replace(100200, r'^Searching for the chime of a Sinister Bell\.\.\.$',
        'Bloodlust Bell rung.\nLooking for a world to invade...')
replace(100300, r'^Could not discover the chime', 'Nobody found.\nRing the bell again to keep looking.')
# Test 2026-10-08: the host saw the invader's line and the other way round - 101002 is shown to
# the host whose world is entered, 101200 to the invader.
replace(101002, r'^Sinister Bell resonates', 'Someone is about to invade your world.')
replace(101200, r'^Discovered the chime of a Sinister Bell\.$',
        'Found a world to invade.\nEntering...')
replace(100011, r'Small Resonant Bell with matching password',
        'Hunt Bell rung (password).\nWaiting for players to join...')
replace(100110, r'Beckoning Bell with matching password',
        'Kin Bell rung (password).\nLooking for a host to join...')
for i in (101300, 101400, 101500):
    replace(i, r'^Discovered the chime of a Beckoning Bell,', 'Found a host,' +
            re.sub(r'^Discovered the chime of a Beckoning Bell,', '', by_category[EVENT_TEXT][i]))
replace(10011270, r'Beckoning Bell', 'Ring the Hunt Bell to call\nother players into your game.')
replace(10011280, r'Silencing Blank', 'Use the Parting Blank to end co-op.')
# The bell / blank menu (category 200 labels, 201 their help lines, 204 the quick-search prompts).
for i in (266302, 280140, 280240):
    replace(i, r'^Sinister Bell$', 'Bloodlust Bell', category=200)
for i in (302000, 302100):
    replace(i, r'^Silencing Blank$', 'Parting Blank', category=200)
replace(302110, r'^Silence Beckoning Bell$', 'Stop the Hunt Bell', category=200)
replace(302111, r'^Silence Small Resonant Bell$', 'Stop the Kin Bell', category=200)
replace(302112, r'^Silence Sinister Resonant Bell$', 'Stop the Bloodlust Bell', category=200)
replace(302120, r'^Disrupt Resonance$', 'End co-op', category=200)
replace(302123, r'^Disrupt Resonance$', 'End co-op', category=200)
replace(302124, r'^Disrupt Resonance and Return Home$', 'End co-op and go home', category=200)
replace(269002, r'Beckoning Bell', 'Quick search: find a host and join their game.', category=201)
replace(269003, r'Sinister Bell', 'Quick search: find a world to invade.', category=201)
replace(302110, r'cease requesting co-op', 'Stop calling other players.', category=201)
replace(302111, r'stop helping', 'Stop looking for a host.', category=201)
replace(302112, r'cease threat', 'Stop looking for a world to invade.', category=201)
replace(269001, r'Beckoning Bell', 'Find a host and join their game?', category=204)
replace(269002, r'Sinister Bell', 'Find a world to invade?', category=204)
replace(302200, r'Beckoning Bell', 'Kin Bell rung.\nLooking for a host to join...', category=204)
replace(302201, r'Sinister Bell', 'Bloodlust Bell rung.\nLooking for a world to invade...', category=204)
replace(302210, r'matching password', 'Kin Bell rung (password).\nLooking for a host to join...', category=204)
replace(2000, r'^Ring the Beckoning Bell', 'Need help? Ring the Hunt Bell.', category=2)
replace(1121, r'Beckoning Bell', 'Call other players into your game\nwith the Hunt Bell.', category=2)
replace(1122, r'Small Resonant Bell', "Join another player's game\nwith the Kin Bell.", category=2)

# Warping together re-runs the summon lines for the same two players. BBSC points the role table
# at these "caught up" lines while a warp is on (bloodborne_re.cpp, TickBbscRejoinLines). They use
# placeholder "*" texts of this category that no talk script references.
replace(15000011, r'^\*$', 'You caught up with <?hostName?>.')
replace(15000012, r'^\*$', '<?joinName?> caught up with you.')
replace(15000035, r'^\*$', "You are in the host's Hunter's Dream.")
# The Communion Chalice's question before the drink (its goods row's yesNoDialogMessageId), an empty
# "*" slot among the game's item prompts that nothing refers to.
replace(10010854, r'^\*$', "Drink from the Communion Chalice\nand share progress with the other hunters?")

# The Duel Bell (PvP toggle, bloodborne_re.cpp "PvP toggle item"): goods 4313 is an unused key item
# with empty slots in the item name / info / caption tables (msg/engus/item.msgbnd: 10, 20, 24).
items = bbfiles.read_bnd4(bbfiles.read_dcx(bbfiles.GAME + r'\msg\engus\item.msgbnd.dcx'))
for _, (fid, data) in items.items():
    by_category[fid] = bbfiles.read_fmg(data)


def fill(msg_id, text, category):
    """An item text slot that exists but is empty in the game."""
    if msg_id not in by_category[category]:
        raise SystemExit('message %d/%d has no slot' % (category, msg_id))
    if by_category[category][msg_id] is not None:
        raise SystemExit('message %d/%d is not empty: %r' % (category, msg_id,
                                                             by_category[category][msg_id]))
    entries.append((category, msg_id, text, '(empty)'))


fill(4016, 'Hunt Bell', 10)
fill(4016, 'Ring to call other players into your game.\nCosts no Insight.', 20)
fill(4016, 'An old bell carried by the hunters of the\nworkshop, its toll heard across every world.\n\n'
           'Ring it, and hunters who answer with a\nKin Bell may join your hunt. Together you\n'
           'walk the night, through every lamp,\ndeath and dream, until the hunt is done.', 24)
fill(4121, 'Kin Bell', 10)
fill(4121, 'Ring to join another player''s game.\nCosts no Insight.', 20)
fill(4121, 'A small bell of the choir, its chime\nfaint, but true.\n\n'
           'Hunters who hear a Hunt Bell may answer\nwith it, crossing into another world to\n'
           'stand at the side of kin. What is slain\nthere is slain for both.', 24)
fill(11201, 'Bloodlust Bell', 10)
fill(11201, 'Ring to invade another player''s world.\nCosts no Insight.', 20)
fill(11201, 'A bell that sounds only for the blood-drunk.\n\n'
           'No maiden need ring for its toll to be\nheard: every hunter of the night is prey,\n'
           'alone or in company, wherever they walk.\n\nThe hunt for hunters never ends.', 24)
fill(11251, 'Parting Blank', 10)
fill(11251, 'Use to end co-op.\nGuests go home; the host sends everyone home.', 20)
fill(11251, 'A blank of silent powder.\n\n'
           'Its inaudible burst breaks the resonance\nbinding hunters of different worlds, and\n'
           'each returns to their own night.', 24)
# The vanilla bells and the Silencing Blank cannot be used any more; their info says what replaced them.
replace(200, r'.', 'No longer used. Use the Hunt Bell.', category=20)
replace(205, r'.', 'No longer used. Use the Kin Bell.', category=20)
replace(225, r'.', 'No longer used. Use the Bloodlust Bell.', category=20)
replace(111, r'.', 'No longer used. Use the Parting Blank.', category=20)
fill(201, 'Communion Chalice', 10)
fill(201, "A cup of communion between hunters. Share\nkeys, weapons and runes in co-op; a guest\n"
     "also carries the host's progress home.", 20)
fill(201, "A chalice of the Healing Church, passed between\nhunters who walk the same night.\n\n"
     "Those who drink partake of one another's blood,\nand of all it carries: keys, arms and runes.\n"
     "A guest also carries home the host's lamps\nand the beasts the host has slain.\n\n"
     "Your own strength remains your own.", 24)
fill(4313, 'Duel Bell', 10)
fill(4313, 'Ring in co-op to turn on PvP between players.\nRing again to turn it off.', 20)
fill(4313, 'A cracked bell with a sour, discordant chime.\n\n'
           'Ring it while hunting together, and the\nresonance binding the hunters turns to\n'
           'rivalry: they may strike one another as foes.\n\n'
           'Ring it again to restore the old bond.\nOnly the beckoner decides; a cooperator\n'
           'who rings it merely asks.', 24)


def cpp(text):
    out = []
    for ch in text:
        if ch == '\n':
            out.append('\\n')
        elif ch in '"\\':
            out.append('\\' + ch)
        elif ord(ch) < 0x80:
            out.append(ch)
        else:
            out.append('\\u%04X' % ord(ch))
    return 'u"' + ''.join(out) + '"'


with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Generated by tools/gamedata/gen_messages.py from the game\'s English text - do not edit.\n')
    f.write('// {category, message id, BBSC text}   // original text\n')
    for category, msg_id, text, original in entries:
        f.write('{%d, %d, %s}, // %s\n' % (category, msg_id, cpp(text),
                                          original.replace('\n', ' ')))
print(len(entries), 'messages ->', OUT)
