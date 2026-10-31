# WXPlane for X-Plane 12

Replaces X-Plane 12's flawed weather with live **NOAA** data, refreshed every **30 minutes** by default (`update_minutes`) and whenever you fly 40 nm:

* **METARs** from NOAA's Aviation Weather Center (surface wind, visibility, fog, clouds, precipitation, temperature, pressure)
* **ATIS** (D-ATIS) merged over the METAR for airports near you
* **Winds aloft and temperatures aloft** (NOAA FD winds), applied from the surface to FL600 (60,000 ft) instead of X-Plane's own

Requires **X-Plane 12.3 or newer** (it uses the `XPLMWeather` plugin API, SDK 4.2.0).

## Menu (Plugins → WXPlane)

| Item | What it does |
|---|---|
| **Enabled** | On/off. Off removes all of the plugin's weather and gives control back to X-Plane. |
| **Refresh Weather Data Now** | Downloads and applies immediately (no 1–2 min fade). Also turns the plugin on if it was off. |
| **Show Local METAR/ATIS Data...** | Opens a scrollable, resizable window listing every nearby station: raw METAR, ATIS text and letter, what WXPlane applied to the sim (visibility, fog, clouds, wind), and the NOAA winds-aloft stations in use. Mouse wheel scrolls. |
| **Radar Storms (NOAA MRMS)** | Turns the NOAA radar mosaic into storm cells (see Radar below). On by default. |
| **Radar Outside US (experimental)** | Uses RainViewer image tiles where NOAA has no radar. Off by default. |
| **Apply Weather Immediately (may stutter)** | Off by default: weather fades in over 1-2 minutes. On: first load, refreshes and manual refresh force X-Plane to rebuild at once, which is faster but can stall for a moment. |
| **Use ATIS** | Merge D-ATIS data for airports near you. |
| **Use NOAA Winds/Temps Aloft** | On (default): winds and temperatures at every level come from NOAA. Off: only the lowest 10,000 ft is touched and X-Plane's own winds/temps are used above. |
| *status lines* | Last update time, METAR/ATIS/fog counts, number of NOAA winds-aloft stations, nearest station. |

Settings are saved to `Output/preferences/WXPlane.cfg`.

## How it works

1. Every `update_minutes` (30; or when you move 40 nm from the last download point) a background thread asks NOAA for all METARs within 120 nm **of the aircraft**, so the data follows you along your route. The sim is never blocked by the network.
2. The nearest 40 reports are applied with `XPLMSetWeatherAtAirport` (replacing the sim's own METAR for that airport) or `XPLMSetWeatherAtLocation`. Only changed stations are re-sent and stations that left range are erased, so stale stations never linger.
3. If an ATIS exists for one of the 2 nearest airports within 35 nm, its values override that airport's METAR values.
4. **Winds/temps aloft:** NOAA FD winds (3,000–39,000 ft, ~175 US stations plus Alaska/Hawaii) are downloaded every 30 minutes. For each weather report, the 4 nearest FD stations are blended by inverse-distance-squared on the wind *vectors* (so opposing winds cancel instead of averaging compass numbers). The report then carries a surface wind layer plus the aloft layers, and a temperature profile anchored to the airport's own METAR temperature. The highest NOAA wind is held up to FL600 so the sim's own winds never take over above FL390. X-Plane interpolates between reports and layers.
5. **Smooth transitions:** periodic updates are applied without the "immediate" flag, so X-Plane fades them in over 1–2 minutes. The first load and the manual Refresh apply immediately.

### Visibility

* Below 10 SM the METAR/ATIS value is used exactly (`4SM` = 6.4 km, `1/4SM` = 400 m).
* "10 SM or more" (`10SM`, `9999`, CAVOK) maps to `clear_vis_m` (40 km), since those reports are a floor rather than a measurement.
* **Self-check:** about 100 s after each update (15 s after a manual refresh), when you are within 15 nm of a reporting airport, the plugin asks X-Plane what visibility it is *actually* using there and logs it (`visibility check ...` in `Log.txt`). If it differs from the report by more than 15 % (the sim blends in its own weather), the plugin adjusts what it sends and re-applies, up to 4 passes per download, and remembers the correction. Set `vis_autocal = 0` to turn this off.

### Fog, haze and the surface cloud layer

* **Haze with fog:** a fog report (`FG`, `FZFG`, `BCFG`, `MIFG`, `PRFG`, `VV`) does three things: it adds a ground-based cloud layer, it **caps visibility** (fog ≤ 1 km, vertical visibility ≤ 800 m, patchy/shallow fog ≤ 3 km, never raising a lower reported value), and it sets the dewpoint equal to the temperature so the air is saturated. Mist (`BR`) and haze (`HZ`) keep the exact reported visibility (US reports use them up to ~6 SM). Set `fog_haze = 0` to switch the caps off.
* **Layer starts at the ground** and is at least 60 m deep (120 m for full fog, or the `VV` height), so it is part of the surface weather rather than a thin sheet just above it.
* **Hitches:** X-Plane builds cloud layers on demand, and re-sending identical weather makes it rebuild them. WXPlane therefore only touches stations whose data actually changed, and erases only stations that left range. An update where nothing changed sends nothing. The log line `update: N stations, M changed, K removed` shows this.
* A plugin cannot force X-Plane to pre-build a layer. If a hitch remains when you first climb, the `visibility check` section of the log also prints the cloud layers X-Plane actually holds at your airport (type, coverage, base, top), which tells us whether the layer exists at the surface.
* Reports are centred on where you **are**, so fog and low cloud under your route keep updating at FL370.

## Install

Copy the `WXPlane` folder (containing `lin_x64`, `mac_x64`, `win_x64`) into `X-Plane 12/Resources/plugins/`, then restart X-Plane. If you have the old `WeatherFix` folder, delete it.

## Build

1. Download **XPSDK420** (or newer) from <https://developer.x-plane.com/sdk/plugin-sdk-downloads/> and unzip it.
2. Install libcurl development files: Linux `sudo apt install libcurl4-openssl-dev`; macOS needs nothing; Windows `vcpkg install curl:x64-windows-static`.
3. ```
   cmake -S . -B build -DXPLANE_SDK=/path/to/SDK
   cmake --build build --config Release
   ctest --test-dir build
   ```
   Output: `build/package/WXPlane/<platform>_x64/WXPlane.xpl`.

No compiler? Push this folder to a GitHub repo; `.github/workflows/build.yml` builds all three platforms and produces a ready-to-install `WXPlane` artifact.

## Config keys (`WXPlane.cfg`)

`update_minutes` (30), `search_nm` (120), `max_stations` (40), `radius_nm` (30), `fill_gaps` (1), `fill_range_nm` (60), `fill_cell_nm` (12), `fill_far_range_nm` (120), `fill_far_cell_nm` (30), `station_radius_auto` (1: each station's radius is limited to 49% of the distance to its nearest neighbour, so no two station records overlap and none dilutes another's weather), `max_age_hours` (3), `refetch_move_nm` (40), `use_atis` (1), `atis_range_nm` (35), `atis_max_airports` (2), `use_aloft` (1), `aloft_stations` (4), `aloft_range_nm` (250), `aloft_fcst_hours` (6; 12 or 24 also valid), `aloft_ceiling_ft` (60000), `normal_ceiling_ft` (10000), `fog_layer` (1), `fog_haze` (1), `fog_top_agl_m` (200), `clear_vis_m` (40000; used for 10 SM or more and when a report has no visibility), `vis_scale` (1), `vis_autocal` (0), `vis_check_s` (100), `radar` (1), `radar_global` (0), `radar_range_nm` (150), `radar_cell_nm` (12), `radar_max_cells` (60), `radar_min_dbz` (20), `radar_rain_full_mmh` (40), `radar_turbulence` (1), `radar_echo_tops` (1), `radar_map_tops` (1), `storm_coverage_max` (0.95), `first_load_immediate` (1), `apply_immediately` (0), `apply_per_tick` (500), `startup_delay_s` (0), `startup_wait_fps` (0), `startup_staged` (0), `startup_radar_delay_s` (8), `startup_max_wait_s` (90), `startup_wait_scenery` (0), `transition_s` (0), `update_transition_s` (0).

## Limits you should know about

* **Weather radar:** X-Plane has no plugin API for feeding in radar images, and its radar draws from the sim's own cloud/precipitation state. WXPlane sets cloud layers, thunderstorm/CB clouds and precipitation from METARs, which the radar will show around those stations, but it cannot reproduce real NOAA radar echoes.
* **Winds aloft coverage:** NOAA FD winds cover the US, Alaska, Hawaii and some Pacific stations. Elsewhere the plugin falls back to X-Plane's own winds/temps and says so in the menu status line.
* ATIS comes from the free D-ATIS service (`datis.clowd.io`), which covers major **US** airports only.
* The XPLMWeather API is marked *experimental* by Laminar Research.
* Date-line crossings are clamped at ±180° longitude. It needs an internet connection; with none, the last applied weather simply stays.

## Fog layer notes

X-Plane blends cloud layers between neighbouring reports slot by slot. When any nearby station has fog, WXPlane keeps cloud slot 0 reserved for fog at every station (empty where there is none), so fog is only ever blended with fog. This avoids the layer flickering as you fly over it. The fog layer is at least 100-120 m thick for the same reason. The visibility self-check is skipped while you are more than 1000 m above the ground, so it never re-applies weather (and rebuilds clouds) during cruise.

## Radar storms

X-Plane's weather API cannot take a radar image, so WXPlane builds the storms from radar data instead. The aircraft's weather radar draws what the sim simulates, so if the sim's storms match the real ones, the radar picture does too.

Every update (`update_minutes`, 30 by default) a background thread downloads:

* **MergedReflectivityQCComposite** from NOAA's MRMS system (`mrms.ncep.noaa.gov`), a quality-controlled mosaic of the whole US radar network at about 1 km, valid within a couple of minutes.
* **EchoTop_18**, the height in km MSL of the 18 dBZ echo, used as the storm top.

Both are gzipped GRIB2 files. WXPlane decodes them itself (no extra libraries) and only keeps the area around the aircraft (`radar_range_nm`). The CONUS, Alaska, Hawaii, Caribbean and Guam products are used by position.

The echoes are grouped into blocks of about `radar_cell_nm`; the strongest and nearest `radar_max_cells` become location records on top of the weather of the nearest METAR station:

* rain rate from reflectivity (Marshall-Palmer), scaled so `radar_rain_full_mmh` is full precipitation;
* a cloud layer in slot 2 that goes from stratus (about 20 dBZ) to cumulonimbus (40 dBZ and up). Its top is the real radar echo top (limited to the tropopause plus 2.5 km), so the cloud and the FLxxx label on the map agree; its coverage grows with intensity up to `storm_coverage_max` (0.95);
* lower visibility under heavy rain (800 m at 50 dBZ, 8 km at 25 dBZ);
* turbulence and gusts from about 30 dBZ upwards, below the storm top.

Only cells that changed are re-sent, and cells with no echo left are erased, so storms move and fade between updates. The window under "Show Local METAR/ATIS Data..." lists the radar source, its age and the strongest cells.

**Limits:** radar mosaics are 2D, so the base height and the shape of a storm are estimated; reflectivity above 70 dBZ is capped (hail and clutter); the API is limited to a few hundred records; the data is a few minutes old when you get it; and cells are only applied where at least one METAR station is within the search radius. The MRMS file layout (grid, PNG/simple packing) was built from the GRIB2 specification and tested with generated files; it has since decoded live NOAA CONUS data in real X-Plane sessions (the log showed e.g. 122 echo blocks, 120 cells, echo tops up to 12 000 m from radar). Not yet seen live: the Alaska/Hawaii/Caribbean/Guam products and the experimental global RainViewer fallback.

### Outside the US (experimental)

No free global radar data feed offers raw values. With `radar_global = 1` WXPlane uses RainViewer's public radar tiles instead (past frame, zoom 6). Those tiles are coloured pictures, so the intensity is inferred from the colour, and tops are estimated from reflectivity. The built-in colour table is an approximation. If storms look too strong or too weak, create `WXPlane_radar_palette.csv` next to `WXPlane.cfg` with lines like `r,g,b,dBZ` (taken from RainViewer's colour scheme 2 table) and it will be used instead. RainViewer's free access is for personal and educational use and rate limited; please respect their terms.

## Tests

`tests/test_wx.cpp` (parsers, model, radar), plus `tests/test_inflate.cpp`, `tests/test_png.cpp`, `tests/test_grib2.cpp` and `tests/harness.cpp` (the plugin against a mock X-Plane and mock network; needs the files written by `tests/gen_grib.py` and `tests/gen_mrms.py`).

## What is loaded, and how often

| Data | Source | Requests | When |
|---|---|---|---|
| METARs | NOAA AWC | 1 (bounding box) | every `update_minutes` (30) |
| ATIS | D-ATIS | 1 per airport, at most `atis_max_airports` (2) within `atis_range_nm` | every update; airports with no ATIS are skipped for 1 hour |
| Winds/temps aloft | NOAA AWC FD winds | 2 (low + high), plus a one-off coordinate lookup | re-downloaded at most every 30 minutes, cached otherwise; coordinates cached for 6 hours |
| Radar | NOAA MRMS | 2 files (reflectivity + echo tops, a few MB each) | every update |
| Global radar (optional) | RainViewer | 1 index + up to 9 tiles | every update, only outside NOAA coverage |

An update also happens early when you move more than `refetch_move_nm` (40 nm; at most once a minute), and when you press "Refresh Weather Data Now" or toggle a data option. Applying is incremental: only records that changed are sent to X-Plane.

**Start-up.** See "Start-up timing" and "Ready within a minute" below: the plugin waits for the sim to run at a steady frame rate (counted from the first frame), then fetches everything and applies it as ONE update, at once.

## Storm tops on the map

A map layer called "WXPlane storm tops" (toggle it in the map's layer list; `radar_map_tops = 0` disables it) labels storm cells with their top as a flight level (the storm cloud in the sim is built to exactly that top whenever the radar provides an echo top, capped at tropopause + 2.5 km; estimated tops, marked `~`, are shaped to avoid flat slabs), rounded to 1000 ft: `FL390`. A leading `~` (`~FL390`) means the top is estimated because no echo-top data was available. Zoomed out only cells of 40 dBZ and up are labelled, zoomed in 30 dBZ and up. X-Plane does not let plugins change the text of its own weather layer, so these labels sit on top of it.

## Weather settings screen

X-Plane's weather settings screen is fed by its own base weather. WXPlane sets weather through X-Plane's weather API (station and location records), which does not update that screen, so the screen will not show the NOAA values. The "Show Local METAR/ATIS Data..." window shows what is applied.

## Keeping it smooth

Every weather update makes X-Plane rebuild what changed, so WXPlane tries to change as little as possible:

* Only records whose content changed are sent; unchanged ones are left alone.
* Storm values are quantized (5 dBZ, 500 m tops, quarter coverage) so small radar wobbles change nothing, and storms already shown stay until they fall 5 dBZ below the threshold (and 15% beyond the range limit).
* All changed records go to X-Plane in one update (up to `apply_per_tick`, 500) and fade in over 1-2 minutes, because every separate weather update is another rebuild. Nothing is forced to rebuild at once unless you turn on `apply_immediately`. The log shows `weather update took N ms` for any update slower than 20 ms, which tells us if the sim itself is the slow part.
* The fog slot layout stays on for 30 minutes after the last fog report, so the clouds of all stations are not moved around when a fog report appears or disappears.
* The visibility self-check only runs near the ground, makes at most 2 corrections, and ignores differences under 25%.
* Everything slow (downloads, unzipping, decoding) runs on a low-priority background thread.

If you still see hitches, set `radar_max_cells` lower (30), or turn radar off from the menu to check whether the storms are the cause, and send me the `[WXPlane]` lines from `Log.txt`.

## Visibility self-check

`vis_autocal` compares the visibility X-Plane reports with the METAR and corrects our value if they differ (X-Plane tends to blend in its own, much clearer, visibility). X-Plane fades new weather in over minutes, so the check waits until the value it reads back has stopped moving (under 4% change between two checks a minute apart, up to six tries) before correcting, and never lowers the correction below 0.5. If X-Plane's reading does not come down after a lowering (something else is driving it), the correction is undone and the check stops. Radar storm records within 60 nm of a station carry that station's visibility (when it is 5 km or more) instead of 40 km, so they do not dilute the METAR value around the station. Checking too early used to read the old weather and push visibility far below the METAR. Only runs near the ground, within 15 nm of a station, and at most two correction passes per download.

## Visibility self-check is off by default

`vis_autocal` now defaults to 0. In the logs X-Plane's own visibility reading was driven by other things than our value (it rose after we lowered ours), so the corrections never helped and each one re-applied every record. WXPlane sends the exact METAR/ATIS visibility; set `vis_autocal = 1` to bring the self-check back.

## Stall log

If the sim stands still for more than 1.5 s, `Log.txt` gets a line `sim stalled for N s (our last weather update was M s ago)`. If the stalls never follow one of our updates closely, the freeze is not caused by WXPlane (map opening, texture streaming, other add-ons).

## No gaps, smooth blending (gap fill)

Station records never overlap (see `station_radius_auto`), so the space between the circles would otherwise fall back to X-Plane's own weather. With `fill_gaps = 1` the plugin covers the whole area around the aircraft with extra location records: a fine grid (`fill_cell_nm` 12 nm) out to `fill_range_nm` (60 nm) and a coarse one (`fill_far_cell_nm` 30 nm) out to `fill_far_range_nm` (120 nm, never beyond `search_nm`). Each record holds an inverse-distance blend (power 3) of the nearest four stations, so the closest airport dominates and the weather changes gradually from one station to the next instead of stepping or being mixed by X-Plane at the aircraft. Blended: wind (as a vector), visibility, temperature, dew point, pressure. Clouds and weather phenomena come from the nearest station; ground fog stays at its airport. Beyond `fill_far_range_nm` X-Plane's own weather applies; the grid follows the aircraft. About 100 to 120 more records in a typical update; `fill_gaps = 0` turns it off.

## NOAA cross-check and completing reports

The plugin reads each METAR from NOAA's aviationweather.gov as raw text and also gets NOAA's decoded fields (`visib`, `wdir`, `wspd`, `wgst`, `temp`, `dewp`, `altim`, `clouds`, `vertVis`) in the same response. After parsing the text, the two are compared: elements the text parse missed are filled in and, where they disagree, NOAA's decoded value is used. Anything still missing (wind, visibility, temperature, dew point, pressure) is filled from the nearest stations that have it (inverse-distance weighted, within 80 nm). Both are noted in the METAR/ATIS window ("Completed: ...") and counted in `Log.txt` (`NOAA crosscheck: ...`). Clouds are never guessed from neighbours.

## Start-up fade (patchy clouds)

X-Plane normally fades new weather in over 1-2 minutes. Cloud layers dissolve through a noise mask during that fade, which shows as blotchy, cut-out cloud patches (seen from the cockpit while it loads in). With `first_load_immediate = 1` the start-up updates (the stations, then the radar storms) are applied at once, so the dissolve is over right away; later 5-minute updates still fade in softly. This can cause one short hitch when the first update lands. `apply_immediately = 1` does the same for every update.

## Ready within a minute

Defaults aim for fully loaded weather about 30 to 45 seconds after the plugin loads: the first download starts after `startup_delay_s` (5 s) and a steady frame rate (at most `startup_max_wait_s`, 20 s), and everything (stations, gap fill, radar storms) goes to X-Plane as ONE update that is applied at once (`first_load_immediate`), so there is no second update and no 1-2 minute fade-in. After that the weather is refreshed every `update_minutes` (30; it was 5) and when you have flown `refetch_move_nm` (40 nm), with soft fades for those later changes. Set `update_minutes = 5` to get the old frequent refresh back, and `startup_staged = 1` to split the first load in two. Old config files take the new start-up defaults once (`config_version`); other settings are kept.

## Start-up timing

X-Plane does not run plugin callbacks while it is still loading (often 1 to 2 minutes after the plugin is enabled), so all start-up waits (`startup_delay_s`, the steady-frame-rate wait and its `startup_max_wait_s` limit of 90 s) are counted from the moment the sim starts running. The weather goes in as soon as the frame rate has been steady for 4 s, which is the moment the sim becomes usable, instead of while it is still crawling at a fraction of a frame per second. The `sim stalled` log lines now measure the real gap between frames.

## Visibility scale

`vis_scale` (default 1, range 0.2 to 1.5) multiplies every reported visibility below 10 SM before it is handed to X-Plane (stations, gap-fill and radar storm records alike). At 1 the METAR/ATIS value goes out exactly; 10 SM or more goes out as `clear_vis_m`. A test with a 4 SM report suggested X-Plane's value may look about twice as far as the METAR distance, so 0.5 may look closer to reality in some conditions; this is unconfirmed, so the default is the exact value.

## Disabling and refreshing

Switching the plugin off (menu or Plugin Admin) removes our weather records from the next pre-flight-model callback, the only place X-Plane's weather API is meant to be used, and hands the weather back to X-Plane's own system. "Refresh Weather Data Now" downloads fresh data and applies it at once (it does not fade in over 1-2 minutes).

## What this is not

WXPlane injects METAR, ATIS, winds-aloft and radar observations into X-Plane's weather engine; it does not replace that engine. X-Plane still blends and smooths the records it is given, so what you see can differ somewhat from the numbers sent (visibility and clouds especially). A compiled `WXPlane.xpl` is produced by the GitHub Actions build; the source zip alone cannot be loaded by X-Plane. Treat this as an experimental build: only the logic is covered by tests, not X-Plane's rendering.

## Apply the weather as early as possible

X-Plane starts with its own weather and, when a plugin replaces it, fades the clouds, fog and light over to the new weather. That fade is easy to see once the cockpit is on screen, so the plugin now does not wait for the sim to settle: `startup_delay_s` (0) and `startup_wait_fps` (0). The download starts on the very first frame the sim runs (X-Plane's own loading phase, when the screen is still loading) and the result is applied at once. The log line `update (+N s after the sim started running)` shows when that happened. If applying while the sim loads makes the loading itself slower or choppy, set `startup_wait_fps = 1` (wait up to `startup_max_wait_s` for a steady frame rate) and `startup_delay_s = 15`. Old config files take these start-up defaults once (`config_version` 5).

## The start-up load is always one update

Whatever `apply_per_tick`, `transition_s` or `update_transition_s` say, the start-up load (stations, gap fill and storms) goes to X-Plane as ONE update. A log from a session with `transition_s = 90` left over in the config from an older test build showed the 226 records being fed in over 135 seconds, which looked like two minutes of changing clouds, fog and light. Old config files now get `transition_s = 0`, `update_transition_s = 0` and `apply_per_tick = 500` once (`config_version` 6).

## Staying in control of the weather

X-Plane gives each weather record a weight based on its age, so every record WXPlane sends now has age 0 (a 50-minute-old METAR would otherwise count for less than X-Plane's own weather). The log also gains a `weather watch` line 30 s after each update, then every minute for 10 minutes and every 5 minutes after that: it shows the wind, visibility and cloud X-Plane is using at the aircraft next to what the surrounding stations call for. It only logs. Turn off X-Plane's own real weather as well, since it blends its downloaded METARs with ours.

## Cloud edges between records

Each station's circle stops at 0.49 x the distance to its nearest neighbour (`station_radius_factor`) and gap-fill tiles at 0.8 x their cell size (`fill_radius_factor`), so neighbours only touch. Making them overlap (tried 0.65 / 0.95) softens the cloud walls where two reports differ, but X-Plane then stacks the overlapping cloud layers and the tops go dark grey, so the defaults stay at touching-only. Raise them in `WXPlane.cfg` if you prefer soft edges over clean tops.

## Overcast that looked dark from above

A solid, thick, fully closed sheet rendered with a dark grey-blue top when seen from above. Overcast is now 94% coverage and thinner: stratus 1400 m (overcast) / 1100 m (broken) / 800 m (scattered), cumuliform 1600 / 1300 / 900 m (it was 2200 / 1600 / 900 and 2200 / 1800 / 1000). The sheet stays deep enough not to go black from below.

Cloud layers at one station never overlap vertically: every top stays at least 150 m below the next layer's base. The lower layer is trimmed first (never under 120 m thick); if two bases are closer together than that allows, the upper layer is lifted just enough to keep the gap.

## Radar every 5 minutes, and storm spikes

* **Radar refresh:** NOAA's MRMS mosaic is republished about every 2 minutes, so the storm radar is now refetched on its own every `radar_minutes` (5; minimum 0.1 for testing). A radar refresh downloads only the MRMS grids, keeps the stations and winds aloft from the last full update, and sends X-Plane only the storm records that changed (plus erases storms that ended). It is applied without the "immediate" flag, so X-Plane fades the change in over its usual 1-2 minutes. METARs, ATIS and winds aloft still refresh every `update_minutes` (30).
* **Spikes:** a radar block whose echo top is far above its neighbours (or that has no neighbours) rendered as a thin tower. With `storm_smooth_tops = 1` each measured top is limited to the mean of the neighbouring blocks' tops plus 2 km, and a lone block to 8 km. Tops are only ever lowered, and the map's FL labels use the same smoothed values. Set `storm_smooth_tops = 0` for the raw radar tops.

## Cloud look: smooth curtains vs billowy

Low and overcast layers used to be sent as X-Plane cloud type 1.0 (stratus), which it draws as a smooth sheet with vertical curtain-like edges; storm clouds (type 3) and cumulus (2) are billowy. Where the two meet in one cloud the left and right halves looked completely different. Every non-cirrus layer (overcast, broken, scattered and few, plus towering cumulus) now uses `low_cloud_type = 3` (cumulonimbus-style billowy cloud; 2 is cumulus), and every radar storm cloud is type 3 (weak echoes used to ramp down towards smooth type 1). Set `low_cloud_type = 1` for the old smooth sheets; values in between mix the looks.

## Varying storm tops

Storm blocks without a measured echo top got one estimated height per intensity, so many storms topped out at exactly the same altitude (a flat plateau with straight walls at its edge). Each estimated top now varies by up to +/- `storm_top_variation_m` (1500 m; 0 turns it off). The offset is derived from the block's key, so it is the same on every update and storms don't change height from one refresh to the next. Measured echo tops are not varied. The map's FL labels use the same values.
