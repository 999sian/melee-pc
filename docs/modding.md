# Modding

melee-pc loads mods at boot. A mod is a folder that can do any combination of:

- **Replace or add disc files** (`files/`) — costumes, stages, menus, sounds,
  effects, anything the game reads from the disc.
- **Patch fighter attributes** (`tunables` in `mod.json`) — gravity, jumps,
  weight, speeds, landing lag, per character or for everyone.
- **Expose settings** (`config` in `mod.json`) that tunables and plugins read,
  overridable per user.
- **Run native code** (`plugin`) through a versioned C API with hooks for
  boot, scene changes, matches, fighter spawns and every frame.

The mod set is fixed when the game starts. Enable, disable and rescan mods on
the launcher's **Settings → Mods** page; changes apply on the next Play. Each
character, stage and item a mod adds is listed under it with its own On/Off
switch, so you can keep a mod but leave out some of what it adds (saved in
`mods.cfg` as `pack <mod id>/<pack id> 0`).

## Where mods go

| Location | Use |
|---|---|
| `%APPDATA%\melee-pc\mods\` (Windows), `~/.local/share/melee-pc/mods/` (Linux), `~/Library/Application Support/melee-pc/mods/` (macOS) | Your mods. The launcher's **Open folder** button opens it. |
| `mods/` next to the executable | Bundled mods (the build puts the examples here). |

Each mod is a sub-folder containing `mod.json`. If the same `id` exists in both
places, the copy in your user folder wins.

Enabled state and config overrides are saved in `mods.cfg` next to
`launcher.cfg`. It is plain text and safe to edit:

```
enabled example.moon-gravity 1
config example.moon-gravity gravity_scale 0.3
```

## mod.json

```jsonc
{
  "id": "yourname.cool-mod",      // required: 1-64 chars of a-z 0-9 . _ -
  "name": "Cool Mod",
  "version": "1.0.0",
  "author": "you",
  "description": "One line shown in the launcher.",

  "priority": 0,                  // load order: lower first; later mods win conflicts
  "enabled_by_default": true,
  "affects_gameplay": true,       // false for purely cosmetic mods (see Netplay)
  "api_version": 1,               // minimum mod API this mod needs
  "dependencies": ["other.mod"],  // loaded first; this mod is disabled without them
  "conflicts": ["rival.mod"],     // both cannot be active at once

  "files": "files",               // overlay folder, default "files"
  "plugin": "my_plugin",          // optional: my_plugin.dll / .so / .dylib

  "config": { ... },              // see Config
  "tunables": { ... }             // see Tunables
}
```

`//` and `/* */` comments and trailing commas are allowed. Problems (bad JSON,
a missing dependency, a plugin that fails to load) are shown in red under the
mod on the Mods page and written to the log; a broken mod is skipped, never
fatal.

## Files: the virtual disc

Everything under `files/` is mapped onto the disc root, path for path,
case-insensitively:

```
mods/blue-fox/files/PlFxNr.dat        replaces /PlFxNr.dat
mods/my-stage/files/GrNBa.dat         replaces /GrNBa.dat
mods/my-pack/files/audio/custom.hps   adds    /audio/custom.hps
```

Files are streamed from your folder when the game opens them; nothing is
copied. When two active mods provide the same path, the one later in load order
wins and the log says so (`mods: b overrides /X.dat from a`).

**The game trusts these files completely.** A replacement must be the same kind
of file the game expects, *including its internal symbol names*. For example,
costume files carry their costume in their symbol names — Fox's default costume
`PlFxNr.dat` exports `PlyFox5K_Share_joint`, while the green costume
`PlFxGr.dat` exports `PlyFox5KGr_Share_joint` — so copying one costume over
another without renaming the symbols stops the game with
`Cannot find symbol ...`. Community tools (HSDRaw, DAT Texture Wizard) handle
this; run the game once with your mod and check the log.

Only ship files you made. Do not redistribute files from the disc.

## Character packs (new roster slots)

A mod can add new characters. Each one gets its own slot on extra pages of the
character select screen: **L / R** on the character select flips between the
disc roster (page 1) and pack pages (25 characters each). A pack character
is built on a *base* character: it uses the base's moveset and behaviour, and
its own data file (stats, hitboxes, move data), animations, models, costumes,
icon and name.

```jsonc
"fighters": [
  {
    "id": "kestrel",                 // unique within the mod; full id "<mod id>/kestrel"
    "name": "Kestrel",                  // shown on the character select name plate
    "base": "Fox",                   // moveset/behaviour (see the list below)
    "icon": "css_icon.png",          // optional, relative to the mod folder; 64x56 is native
    "data": "PlKs.dat",              // fighter data file (Pl<xx>.dat layout)
    "data_symbol": "ftDataFox",      // optional: root symbol in "data"; default is the base's
    "animations": "PlKsAJ.dat",      // animation archive (Pl<xx>AJ.dat layout)
    "costumes": [                    // at least one, up to 16
      "PlKsNr.dat",                  // symbols default to the base's costume in the same slot
      { "file": "PlKsBu.dat", "joint": "PlyFox5KBu_Share_joint",
        "matanim": "PlyFox5KBu_Share_matanim_joint" }
    ],
    "tunables": { "gravity": { "mul": 1.1 } },  // optional, this character only

    // Optional art and audio (paths relative to the mod folder):
    "portrait": "csp.png",           // character-select door portrait, every costume
    "stock_icon": "stock.png",       // HUD stock icon (also the results table icon)
    "announcer": "name.ogg",         // name call on picking and on winning (.ogg/.wav)
    "victory_theme": "fanfare.ogg",  // replaces the base's victory theme, plays once
    "emblem": "emblem.png",          // series emblem: behind the HUD damage %, results panel
    "name_image": "name.png",        // results-screen name label (about 5:1)
    "winner_name_image": "win.png",  // results-screen winner banner (about 9:1)
    "series": "Ness"                 // whose 3D series emblem the results background shows
  }
]
```

A costume object can override the art for itself:
`{ "file": "PlKsBu.dat", "portrait": "csp_blue.png", "stock_icon": "stock_blue.png" }`.
Images are PNGs of any size; they are resampled to the slot they replace
(64x56 for the select-screen icon). Leave a field out and the base
character's art or sound is used.

**Two-fighter bases.** Ice Climbers (`"base": "Ice Climbers"`), Zelda and
Sheik packs need the other half too, in a `partner` block with its own
files; the pair always wears the same costume slot, and holding A to start
as Sheik works as usual:

```jsonc
{ "id": "duo", "name": "Duo", "base": "Ice Climbers",
  "data": "PlD1.dat", "animations": "PlD1AJ.dat", "costumes": ["PlD1Nr.dat"],
  "partner": { "data": "PlD2.dat", "animations": "PlD2AJ.dat", "costumes": ["PlD2Nr.dat"],
               "tunables": { "weight": { "mul": 0.9 } } } }
```

The files are looked up on the virtual disc, so ship them under the mod's
`files/` folder with **new names** (`PlKs.dat`, not `PlFx.dat`): a pack adds
files, it does not replace the base character's. Symbol names default to the
base's, so a pack made by copying and editing the base's files needs no
symbol fields; set `data_symbol` / `joint` / `matanim` only when your files
use different names. A file or symbol that does not match stops the game with
`Cannot find symbol ...` when the character loads, exactly like a bad file
replacement. Animation archives must have the same animations as the base.

Records and unlocks are counted under the base character, so saves are
never touched. See **Everything a pack can replace** below for the complete
list of a pack's assets.

**Bases.** Every playable character. Packs built on Kirby, Jigglypuff or
Mr. Game & Watch are limited to the base's number of costumes (their code has
per-costume tables), and Kirby's copy star and Yoshi's egg keep the base
character's tuning. Bosses, wireframes and Sandbag cannot be bases.

**Several of the same base** in one match work: Fox, two different Fox-based
packs and a CPU Fox each load their own files.

**Netplay.** Pack pages work online. Both players run the character select
from the same synced inputs and the page state is part of the rollback
snapshot, so picks come out identical on both sides; the registered packs are
in the gameplay hash, so two players with different packs are refused at the
handshake. (Verified so far: the handshake agrees with packs installed; a full
two-player online pick has not been run end to end yet.)

## Map packs (new stage slots)

A mod can add new stages. They appear on extra pages of the stage select:
**L / R** flips between the disc's stages (page 1) and map-pack pages (29
stages each; RANDOM stays on every page and draws from the disc's stages
and every map pack alike, online too). A map pack
is built on a *base* stage: it runs that stage's code -- moving platforms,
hazards, transformations -- with its own stage file and, optionally, its own
icon, preview, name plate and music.

```jsonc
"stages": [
  {
    "id": "skyway",                 // unique within the mod
    "name": "Skyway",               // log and pack lists
    "base": "Battlefield",          // the stage whose code it runs (see below)
    "file": "GrSkw.dat",            // stage file on the virtual disc
    "icon": "sss_icon.png",         // optional: stage-select icon
    "preview": "sss_preview.png",   // optional: preview shown while hovering
    "name_image": "sss_name.png",   // optional: name plate shown while hovering
    "music": "skyway.ogg"           // optional: replaces the stage music, loops
  }
]
```

The stage file goes under the mod's `files/` folder with a **new name** (it is
added, the base stage keeps its own). It must have the base stage's layout --
the base's symbols (`map_head`, `coll_data`, `grGroundParam`, ...) and a
`grGroundParam` entry for the base stage -- because the base's code reads it.
Editing a copy of the base's `Gr*.dat` guarantees that. A file that does not
match stops the game when the stage loads. Stages that load extra files of
their own (Pokémon Stadium's transformations, for example) load the base's
unless the map pack swaps them with `replace_files` (below).

### New geometry

A map pack's stage is not tied to its base's shape: the collision (any number
of solid islands, slopes, steps, overhangs, pass-through platforms), the
spawn points, item spawns, camera bounds and blast zones, and the visible
model are all the map's own. melee-pc sizes collision from the stage file, so
there is no vertex or line budget beyond the file format's (65,535 vertices,
32,767 lines); a 4,200-line test stage plays normally.

`tools/modkit` builds such a stage from a short JSON description:

```sh
python tools/modkit/stagebuild.py level.json --disc Melee.iso --out mymod/files --file GrMy.dat
```

```jsonc
{
  "base_file": "GrNBa.dat",                       // the base stage's file (Battlefield)
  "name": "My Level",
  "solids":    [[[-80, 0], [80, 0], [60, -30], [-60, -30]]],   // closed outlines
  "platforms": [[[-50, 30], [-20, 30]], [[20, 30], [50, 34]]], // polylines
  "spawns": [[-40, 1], [40, 1], [-15, 1], [15, 1]],
  "items": [[-30, 10], [0, 10], [30, 10]],
  "camera": [-160, 160, 160, -80],                 // left, top, right, bottom
  "blastzone": [-240, 250, 240, -140]
}
```

Each solid edge becomes a floor, ceiling or wall by the way it faces (floors
up to 45 degrees), floors that end at a wall get grabbable ledges, and the
visible stage is generated from the same outlines (with test textures), so
what you see is what you stand on. It also writes `sss_icon.png`,
`sss_preview.png` and `sss_name.png` for the map pack. `procgen_stage.py
<seed>` prints a random level description to feed it -- a stepped or sloped
main island, optional floating islands and two to five platforms.

Build on a base whose stage code does not move its own collision: Battlefield
(tested) or Final Destination. A base with moving or transforming collision
(Pokémon Stadium, Yoshi's Story's cloud, Rainbow Cruise) expects its own
collision layout and should only get its vertices moved, not new topology.

Leave out an image and the base stage's is shown -- its real icon even when
the base is still locked on your save. Images are PNGs of any size,
resampled to the slot they replace.

**Bases** are the stages on the VS stage select, by English or internal
name: Fountain of Dreams, Pokémon Stadium, Princess Peach's Castle, Kongo
Jungle, Brinstar, Corneria, Yoshi's Story, Onett, Mute City, Rainbow Cruise,
Jungle Japes, Great Bay, Hyrule Temple, Brinstar Depths, Yoshi's Island,
Green Greens, Fourside, Mushroom Kingdom, Mushroom Kingdom II, Venom, Poké
Floats, Big Blue, Icicle Mountain, Flat Zone, Dream Land, Yoshi's Island N64,
Kongo Jungle N64, Battlefield and Final Destination. Simple bases
(Battlefield, Final Destination, Dream Land) are the easiest to build on.

A map pack only applies to the VS match it was picked for; other modes keep
using the disc stages. Online, each player pages their own stage select and
the pick carries the map pack; installed map packs are part of the gameplay
hash, so both players must have the same ones.

## Item packs (new items)

A mod can add new items. An item pack is built on a *base* common item: it
runs that item's code -- pick up, throw, swing, shoot, eat, explode -- with
its own model, attributes and hitboxes, and joins the random item draw next
to its base.

```jsonc
"items": [
  {
    "id": "giantsword",            // unique within the mod
    "name": "Giant Sword",         // log and pack lists
    "base": "Beam Sword",          // the common item whose code it runs
    "file": "ItGiantSword.dat",    // item file on the virtual disc
    "symbol": "itArticle",         // optional: the Article's public symbol
    "frequency": 0.5               // optional: spawn weight vs. the base (1)
  }
]
```

The file goes under `files/` with a **new name** and holds the item's
*Article* -- the structure ItCo.dat keeps per common item in
`itPublicData`: common attributes, the base item's own attributes, hurtboxes,
states (animations and hitboxes), model and dynamics -- as a public symbol
(`itArticle` unless `"symbol"` says otherwise). It must have the base item's
layout, because the base's code reads it; start from the base's Article
exported from ItCo.dat with an HSD editor (HSDRaw, for example), change the
model, numbers and hitboxes, and save it under the symbol name. An Article
that does not match the base misbehaves or stops the game when the item
spawns. A missing symbol is logged once and the item never spawns.

**Spawning.** Each pack gets its own entry in the random draw right after its
base, for items appearing on the stage and for items coming out of crates,
barrels, capsules and party balls. `frequency` is relative to the base's own
weight on that stage: 1 is as common as the base, 0.5 half as common, 0
never. A pack only spawns where its base can -- if the base is switched off
in the item switch, or the stage never spawns it, neither does the pack --
and the base keeps its own weight (the pack's entry adds to the
total, so every item becomes a little rarer).

**Bases** are the common items, by English or internal name: Capsule, Crate,
Barrel, Egg, Party Ball, Barrel Cannon, Bob-omb, Mr. Saturn, Heart Container,
Maxim Tomato, Starman, Home-Run Bat, Beam Sword, Parasol, Green Shell, Red
Shell, Ray Gun, Freezie, Food, Motion-Sensor Bomb, Flipper, Super Scope, Star
Rod, Lip's Stick, Fan, Fire Flower, Super Mushroom, Poison Mushroom, Hammer,
Warp Star, Screw Attack, Bunny Hood, Metal Box and Cloaking Device. The Poke
Ball itself is not a base (it spawns Pokemon, not itself).

**Pokemon** are bases too: Goldeen, Chikorita, Snorlax, Blastoise, Weezing,
Charizard, Moltres, Zapdos, Articuno, Wobbuffet, Scizor, Unown, Entei, Raikou,
Suicune, Bellossom, Electrode, Lugia, Ho-oh, Ditto, Clefairy, Togepi, Staryu,
Chansey, Porygon2, Cyndaquil, Marill and Venusaur (English or internal name;
Mew and Celebi only come from their own rare roll, so they are not bases). A
Pokemon pack comes out of Poke Balls next to its base, with `frequency`
relative to the base's Poke Ball weight. Its Article is the Pokemon's entry in
ItCo.dat's Pokemon table rather than the item table.

**Character, projectile and stage items** are bases as well, by their
internal name from the game's item list (`src/melee/it/forward.h`, without
`It_Kind_`): `Link_Bomb`, `Fox_Blaster`, `Fox_Laser`, `Peach_Turnip`,
`Samus_Missile`, `Mario_Fire`, `Lugia_Aeroblast`, `Old_Kuri` (Mushroom
Kingdom's Goomba), `Kyasarin_Egg` and so on. These never come from the random
draw -- a fighter, item, Pokemon or stage spawns them -- so a pack on one
*replaces* spawns of its base instead, and two optional fields say which:

```jsonc
{ "id": "bigbomb", "name": "Big Bomb", "base": "Link_Bomb",
  "file": "ItBigBomb.dat",
  "fighter": "Link",          // only bombs Link pulls; or a character pack id
  "stage": "Battlefield",     // only on this stage; or a map pack id
  "frequency": 100 }          // 100: nearly every spawn; 1: half of them
```

`fighter` takes a character name (which also matches character packs built
on it) or a character pack's id (this mod's, or `"<mod id>/<pack id>"`), so a
character pack can ship its own projectiles. Items spawned by an item (a
bomb's explosion, a Pokemon's attack) count as their owner's. `stage` takes a
VS stage name or a map pack id. `frequency` weighs every matching pack
against the base's own 1. The Article comes from where the base's does: the
fighter's `Pl??.dat` item list (`ftData` +0x48), a stage's ground file, or
ItCo.dat's tables for item projectiles and Pokemon attacks.

**Training mode** lists every item pack on a common item after the disc's
items in the Item menu, under its own name; Pokemon packs come out of the
Poke Ball there as everywhere else.

Item packs are part of the gameplay hash, so both netplay players must have
the same ones; the draw uses the synced random seed. With
`MELEE_DEBUG_VS_ITEMS=4:"Beam Sword"` (see [debugging.md](debugging.md)) a
debug match spawns only that base, often, which makes a pack easy to watch.

## Swapping other files for one pack (`replace_files`)

A character pack or a map pack can swap any other disc file **only while it
is in the match**, so the base keeps its own files everywhere else:

```jsonc
"fighters": [{ "id": "bigfox", "base": "Fox", ...,
  "replace_files": {
    "audio/us/fox.ssm": "audio/us/bigfox.ssm",  // English voice + sounds
    "audio/fox.ssm":    "audio/bigfox.ssm"      // Japanese voice
  } }],
"stages": [{ "id": "stadium2", "base": "Pokemon Stadium", "file": "GrPx.dat",
  "replace_files": {
    "GrPs1.dat": "GrPx1.dat",                   // fire transformation
    "audio/us/pstadium.ssm": "audio/us/stadium2.ssm"
  } }]
```

The key is the disc file the game asks for; the value is the file loaded
instead, added under `files/` with its own name. Both must be on the virtual
disc. Typical uses:

- **Sound banks** (`audio/<name>.ssm`): a character's voice and sound effects,
  or a stage's sounds. The game reads `audio/us/` when the language is
  English and `audio/` otherwise, so swap both if both matter. A bank must not
  be bigger than the one it replaces (the game's sound memory is sized for
  its own banks); a bigger one is refused with a log line. The banks keep
  the same sound numbering, so edit a copy of the base's bank.
- **A stage's extra files**: Pokémon Stadium's transformations
  (`GrPs1.dat`-`GrPs4.dat`) and the like.
- Effect files and anything else a match loads.

A character pack's swaps apply only when no other player in the match runs
its base character (or another pack on the same base); that player would
otherwise get the pack's files too. Files the game keeps loaded between
matches (`IfAll`, `ItCo`, `EfCoData.dat`, `EfMnData.dat`, `LbRb.dat`,
`audio/main.ssm`) cannot be swapped per match and are refused; replace them
with a plain file in `files/` instead. The log shows `mods: loading X for Y`
the first time each swap is used in a match. Swaps are part of the gameplay
hash.

## Everything a pack can replace

Every asset a new character, stage or item shows or plays, and how a mod
supplies its own. "Swap" means a `replace_files` entry (above), which applies
only to matches the pack is in -- and, for a character pack, only when no
other player is on the same base character (that player would get the
pack's files too; the base's are used then).

**Character packs**

| Asset | Supplied by |
|---|---|
| Stats, attributes, hitboxes, move data, model, materials | `data` (`Pl<xx>.dat` layout) |
| Animations | `animations` (`Pl<xx>AJ.dat` layout, same animations as the base) |
| Costumes (model, textures, material animation) | `costumes` |
| Character-select icon, door portrait, name plate text | `icon`, `portrait` (per costume too), `name` |
| HUD stock icon, results table icon | `stock_icon` (per costume too) |
| HUD damage emblem, results panel emblem | `emblem` (PNG); or `series` to use another character's |
| Results name label, winner banner | `name_image`, `winner_name_image` |
| Results 3D background emblem | `series` (picks one of the game's emblem models) |
| Results / in-match model | the pack's costumes |
| Announcer name call, victory theme | `announcer`, `victory_theme` |
| Voice and sound effects | swap `audio/us/<base>.ssm` and `audio/<base>.ssm` |
| Visual effects (move effects, particles) | swap `Ef<Xx>Data.dat` |
| Results-screen win/lose poses | swap `GmRstM<Xx>.dat` |
| Attribute tweaks | `tunables` |

Still the base's: the moveset's code (inherent to building on a base), the
hat Kirby gets from copying the character, intro/ending/trophy poses outside
VS results, and records. Sounds, effects and poses swapped while the base is
also in the match fall back to the base's for everyone.

**Map packs**

| Asset | Supplied by |
|---|---|
| Collision, spawns, item spawn points, camera, blast zones, models, textures, animations, stage parameters | `file` (the stage file; `tools/modkit/stagebuild.py` builds one) |
| Stage-select icon, preview, name plate | `icon`, `preview`, `name_image` |
| Music | `music` (loops) |
| Stage sound effects | swap `audio/us/<stage>.ssm` / `audio/<stage>.ssm` |
| Extra files the stage loads (transformations, ...) | swap them by name |

Still the base's: the stage's code (moving parts, hazards), and its
collision layout when that code moves collision (see **New geometry**).

**Item packs**

| Asset | Supplied by |
|---|---|
| Model, textures, attributes, hitboxes, hurtboxes, states (animations), dynamics | `file` / `symbol` (the item's Article) |
| Spawn weight, where it spawns | `frequency`; `fighter` / `stage` for owned items |
| Training-mode menu name | `name` |

Still shared: item sound effects and common item effects live in files the
game keeps loaded all the time (`audio/main.ssm`, `EfCoData.dat`), so a pack
can point at different existing sounds and effects in its attributes but not
add new ones; replacing those files with a plain `files/` entry changes them
for every item. The item's code is its base's.

## Tunables

Tunables patch a fighter's attributes every time they are loaded from its data
file, before size, metal and bunny-hood modifiers are applied:

```jsonc
"tunables": {
  "fighters": {
    "Fox":          { "max_jumps": 3, "weight": { "add": 10 } },
    "Ice Climbers": { "gravity": { "mul": 0.9 } },   // patches Popo and Nana
    "*":            { "walk_max_vel": { "mul": "$walk_scale" } }
  }
}
```

A value is one of:

| Form | Effect |
|---|---|
| `0.25` | set to the number |
| `{ "set": 0.25 }` | same |
| `{ "mul": 1.5 }` | multiply |
| `{ "add": -2 }` | add |
| `"$key"` (anywhere a number goes) | use the mod's config value `key` |

Patches apply in load order, so a later mod's `mul` stacks on an earlier mod's
`set`. Integer fields are rounded.

**Fighter names** (case, spaces and punctuation ignored): Mario, Fox,
Captain Falcon, Donkey Kong (DK), Kirby, Bowser, Link, Sheik, Ness, Peach,
Popo, Nana, Ice Climbers, Pikachu, Samus, Yoshi, Jigglypuff (Puff), Mewtwo,
Luigi, Marth, Zelda, Young Link, Dr. Mario (Doc), Falco, Pichu,
Mr. Game & Watch (GnW), Ganondorf, Roy, Master Hand, Crazy Hand,
Male Wireframe, Female Wireframe, Giga Bowser, Sandbag, and `*` / `all`.

**Attributes** (`ftCo_DatAttrs` in `src/melee/ft/types.h`):

`walk_accel_mul`, `walk_accel_base`, `walk_max_vel`, `slow_walk_max`,
`mid_walk_point`, `fast_walk_min`, `ground_friction`, `dash_initial_velocity`,
`dash_accel_mul`, `dash_accel_base`, `dash_max_velocity`,
`run_animation_scaling`, `max_run_brake_frames`,
`ground_max_horizontal_velocity`, `jump_startup_time`,
`jump_h_initial_velocity`, `jump_v_initial_velocity`,
`ground_to_air_jump_momentum_multiplier`, `jump_h_max_velocity`,
`hop_v_initial_velocity`, `air_jump_v_multiplier`, `air_jump_h_multiplier`,
`max_jumps` (int), `gravity`, `terminal_velocity`, `air_drift_stick_mul`,
`aerial_drift_base`, `air_drift_max`, `aerial_friction`, `fast_fall_velocity`,
`air_max_horizontal_velocity`, `jab_2_input_window`, `jab_3_input_window`,
`standing_turn_frames`, `weight`, `model_scaling`, `initial_shield_size`,
`shield_break_initial_velocity`, `rapid_jab_window` (int),
`clank_animation_length`, `hit_spark_variant` (int),
`ledge_jump_horizontal_velocity`, `ledge_jump_vertical_velocity`,
`item_throw_velocity_multiplier`, `heavy_throw_velocity_multiplier`,
`specials_ground_speed_retention`, `kirby_b_star_damage`,
`normal_landing_lag`, `landingairn_lag`, `landingairf_lag`, `landingairb_lag`,
`landingairhi_lag`, `landingairlw_lag`, `name_tag_height`,
`passivewall_vel_x`, `wall_jump_horizontal_velocity`,
`wall_jump_vertical_velocity`, `passiveceil_vel_x`, `trophy_scale`,
`screw_attack_launch_velocity`, `wall_jump_min_approach_speed`,
`damageice_ice_size`, `damageicejump_vel_y`, `damageicejump_vel_x_mult`,
`respawn_platform_scale`, `warp_star_hitbox_scale`,
`camera_zoom_target_bone` (int).

Unknown fighters or attributes are logged and skipped. Character-specific
attributes (each fighter's special-move parameters) are not exposed yet.

## Config

```jsonc
"config": {
  "gravity_scale": { "type": "number", "default": 0.5, "min": 0.1, "max": 2, "label": "Gravity" },
  "show_hud":      { "type": "bool",   "default": true },
  "greeting":      { "type": "string", "default": "hi" },
  "short_form":    1.25               // type inferred from the default
}
```

Numbers are clamped to `min`/`max`. User overrides live in `mods.cfg`; a
plugin can change them at runtime with `config_set_*`, which also persists.
(The launcher does not edit config values yet; edit `mods.cfg`.)

## Native plugins

The header is [`src/pc/mods/melee_mod.h`](../src/pc/mods/melee_mod.h); it is
self-contained and is the complete API reference. A plugin exports:

```c
#include "melee_mod.h"

static const MeleeModAPI* api;
static MeleeModHandle self;

static void on_spawn(MeleeModHook hook, intptr_t port, void* user) {
    MeleePlayerState p;
    if (api->get_player((int32_t)port, &p))
        api->log(self, "port %d is fighter %d", (int)port + 1, p.fighter_kind);
}

MELEE_MOD_EXPORT int melee_mod_init(const MeleeModAPI* game, MeleeModHandle h) {
    if (game->api_version < 1) return 1;          // refuse to run on an older game
    api = game; self = h;
    api->register_hook(self, MELEE_HOOK_FIGHTER_SPAWN, on_spawn, NULL);
    api->set_fighter_attr(self, MELEE_FIGHTER_ALL, "gravity", MELEE_ATTR_MUL, 0.8f);
    return 0;                                     // nonzero = failed, plugin unloaded
}

MELEE_MOD_EXPORT void melee_mod_shutdown(void) { }   // optional
```

Build it as a shared library with no link dependency on the game:

```sh
# Windows (MinGW, same toolchain as the game)
gcc -shared -O2 -static-libgcc -I melee-pc/src/pc/mods my_plugin.c -o my_plugin.dll
# Linux / macOS
gcc -shared -fPIC -O2 -I melee-pc/src/pc/mods my_plugin.c -o my_plugin.so   # .dylib on macOS
```

What the API offers (v1):

| Area | Functions |
|---|---|
| Logging & identity | `log`, `mod_id`, `mod_dir`, `mod_is_enabled`, `game_version` |
| Config | `config_number/bool/string`, `config_set_number/bool/string` |
| Hooks | `register_hook`, `unregister_hook` for `BOOT`, `FRAME`, `SCENE_CHANGE`, `MATCH_START`, `MATCH_END`, `FIGHTER_SPAWN` |
| Tunables | `set_fighter_attr`, `clear_fighter_attrs`, `fighter_attr_count`, `fighter_attr_name` |
| Live state | `scene_kind`, `in_match`, `is_netplay`, `frame_count`, `get_player`, `get_player_attr`, `set_player_attr`, `set_player_percent` |
| Virtual disc | `file_provider` |

Rules:

- Everything runs on the game thread; only call the API from `melee_mod_init`
  or a hook.
- The API only grows by appending. Check `api->size` before using a field newer
  than the version you built against.
- `set_player_attr` and `set_player_percent` refuse to run during netplay.
- Plugins are native code with full access to your computer. Only install
  plugins you trust or built yourself.

`modding/examples/training-helper/` is a complete plugin; the build compiles it
into `build/mods/training-helper/`.

## Netplay

Both players must run the same gameplay mods. The active set — ids, versions,
replaced file sizes, every attribute patch and whether a plugin is loaded — is
hashed into the online handshake, and two players whose sets differ are refused
with `unlock state or gameplay mod mismatch` instead of desyncing mid-match.
With no gameplay mods the hash is zero, so unmodded players stay compatible
with each other and with older builds.

Mods with `"affects_gameplay": false` are left out of the hash so cosmetic
packs (costume recolours, menu art) do not block matches. Only mark a mod
cosmetic if it truly cannot change the simulation — a replaced fighter file
that alters hitboxes is gameplay.

The rollback netcode re-simulates frames, so a `FRAME` hook that writes game
state is only correct offline; check `api->is_netplay()`.

## Examples

Built into `<build>/mods/` and disabled by default:

| Mod | Shows |
|---|---|
| `moon-gravity` | `*` tunables driven by a `config` value |
| `fox-triple-jump` | per-character `set` / `mul` / `add` |
| `training-helper` | a native plugin: hooks, config, live state, offline-only writes |

To try one, enable it on the Mods page and press Play, or boot straight into a
4-CPU test match:

```sh
MELEE_BOOT_SCENE=vs MELEE_DEBUG_VS=cpu4 build/melee <disc>
```

The log (`MELEE_LOG_FILE=path`) shows every mod line prefixed `mods:` and every
plugin line prefixed with the mod id.

## Not yet supported

- A character pack's voice, effects and results poses are file swaps, so
  they fall back to the base's when the base character is also playing;
  Kirby's copy hat stays the base's; new 3D results emblems can't be added
  (`series` picks an existing one).
- Item packs are not in the in-game item switch (switch them in the
  launcher instead). Two things are read from the base item's own entry
  rather than the pack:
  the Ray Gun's ammo count for the "shots fired" bonus, and the Super
  Mushroom's grow/shrink animation on the fighter.
- Character-specific attribute tunables and move/hitbox data patches.
- Editing config values in the launcher, and mod archives (`.zip`).
- Plugins on Android, iOS and the browser build. The loader itself is portable
  (SDL paths, no platform code), but so far it has only been built and tested
  on Windows x86-64.
