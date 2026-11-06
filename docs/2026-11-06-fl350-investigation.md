# WXPlane investigation, 2026-11-06: FL350 altitude excursion and related issues

Evidence: X-Plane 12.4.4 log of the 2026-11-05 flight (ToLiss, FL350 over the San Juan Mountains, Colorado), lines 11067–11440.
Each item is marked **confirmed** (traced in the source and matched in the log), **suspected** (plausible, needs a flight to confirm)
or **not a defect**.

## The event, in order (log)

| Line | What the log shows |
|---|---|
| 11082–11084 | Steady FL350: true 34,983 ft, indicated 35,000 ft, outside pressure 238.9 hPa (ISA at FL350 is 238.4 hPa: normal). |
| 11067, 11072 | KC24 (Creede, CO) becomes the nearest station. The profile we send now starts at **28,510 ft** with seven calm layers between 28,510 and 28,530 ft, and the temperatures are **+3 °C from FL050 to FL240** (ISA+36 at FL240). |
| 11149–11160 | Same bad profile; the sim's temperatures are pulled towards it (FL180 −5 °C, FL240 −13 °C, from −13/−26). |
| 11161–11166 | Outside pressure at the aircraft rises from 239 to 263 hPa at constant true altitude; indicated altitude falls to 32,253 ft (indicated minus true −3,673 ft). The autopilot climbs to restore 35,000 indicated. |
| 11167–11220 | Climb at up to 6,393 fpm to 39,205 ft true, indicated back at ~35,000. No vertical air, no turbulence. |
| 11231 | X-Plane's temperatures now equal ours: +3 °C from FL050 to FL240. |
| 11241–11360 | Pressure field relaxes (230 → 209 hPa), the aircraft descends at up to 6,384 fpm back towards FL350. |

**Cause: a temperature-column error, from a wrong station elevation.** KC24's elevation arrived as 8,680 m (its 8,680 ft read as
metres). That single number made WXPlane treat everything below 8,690 m (28,510 ft) as underground:

1. every forecast temperature below 8,990 m was dropped and the column from the ground to FL240 was filled with the surface temperature
   (+3 °C);
2. the 12 wind-grid altitudes below "the ground" were clamped to 1 m above each other (the seven layers 28,510–28,530 ft).

That warm column, written into the sim's region temperature array every 20 s and all at once, raised the pressure at FL350 by about
24 hPa. A barometric altimeter reads pressure, so it read ~4,000 ft low and the autopilot climbed ~4,000 ft to "hold" FL350.

Hydrostatic check: a column about 20 °C warmer than before over the ~5 km from the ground to FL240 raises the pressure at FL350 by
roughly 5 %. The log shows about 10 % (239 → 263 hPa). So the temperatures explain about half of the jump, by this estimate.
A second channel from the same bad number is likely. X-Plane knows the field as "C24", not "KC24", so WXPlane sent KC24 as a location
record with 8,680 m as its ground altitude. How X-Plane uses that altitude for pressure is undocumented (**suspected**). Both channels
come from the wrong elevation, which is now corrected before anything is built from it.

The wind layers did **not** drive the climb: the aircraft was always above them (the calm band ended at 28,530 ft; at FL350 the wind
changed from 282/40 to 303/54 kt). The ground-speed drop is consistent with the climb itself (the aircraft trading speed for 6,000 fpm)
plus that wind change. It was not logged, so this part is **suspected**; airspeeds and ground speed are now logged.

## Issue 1: wind-layer geometry

| # | Finding | Status | Where |
|---|---|---|---|
| 1.1 | Layer altitudes came from a fixed 12-altitude grid plus the surface layer. | — | `wxmodel.cpp buildXp` |
| 1.2 | Grid altitudes under the station were clamped to `previous + 1 m`, squeezing them together above the "ground". | **confirmed** | `buildXp`, `w.altMsl = max(gft, prevM + 1)` |
| 1.3 | The terrain (station elevation) shaped the whole profile; with a wrong elevation every level below 28,510 ft was lost. | **confirmed** | `buildXp`, `st.elevM + 300` filters |
| 1.4 | No AGL/MSL mix-up in winds aloft: FD and model altitudes are MSL and stay MSL. | not a defect | `windsaloft.cpp` |
| 1.5 | Missing levels are not set to 0. The 000/0 kt layers were the calm KC24 surface wind (00000KT) held through the clamped levels. FD "9900" (light and variable) is real data and decodes to 0 kt. | not a defect | `decodeEntry` |
| 1.6 | Record ceilings (max_altitude_msl_ft = cruise ceiling with aloft data) do not cut wind layers. | not a defect | `buildXp` |
| 1.7 | X-Plane's handling of 1-m-apart layers is unknown; it no longer matters, they are not produced. | — | — |
| 1.8 | The bad layers coincide with KC24 becoming nearest (line 11067); the excursion came from the temperatures sent at the same time. | **confirmed** | log |

**Fix** (`buildAloftProfile`, `wxmodel.cpp`): every record uses the same nominal altitudes (3,000 … 66,000 ft), strictly ascending and at
least 150 m apart. A level under the ground carries the surface wind instead of being squeezed. A grid level within 150 m of the surface
layer is skipped, never moved. Winds are interpolated as vectors between valid levels across any gap. NaN, negative, >300 kt and
out-of-range values are dropped and counted, never replaced by 0. With no forecast winds the record keeps only its surface wind; nothing
is invented.

## Issue 2: pressure and temperature

| # | Finding | Status |
|---|---|---|
| 2.1 | 239 hPa near FL350 is the correct static pressure (ISA 238.4 hPa). | not a defect |
| 2.2 | QNH goes in `pressure_sl` (since 2026-11-04), in Pa; QNH 1025 hPa → 102500 Pa. | not a defect |
| 2.3 | Units: Pa in the API, hPa in the log, inHg only for the baro setting read-back. °C throughout; no Kelvin anywhere. | not a defect |
| 2.6 | **Station elevation** taken as given from aviationweather.gov; no sanity check. KC24 came in as 8,680 "m". | **confirmed** |
| 2.7 | Temperatures below "the ground" were replaced by the surface temperature; with a wrong elevation that spanned FL050–FL240. | **confirmed** |
| 2.8 | The region temperature array was overwritten in one step every 20 s: a new column took effect at once. | **confirmed** |
| 2.9 | SDK fields used correctly (`temperature_alt` layers at `atmosphere_alt_levels_m`, `pressure_sl`). | not a defect |
| 2.10 | **Mislabelled diagnostic:** the weather-watch line called `pressure_alt` from `XPLMGetWeatherAtLocation` "pressure at the aircraft" (1008–1045 hPa at FL350). It is a QNH-style value for the spot; the real static pressure is the altitude watch's "outside pressure". | **confirmed** (label only) |
| 2.11 | X-Plane's own FL360 temperature stays ~4 °C colder than ours (−57 vs −53) all flight. Small; X-Plane's interpolation between its levels. | suspected, harmless |

**Fixes:**
- `checkStationElevations` (`sources.cpp`) runs on every download. An elevation outside −450…5,600 m is read as feet when that fits,
  otherwise it takes the neighbours' median. An elevation far from all neighbours that fits them in feet is converted. Each correction is
  logged ("station elevations corrected: KC24 elevation 8680 m -> 2646 m (…)").
- Temperatures keep every valid forecast level, including those below the terrain (the free-air temperature, which sets the pressure at
  altitude). Only the 300 m just above the ground is left to the surface report. Impossible values are dropped: not finite, outside
  −95…+50 °C, or more than 40 °C from standard.
- The region temperature array starts from what the sim already has and moves at most 1.5 °C per level per 20 s. A large difference logs
  "region temperatures ramping". A future error of this kind can no longer move the altimeter thousands of feet at once.
- Weather-watch label corrected.

## Issue 3: ground speed

- Wind vectors are built and interpolated as u/v components everywhere: FD blending, global model, station blend, record grid, region
  resample. 350° and 010° give 000°. **Not a defect** (now covered by a test).
- Speeds: FD/model in kt → m/s with 0.514444 for the API. **Not a defect.**
- **Confirmed:** the region wind array was rewritten in one step. Its altitude layout also followed the nearest station (the squeezed
  layers went into the sim too, line 11233). **Fix:** fixed region altitudes (1,000 … 60,000 ft), at most 3 m/s of vector change per level
  per 20 s.
- The ground-speed drop is not in the log (it wasn't recorded). Airspeeds, Mach and ground speed are now logged; the cause is **suspected**
  (the climb, plus the 40 → 54 kt wind change at FL350).

## Issue 4: wet pavement in dry weather

- A dry METAR gives `precip_rate = 0`. Gap-fill tiles take 0 when the radar picture is valid. **Not a defect** (test added).
- A station inside a radar echo gets the radar's rain even when its own METAR is dry (`plugin.cpp`, natural picture). MRMS composite
  reflectivity includes rain that evaporates before reaching the ground. **Suspected** contributor in light echoes. Unchanged, because
  removing it makes dry holes in rain areas, which you asked to avoid.
- Without a valid radar picture, a gap-fill tile copies the nearest station's precipitation flag (`blendAt`). **Suspected** contributor
  near a raining airport when the radar is off or failed.
- WXPlane never writes `runway_friction` or `rain_percent`, so it cannot leave a stale runway state. At start-up X-Plane runs its own
  weather until WXPlane's first update, and any wetness from that is X-Plane's. **Suspected**; needs the start-up log.
- **Added:** "surface state" log lines for the first three updates: the nearest airport's METAR, the rain we sent, the sim's
  `rain_percent`, `runway_friction` and rain on the aircraft. That separates WXPlane's rain, the sim's state and scenery materials.

## Issue 5: magnetic and true wind

- WXPlane uses real D-ATIS text (`datis` API), parsed for wind, visibility, clouds and altimeter (`mergeAtis`). It does not generate ATIS
  text.
- **Confirmed defect:** ATIS winds (magnetic) replaced the METAR wind (true) unconverted. With 7–15° of variation over the US, the surface
  wind at D-ATIS airports was off by that much.
- **Fix:** `mergeAtis` flags an ATIS wind as magnetic. On the sim thread, `annotateMagneticVariation` converts it once, with X-Plane's
  variation at the airport: TRUE = MAGNETIC + east variation, normalised 1–360, north = 360. Calm and variable winds are untouched. The SDK
  does not document the sign of `XPLMGetMagneticVariation`, so it is checked once against `XPLMDegMagneticToDegTrue` at the aircraft. Each
  conversion is logged ("wind reference: KCMH ATIS 150 MAGNETIC, variation 6.8 WEST, applied 143 TRUE").
- METAR, FD and global-model winds stay true; there is no second conversion. The ATIS window shows the broadcast text verbatim
  (magnetic), the injected wind is true.

## Issue 6: record continuity

- Fill tiles take the nearest station's elevation and blended QNH. With elevations validated, a bad elevation no longer spreads to its
  tiles.
- The region arrays (winds and temperatures) now change gradually as the aircraft moves from one record's area to the next.
- The station-list "moved" update (line 11085) replaced all 114 records in one fade-in. That is normal, and the records were fine except
  KC24's.

## Issue 7: validation

`validateProfile` (`wxmodel.cpp`) runs inside `validateXp` for every record, and again before every region write. It checks:
- wind layers ascending and ≥100 m apart;
- wind speed 0–310 kt, direction 0–360°, altitude −500…30,000 m;
- every value finite;
- temperatures −95…+60 °C and within 40 °C of standard above 1,000 m;
- QNH 870–1,085 hPa;
- precipitation 0–1.

Bad entries are dropped and reported in the "validation adjusted" line. A QNH that fails is marked unknown, not set to standard. A region
profile with fewer than two valid wind layers is not written at all: the sim keeps the last good one.

## Issue 8: diagnostics

- `diag_verbosity` in `WXPlane.cfg`: 0 = off, 1 (default) = a flight-state summary every 5 min plus every event, 2 = every 10 s.
- **Event:** the indicated-minus-true gap moving 200+ ft in 10 s; 2,500+ fpm with the autopilot target within 1,000 ft; or ground speed
  moving 25+ kt. It logs UTC, position, IAS/TAS/Mach/GS and the wind at the aircraft, then (at most once a minute) the full weather
  picture: winds and temperatures sent and read back, the records covering the aircraft and their QNH.
- Also logged: station elevation corrections, magnetic conversions, region ramping, rejected values. When the FD download works but
  nothing parses, the answer's HTTP status, size and first 160 characters are logged.

## Also found: NOAA winds aloft never parsed during this flight

Every update logged "NOAA winds aloft: no data parsed", so the whole flight ran on one global-model point. The download itself worked.
The cause is not visible in the log and aviationweather.gov is not reachable from the build environment. The next log will show the
answer's start. The parser now also accepts 4-letter identifiers (KABQ → ABQ) in case the product switched to them.

## Issue 9: SDK compliance

- Weather calls run in a flight loop at `xplm_FlightLoop_Phase_BeforeFlightModel`; every `XPLMBeginWeatherUpdate` has its
  `XPLMEndWeatherUpdate`. In-flight updates are fade-in, not immediate.
- Disable/enable erases WXPlane's records (`XPLMEraseWeatherAt*`, then an empty replacing update).
- The plugin writes no aircraft position, altitude, speed or autopilot state. It only writes the weather records and the region weather
  arrays (writable per Laminar's weather-datarefs article).

## Tests

Unit tests (`test_wx`): 435 pass. New:
- station elevation (KC24 8,680 → 2,646 m; real high fields like KLXV untouched; no neighbours; NaN);
- high-elevation profile (no squeezed layers, FL180/FL240 temperatures from the forecast, FL350 jet wind);
- the same with the bad elevation (still ordered and spaced);
- malformed FD values (NaN, 900 kt, ISA+60) dropped and interpolated across;
- no data (nothing invented);
- 350°/010° interpolation and a strong gradient;
- magnetic → true (east/west, across 360, near-zero and large variation, calm/variable);
- ATIS flagged magnetic, METAR not;
- validation of squeezed layers, NaN, bad QNH/rain;
- dry vs rain METAR precipitation;
- 4-letter FD identifiers.

Mock X-Plane harness: default and ramp modes pass. The staged-data mode fails one check (line 321, update end/immediate). It fails the
same way on the code before these changes, so it is not new.

**Needs a flight:** the FL350 cruise over high terrain, record-boundary crossings at cruise, wet runways at start-up, the magnetic
variation sign check, and why the FD product doesn't parse.
