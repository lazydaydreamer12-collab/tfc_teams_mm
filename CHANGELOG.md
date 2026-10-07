# Changelog

## 1.1.3

- **Bots even the teams sooner** (`balance_bot_delay`, default 10 s). FoxBot counts
  spectators toward its bot total and kicks bots from either team when humans join, which
  left gaps like 5 v 3 standing for minutes behind `balance_delay`. When the big team has a
  bot that can move, the first one to die is moved after `balance_bot_delay`, or the
  best-placed one is moved alive after another `balance_bot_delay`. Humans still wait for
  `balance_delay`. `balance_bot_delay 0` turns this off.
- Chat lines with a `%` in them lost letters on players' screens: the mixed-scramble line read
  "(50 hance each)". The game's client treats chat text as a format string, so every `%` is
  now sent doubled and shows as one.

## 1.1.2

- Fixes 1.1.1, which stopped seeing bots at all: FoxBot adds its bots without the plugin
  being told, and 1.1.1 only counted players it had been told about. Scrambles and balancing
  saw only the humans ("2 players, 0 to move"), and the join block counted 2 v 0 with bots on
  both teams. Bots are now picked up the first time they move.

## 1.1.1

- A player connecting into a slot a bot had just left was taken for that bot while their
  game was still loading: counted on the bot's team, and given a stats record named
  `BOT:<their name>`. Players now count only once they are in the game: put in the server,
  or - for bots, which FoxBot adds without the plugin being told - seen playing.
- The capture debug lines (`CAPDBG`) no longer show an item from the previous map or the
  slot's previous player.

## 1.1.0

**Matching players**
- Balancing and scrambles go by each player's **skill rating** (points per 10 minutes on a team,
  carried from map to map) instead of this map's frags - `balance_by_skill` (on by default).
  New players start at the median rating.
- **`pick_mode`**: `rank` (default - the most even teams), `random` (rank ignored), or `mixed`
  (rank picks who should swap, then each swap rolls `pick_mixed_chance` %, default 50). Applies
  to scrambles, balancing and Auto Assign, and can differ per map.
- **Auto Assign is recognised** (`jointeam 5`). Auto Assign users are moved before players who
  picked their team (`balance_prefer_auto`), and Auto Assign puts a player on the team with
  fewer players, then the weaker one (`join_auto_skill`).
- **Rivals**: `!rival [name]` - your nemesis, favourite target and most even opponent - and the
  **rivalry of the map** at the end of each map (`stats_rivals`).

**Name tracker**
- Every name each SteamID has used, kept in `tt_names.txt`. Admins: `!names <name|#userid|SteamID>`
  in chat (a window) or `tt_names` in the server console. Also shows how often a player picked a
  team vs used Auto Assign. `names_track 0` turns it off.

**Stats and awards**
- New awards: **longest kill streak**, **most melee kills**, **most healing**.
- `!stats` shows best kill streak, melee kills, and for medics the health given and cures.
- Optional `stats_points_heal` - rating points per 100 health healed (0 by default).
- The end-of-map window leaves the class list to `!awards` when long names and every award
  would not fit.

**Discord**
- The end-of-map post - score, next map, MVP, awards, rivalry, top players - to a Discord channel.
- The webhook is never in the config: set it with `tt_discord <id> <token>` (fits through
  `amx_rcon`) or a `discord=` line in `tt_secret_import.txt`; it is stored encrypted for that
  machine only. `tt_net` shows the state, `tt_net_test` sends a test. `net_enabled 0` turns it off.
- Sent on a separate thread, so the game never waits on the network. Linux uses BearSSL
  (MIT licence, `third_party/`).

**Other**
- `!teams` says which scramble and pick mode the map uses.

Upgrading from 1.0.0: `tt_stats.txt` loads as before (new counters start at 0). New settings
have defaults, so an old `tfc_teams.ini` works - copy the new lines from `configs/tfc_teams.ini`
to change them.

## 1.0.0

First release: auto-balance on death with a forced move after a deadline, join blocking, the
`!scramble` vote (respawn, now, spectator reset), player stats and end-of-map awards, per-map
settings, chat tips, Windows and Linux builds.
