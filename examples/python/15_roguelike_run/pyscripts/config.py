"""Every tunable for the roguelike run example. No logic lives here.

Import as `rogue_config` (main.py registers all modules in sys.modules via
bd.import_script before any sibling imports them).
"""

# --- run / scoring ---------------------------------------------------------

DEFAULT_SEED = 1337
RARE_KEYWORDS = ("backpack", "bfg", "invulnerability")  # matched lowercase
SECOND_MUTATOR_MIN_DEPTH = 3  # maps deep before a second mutator can roll
SECOND_MUTATOR_CHANCE = 0.30

SCORE_PER_KILL = 10
SCORE_PER_SECRET = 100
SCORE_PER_RARE = 50
SCORE_PER_MAP_CLEAR = 25
SCORE_PER_LEVEL = 150  # bonus score awarded on each level-up

# --- affixes (Diablo-2-style monster titles) --------------------------------

AFFIX_CHANCE_BASE = 0.25       # spawn chance of any affix at depth 0
AFFIX_CHANCE_PER_DEPTH = 0.04  # +4% per map of depth
SUFFIX_CHANCE = 0.45           # an affixed monster also rolls a suffix
UNIQUES_PER_MAP = 1            # crowned uniques per map...
UNIQUE_PER_ELITE_HUNT = 1      # ...plus this many with the elite_hunt mutator
UNIQUE_HEALTH_MULT = 3.0
UNIQUE_XP_MULT = 5.0
UNIQUE_COLOR = (255, 200, 40)  # gold: ring, bar and title of a unique

CHAMPION_RING_RADIUS = 16.0    # world units
UNIQUE_RING_RADIUS = 26.0
RING_ALPHA = 0.9

# --- XP / levels -------------------------------------------------------------

XP_PER_LEVEL = 250  # xp needed to go from level n to n+1: XP_PER_LEVEL * n

# --- canvas id scheme --------------------------------------------------------
# Persistent display list; reusing an id replaces the item. The bd.ui toolkit
# owns ids >= 900000; these stay well clear of it.

DEATH_BG = 520            # full-screen dim behind the death panel
SLIM_SCORE = 540          # slim HUD lines
SLIM_SUB = 541
SLIM_HINT = 542
POPUP_BASE = 600          # 600..649 round-robin score/XP popups
POPUP_SLOTS = 50
PLAYER_RING = 560         # player aura ring (level-up, vampire proc)
MONSTER_VISUAL_BASE = 700  # + slot * 3: ground ring, health bar, title text
HUD_ANNOUNCE = 1          # bd.hud_text id (separate namespace from canvas ids)

# Overhead title text heights (normalized screen-height fractions; world
# text keeps a constant on-screen size with distance).
TITLE_H_AFFIXED = 0.014
TITLE_H_CHAMPION = 0.016
TITLE_H_UNIQUE = 0.020

# --- death loop ----------------------------------------------------------------

DEATH_COUNTDOWN_SECONDS = 3
CHECKPOINT_NAME = "roguelike"  # delete roguelike.zds from saves for a fresh run
