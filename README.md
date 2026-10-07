# TFC Teams — auto-balance + !scramble vote (metamod-p)

A metamod-p plugin for Team Fortress Classic. It keeps TFC teams even by moving
players **only while they are dead**, stops people joining a team that is already ahead,
and lets players vote to scramble the teams by score.

**What it does** (Windows and Linux servers):

- **Auto-balance** - moves a player from the bigger team when they die; a forced move after a
  warning if nobody on that team dies; never moves a flag carrier; respects class limits.
- **`!scramble` vote** - scrambles at respawn, all at once, or as a spectator reset; admins can
  force or cancel one.
- **Skill rating** - points per 10 minutes, carried from map to map, used to balance and
  scramble so the teams are even in skill, not just in numbers.
- **Pick mode** - `rank` (most even teams), `random`, or `mixed` (rank picks the swaps, a dice
  roll decides each one).
- **Auto Assign** - tells Auto Assign from a picked team; Auto Assign users are moved first and
  are put on the weaker team.
- **Player stats** - `!stats` (and per class), `!rank`, `!top10`, `!weapons`, `!rival`, and an
  end-of-map window of awards: MVP, kills, kill streak, caps, carrier kills, accuracy,
  headshots, damage, melee kills, healing, buildings destroyed, the best of each class and the
  rivalry of the map.
- **Name tracker** - every name a SteamID has used, for admins (`!names`).
- **Discord** - the end-of-map awards posted to a channel; the webhook is stored encrypted.
- **Per-map settings** and **chat tips**.

See [CHANGELOG.md](CHANGELOG.md) for what changed in each version.

## Building from source

The plugin builds against the metamod-p SDK (the `metamod/` and `hlsdk/` folders from
[Bots-United/metamod-p](https://github.com/Bots-United/metamod-p)). Put this repository inside
that folder, next to `metamod/` and `hlsdk/`. On a TFC install that is
`tfc\addons\metamod\metamod_sdk\`:

```
tfc/addons/metamod/metamod_sdk/
    metamod/
    hlsdk/
    tfc_teams_mm/      <- this repository
```

- **Windows:** Visual Studio, `tfc_teams_mm.sln`, **Release | Win32**. With the layout above, the
  build copies the DLL to `tfc\addons\tfc_teams_mm\tfc_teams_mm_mm.dll`.
- **Linux:** `make -f Makefile.linux` (32-bit g++: `apt install g++-multilib`). Elsewhere, point it at
  the SDK: `make -f Makefile.linux SDK=/path/to/metamod-p`.

## Install

1. The source folder goes in `tfc\addons\metamod\metamod_sdk\tfc_teams_mm\` (see *Building from source*).
   Open `tfc_teams_mm.sln` and build **Release | Win32**. The post-build step creates
   `tfc\addons\tfc_teams_mm\` and copies `tfc_teams_mm_mm.dll` there. It fails
   if the server is running and holding the DLL open.
2. `configs\tfc_teams.ini` goes in `tfc\addons\tfc_teams_mm\configs\`.
3. `tfc\addons\metamod\plugins.ini` needs this line:
   `win32 addons\tfc_teams_mm\tfc_teams_mm_mm.dll`
4. Restart the server. `meta list` should show **TFC Teams (balance + scramble)** as RUN.

### Linux server

The Linux build is `tfc_teams_mm_mm_i386.so`, a 32-bit library like HLDS itself.

1. Copy the whole `tfc/addons/tfc_teams_mm/` folder to the server: the `.so`, `configs/tfc_teams.ini`
   and `docs/`. The stats file `tt_stats.txt` and `tt_trace.log` are created next to the `.so`. To
   keep your stats, copy `tt_stats.txt` too; it is the same format on Windows and Linux.
2. In the server's `tfc/addons/metamod/plugins.ini`:
   `linux addons/tfc_teams_mm/tfc_teams_mm_mm_i386.so`
   Use one line only. Metamod on Linux ignores `win32` lines, and Windows ignores `linux` lines,
   so one plugins.ini can hold both.
3. Restart the server, or run `meta load addons/tfc_teams_mm/tfc_teams_mm_mm_i386.so`, then check
   `meta list`.

Linux paths are case-sensitive: the folder must be `addons/tfc_teams_mm` in lower case, and
`amxmodx/configs/users.ini` is read from the same `addons` folder.

The library needs nothing newer than glibc 2.2.4. It is built without the C++ runtime, so it
doesn't depend on the server's libstdc++. To build it yourself on Linux, install a 32-bit g++
(`apt install g++-multilib`) and run `make -f Makefile.linux` in this folder. The Makefile finds the metamod-p SDK
in the parent folder (the layout under *Building from source*). If it's elsewhere, pass
`make -f Makefile.linux SDK=/path/to/metamod-p`.

## Player chat

| Say | What it does |
|---|---|
| `!scramble` / `/scramble` / `scramble` | Vote to scramble. It passes at `vote_percent` of eligible players. |
| `!teams` | Shows the team counts, frag totals, and scramble votes. |
| `!forcescramble` | Admin only. Scramble now. Movers switch at their next respawn. |
| `!forcescramble now` | Admin only. Everyone who has to move is moved immediately. Living players die, the same as picking "change team". |
| `!forcescramble reset` | Admin only. Spectator reset: everyone goes to spectator, then after `scramble_reset_delay` seconds all are placed on the scrambled teams at once, with their classes and frags back. |
| `!forcescramble respawn` | Admin only. The normal "switch at next respawn" scramble, whatever `scramble_mode` says. |
| `!cancelscramble` | Admin only. Drops any pending moves and clears the votes. During a reset countdown it places everyone immediately instead (nobody is left in spectator). |
| `!rival` / `!rival <name>` | Head-to-head: your nemesis (who has killed you most), your favourite target, and your most even opponent. |
| `!names <name>` | Admin only. Every name that player has used (also `!names #<userid>` or `!names <SteamID>`, and part of an old name finds people who are not on). The line is hidden from chat. |

Admins are `admin STEAM_…` lines in the ini, plus AMX Mod X `users.ini` SteamID entries
that have the `amxx_flag` access flag (`j` by default).

## Chat tips

So players know the commands exist, the plugin puts tips in chat:

- **Every `advert_interval` seconds** (default 300, the first at `advert_first` = 270 s into the
  map), a line to everyone. It takes turns between "Teams stacked? Say !scramble ... !teams" and
  "!stats / !rank / !top10", skipping whichever is turned off (`vote_enabled`,
  `stats_enabled`). No tip goes out during a scramble, or when no humans are on.
- **When someone joins** (`advert_join` = 40 s after they connect), the same two lines just to
  them. It's once per player per server session, not again at every map change.
- **Your own tips:** `advert "text"` lines in `tfc_teams.ini` (up to 8) replace the built-in ones
  and are used in turn. Keep each under about 170 characters. Text in quotes can contain `//`.
- `advert_interval 0` or `advert_join 0` turns that part off. They can also differ per map (see
  Per-map settings).

The RTV plugin's tips start at 120 s with the same interval, so the two alternate.

## Server console / rcon

`tt_status` · `tt_scramble [now|reset|respawn]` · `tt_cancel` · `tt_reload` · `tt_move <name|#userid> <1-4|team name>`

`tt_names <name|#userid|SteamID>` · `tt_discord <id> <token>|off` · `tt_net` · `tt_net_test`

`tt_move` moves one player immediately. It is handy for testing on your own with bots.

## Matching players

The rating from *Player stats* (points per 10 minutes on a team, carried from map to map) is each
player's **skill**. With `balance_by_skill 1` (the default):

- **Balancing** moves the player who brings the two teams' total skill closest together, not just
  this map's frags. A team that is up on players *and* has the best players gives up one of its best.
- **Scrambles** (respawn and now, as well as reset) deal players out by skill.
- **New players** start at the median rating, so they count as average until they have played.
  This map's frags break ties between equal ratings.

**Rank, random or mixed** (`pick_mode`, for scrambles and balancing alike - it can differ per
map like any setting):

| `pick_mode` | Scrambles | Balancing | Auto Assign |
|---|---|---|---|
| `rank` (default) | Dealt by rank: the most even teams | Moves the best fit | Fewest players, then the weaker team |
| `random` | Random teams (sizes still even) | A random player from the big team | TFC's own: fewest players |
| `mixed` | The rank deal picks who *should* swap; each swap then rolls `pick_mixed_chance` % (default 50) to happen | Rank order, but each player rolls; those who roll out go to the back of the queue | Rank breaks the tie only if the roll says so |

Rank means skill with `balance_by_skill 1`, this map's frags with 0. In every mode the balancer
still moves bots first, Auto Assign users before people who picked their team, and newest joiners
first, and never moves someone onto a team where their class is full when anyone else could go.
A mixed scramble says in chat how the dice fell ("2 of 3 swaps rolled in"), so one that moves
fewer people than expected - or nobody - does not look broken. Mixed swaps pair a player going
one way with a similar-rank player coming back, so a swap that rolls out keeps both where they are
and the team sizes stay even. `!teams` says which mode the map uses.

**Picking a team or Auto Assign.** The team menu sends `jointeam 1`-`4` for a team and
`jointeam 5` for the Auto Assign button (read from the TFC client and tfc.so). The plugin
remembers which each player used:

- `join_auto_skill 1`: Auto Assign puts the player on the team with the fewest players (TFC's own
  rule) and, between teams with the same number, the one with less total skill. The player is told
  which team and why.
- `balance_prefer_auto 1`: when the balancer has to move someone, people who used Auto Assign go
  before people who picked their team (bots still go first).
- `!names` shows how often a player picked a team and how often they used Auto Assign.

**Rivals** (`stats_rivals 1`). Every kill is also counted per pair of players (at least one of them
a person): `!rival` shows your nemesis, your favourite target and your most even opponent, and the
end-of-map awards show the **rivalry of the map**, the pair with the most kills both ways. It is
saved in `tt_stats.txt` as `V` lines.

## Name tracker

`tt_names.txt` next to the DLL keeps every name each SteamID has played under: when it was first
and last seen and how many visits. Admins look people up with `!names` in chat (a window) or
`tt_names` in the server console. Bots and LAN players (no SteamID) are not tracked.
`names_track 0` stops recording.

## Discord: end-of-map awards

At the end of every map the plugin posts one message to a Discord channel: the score, the next
map, the MVP, the awards, the rivalry of the map and the top players. Nothing else is posted.
Names are escaped, and Discord is told not to turn anything into a ping, so a player called
`@everyone` stays plain text. The next map comes from AMX Mod X's `amx_nextmap`, which the RTV
plugin keeps set to the vote's winner; it is left out when unknown.

### Setting up

1. In Discord: channel settings > Integrations > Webhooks > New Webhook > Copy Webhook URL.
2. Give the plugin the link, one of two ways:
   - **Server console or rcon:** `tt_discord <id> <token>` - the long number and the long code
     from the link (`https://discord.com/api/webhooks/<id>/<token>`). The whole link in quotes
     works too, but **through AMX Mod X's `amx_rcon` use the short form**: `amx_rcon` passes on
     only 127 characters, which cuts a webhook link off.
   - **A file:** put the line `discord=https://discord.com/api/webhooks/...` in
     `tt_secret_import.txt` next to the plugin. It is read at the next map change, stored
     encrypted, and the file is wiped and deleted.
3. `tt_net_test` posts a test message. In game, admins get a chat line saying whether it worked;
   `tt_net` in the server console shows the details, and `tt_trace.log` has a `Net:` line for
   every failure.

`tt_discord off` removes the webhook; `net_enabled 0` in `tfc_teams.ini` stops posting.

### Keeping the link safe

A webhook link is a password for posting in that channel, so it is **never in `tfc_teams.ini`**:

- It is stored in `tt_secret.dat`, **encrypted with a key made from this machine's own ID**
  (Windows MachineGuid, Linux `/etc/machine-id`; in a container without one, a random key in the
  server user's home folder). Copied anywhere else - a backup, an FTP download, GitHub - the file
  is noise and the plugin there asks for the link again.
- The plugin never writes the link to `tt_trace.log` or the console; `tt_net` shows only
  `https://discord.com/api/webhooks/<id>/****`.
- **rcon is logged:** HLDS writes rcon commands, link included, to its logs, and AMX Mod X logs
  every `amx_rcon` command line in `addons/amxmodx/logs`. The import file avoids that.
- Anyone who can run programs on the game server machine itself, as the server's user, can still
  get it, just as the plugin does.

If the link gets out, delete the webhook in Discord and make a new one.

### How it is sent

On a separate thread, so the game never waits on the network (8-second timeout). Windows uses
WinHTTP; Linux uses [BearSSL](https://bearssl.org) 0.6 (MIT licence;
`third_party/bearssl-0.6.tar.gz`, unpacked by `Makefile.linux`) and checks the certificate against
the system's root certificates, or a built-in set if the system has none (`net_ca_file` for your
own). If Discord says to slow down (HTTP 429), the plugin waits as told and tries once more.

## How a player is moved (and why the old scramble plugins didn't)

TFC keeps a player's team in its own `CBasePlayer` member (`team_no`). It only copies that
into `pev->team`. Writing `pev->team` changes the scoreboard colour and nothing else. That
is what `tfc_vote_scramble` / `tfc_hybrid_scramble` did, which is why they "ran" but nobody
really changed team.

This plugin moves players with TFC's own two commands:

1. It sends the game **`jointeam N`**. In tfc.so this is `ClientCommand → TeamFortress_TeamSet(N)`,
   the same thing the team menu does.
2. It then sends the **class name** (`soldier`, `medic`, …) so `ChangeClass` gives the player
   back the class TeamSet cleared.

Both commands go straight into the game DLL's `ClientCommand`, with the arguments served
through hooked `CMD_ARGV/ARGC/ARGS`. That is the same route AMXX's `amx_client_cmd` uses,
and it works on bots.

The class menu that TeamSet pops up is swallowed, because the class is restored straight away.

### Class limits

TFC checks every class pick in `CBasePlayer::CantChange`, and the class the plugin gives back
goes through the same check. Two things can refuse it:

- **The map's illegal classes** (`info_tfdetect`, per team). TFC says `#Game_cantplayclass`.
- **The server's `cr_<class>` cvars** (`cr_scout` … `cr_engineer`, `cr_random`). `0` means no
  limit, `-1` means nobody may play that class, and `N` means at most N per team. TFC says
  `#Game_enoughofclass`. The cvars are read live, so a server or map config that sets them is
  obeyed.

When the class a player had is not allowed on the new team:

- **A person** gets the class menu, plus a chat line saying which class is full, so they can
  pick another.
- **A bot** can't use the menu, so it is given the first open class instead.

The balancer tries to avoid this in the first place. Anyone whose class is full on the smaller
team is ranked last among the players it could move.

What TeamSet does, read from tfc.so, and what that means for the plugin:

- **It refuses during the death animation** (`deadflag == DEAD_DYING`). So "on death" means
  after the death animation and before respawn. TFC's PlayerDeathThink always leaves at least
  one frame in that window.
- **It refuses a second change within 1 second.** A refused move is retried 1.5 s later.
- **It refuses a team at its `info_tfdetect` limit** (`#Game_teamfull`). The plugin reads
  those limits from the map, so it never picks a full team.
- **It counts as a suicide for the player.** They get one extra death on the scoreboard and
  a skull in the kill feed; the frag is handed back. That is the same as changing team by hand.

## Map teams

These come from the map itself:

- `info_tfdetect number_of_teams`
- the per-team player limits (`ammo_medikit` / `ammo_detpack` / `maxammo_medikit` / `maxammo_detpack` = teams 1–4)
- the illegal-class masks (`maxammo_shells/nails/rockets/cells`). A value of **-1 means a
  civilians-only team**, like hunted's VIP. The plugin never balances into or out of that team.
- which teams have `info_player_teamspawn` / `i_p_t`

The key→team mapping was read out of `ParseTFDetect` in tfc.so; it is not guessed. If the
plugin is loaded mid-map, it reads the same entities from the .bsp.

## Balance rules

Balancing starts when the biggest team has `balance_threshold` or more players than the
smallest team, for `balance_delay` seconds.

The biggest team is ranked like this:

1. Bots come before humans (`balance_prefer_bots`).
2. People who joined with Auto Assign come before people who picked their team (`balance_prefer_auto`).
3. Players who picked their team within `balance_new_window` seconds come next, newest first.
4. Everyone else is ordered by **fit**: the player whose move brings the two teams' totals
   closest together - total skill with `balance_by_skill 1`, this map's frags with 0. A player
   worth f moving from a team with total B to one with S leaves a gap of |B − S − 2f|.

The top `balance_candidates` are *preferred*. A preferred player is moved the moment they
are dead. If none of them has died after `balance_patience` seconds, anyone on the big team
who dies is moved.

A moved player is immune for `balance_immunity` seconds. Balancing pauses while a scramble
is running.

**When nobody on the big team dies** (someone AFK in spawn, or a defence that never gets
killed), balancing would otherwise wait forever. After `balance_force_after` seconds (60 by
default, counted from when balancing starts), the plugin steps in:

1. It picks the best-ranked player on the big team who is **not carrying a flag or goal item**.
   A carried goal item follows its carrier; the plugin reads that from the item, as
   `tfgoalitem_GiveToPlayer` in tfc.so sets it.
2. That player gets `balance_force_warn` seconds of warning (10 by default), on screen and in chat.
3. Then they are moved while alive. TFC kills a living player it moves, just like picking
   "change team", and the frag is handed back.

If they die during the warning, they are moved then instead. If they pick up the flag during
the warning, someone else is chosen. If the teams even out, the move is called off and the
player is told. Set `balance_force_after 0` to only ever move players on death.

**When a bot can even the teams**, it is done sooner (`balance_bot_delay`, 10 seconds by
default). FoxBot fills the server up to a head count that includes spectators, and when a
human joins it kicks a bot from whichever team it picks, so a 5 v 3 can otherwise sit there
until someone leaves. If the big team has a bot that is free to move (not immune, not
carrying a goal item), the plugin acts after `balance_bot_delay` instead of `balance_delay`:

1. The first bot on the big team to die is moved.
2. If none dies within another `balance_bot_delay` seconds, the best-ranked bot is moved
   alive, with no warning (TFC kills it on the way over and hands the frag back).

Humans are not moved and not warned while this is happening; they are only considered once
`balance_delay` is up. Set `balance_bot_delay 0` to treat bots like everyone else.

## Scramble rules

When the vote passes, or an admin forces it, players are dealt out by skill (`balance_by_skill 1`;
this map's frags with 0), best first. Each goes to the team with the lowest total among those
still under an even share, so
team sizes end up at most 1 apart. The piles are then matched to team colours so as many
people as possible stay where they are.

Each player who has to move gets a chat line and a centre message, and switches on their
next death. Moves still waiting after `scramble_max_wait` seconds are dropped, and the
balancer takes over. After a scramble, voting is closed for `vote_cooldown` seconds.

## Player stats

Stats are kept per SteamID. Bots are kept as `BOT:<name>`: they get a rating so the scramble can
place them, but they never appear in `!rank` or `!top10`. The file is `tt_stats.txt` next to the
DLL. It is saved at every map end, every `stats_save_interval` seconds, and with the console
command `tt_stats_save`.

| Say | Shows |
|---|---|
| `!stats` / `!stats <name>` | A **window** (TFC's MOTD panel): rating, rank, this map / lifetime time, kills, deaths, K/D, best kill streak, melee kills, healing (medics), caps, pickups, carrier kills, accuracy, headshots, damage, buildings, the top weapons, and a list of the **classes** played |
| `!stats <class>` / `!stats <name> <class>` | The same window for **one class**: `!stats soldier`, `!stats engineer` (also `demo`, `hw`, `engy`). Shows that class's numbers and weapons. |
| `!top10` (`!top`, `!rankings`) | The **rankings window**: the top 15 by rating, with you marked `>>` |
| `!rank` | Your rank and rating in chat |
| `!awards` | The last map's awards again (window) |
| `!weapons` / `!weapons <name>` | Every weapon, as a table in your console (headshots only ever appear for the sniper rifle) |

The window is TFC's own message-of-the-day panel (`TeamFortressViewport::MsgFunc_MOTD`,
client.so 0x8af40). It holds up to 1536 characters in a proportional font, which is why the text
is written as "label: value" lines. Close it with OK or Esc.

`stats_window 2` switches to a paged menu on the left of the screen instead: overview, one page
per class, and all weapons. It uses 8 = back, 9 = next and 0 = close, and stays up for
`stats_menu_time` seconds. `stats_window 0` gives chat lines.

**Class stats.** Every number is also counted under the class the player was at the time.
Sentry gun and dispenser kills always go to the engineer. Engineers also get **built** (sentries
and dispensers, from TFC's `Built_Dispenser` / `Sentry_Built_Level_1` log lines) and **enemy
buildings destroyed** (`Sentry_Destroyed` / `Dispenser_Destroyed` `against` an enemy). Class
counts start with this version, so older lifetime stats show in the overview only.

Only `!` works for these. AMX Mod X's stats.amxx already answers `/rank`, `/stats` and `/top15`.

**End of map.** At intermission, with `stats_summary 2`:

- Every player gets a **window** over the scoreboard. It shows the awards (MVP, most kills, longest
  kill streak, caps, carrier kills, best accuracy with 30 shots minimum, headshots, damage, melee
  kills, healing and enemy buildings destroyed), the **best of each class**, and **their own map**:
  kills, deaths, best streak, points, time per class, and the rating change. With long names and
  every award given the window would not fit, so the best of each class is then left to `!awards`.
- **"MAP AWARDS / MVP"** fades in across the top of the screen.
- The awards go into chat.

The client opens the MOTD window even during intermission: `TeamFortressViewport::ShowVGUIMenu`
(client.so 0x8a370) only lets menu 5 through then. `stats_summary 1` gives chat lines only.

**How each number is measured.** All of this was read from tfc.so. See `tt_stats.cpp` for the
addresses.

- **Kills, deaths, suicides and teamkills** come from TFC's own kill log lines, which the plugin
  sees whether or not server logging is on. The suicide TFC records when this plugin moves
  someone is not counted.
- **Damage and hits** come from what the game records on the victim each frame (who hit them and
  how much), read just before TFC clears it.
  - Hitscan weapons: what hit them is the **weapon entity** (`tf_weapon_sniperrifle`, owned by
    the player), as seen on the live server.
  - Rockets and grenades: the projectile is freed in the frame it explodes, so the owner and
    weapon of every owned entity are noted each frame, keyed by edict and serial number, and read
    from there.
  - The pyro's incendiary cannon fires a rocket with the **same classname as the soldier's**
    (`tf_rpg_rocket`, and the kill log says `rocket`; `CTFIncendiaryCRocket::Spawn`, tfc.so
    0xf29f0). Only a pyro has the incendiary cannon, so a pyro's rocket counts as the incendiary
    cannon.
- **Shots** come from the effect event every TFC weapon plays when it fires. The same event is
  also what tells the sniper rifle and autorifle apart, since they share a model, and it tells
  which gun fired a nail.
- **Headshots** are TFC's own. Only the sniper rifle has one: a charged shot into hitgroup 1.
  `CBasePlayer::TraceAttack` doubles the damage and sends the sniper `#Sniper_headshot`, and the
  plugin counts that message. The first version counted any trace that ended in a head. That also
  picked up the sniper's laser dot and bots' line-of-sight checks, and credited shotguns with
  hundreds of "headshots". Those old counts were reset once, the first time this version loaded
  `tt_stats.txt`.
- **Flag pickups, caps and carrier kills** come from who each flag/goal item is attached to. In
  the first live run, on 2fort, ravelin, flagrun, avanti, baconbowl, copper3 and cz2, every real
  capture happened in a single frame: the carrier activated a goal (a `triggered "..."` log line),
  lost the item, and on most maps the team score went up.
  - A carrier who loses the item alive **as the team score rises (within 0.5 s)** captured it.
  - If the score doesn't rise, it still counts when the goal they reached has **`cap`** in its name
    and nothing activated with it has **`return`** in its name (`stats_cap_word` /
    `stats_return_word`). cz2's zone captures (`#cz_bcap1`..`5`) score only later, when points are
    held, and flagrun's `"blue 3 return result"` puts your own flag back.
  - 2fort-style `Captured_*_Flag` log lines count directly.
  - A kill of someone carrying is a carrier kill.
- **Accuracy** is hits divided by shots. One rocket or shotgun blast that hurts three people is
  one hit, so accuracy never goes over 100%. Hitting yourself or a team-mate doesn't count.
- **Grenades and detpacks:** each throw counts as one shot. The pieces a grenade breaks into
  (MIRV bomblets, nail-grenade nails) don't count as extra shots, but their hits and damage go
  to that grenade.
- **Kill streak:** enemy kills in a row without dying. A teamkill neither adds to it nor ends it;
  being moved by this plugin doesn't end it either. The award needs a streak of at least 2.
- **Melee kills:** crowbar (and the civilian's umbrella), spanner, knife and medikit hits. The
  medic's infection is not melee.
- **Healing:** TFC logs `"medic" triggered "Medic_Heal" against "team-mate"` when the medikit
  heals someone (only when they were hurt, or for the overheal of up to 50 over their maximum),
  and `Medic_Cured_...` / `Medic_Doused_Fire` for cures (`CTFMedikit::AxeHit`, tfc.so 0x125830).
  The line carries no amount, so the health healed is the team-mate's health right after the heal
  minus at the start of that server frame.
- **Not credited:** sentry gun damage and building kills that TFC only tracks internally. Sentry
  kills are credited, from the kill log.

**Rating.** The rating is points per 10 minutes on a team: kill 1, carrier kill +2, cap 5,
flag pickup 1, teamkill −2, and optionally healing (`stats_points_heal`, points per 100 health
healed; 0 by default). All of these can be changed in the ini. After each map (once
`stats_min_map_minutes` has been played) it becomes 70% of the old rating plus 30% of that map.
A new player starts at the median rating of everyone known. Scrambles and the balancer use this
rating (see *Matching players*), with this map's frags as a tiebreak.

### Capture debugging (`stats_debug_caps 1`)

Maps capture in different ways, so while capture detection is being checked on real maps,
`tt_trace.log` gets a `CAPDBG` line for each of these:

- `CAPDBG pickup` / `CAPDBG item lost`: who took or lost a goal item, alive or dead, and the
  item itself (entity number, classname, netname, model, team, owner, aiment, movetype).
- `CAPDBG TeamScore`: every team score change. When a score goes up, the line also lists every
  member of that team with their carrying state and last item.
- `CAPDBG log`: every `triggered "..."` line TFC logs. Goals with a netname log it when a player
  activates them, which is where map-specific capture names appear.
- `CAPDBG TextMsg to all`: every broadcast game message, such as "%s CAPTURED the BLUE flag!".
- `CAPDBG NOT counted`: a player lost an item while alive, but their team's score didn't go up.
  This is the line to look for if a real capture was missed.

Open an issue with the `CAPDBG` lines from a map where captures were missed or wrong, and the detection can
be fitted to what that map actually does.

## Spectator reset scramble

Set `scramble_mode reset` for votes to use it, or use `!forcescramble reset` / `tt_scramble reset`
any time. It is made for attack/defend maps, where you want everyone to start again together.

1. The teams are planned by score, the same way as a normal scramble.
2. Everyone on a team goes to spectator through TFC's own `spectate` command. This works even
   with `allow_spectators 0`: the plugin lifts the cvar for that one call and nobody sees a
   "cvar changed" message. Going to spectator removes buildings, detpacks and anything being
   carried, the same as a player doing it themselves.
3. A countdown is shown. Any team picks made during it are refused.
4. After `scramble_reset_delay` seconds, everyone is put on their new team with their class and
   spawns straight away.
5. **Frags:** TFC sets a player's frags to 0 when they go to spectator (`StartObserver`). With
   `scramble_reset_restore_frags 1` each player's frags are put back after they rejoin, so the
   scoreboard is unchanged. TFC's own internal team frag totals keep the drop. Those totals only
   feed its optional score equaliser and the end-of-map log line, not the scoreboard.

## Per-map settings

Any setting can be different on particular maps. For example, respawn scrambles on 2fort and reset
scrambles on dustbowl. There are two ways, and both can be used:

**A section in `tfc_teams.ini`.** The lines under `[dustbowl]` apply only on dustbowl. Put sections
at the end of the file, after the settings for every map, because a later line wins.

```
[2fort]
scramble_mode           respawn

[dustbowl avanti cz2]          // several maps (commas are fine too)
scramble_mode           reset
scramble_reset_delay    4

[cz*]                          // * matches anything
vote_cooldown           600

[all]                          // back to lines for every map
```

**A file `configs/maps/<map>.ini`**, e.g. `configs/maps/dustbowl.ini`. It uses the same
`key value` lines and is read after `tfc_teams.ini`, so it wins. Sections don't work inside it,
because the whole file is for that one map.

- **What changes:** only the keys you list. Everything else keeps the value from the top of
  `tfc_teams.ini`.
- **Map names:** matched without regard to upper or lower case.
- **Checking it:** on each map, `!teams` shows the scramble mode in use. Every per-map setting
  applied is written to the trace log as `Config: dustbowl -> scramble_mode reset`, followed by
  the full `Config (dustbowl): ...` line.
- **Admin override:** `!forcescramble reset` and `!forcescramble respawn` still override the
  map's mode for that one scramble.

## Only one copy

The plugin refuses to load twice. If `plugins.ini` names it twice, for example as both
`tfc_teams_mm.dll` and `tfc_teams_mm_mm.dll`, the second copy writes
`ANOTHER COPY IS ALREADY LOADED` to the trace log and stays out. Two copies running at once is
what stacked Red in the first test: each copy planned and carried out its own scramble.

## Trace log

`tfc\addons\tfc_teams_mm\tt_trace.log` is written while `debug 1` is set. It records:

- the map's team layout at each map start
- every team change seen
- every move, with team, class, deadflag, health and frags before and after
- the balance rankings and every vote

When testing, the lines to look for are `Move OK:` with `class 3->3` (the class came back).

## Licence

GPL-3.0. See `LICENSE`. `third_party/bearssl-0.6.tar.gz` is BearSSL by Thomas Pornin, MIT
licence (`third_party/BEARSSL-LICENSE.txt`).
