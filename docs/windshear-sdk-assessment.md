# Windshear: what the X-Plane 12 SDK can and cannot do (assessment, 2026-11-06)

This answers section 11 of the windshear specification before any windshear model is built. Each answer comes from WXPlane's code,
Laminar's documentation, or WXPlane logs from real flights. Answers marked "measured by the probe" need the test below.

| # | Question | Answer | Basis |
|---|---|---|---|
| 1 | Location-dependent horizontal shear? | **Partly.** Location records (`XPLMSetWeatherAtLocation`, with `radius_nm`) carry a surface wind, and X-Plane blends overlapping records. The smallest radius X-Plane honours and how sharp a boundary stays are **unknown: measured by the probe**. | SDK; WXPlane storm records |
| 2 | A custom vertical wind profile? | **Only region-wide.** Records accept 13 wind layers, but in the 2026-11-03/04 logs X-Plane applied only the lowest one (180°/1 kt at every flight level from FL100 up). Profiles reach the sim through the region arrays (`sim/weather/region/wind_*`), which are one profile for the whole area around the aircraft. That suits broad features (a low-level jet, an inversion over hundreds of km), not a local one. | WXPlane logs, weather-datarefs article ("most regional datarefs are writeable") |
| 3 | Localized downdrafts? | **No.** `XPLMWeatherInfo_t` has no vertical wind component. Its only vertical-air field is `thermal_climb` (thermal strength, upward). The `sim/weather/aircraft/*` datarefs are read-only. | SDK header, weather-datarefs article |
| 4 | Microbursts without touching aircraft physics? | **No, not the downdraft.** At most the horizontal outflow, as a ring of records with outward winds, and only if the probe shows X-Plane keeps a ~2 nm pattern apart. The core (up to ~6,000 fpm down) would need forces on the aircraft, which the spec forbids. | 1 and 3 |
| 5 | Update latency vs short events? | **Unknown: measured by the probe.** Changes go through `XPLMBeginWeatherUpdate`/`XPLMEndWeatherUpdate` in the pre-flight-model loop and are faded in by X-Plane. A microburst's 2–4 min mature stage may be shorter than the fade. | SDK, WXPlane logs |
| 6 | Does X-Plane already create these effects? | **Partly.** X-Plane 12 has its own gusts, turbulence, per-layer wind shear (direction variability), thermals, cloud updrafts/downdrafts, and terrain effects. WXPlane already sends gusts, turbulence near storms and the observed winds. Microbursts are not a known X-Plane feature. | X-Plane 12 weather, WXPlane code |
| 7 | Without interfering with WXPlane's weather? | **Yes, for records:** extra records can go through the same incremental update queue. Region arrays are already written for winds aloft every 20 s. Windshear must modify that one profile, not add a second writer. | WXPlane code |

## What this means for the specification

- **Type A (vertical gradient, low-level jet, inversion):** implementable through the region profile, if the low levels have data.
  The model fetch has 925 hPa as its lowest level, so 1000/975/950 hPa would have to be added for a low-level jet.
- **Type B (gust fronts, fronts, sea-breeze boundaries):** implementable as horizontal winds in records, **if** the probe shows X-Plane
  keeps the boundary sharp enough to matter.
- **Type C (convective downdrafts) and the downdraft of type D (microbursts):** **not implementable faithfully.** They would be modelled
  and logged only, never applied.
- **Microburst outflow:** only if the probe shows a ~2 nm pattern survives X-Plane's blending.

## The probe (menu: Windshear Test: probe X-Plane)

Use it in cruise or well above the ground; the test wind sits below 3,000 ft AGL.

1. Choose the menu item once. A record of radius 1 nm goes 6 nm ahead, 30° right of your track. Every wind layer below 3,000 ft above the
   ground in it becomes 25 kt across your track.
2. For 4 minutes the log reads X-Plane's own wind 300 ft above the ground along a line through the record, from 12 nm before to 12 nm
   beyond it: every 5 s for the first minute, then every 15 s. Lines start with `[WXPlane Windshear]`.
3. Choose it again to stop early. Each new start uses the next radius: 1, 2.5, then 5 nm. The record is erased at stop or after 5 minutes.

From the log:
- **footprint:** how far from the centre the 25 kt reaches, for each radius;
- **sharpness:** how quickly it falls off;
- **fade-in time:** how long X-Plane takes to show it.

Caveats:
- A regular WXPlane update that replaces all records (the log says "one replacing update") also wipes the test record, so run the probe
  just after an update.
- The ground is taken as flat at the height under the aircraft.
