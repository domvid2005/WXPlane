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
| **Use ATIS** | Merge D-ATIS data for airports near you. |
| **Use NOAA Winds/Temps Aloft** | On (default): winds and temperatures at every level come from NOAA. Off: only the lowest 10,000 ft is touched and X-Plane's own winds/temps are used above. |
| *status lines* | Last update time, METAR/ATIS/fog counts, number of NOAA winds-aloft stations, nearest station. |

Settings are saved to `Output/preferences/WXPlane.cfg`.

## How it works

1. Every 5 minutes (or when you move 40 nm from the last download point) a background thread asks NOAA for all METARs within 120 nm **of the aircraft**, so the data follows you along your route. The sim is never blocked by the network.
2. The nearest 40 reports are applied with `XPLMSetWeatherAtAirport` (replacing the sim's own METAR for that airport) or `XPLMSetWeatherAtLocation`. The whole set is replaced on every update, so stale stations never linger.
3. If an ATIS exists for one of the 2 nearest airports within 35 nm, its values override that airport's METAR values.
4. **Winds/temps aloft:** NOAA FD winds (3,000–39,000 ft, ~175 US stations plus Alaska/Hawaii) are downloaded every 30 minutes. For each weather report, the 4 nearest FD stations are blended by inverse-distance-squared on the wind *vectors* (so opposing winds cancel instead of averaging compass numbers). The report then carries a surface wind layer plus the aloft layers, and a temperature profile anchored to the airport's own METAR temperature. The highest NOAA wind is held up to FL600 so the sim's own winds never take over above FL390. X-Plane interpolates between reports and layers.
5. **Smooth transitions:** periodic updates are applied without the "immediate" flag, so X-Plane fades them in over 1–2 minutes. The first load and the manual Refresh apply immediately.

### Visibility

* Below 10 SM the METAR/ATIS value is used exactly (`4SM` = 6.4 km, `1/4SM` = 400 m).
* "10 SM or more" (`10SM`, `9999`, CAVOK) maps to `clear_vis_m` (40 km), since those reports are a floor rather than a measurement.
* **Self-check:** about 100 s after each update (15 s after a manual refresh), when you are within 15 nm of a reporting airport, the plugin asks X-Plane what visibility it is *actually* using there and logs it (`visibility check ...` in `Log.txt`). If it differs from the report by more than 15 % (the sim blends in its own weather), the plugin adjusts what it sends and re-applies, up to 4 passes per download, and remembers the correction. Set `vis_autocal = 0` to turn this off.

### Fog below you at cruise

Fog (`FG`, `FZFG`, `BCFG`, `MIFG`, `VV`) is drawn as a ground-hugging stratus layer (120 m deep by default, or the `VV` height) and also sets ground visibility. Because reports are centred on where you are, fog and low cloud under your route keep updating at FL370.

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

`update_minutes` (5), `search_nm` (120), `max_stations` (40), `radius_nm` (30), `max_age_hours` (3), `refetch_move_nm` (40), `use_atis` (1), `atis_range_nm` (35), `atis_max_airports` (2), `use_aloft` (1), `aloft_stations` (4), `aloft_range_nm` (250), `aloft_fcst_hours` (6; 12 or 24 also valid), `aloft_ceiling_ft` (60000), `normal_ceiling_ft` (10000), `fog_layer` (1), `fog_top_agl_m` (120), `clear_vis_m` (40000; used for 10 SM or more and when a report has no visibility), `vis_autocal` (1), `vis_check_s` (100).

## Limits you should know about

* **Weather radar:** X-Plane has no plugin API for feeding in radar images, and its radar draws from the sim's own cloud/precipitation state. WXPlane sets cloud layers, thunderstorm/CB clouds and precipitation from METARs, which the radar will show around those stations, but it cannot reproduce real NOAA radar echoes.
* **Winds aloft coverage:** NOAA FD winds cover the US, Alaska, Hawaii and some Pacific stations. Elsewhere the plugin falls back to X-Plane's own winds/temps and says so in the menu status line.
* ATIS comes from the free D-ATIS service (`datis.clowd.io`), which covers major **US** airports only.
* The XPLMWeather API is marked *experimental* by Laminar Research.
* Date-line crossings are clamped at ±180° longitude. It needs an internet connection; with none, the last applied weather simply stays.
