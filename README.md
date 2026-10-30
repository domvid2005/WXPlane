# WXPlane for X-Plane 12

Replaces X-Plane 12's flawed weather with live **NOAA** data, refreshed every **5 minutes**:

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

1. Every 5 minutes (or when you move 40 nm from the last download point) a background thread asks NOAA for all METARs within 120 nm **of the aircraft**, so the data follows you along your route. The sim is never blocked by the network.
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

`update_minutes` (5), `search_nm` (120), `max_stations` (40), `radius_nm` (30), `max_age_hours` (3), `refetch_move_nm` (40), `use_atis` (1), `atis_range_nm` (35), `atis_max_airports` (2), `use_aloft` (1), `aloft_stations` (4), `aloft_range_nm` (250), `aloft_fcst_hours` (6; 12 or 24 also valid), `aloft_ceiling_ft` (60000), `normal_ceiling_ft` (10000), `fog_layer` (1), `fog_haze` (1), `fog_top_agl_m` (200), `clear_vis_m` (40000; used for 10 SM or more and when a report has no visibility), `vis_autocal` (1), `vis_check_s` (100), `radar` (1), `radar_global` (0), `radar_range_nm` (150), `radar_cell_nm` (12), `radar_max_cells` (60), `radar_min_dbz` (20), `radar_rain_full_mmh` (40), `radar_turbulence` (1), `radar_echo_tops` (1), `radar_map_tops` (1), `map_airport_wx` (1), `storm_coverage_max` (0.7), `storm_top_jitter_m` (1500), `apply_immediately` (0), `apply_per_tick` (500), `startup_delay_s` (15), `startup_wait_fps` (1), `startup_staged` (1), `startup_radar_delay_s` (20), `startup_wait_scenery` (0), `transition_s` (0), `update_transition_s` (0).

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

Every update (about 5 minutes) a background thread downloads:

* **MergedReflectivityQCComposite** from NOAA's MRMS system (`mrms.ncep.noaa.gov`), a quality-controlled mosaic of the whole US radar network at about 1 km, valid within a couple of minutes.
* **EchoTop_18**, the height in km MSL of the 18 dBZ echo, used as the storm top.

Both are gzipped GRIB2 files. WXPlane decodes them itself (no extra libraries) and only keeps the area around the aircraft (`radar_range_nm`). The CONUS, Alaska, Hawaii, Caribbean and Guam products are used by position.

The echoes are grouped into blocks of about `radar_cell_nm`; the strongest and nearest `radar_max_cells` become location records on top of the weather of the nearest METAR station:

* rain rate from reflectivity (Marshall-Palmer), scaled so `radar_rain_full_mmh` is full precipitation;
* a cloud layer in slot 2 that goes from stratus (about 20 dBZ) to cumulonimbus (40 dBZ and up). Its top comes from the real echo top (limited to the tropopause plus 2.5 km) but is shaped so storms do not look like flat slabs: weaker and edge cells are lower, each block gets a stable random-looking top variation (`storm_top_jitter_m`, 1500), and coverage tops out at `storm_coverage_max` (0.7) instead of solid; cells with echo all around them are tallest and cells at the edge of an echo area are lower (a dome rather than a plateau), and each record covers 75% of its block so towers keep gaps;
* lower visibility under heavy rain (800 m at 50 dBZ, 8 km at 25 dBZ);
* turbulence and gusts from about 30 dBZ upwards, below the storm top.

Only cells that changed are re-sent, and cells with no echo left are erased, so storms move and fade between updates. The window under "Show Local METAR/ATIS Data..." lists the radar source, its age and the strongest cells.

**Limits:** radar mosaics are 2D, so the base height and the shape of a storm are estimated; reflectivity above 70 dBZ is capped (hail and clutter); the API is limited to a few hundred records; the data is a few minutes old when you get it; and cells are only applied where at least one METAR station is within the search radius. The MRMS file layout (grid, PNG/simple packing) was built from the GRIB2 specification and has been tested with generated files, not yet against live NOAA data.

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

**Start-up.** The first download starts `startup_delay_s` (15 s) after the plugin loads and, with `startup_wait_fps = 1`, only once the frame time has stayed under 50 ms for 4 seconds (so not while the sim is still loading; it gives up waiting after 2.5 minutes). With `startup_staged = 1` the first update contains only METAR, ATIS and winds aloft; the radar storms follow `startup_radar_delay_s` (20 s) later in a second update, once the frame rate is steady again, so the sim never has to rebuild everything at once. Set `startup_wait_scenery = 1` to also wait until X-Plane reports no scenery loading (up to 2.5 minutes); it is off by default. The first weather goes to X-Plane as a single update, which X-Plane then fades in over 1-2 minutes. Every weather update makes X-Plane rebuild, so one update is smoother than many small ones. `transition_s` / `update_transition_s` (both 0) can spread the records over time, but each step is another rebuild and causes repeated hitches, so leave them at 0 unless you are experimenting. Downloads and radar decoding run on a background thread at lower priority.

## Storm tops on the map

A map layer called "WXPlane weather" (toggle it in the map's layer list; `radar_map_tops = 0` disables it) labels storm cells with their top as a flight level, rounded to 1000 ft: `FL390`. A leading `~` (`~FL390`) means the top is estimated because no echo-top data was available. Zoomed out only cells of 40 dBZ and up are labelled, zoomed in 30 dBZ and up. X-Plane does not let plugins change the text of its own weather layer, so these labels sit on top of it.

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

## Airport weather on the map

X-Plane's SDK does not let a plugin add to or react to clicks on its own airport info box, so WXPlane labels the airports on the map instead (same "WXPlane weather" layer; `map_airport_wx = 0` turns it off). Every station in view gets a one-line label under its position: ICAO, flight category (VFR/MVFR/IFR/LIFR), wind and ATIS letter. Zoom in until only a few airports are in view and the one nearest the middle of the map shows its full METAR and ATIS text. Airports without a METAR station are not labelled.

## Visibility self-check

`vis_autocal` compares the visibility X-Plane reports with the METAR and corrects our value if they differ. X-Plane fades new weather in over 1-2 minutes (longer while it is busy loading), so the check now waits until the cloud layers it read back match the ones we sent, retrying every 60 s up to four times, and never lowers the correction below 0.3. Before this, a check made too early could read the old weather and push visibility far below the METAR (a 4 SM report ended up at under 1 km), which made everything look dark.
