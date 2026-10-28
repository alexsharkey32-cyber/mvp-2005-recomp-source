# League packs

A league pack is a whole league as plain files: clubs, players, depth charts, managers, last season's stats and
the first season a dynasty plays. Edit it in any spreadsheet, build it, and pick it under **Settings > Game >
Rosters** (or `rosters = 2` and `league = pack-<name>` in mvp2005.ini).

```
python3 tools/leaguepack.py export mods/rosters-2026/DATA/DATABASE/DATABASE.BIG packs/modern --name "MLB 2026" --season 2027
python3 tools/leaguepack.py check packs/modern            # report problems, write nothing
python3 tools/leaguepack.py build packs/modern            # -> mods/pack-modern/
```

`export` then `build` gives back the same database byte for byte, so any pack can start from the modern
rosters (or from the disc's 2005 database) and change only what it needs.

## Files

| file | one row per | key |
|---|---|---|
| `pack.json` | – | `name`, `season` (first season of a dynasty), `description`, `author`, `sim` (`fastball_base`: see hle_pitch_quality) |
| `teams.csv` | club (30 franchises, their AAA / AA / A affiliates, the All-Star squads) | `code` |
| `players.csv` | player | `id` |
| `lineups.csv` | player on a club's depth chart | `team`, `player` |
| `stats.csv` | player with numbers last season / in his career | `id` |
| `managers.csv` | manager | `id` |
| `overlay/` | any file to put in the mod as it is (`overlay/DATA/...`, same layout as the disc) | |
| `rates.txt` (optional) | season: `year k bb hbp 1b 2b 3b hr`, shares of plate appearances | `year` |

Ids are 9 hex digits. Player ids can be anything unique; the club `code` and `id` are the game's 126 slots and
stay as exported (a club is renamed, not replaced: change `location`, `long_name`, `short_name`).

## players.csv

| column | values |
|---|---|
| `team` | club code (blank: free agent) |
| `jerseynum`, `bats` (0 R, 1 L, 2 S), `throws` (0 R, 1 L) | |
| `primary_position` | 0 SP, 1 C, 2 1B, 3 2B, 4 3B, 5 SS, 6 LF, 7 CF, 8 RF, 9 DH, 10 RP |
| `secondary_position` | as above, 11 OF, 12 IF, 13 utility |
| `height`, `weight` | inches - 48, pounds - 100 |
| `birthday` | real date, YYYY-MM-DD (the game's age follows it from `season`) |
| `face` | < 900 a real 2005 head (GHEAD.BIG), 901-915 generic heads; `skin_tone` 0-10, `hairstyle` 0-9, `haircolour` 0-6, `facialhair` 0-7, `boneprofile`, `bodytype` |
| `audioid`, `photo` | commentary name clip / 2005 photo (photo 1 = has one, 2 = none) |
| `platediscipline`, `bunting`, `stealing_aggressive`, `baserunning`, `fielding`, `range`, `throwstrength`, `throwaccuracy`, `durability` | 0-15 |
| `speed` | 0-100 |
| `salary` (in $100,000), `contract_length` (years), `starpower` 0-4, `topprospect`, `hidden` | |
| gear: `battingstance`, `swingtype`, `batcolour`, `batglove`, `wristband`, `elbowguard`, `shinguard`, `socks`, `catchermask`, `fieldglovecolour`, `ditty` | |
| `vsL_...` / `vsR_...` | batting against left- / right-handed pitchers: `contact`, `power` 0-100; hot zones `hit_ul` .. `hit_lr` 0-2; spray `lf_pct`/`cf_pct`/`rf_pct` and `hr_pct`, `fb_pct`, `ld_pct`, `gb_pct`; `chase*`, `take*`, `miss*` 0-15 |
| `pitch_...` (pitchers only) | `stamina` 0-100, `pickoff` 0-15, `fastball_control` 0-100, `fastball_velocity` mph; `pitch2`..`pitch5`: `type` (1 changeup, 2 curve, 3 knuckleball, 4 screwball, 5 sinker, 6 slider, 7 splitter, 8 two-seamer, 9 cutter, 10 circle change, 11 forkball, 12 knuckle curve, 13 12-6 curve, 14 slurve), `movement` 0-15, `description`, `control`, `velocity`; `pitcher_delivery` |

## lineups.csv

Per club and player, the role in the four depth charts the game keeps: against right-handed starters (`R_`) and
left-handed (`L_`), in AL parks (with the DH) and NL parks: `C`, `1B` .. `RF`, `DH`, `B` (bench), `SP1`..`SP5`,
`LR`, `MR`, `SU`, `CP`; `battingorder` 1-9 or -1.

## teams.csv

`location`, `long_name`, `short_name` (the abbreviation), `league` / `division`, `artid` (which club's art: logo,
uniforms, park), the ballpark (`park_name`; `park_base`, the code of another park on the disc to play in, old ones
included: TIGR Tiger Stadium, POLO Polo Grounds, FORB Forbes Field, SHIB Shibe Park, CROS Crosley Field, GRIF
Griffith Stadium, SPOT Sportsman's Park, ASTR Astrodome, VETE Veterans Stadium, CINE Riverfront, CLEV Cleveland
Stadium, METP Metropolitan Stadium, OLYM Olympic Stadium, QUAL Jack Murphy, FENC old Fenway; `park_fences`, the
five posted distances in feet, left line to right line, `330-375-400-375-330`: the outfield is reshaped to them;
built with `--iso`), team tendencies (`pinchhitter` .. `suicidesqueeze`, 0-2), rivalries (`vs_...`), the first / second
half record, and for the 30 franchises the organisation: `org_team_mlb`, `org_team_aaa`, `org_team_aa`,
`org_team_a` (club codes), `org_mlb_manager`, `org_minors_manager` (`m:<manager id>`), `org_budget`.

## Draft classes

`draft/<year>.csv` (the columns of players.csv, without `id` and `team`): the players clubs can draft after
that season. When the game makes its class of made-up rookies, each one is replaced, in order, by the next player
of the file of the same kind (pitcher or position player); best first, since the draft takes from the top. A
year without a file keeps the game's own rookies. Eras made with `tools/eras/build_era.py` come with the real
classes: every player who reached the majors after the farm systems' prospects, in the class of about the year
he turned 21, rated from his first big-league seasons.

## The league in simulated games

With **Settings > Game > Simulated games: Real league of each season** (the default) simulated games come out at
the rates the major leagues had in the season being played: strikeouts, walks, hit batsmen, singles, doubles,
triples and home runs per plate appearance, from `mods/league_rates.txt` (every season 1871-2025, made from the
Lahman database by `tools/eras/league_rates.py`; later seasons play like the last one). The ratings still decide
who is good at what. A pack's own `rates.txt` (same columns) replaces those seasons, for a made-up league with its
own style of play; `calibrate 0` in `sim.txt` leaves that pack's games to the game's own numbers. The game's ratings set players
closer together than real ones are, so each outcome is also spread: `spread_k` (default 3.2), `spread_bb` (1.25),
`spread_hbp`, `spread_1b` (1), `spread_2b` (1.15), `spread_3b` (1.3), `spread_hr` (1.5) in `sim.txt`, 0.5-3.
`tools/simstats.py DUMP --current --year 1977` compares a season in progress with the real one.

The ratings are percentile ratings, so the best players of any era look alike to the simulation. Two optional
files bring back how far the stars stood above their league (era packs have both):

- `hr_factor.txt`: `key home-run-factor hit-factor`, one hitter per line (key: the player's database key in hex).
  The factors multiply his home runs and his hits on balls in play. `tools/eras/hr_factor.py PACK LAHMAN_DIR` makes
  it from the hitters' last two real seasons (Ruth 2.6, Cobb's hits 1.22); `tools/eras/build_era.py` runs it.
- `pitch_factor.txt`: `key strikeout-factor walk-factor`, one pitcher per line. `tools/eras/pitch_factor.py PACK
  LAHMAN_DIR DUMP` makes it from the pitchers' last two real seasons and a simulated season of the pack (a database
  dump made without the file; it measures what the simulation gives each kind of pitcher).

- `bat_factor.txt`: `key strikeout-factor walk-factor`, one hitter per line, from `tools/eras/bat_factor.py PACK
  LAHMAN_DIR DUMP` the same way (the simulation otherwise gives a Luis Arraez four times his strikeouts).

League totals stay on the season's rates either way.

How long starters last follows the season too (era packs before 2000): four-man rotations before 1975, and how
deep starters go and how often managers let them bat or stay in, set so complete games and the aces' innings come
out near the real seasons (from 62 % complete games in 1910 to 7 % in 1993). `drain` and `pull` in `sim.txt`
override the tiring and the pitching changes. Steal attempts follow the season as well (the running 1910s and
1980s, the walking 1950s).
