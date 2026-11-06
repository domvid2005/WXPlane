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

## The probe (removed 2026-11-06 after its results went into the windshear model)

Use it in cruise or well above the ground; the test wind sits below 3,000 ft AGL.

1. Choose the menu item once. Three records go out at the same time, 10 nm from the aircraft: r=1 nm at 50° left of track, r=2.5 nm
   straight ahead, r=5 nm at 50° right. In each, every wind layer below 3,000 ft above the ground becomes 25 kt across your track.
2. For 5 minutes the log reads X-Plane's own wind 300 ft above the ground along a line through each record, from 8 nm before to 8 nm beyond
   its centre: every 10 s for 3 minutes, then every 20 s. Lines start with `[WXPlane Windshear]`, one per record.
3. Don't stop it: it ends by itself with "test stopped (5 min, complete)" and erases the records. Choosing the menu item again stops it
   early.

From the log:
- **footprint:** how far from the centre the 25 kt reaches, for each radius;
- **sharpness:** how quickly it falls off;
- **fade-in time:** how long X-Plane takes to show it.

Caveats:
- If a regular WXPlane update replaces all records meanwhile, the test records are re-sent with it.
- The ground is taken as flat at the height under the aircraft.

## Probe results so far (logs 2026-11-06 11:45 and 11:51, Daytona Beach, ~2,000 ft AGL, region writes on)

| Record radius | Result |
|---|---|
| 1 nm | Stopped at 26 s: too early to see anything (see the fade-in time below). |
| 2.5 nm | **Visible.** The fade-in starts ~30 s after sending and builds over ~60 s; the peak is at ~90 s. At the centre the wind went from 234/5 kt to 166/13 kt: about half of the 25 kt sent, partly blended with the surrounding weather. It reached ±1 nm from the centre (174–175/10 kt), was barely there at ±2 nm (221–230/5 kt) and gone at ±3 nm. |
| 5 nm | Not run yet. |

At +77 s a regular WXPlane update replaced all records, which wiped the test record; it faded out over ~60 s (gone by 153 s). The
probe now re-sends its record with any replacing update.

**What this means:**
- Records do change the low-level wind while the region arrays are being written: the region profile is the background they blend
  over. The earlier guess that the region arrays override records was wrong.
- **Fade-in:** ~30 s delay, then ~60 s ramp, so an effect needs 1.5 minutes or more to be fully there. A gust front that lives for an hour
  fits. A microburst's 2–4 minutes at full strength barely does, and only if it is sent before it matures.
- **Footprint and sharpness:** a 2.5 nm record acts on roughly ±1–1.5 nm and reaches only about half its own wind. The edge is about 1 nm
  wide.
- **Microburst outflow:** it needs opposite winds on either side of a ~2 nm core, so it would mostly blend away at this resolution. It is
  **not representable faithfully.**
- **Gust fronts and frontal boundaries:** a line of records ≥2.5 nm, a few nm deep, gives a boundary about 1 nm wide. Its strength has to
  be set knowing about half of it arrives. Whether larger or stacked records reach full strength is still to be measured.
