# WXPlane for X-Plane 12

Replaces X-Plane 12's flawed weather with live **NOAA** data, refreshed every **5 minutes** by default (`update_minutes`) and whenever you fly 40 nm:

* **METARs** from NOAA's Aviation Weather Center (surface wind, visibility, fog, clouds, precipitation, temperature, pressure)
* **ATIS** (D-ATIS) merged over the METAR for airports near you
* **Winds aloft and temperatures aloft** (NOAA FD winds), applied from the surface to FL600 (60,000 ft) instead of X-Plane's own

Requires **X-Plane 12.3 or newer** (it uses the `XPLMWeather` plugin API, SDK 4.2.0).

## Menu (Plugins → WXPlane)

| Item | What it does |
|---|---|
| **Enabled** | On/off. Off removes all of the plugin's weather and gives control back to X-Plane. |
| **Refresh Weather Data Now** | Downloads fresh data and applies it without freezing the sim; X-Plane fades it in over 1–2 minutes (`manual_refresh_immediate = 1` makes it appear at once, at the cost of a freeze of several seconds). Also turns the plugin on if it was off. |
| **Show Local METAR/ATIS Data...** | Opens a scrollable, resizable window listing every nearby station: raw METAR, ATIS text and letter, what WXPlane applied to the sim (visibility, fog, clouds, wind), and the NOAA winds-aloft stations in use. Mouse wheel scrolls. |
| **Radar Storms (NOAA MRMS)** | Turns the NOAA radar mosaic into storm cells (see Radar below). On by default. |
| **Radar Outside US (experimental)** | Uses RainViewer image tiles where NOAA has no radar. Off by default. |
| **Use ATIS** | Merge D-ATIS data for airports near you. |
| **Use NOAA Winds/Temps Aloft** | On (default): winds and temperatures at every level come from NOAA. Off: only the lowest 10,000 ft is touched and X-Plane's own winds/temps are used above. |

Which layers count: the rough-top records apply to every layer with at least `rough_min_coverage` (0.65) coverage, so broken (BKN, 0.70) decks get them as well as overcast ones. Set it to 0.85 for overcast only.

Rough overcast tops (experimental, OFF by default): with `top_bumps = 1`, WXPlane scatters up to `bump_count` (10) small records over thick overcast decks (`bump_fill_count` (1) per gap-fill tile, at most `bump_max` (200) per update). Each record is the parent layer unchanged - same slot, type, coverage and base - with only the top raised, by at most 20% of the deck thickness and `bump_height_m` (550 m), and never into the layer above. Each circle lies inside its parent record. Earlier builds (on by default) also changed type, coverage and base, which X-Plane blended into cloud walls. Upgrading to config version 19 switches `top_bumps` off once and leaves your other settings alone.
| *status lines* | Last update time, METAR/ATIS/fog counts, number of NOAA winds-aloft stations, nearest station. |

Settings are saved to `Output/preferences/WXPlane.cfg`.

## How it works

1. Every `update_minutes` (5; or when you move 40 nm from the last download point) a background thread asks NOAA for all METARs within 120 nm **of the aircraft**, so the data follows you along your route. The sim is never blocked by the network.
2. The nearest 40 reports are applied with `XPLMSetWeatherAtAirport` (replacing the sim's own METAR for that airport) or `XPLMSetWeatherAtLocation`. Only changed stations are re-sent and stations that left range are erased, so stale stations never linger.
3. If an ATIS exists for one of the 2 nearest airports within 35 nm, its values override that airport's METAR values.
4. **Winds/temps aloft:** NOAA FD winds (3,000–39,000 ft, ~175 US stations plus Alaska/Hawaii) are downloaded every 30 minutes. For each weather report, the 4 nearest FD stations are blended by inverse-distance-squared on the wind *vectors* (so opposing winds cancel instead of averaging compass numbers). The report then carries a surface wind layer plus the aloft layers, and a temperature profile anchored to the airport's own METAR temperature. The highest NOAA wind is held up to FL600 so the sim's own winds never take over above FL390. X-Plane interpolates between reports and layers.
5. **Smooth transitions:** periodic updates are applied without the "immediate" flag, so X-Plane fades them in over 1–2 minutes. The first load applies immediately; the manual Refresh does not (see `manual_refresh_immediate`).

### Visibility

* Below 10 SM the METAR/ATIS value is used exactly (`4SM` = 6.4 km, `1/4SM` = 400 m).
* "10 SM or more" (`10SM`, `9999`, CAVOK) maps to `clear_vis_m` (40 km), since those reports are a floor rather than a measurement.
* **Self-check:** about 100 s after each update (15 s after a manual refresh), when you are within 4 nm of a reporting airport, the plugin asks X-Plane what visibility it is *actually* using there and logs it (`visibility check ...` in `Log.txt`). If it differs from the report by more than 15 % (the sim blends in its own weather), the plugin adjusts what it sends and re-applies, up to 4 passes per download, and remembers the correction. Set `vis_autocal = 0` to turn this off.

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

`update_minutes` (5), `search_nm` (120), `max_stations` (40), `radius_nm` (30), `fill_gaps` (1), `fill_range_nm` (60), `fill_cell_nm` (12), `fill_far_range_nm` (120), `fill_far_cell_nm` (30), `max_age_hours` (3), `refetch_move_nm` (40), `use_atis` (1), `atis_range_nm` (35), `atis_max_airports` (2), `use_aloft` (1), `aloft_stations` (4), `aloft_range_nm` (250), `aloft_fcst_hours` (6; 12 or 24 also valid), `aloft_ceiling_ft` (60000), `normal_ceiling_ft` (10000), `fog_layer` (1), `fog_haze` (1), `fog_top_agl_m` (200), `clear_vis_m` (40000; used for 10 SM or more and when a report has no visibility), `vis_scale` (1), `vis_autocal` (0), `vis_check_s` (100), `radar` (1), `radar_global` (0), `radar_range_nm` (150), `radar_cell_nm` (12), `radar_max_cells` (60), `radar_min_dbz` (20), `radar_rain_full_mmh` (40), `radar_turbulence` (1), `radar_echo_tops` (1), `radar_map_tops` (1), `storm_coverage_max` (0.95), `top_bumps` (0), `bump_count` (10), `bump_fill_count` (1), `bump_max` (200), `bump_height_m` (550), `bump_radius_factor` (0.26), `manual_refresh_immediate` (0).

Advanced settings (start-up gating, `transition_s`, `update_transition_s`, `apply_per_tick`, `first_load_immediate`, station and gap-fill record radii, `low_cloud_type`, storm top handling, `rough_min_coverage`) are no longer written to `WXPlane.cfg`. They keep their built-in defaults; a line added by hand is still read.

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

Every update (`update_minutes`, 5 by default) a background thread downloads:

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
| METARs | NOAA AWC | 1 (bounding box) | every `update_minutes` (5) |
| ATIS | D-ATIS | 1 per airport, at most `atis_max_airports` (2) within `atis_range_nm` | every update; airports with no ATIS are skipped for 1 hour |
| Winds/temps aloft | NOAA AWC FD winds | 2 (low + high), plus a one-off coordinate lookup | re-downloaded at most every 30 minutes, cached otherwise; coordinates cached for 6 hours |
| Radar | NOAA MRMS | 2 files (reflectivity + echo tops, a few MB each) | every update |
| Global radar (optional) | RainViewer | 1 index + up to 9 tiles | every update, only outside NOAA coverage |

An update also happens early when you move more than `refetch_move_nm` (40 nm; at most once a minute), and when you press "Refresh Weather Data Now" or toggle a data option. Applying is incremental: only records that changed are sent to X-Plane.

**Start-up.** See "Start-up timing" and "Ready within a minute" below: the plugin waits for the sim to run at a steady frame rate (counted from the first frame), then fetches everything and applies it as ONE update, at once.

## Storm tops on the map

Removed: the map no longer shows storm-top (FL) labels. The storm clouds themselves still use the radar echo tops.

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

`vis_autocal` compares the visibility X-Plane reports with the METAR and corrects our value if they differ (X-Plane tends to blend in its own, much clearer, visibility). X-Plane fades new weather in over minutes, so the check waits until the value it reads back has stopped moving (under 4% change between two checks a minute apart, up to six tries) before correcting, and never lowers the correction below 0.5. If X-Plane's reading does not come down after a lowering (something else is driving it), the correction is undone and the check stops. Radar storm records within 60 nm of a station carry that station's visibility (when it is 5 km or more) instead of 40 km, so they do not dilute the METAR value around the station. Checking too early used to read the old weather and push visibility far below the METAR. Only runs near the ground, within 4 nm of a station (a small field with no METAR gets the blend of its neighbours, so there is nothing to compare against), corrections are sent without the immediate flag so they do not freeze the sim, and at most two correction passes per download.

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

Switching the plugin off (menu or Plugin Admin) removes our weather records from the next pre-flight-model callback, the only place X-Plane's weather API is meant to be used, and hands the weather back to X-Plane's own system. "Refresh Weather Data Now" downloads fresh data and applies it like any other update, fading in over 1-2 minutes (set `manual_refresh_immediate = 1` for the old instant-but-freezing behaviour).

## What this is not

WXPlane injects METAR, ATIS, winds-aloft and radar observations into X-Plane's weather engine; it does not replace that engine. X-Plane still blends and smooths the records it is given, so what you see can differ somewhat from the numbers sent (visibility and clouds especially). A compiled `WXPlane.xpl` is produced by the GitHub Actions build; the source zip alone cannot be loaded by X-Plane. Treat this as an experimental build: only the logic is covered by tests, not X-Plane's rendering.

## Apply the weather as early as possible

X-Plane starts with its own weather and, when a plugin replaces it, fades the clouds, fog and light over to the new weather. That fade is easy to see once the cockpit is on screen, so the plugin now does not wait for the sim to settle: `startup_delay_s` (0) and `startup_wait_fps` (0). The download starts on the very first frame the sim runs (X-Plane's own loading phase, when the screen is still loading) and the result is applied at once. The log line `update (+N s after the sim started running)` shows when that happened. If applying while the sim loads makes the loading itself slower or choppy, set `startup_wait_fps = 1` (wait up to `startup_max_wait_s` for a steady frame rate) and `startup_delay_s = 15`. Old config files take these start-up defaults once (`config_version` 5).

## The start-up load is always one update

Whatever `apply_per_tick`, `transition_s` or `update_transition_s` say, the start-up load (stations, gap fill and storms) goes to X-Plane as ONE update. A log from a session with `transition_s = 90` left over in the config from an older test build showed the 226 records being fed in over 135 seconds, which looked like two minutes of changing clouds, fog and light. Old config files now get `transition_s = 0`, `update_transition_s = 0` and `apply_per_tick = 500` once (`config_version` 6).

## Staying in control of the weather

X-Plane gives each weather record a weight based on its age, so every record WXPlane sends now has age 0 (a 50-minute-old METAR would otherwise count for less than X-Plane's own weather). The log also gains a `weather watch` line 30 s after each update, then every minute for 10 minutes and every 5 minutes after that: it shows the wind, visibility and cloud X-Plane is using at the aircraft next to what the surrounding stations call for. It only logs. Turn off X-Plane's own real weather as well, since it blends its downloaded METARs with ours.

## Cloud edges between records

Each station's circle stops at 0.49 x the distance to its nearest neighbour (`station_radius_factor`) and gap-fill tiles at 1.0 x their cell size (`fill_radius_factor`). Making them overlap (tried 0.65 / 0.95) softens the cloud walls where two reports differ, but X-Plane then stacks the overlapping cloud layers and the tops go dark grey, so the defaults stay at touching-only. Raise them in `WXPlane.cfg` if you prefer soft edges over clean tops.

## Overcast that looked dark from above

A solid, thick, fully closed sheet rendered with a dark grey-blue top when seen from above. Overcast is now 94% coverage and thinner: stratus 1400 m (overcast) / 1100 m (broken) / 800 m (scattered), cumuliform 1600 / 1300 / 900 m (it was 2200 / 1600 / 900 and 2200 / 1800 / 1000). The sheet stays deep enough not to go black from below.

Cloud layers at one station never overlap vertically: every top stays at least 150 m below the next layer's base. The lower layer is trimmed first (never under 120 m thick); if two bases are closer together than that allows, the upper layer is lifted just enough to keep the gap.

## Radar every 5 minutes, and storm spikes

* **Radar refresh:** NOAA's MRMS mosaic is republished about every 2 minutes, so the storm radar is now refetched on its own every `radar_minutes` (5; minimum 0.1 for testing). A radar refresh downloads only the MRMS grids, keeps the stations and winds aloft from the last full update, and sends X-Plane only the storm records that changed (plus erases storms that ended). It is applied without the "immediate" flag, so X-Plane fades the change in over its usual 1-2 minutes. METARs, ATIS and winds aloft still refresh every `update_minutes` (5).
* **Spikes:** a radar block whose echo top is far above its neighbours (or that has no neighbours) rendered as a thin tower. With `storm_smooth_tops = 1` each measured top is limited to the mean of the neighbouring blocks' tops plus 2 km, and a lone block to 8 km. Tops are only ever lowered, and the map's FL labels use the same smoothed values. Set `storm_smooth_tops = 0` for the raw radar tops.

## Cloud look: smooth curtains vs billowy

Low and overcast layers used to be sent as X-Plane cloud type 1.0 (stratus), which it draws as a smooth sheet with vertical curtain-like edges; storm clouds (type 3) and cumulus (2) are billowy. Where the two meet in one cloud the left and right halves looked completely different. Every non-cirrus layer (overcast, broken, scattered and few, plus towering cumulus) now uses `low_cloud_type = 3` (cumulonimbus-style billowy cloud; 2 is cumulus), and every radar storm cloud is type 3 (weak echoes used to ramp down towards smooth type 1). Set `low_cloud_type = 1` for the old smooth sheets; values in between mix the looks.

## Varying storm tops

Storm blocks without a measured echo top got one estimated height per intensity, so many storms topped out at exactly the same altitude (a flat plateau with straight walls at its edge). Each estimated top now varies by up to +/- `storm_top_variation_m` (1500 m; 0 turns it off). The offset is derived from the block's key, so it is the same on every update and storms don't change height from one refresh to the next. Measured echo tops are not varied. The map's FL labels use the same values.

## Old settings stick (config version 7)

WXPlane writes every setting to `WXPlane.cfg` when it starts, so a value saved by an earlier test build stays even after the default changes. A leftover `low_cloud_type = 2` kept the layers at type 2 and a leftover `station_radius_factor = 0.65` kept the record overlap. Config files older than version 7 now get `low_cloud_type`, `station_radius_factor`, `fill_radius_factor`, `storm_smooth_tops`, `storm_top_variation_m` and `radar_minutes` reset to their defaults once; every other setting is kept.

## Clearing X-Plane's previous weather

Before the first load is applied (and again when the plugin is re-enabled from the menu), WXPlane runs X-Plane's own `sim/operation/regen_weather` command, which Laminar documents as an immediate full reset of the current weather, waits 3 seconds for it to take effect, and then hands over its records. That keeps clouds from the weather X-Plane started with from blending into ours. It is logged as `cleared X-Plane's previous weather (regen_weather) before applying ours`. Set `regen_weather_on_start = 0` to skip it. The 5-minute radar refreshes and the 30-minute updates do not do this; they only change the records that changed.

The plugin can only erase its own records through the weather API; X-Plane's own weather can only be reset with that command.

## Shorter hand-over waits

Two pauses before the first update were shortened: the quiet period after scenery loading (`startup_calm_s`, 3 -> 1.5 s) and the pause after X-Plane's weather reset (`regen_wait_s`, 3 -> 1.5 s). The remaining time between the plugin applying the weather and X-Plane finishing it is X-Plane building its clouds; Laminar's documentation says clouds take about a minute even with the immediate flag, and longer while the sim is still loading textures and scenery.

## Removed: Apply Weather Immediately, map storm tops

The "Apply Weather Immediately" menu option and the `apply_immediately` setting are gone: the start-up load and the manual Refresh always apply immediately, everything else fades in over X-Plane's usual 1-2 minutes. The map layer with storm-top numbers (and `radar_map_tops`) is gone too.

## Mist (BR)

A METAR with BR (mist) used to change only the visibility, so nothing was visible on the ground. It now also gets a thin ground layer (90 m deep, 45% coverage), in addition to the exact reported visibility. Fog, shallow fog and patchy fog are unchanged.

## Low ceilings and mist

* Mist (BR) with a broken/overcast ceiling below 600 ft AGL gets no separate mist layer: the ceiling layer takes the ground slot and sits at its reported height (a 100 ft ceiling stays at 100 ft), and the next two decks keep their slots, so BKN001 / BKN050 / BKN120 all appear. The reduced visibility still comes from the reported value.
* Layers whose base is within 100 m of the ground (ground fog, a very low ceiling) keep a 40 m gap to the next layer instead of 150 m.
* `vis_autocal` is on by default: on the ground, within 15 nm of a station, once X-Plane's visibility reading has stopped moving and is more than 25% off the report, the records are resent with a corrected visibility (two tries at most). It restores the plain value if X-Plane does not follow.

## Safeguards and diagnostics

Every record is checked before it is sent (`validateXp`): coverage is clamped to 0..1, a top at or below its base is lifted, layers thinner than 30 m or thicker than 16 km are corrected, layers that overlap or are out of order are separated (radar storm records are exempt on purpose, since a storm legitimately runs through the station's layers), and the record's ceiling is raised to its highest cloud top. Deep convection and layered frontal decks pass untouched. A radar storm now takes a free cloud slot and leaves the station's own layers alone; only when all three are taken does it replace the highest one.

The log gains a `submitted weather:` line per update (stations, storm records, rough-top records, validation adjustments) and, in the weather watch and the visibility check, a list of the records we submitted that cover that point, so expected, submitted and X-Plane-reported weather can be compared.

## Storm cluster edges

A radar storm record stops sharply at its circle, so a full-height block at the rim of a storm system showed as a cliff against the weather beside it. Blocks with fewer than 5 neighbouring blocks (the rim of a system, or the cut made by `radar_max_cells`) now get up to 45% less height above their base (never below 1500 m, and measured tops are still used for interior blocks) and up to 25% less coverage, in proportion to how isolated they are. Interior blocks, and the cloud base and station layers, are unchanged.

## Menu refreshes

Refresh Weather and the menu toggles are coalesced: at most one download per 60 s (`manual_refresh_gap_s`, advanced key). Turning radar on fetches only the radar and fades it in; stations and winds are kept. The log line after each update says `[fade-in]` or `[immediate: sim pauses while X-Plane rebuilds]`.

`storm_radius_factor` (advanced key, default 0.9, 0.6-2.0): storm record radius as a multiple of the radar cell size. Raise it (1.2-1.5) to make neighbouring storm cells overlap more; X-Plane then blends their edges instead of showing square steps. Untested in the sim.

## Storm shape tuning (advanced keys, all defaults keep the old behaviour)

- `radar_cell_nm` / `radar_max_cells`: smaller blocks follow the storm outline more closely (8 nm and 100 cells is a reasonable test; more records = more load).
- `storm_top_step_m` (default 500): tops are rounded to this step; 100-250 removes the terraces between neighbouring cells.
- `storm_top_blend` (default 0, max 0.8): with `storm_smooth_tops` on, pulls each top toward the distance-weighted mean of its neighbours (0.3-0.5 rounds the steps; spikes are still clipped first).
- `storm_top_variation_known_m` (default 0): a fixed per-block height variation for measured echo tops too (estimated tops use `storm_top_variation_m`).

None of these has been checked against X-Plane's renderer yet. Compare one at a time.

## Storm holes

Blocks enclosed on all four sides by storm cells, but with no cell of their own (cut by the cell cap, too weak, or empty), get a cell interpolated from their neighbours: mean reflectivity, top and coverage, at most a quarter of `radar_max_cells` extra. The radar log line reports how many (`N hole(s) interpolated`). Notches at the storm edge are left alone. `storm_fill_holes = 0` turns it off. Untested in X-Plane's NEXRAD map.

## Overlap (no gaps)

Gap-fill tiles (`fill_radius_factor`) and storm cells (`storm_radius_factor`) now default to 1.0 x their grid size: every tile reaches the centres of its neighbours, so fill tiles overlap each other, the storm cells and the station circles, and no uncovered ground is left between them. X-Plane blends overlapping records, so edges between records are softer but overlapped cloud layers can look denser or darker. If a value was set by hand in `WXPlane.cfg` it still wins (the old defaults were 0.8 and 0.9).

## Round dry spots

Station and gap-fill records inside a radar storm now take the storm's rain (inverse-distance mean of the cells within one cell radius, never lowering a station's own reported rain). Before, a station that reported no rain left a round dry spot the size of its own circle inside the storm in X-Plane's NEXRAD view.

## Square radar shapes (v21)

Three changes break up the 12 nm block grid. All three are settings, and config files older than v21 take the new values once.
- Finer blocks: `radar_cell_nm` 6 (was 12) and `radar_max_cells` 150 (was 60). Note 150 cells of 6 nm cover less ground than 60 of 12 nm, so far storms drop out first (closest and strongest are kept).
- Smoother values: `storm_rain_blend` 0.4 pulls each cell's reflectivity (rain rate, now at 1 dBZ resolution) toward its neighbours; `storm_top_blend` 0.4 and `storm_top_step_m` 250 do the same for tops.
- Off-grid placement: `storm_centroid` puts each cell at the reflectivity-weighted centre of its echo instead of the block centre, which removes the straight lines. Cells can sit up to half a block off the grid, so `storm_radius_factor` now defaults to 1.3 to keep them overlapping.

Set `radar_cell_nm = 12`, `radar_max_cells = 60`, `storm_rain_blend = 0`, `storm_top_blend = 0`, `storm_centroid = 0` to go back to the earlier look.

## Ragged storm outlines (v22)

- Storm blocks are back to 12 nm (`radar_cell_nm = 12`, `radar_max_cells = 60`; config files older than v22 take these once).
- `storm_rim` (default on): around every confirmed block, in the directions that have no block, a smaller, lighter, lower "rim" cell is added at a randomised distance and angle (stable between updates, derived from the block's key). Only some directions get one, so a straight side becomes uneven. In an inner corner (3+ blocks nearby, like the notch of an L) a full cell is added with the neighbours' mean values. Rim cells have 50-85% of the normal radius (`RadarCell::radiusScale`), so the outline is lumpy instead of a chain of equal circles. At most three quarters of `radar_max_cells` extra cells.
- Rim and interpolated cells are not used as sources when smoothing tops or rain, so they cannot pull measured values down. Smoothing never takes more than 1.5 km off (or adds more than 1.5 km to) a measured top.
- The log radar line ends with `N hole(s) interpolated, M rim cell(s)`.

### Rounded, fading edges

- `storm_rim_rings` (default 3, 1-3): the rim is built in rings. The second ring starts from the first ring's cells and is fainter, lower and smaller again, so the storm fades out over two steps instead of ending at one wall. More rings = softer edge, more records.
- An inner corner now means two or more confirmed blocks right next to the spot (within 1.2 cells). Before, a spot beside a straight side of a wide storm counted as a corner and got a full-strength cell, which pushed the wall outward instead of rounding it.

### Pits

`storm_fill_pits` (default on): with `storm_smooth_tops`, a block whose top is under 60% of the mean of at least four blocks around it (typically a weak-echo block with no measured echo top, whose height was only estimated from reflectivity) is lifted to exactly that mean. Such a block otherwise renders as a sheer rectangular crater in the middle of a storm. Blocks at the edge of a cluster (fewer than four neighbours) are left alone.

### Canyons: fill tiles under storms

`storm_replaces_fill` (default on): a background gap-fill tile whose centre is within 0.8 x storm radius of a confirmed storm cell (rim cells excluded) is no longer sent. Reason (from a log of a canyon through a storm): a fill tile of 12 nm radius with a 2.3 km deck sat 4 nm from the aircraft, inside storm cells with 13 km tops; X-Plane blends overlapping records slot by slot, so the low tile cut a sheer trench. The overlap of fill tiles was widened to 1.0 earlier, which made this worse. The update log line now says how many tiles were left out.

### Corner cut and thinner rings

- Every confirmed block now checks its 8 sides. Each open side (no confirmed block there) adds edge taper and shrinks the block's circle, down to 75% when fully open, so the square's corners are carved away and the outline ramps down in a curve.
- Rim cells carry a coverage multiplier (`coverScale`): 0.6 on the first ring, 0.45 on the second, about 0.34 on the third, 0.85 in inner corners. The storm now thins out ring by ring instead of stopping at one coverage step (the station deck underneath still sets the floor).
- Defaults: 3 rings, rim budget up to `radar_max_cells` (60) extra cells. More records than before; lower `storm_rim_rings` if the load is too high.

### Tops that fail to download, and light rain

- A truncated echo-top download is retried once; if it still fails, the last good tops are reused for up to 45 minutes (the log says so). Before, the update fell back to heights estimated from reflectivity, which changed every storm's height at once (13.5 km measured became 10.5 km estimated in one log) and rebuilt the sky with different shapes.
- `storm_replaces_fill` now only applies under storm cells with a top of 6 km or more. Under light, low echoes (20-30 dBZ cells with 4 km tops) the background fill tile stays: removing 53 tiles there left 40-58% cumulus patches as the only cloud, which rendered as small odd clumps with dark shadows.

### Channels

Hole filling now also fills a **channel**: a spot between confirmed blocks on two opposite sides (north/south, east/west or either diagonal) with at least 3 blocks within 1.4 cells, even though its ends are open. Before, only spots enclosed on all four sides were filled, so a one-block-wide gap between two groups of storm cells stayed empty; X-Plane blends the low records under it and renders a sheer canyon through the storm. The fill runs 3 passes (was 2), so a gap up to about three blocks wide closes from both sides. Straight outer edges are not touched.

### Shared storm base (`storm_base_smooth`, default on)

Each storm cell grows out of the deck of its nearest station, so neighbouring cells could end up with bases about 900 m apart (1051 m next to 1987 m in one log). X-Plane blends the records, which gave a stepped underside and walls. Cells within 1.7 cell widths of each other now share one distance-weighted mean base. Tops, coverage and the rim taper are unchanged. Set `storm_base_smooth = 0` to return to per-station bases.

### Level storm tops (`storm_top_max_drop_m`, default 1500)

After the spike/pit smoothing, no two neighbouring storm cells (closer than 1.5 cells) may differ in top by more than this many metres per cell width. The excess is split between the taller and the lower cell, so a core eases down towards its neighbours instead of dropping off a wall; no cell moves more than 2.5 km from its measured top. Rim cells keep their own taper. `0` turns it off.

### Storm settings window

`Plugins > WXPlane > Storm Settings (turbulence)...` opens a small window with a slider for the storm turbulence, 0-200 % in 5 % steps (`radar_turbulence`, 100 % = scaled to the radar reflectivity as before). It is saved when you release the slider and the storms are rebuilt with the new value.

### Storm coverage stays at 95 %

`storm_coverage_max` is 0.95 again. A trial at 1.0 removed the natural breakup of the cloud; set `storm_coverage_max = 1.0` in `WXPlane.cfg` if you want solid storm cores. Config files that were moved to 1.0 by that trial (config version 23) are put back to 0.95 once.

### Rain pits and wider overlap (v24)

* `storm_fill_pits` now also covers rain: a measured block at least 6 dBZ below the mean of its 4+ neighbours is lifted to 2 dBZ under that mean, so no pocket of light rain sits in the middle of a storm (a block on an edge, with fewer neighbours, is left alone).
* `storm_radius_factor` 1.3 -> 1.45 (storm records of about 17 nm instead of 15.6 nm at 12 nm cells) so neighbouring circles overlap more and the outline between them is smoother.
(`storm_rain_blend` stays at 0.4: 0.6 pulled the heaviest core's rain below full strength.)
Config files older than version 24 take the new radius once.

### Solid storm cores (`storm_core_coverage`, default 1.0)

Cells with no open side (surrounded by other storm cells) and 35 dBZ or more get this coverage instead of `storm_coverage_max` (0.95). The 5 % breakup stays on the rim and on weaker cells, where it looks natural; inside the storm it showed up as 1-2 nm pockets of clear air and rain-free spots on the NEXRAD map. Set `storm_core_coverage = 0.95` to turn it off.

### Back to ragged walls (v25)

The look of the storm walls got too solid after three changes, so their defaults are back to the earlier behaviour: `storm_top_max_drop_m` 0 (tops are not levelled between neighbouring cells; set 1500 to level them), `storm_core_coverage` 0.95 (no solid cores; 1.0 makes them solid) and `storm_radius_factor` 1.3 (1.45 overlaps more). The shared storm base, the rain-pit fill and the turbulence slider stay. Config files older than version 25 take these three values once.

### Storm outline (`storm_contour`, default on)

The storm edge now follows one smooth outline drawn around all the blocks (your drawing): a soft union of rounded squares, so concave corners and notches fill in, convex corners round off, and a thin one-block strip gets a gentle bulge. A low-frequency random wobble (stable from update to update, fixed to the map, not to the blocks) makes the outline uneven but flowing.

* Small "skin" cells (keys `C...`, 55-100 % of a cell apart, randomised, lighter and lower than the block they come from) are placed just inside the outline, so the edge of the cloud follows it. They replace the random rim rings (`storm_rim`, used only when `storm_contour = 0`).
* Every measured block's record radius is limited (`RadarCell::contourScale`, 40-100 %) so its circle does not reach past the outline; blocks in the middle of a storm keep their full radius.
* At most `radar_max_cells` skin cells, nearest to the aircraft first. Set `storm_contour = 0` to go back to the random rim rings.

### Winds aloft without gaps

* A level an FD station does not report (it often skips one, or has no wind or temperature at the lowest levels) is interpolated from the levels above and below it, as vectors, before the stations are blended. A station is held at its top or bottom level for at most 6000 ft, so one with only the low-level product never adds a wind at FL390.
* A report with no FD station within `aloft_range_nm` now uses the nearest ones up to 1000 nm away instead of falling back to the sim's own winds. Beyond that, X-Plane's own winds are used and the status line says so.
* FD station positions come first from the reports already downloaded ("ABI" is "KABI"), then from the METAR lookup in batches of 30; a failed batch is skipped and retried at the next update instead of ending the lookup (the likely cause of a session with 0 FD stations). Stations up to 1200 nm away are kept.
* The top wind is held up to the ceiling (FL600) as before, and the top temperature is held there too (the stratosphere is nearly isothermal).

### Global-model winds aloft (`upper_air`, default on)

NOAA's FD winds stop at FL390 and cover only the US and a few Pacific stations. WXPlane therefore also asks Open-Meteo's GFS endpoint (`api.open-meteo.com/v1/gfs`, free, no key; check its terms if you redistribute the plugin) for wind speed and direction, temperature and geopotential height on the 925, 850, 700, 500, 400, 300, 250, 200, 150, 100 and 70 hPa levels (to about FL600) at up to 9 points spread over the weather stations, once every 45 minutes (or when you have moved 60 nm).

* **Where it is used:** wherever FD has no station within `aloft_range_nm` (everything outside the FD coverage gets real winds and temperatures instead of the sim's own, using the nearest model points up to 2500 nm away), and above the highest FD level of a report (more than 1000 ft above it, so normally above FL390). FD stays the source below that, as it is corrected by observations.
* **Heights:** the model's own geopotential height of each pressure level is used (ISA only when it is missing), so a cold or warm air mass puts the levels where they really are.
* **Safety:** if the endpoint refuses the highest levels the request is repeated with the levels up to 200 hPa; if both fail the previous download is kept for up to 3 hours, and `Log.txt` gets a line `global-model winds aloft unavailable (reason)`. A normal update logs `global-model winds aloft: N point(s), up to FLxxx`.
* `upper_air = 0` turns it off (FD only, as before).
* This could not be tried against the real service while it was written; the request and answer formats follow Open-Meteo's published pattern and are covered by tests with made-up answers. If the log shows "unavailable" or "answered without wind levels", send it.

### Winds aloft in the log

The "weather watch" and "visibility check" blocks now also print `winds aloft we sent for ...` (altitude, direction/speed in knots) and `winds aloft X-Plane reports there:` (what the sim's own read-back shows), so a wind that looks wrong can be checked against what was sent. The global-model line also says how many points came back out of how many were asked for, and shows the start of the answer when fewer came back.

### Same wind layers in every record

Every record that carries winds aloft now has exactly 13 wind layers: the surface wind plus the same 12 altitudes (3000, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000, 45000, 53000, 60000 ft), interpolated as vectors from the NOAA/model levels. Before, a station on high terrain skipped the levels under its ground and a record without the model's top levels stopped lower; the unused slots were left undefined, and X-Plane fills those from its own winds, which are calm after WXPlane resets the weather. Blending such records with full ones gave about 1 kt at FL360 where the real wind was close to 100 kt. A level under the terrain carries the wind of the lowest level above it, at an altitude 1 m over the previous layer so the list stays ascending.


## FL350 excursion fix, station elevations, magnetic ATIS winds (2026-11-06)

Full investigation: `docs/2026-11-06-fl350-investigation.md`.

- Station elevations are checked on every download. KC24 came in as 8,680 m (its feet read as metres), and that pushed the whole wind and
  temperature profile above 28,510 ft. The warm column it produced made the altimeter read 4,000 ft low at FL350.
- Winds aloft use the same nominal altitudes in every record, strictly ascending and at least 150 m apart; nothing is squeezed against
  the terrain. Forecast temperatures are kept below the terrain too.
- `validateProfile` drops impossible wind, temperature, QNH and rain values before anything reaches X-Plane.
- The sim's region wind and temperature arrays change gradually: at most 3 m/s and 1.5 °C per level every 20 s, on fixed altitudes.
- ATIS winds (magnetic) are converted to true with X-Plane's variation at the airport; METAR and winds-aloft winds are already true.
- `diag_verbosity` (0/1/2): event-triggered flight-state logging (UTC, position, IAS/TAS/Mach/GS, wind, then the full weather picture);
  surface-state lines at start-up for wet-runway reports.

## Windshear: capability probe (2026-11-06, removed)

`docs/windshear-sdk-assessment.md` sets out what X-Plane's weather API can do for windshear, with the results of the in-sim probe. That
probe's menu item and the humidity-aloft experiment (it did not change X-Plane's pop-up showers) have been removed.

## Realistic windshear (2026-11-06)

Menu **Enable Realistic Windshear** (on by default). Gust fronts and microburst outflow form near
real convection (radar echoes, TS reports) with seeded, frame-rate-independent probabilities. They are sent to X-Plane as low-level wind
records, ahead of time and scaled for X-Plane's fade-in and blending. Downdrafts are modelled and logged only: X-Plane's weather API has
no vertical wind. Details, limits and calibration: `docs/windshear.md`.

## Dry runways when no recent rain (2026-11-06)

Always on (no menu item); `dry_runway_min = 45` sets the dry time.
- **History:** the METAR request now asks for the last 2 hours. Each station keeps its latest report; the older reports (up to 80 min
  back), begin/end remarks (`RAB10E35`, `RAE50`), hourly amounts (`P0000`) and recent-weather groups (`RERA`) tell when precipitation
  was last seen. Showers in the vicinity (`VCSH`) don't count.
- **What it changes:** near the airport the aircraft is at or approaching (within 8 nm, below 3,000 ft AGL), X-Plane's ground condition
  (`sim/weather/region/runway_friction`, the weather screen's runway condition; it covers taxiways and aprons too) is set to dry, when
  all of these hold:
  - that airport has had no precipitation for 45 minutes, counting the report's age;
  - the report is no more than 90 minutes old;
  - the history reaches back far enough;
  - WXPlane sends no rain there;
  - the ground is currently wet or has standing water (1–6). The scale, confirmed from the weather screen: 0 dry, 1–3 wet, 4–6
    standing water, 7–9 snow, 10–12 ice, 13–15 snow and ice. It is read and written by its own type; read as a whole number it always
    gave 0.
- **What it never touches:** snow or ice (7–15), or anything after snow or freezing precipitation in the last hour.
- **Log:** every decision is logged as "dry runway: …".
