# Weather Fix for X-Plane 12

Replaces X-Plane 12's flawed weather with live **NOAA Aviation Weather Center METARs**, merged with **D-ATIS** where available, and refreshes it every **5 minutes**.

Requires **X-Plane 12.3 or newer** (it uses the `XPLMWeather` plugin API, SDK 4.2.0).

## Menu (Plugins → Weather Fix)

| Item | What it does |
|---|---|
| **Enabled** | On/off. Off removes all of the plugin's weather and gives control back to X-Plane. |
| **Refresh Weather Data Now** | Downloads and applies immediately (no 1–2 min fade). Also turns the plugin on if it was off. |
| **Use ATIS** | Merge D-ATIS wind/visibility/clouds/temperature/altimeter for the airports near you. |
| **Apply Temp/Pressure at Cruise Altitude** | Raises each report's vertical reach from 10,000 ft to 45,000 ft (see below). |
| *status lines* | Last update time, stations applied, fog count, nearest station. |

Settings are saved to `Output/preferences/WeatherFix.cfg` (update interval, search radius, fog depth, …).

## How it works

1. Every 5 minutes (or when you move 40 nm from the last download point) a background thread asks NOAA for all METARs within 120 nm **of the aircraft**, not of an airport. The sim is never blocked by the network.
2. The nearest 40 reports are converted and applied with `XPLMSetWeatherAtAirport` (replacing the sim's own METAR for that airport) or `XPLMSetWeatherAtLocation` (stations X-Plane doesn't know). The set is replaced as a whole on each update, so stale stations never linger.
3. If an ATIS is available for one of the 2 nearest airports within 35 nm, its values override that airport's METAR values.

### Weather below you at cruise

* Reports are centred on where you **are**, so fog, low cloud and visibility under your route keep updating at FL370 just as on the ground.
* Fog (`FG`, `FZFG`, `BCFG`, `MIFG`, `VV`) is drawn as a ground-hugging stratus layer (120 m deep by default; `VV` reports use their own depth) *and* sets ground visibility. X-Plane uses cloud layers regardless of a report's altitude limit.
* Only the **surface** wind is set; winds aloft stay with X-Plane's own data, so METAR winds can't corrupt your cruise winds.
* By default temperature/pressure from a report applies up to 10,000 ft MSL. "Apply Temp/Pressure at Cruise Altitude" extends that to 45,000 ft, but it applies the *surface* temperature offset from ISA all the way up, which can make cruise temperatures unrealistic on hot or cold days. Leave it off unless you want it.

## Install

Copy the `WeatherFix` folder (containing `lin_x64`, `mac_x64`, `win_x64`) into `X-Plane 12/Resources/plugins/`, then restart X-Plane.

## Build

1. Download **XPSDK420** (or newer) from <https://developer.x-plane.com/sdk/plugin-sdk-downloads/> and unzip it.
2. Install libcurl development files: Linux `sudo apt install libcurl4-openssl-dev`; macOS needs nothing; Windows `vcpkg install curl:x64-windows-static`.
3. ```
   cmake -S . -B build -DXPLANE_SDK=/path/to/SDK
   cmake --build build --config Release
   ctest --test-dir build        # parser/model unit tests, no X-Plane needed
   ```
   The result is `build/package/WeatherFix/<platform>_x64/WeatherFix.xpl`.

No compiler? Push this folder to a GitHub repo; `.github/workflows/build.yml` builds all three platforms and produces a ready-to-install `WeatherFix.zip` artifact.

## Config keys (`WeatherFix.cfg`)

`update_minutes` (5), `search_nm` (120), `max_stations` (40), `radius_nm` (30), `max_age_hours` (3), `refetch_move_nm` (40), `use_atis` (1), `atis_range_nm` (35), `atis_max_airports` (2), `fog_layer` (1), `fog_top_agl_m` (120), `normal_ceiling_ft` (10000), `cruise_ceiling_ft` (45000), `clear_vis_m` (40000 – used for "10 SM / 9999 or more").

## Limits you should know about

* Data sources: METAR from NOAA AWC (worldwide). ATIS comes from the free D-ATIS service (`datis.clowd.io`), which covers **major US airports only**; elsewhere METAR is used alone. NOAA winds aloft and TAFs are not used.
* The XPLMWeather API is marked *experimental* by Laminar Research; behaviour may change between X-Plane versions. Weather set without "immediate" fades in over 1–2 minutes, and clouds can take a minute longer.
* Date-line crossings are clamped at ±180° longitude.
* It needs an internet connection; with none, the last applied weather simply stays.
