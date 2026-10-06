# Changelog

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
