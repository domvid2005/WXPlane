# Realistic turbulence (2026-11-06)

Menu: **Enable Realistic Turbulence** (`realistic_turbulence`, on by default) and **Max Turbulence...** just below the toggles, above the status lines
(`max_turbulence`, 0 to 1.0).
Code:
- `src/gribx.*`: GRIB2 decoding (JPEG2000, CCSDS, Lambert).
- `src/turbulence.*`: the model; no X-Plane or network calls.
- `src/turbfetch.*`: the worker thread.
- `turbulenceTick` in `src/plugin.cpp`.

Log lines start with `[WXPlane Turbulence]`.

## Sources

| | NOAA GTGN (US) | ECMWF Open Data (elsewhere) |
|---|---|---|
| What | Graphical Turbulence Guidance Nowcast: EDR (GRIB2 0.19.30) | IFS forecast winds; **no EDR is published** |
| Grid | HRRR Lambert conformal, 3 km, 1799 x 1059 | 0.25° lat/lon, global |
| Levels | every 1,000 ft, 100 ft to 50,000 ft | 700, 600, 500, 400, 300, 250, 200, 150, 100 hPa |
| Updates | every 15 min (analysis) | runs at 00/06/12/18Z, steps every 3 h |
| Packing | JPEG2000 (5.40) with a bitmap, decoded with OpenJPEG | CCSDS (5.42), decoded with libaec |
| What is downloaded | the levels within ~9,500 ft of the aircraft, by byte range (header walk of the file first) | u and v at the 9 levels, two steps around now, by byte range from the `.index` |
| Size | ~0.6 MB per level (about 3–5 MB per 15 min in cruise) | ~11 MB per step (a step lasts 3 h) |
| Used when | analysis ≤ 45 min old | step within 4 h of now; FL100 to FL500 only |

- **GTGN files:** NOMADS keeps them in hour folders, `gtgn.YYYYMMDD/HH/gtgn.tHHMMz.3km.grib2` (seen 2026-11-06), about 27 MB
  each and posted 1–2 minutes after their time. There are no `.idx` files. The newest of the last 2 hours is tried by name. The
  valid time comes from the GRIB2 header. The first builds used `gtgn.YYYYMMDD/gtgn.tHHMMz.3km.grib2` (from NWS SCN 26-62) and the
  day-folder listing; both missed the hour folder.
- **Crops:** GTGN levels are cropped to 141 x 141 points (about 210 km) around the aircraft, and ECMWF fields to ±12°. They are cropped
  again when the aircraft nears the edge, from the downloaded bytes.
- **Choosing a source:** ECMWF is downloaded only when GTGN does not fully cover the aircraft (outside the US, near the edge of the
  grid, or no current analysis).
- **Licences:** ECMWF Open Data is CC BY 4.0 (© ECMWF). NOAA data is public domain.

## The ECMWF estimate (not EDR)

- **The index:** Ellrod & Knapp (1992) TI1 = vertical wind shear × total deformation, for each layer between two pressure levels:
  - VWS = |ΔV| / Δz, with Δz from the ISA heights of the two levels (not the model geopotential; that saves a third of the download);
  - DEF = √(DST² + DSH²), where DST = ∂u/∂x − ∂v/∂y and DSH = ∂v/∂x + ∂u/∂y, from centred differences of the layer-mean wind.
- **Units and thresholds:** TI1 is in 10⁻⁷ s⁻². Commonly used thresholds are about 4 for light to moderate, 8 for moderate and 12
  for moderate to severe.
- **EDR-equivalent:** each TI1 value is converted to the centre of its category: 0 → 0, 2 → 0.03, 4 → 0.12, 8 → 0.22,
  12 → 0.32, and 16 or more → 0.40. That top value is held, so the estimate alone never reaches "severe". Below about TI1 2.5 the
  result is under 0.07, so it adds nothing.
- **Uncertainty:** about one category. Ellrod's thresholds were set on coarser grids; a 0.25° grid resolves stronger gradients. The
  log always labels this value "ECMWF Ellrod TI1 x (EDR-equivalent y, estimate)".
- **Not used below FL100:** the index is a clear-air-turbulence diagnostic. Low-level turbulence outside the US is left to X-Plane and
  to WXPlane's gust turbulence at airports.

## From EDR to X-Plane

1. **Sampling:** the EDR is sampled at the aircraft's position at each altitude of `sim/weather/region/turbulence`. Those are the 13
   wind layers (`sim/weather/region/wind_altitude_msl_m`). This keeps the vertical structure; X-Plane interpolates between layers.
   - GTGN is interpolated bilinearly between grid points and linearly between levels. A level below the ground is skipped.
   - ECMWF is interpolated in space and linearly in time between its two steps.
   - Near GTGN's grid edge the weight goes from 1 to 0 between about 60 km and 9 km from the edge, so the switch to ECMWF is a blend.
2. **Pockets:** smooth value noise (8 km cells, 3,000 ft layers, 4 min) multiplies the EDR by about 0.85–1.2 with GTGN and 0.7–1.45
   with ECMWF. It is deterministic in position and simulation time. It adds rougher and smoother pockets inside the data's envelope,
   most where the source grid is coarse. The second-to-second shaking is X-Plane's own; WXPlane does not move the aircraft.
3. **Time:** a new GTGN analysis is faded in over 3 minutes. Each layer is smoothed with a 20 s time constant, and the value written
   changes by at most 0.5 per 5 s write.
4. **Max turbulence:** the EDR is multiplied by it. At 0, zero is written.
5. **Calibration:**
   - **Assumption:** the load-factor spread a typical transport should feel is σg = 0.65 × (EDR − 0.07). Air below EDR 0.07
     (smooth to very light) adds nothing. The ~1-minute peak (about 3σ) is then ~0.35 g at EDR 0.25 and ~0.85 g at EDR 0.5.
   - **Measured:** the X-Plane level that gives that σg (2026-11-06), in three steps:

| Measurement | Held level | Load-factor sd | Per level |
|---|---|---|---|
| response test, 250 kt, 3,000 ft | 2 / 3 / 5–10 | 0.21 / 0.39 / 0.40–0.42 g | saturates at 3 |
| first flight, climbing through 11,000 ft | 0.6 | 0.14 g | 0.23 g |
| ToLiss A319 climbing FL165–FL205, GTGN EDR 0.30–0.38 | 0.56–0.75 | 0.24–0.31 g | ~0.40 g |

   - **The A319 point:** it was about 1.6 times the target for moderate air, and it felt too strong. The response grows with
     airspeed, so the fastest measurement is used. That keeps slower flight on the gentle side: 0.40 g at level 1, and the earlier
     shape above it scaled by the same factor (0.56 g at 2, 0.68 g at 3).
   - **Resulting mapping:** EDR ≤ 0.07 → 0, 0.10 → 0.05, 0.20 → 0.21, 0.30 → 0.37, 0.45 → 0.62, 0.60 → 0.86. Level 1 is only reached
     around EDR 0.7 (severe).

   X-Plane's response depends on the aircraft, as a real aircraft's does. The calibration targets the air, not one aircraft.
6. **Writing:** only `sim/weather/region/turbulence` is written, every 5 s. Its earlier values are saved first and put back when the
   feature is turned off, the weather is turned off, or the plugin is disabled. Storm and gust turbulence in WXPlane's weather records
   is unchanged except for the Max turbulence scale.

## Pilot reports (2026-11-06)

Code: `src/hazards.*` (parsing, reporter size, floor), `reportsStep` in `src/turbfetch.cpp`, map layers in `src/plugin.cpp`.

- **Data:** NOAA AWC data API (`pirep`) every 10 minutes (and after a move of 300 nm) whenever the weather is on: reports within
  about ±8° latitude and ±12° longitude of the aircraft, last 90 minutes.
- **Reporter size:** a reported intensity describes how the reporting aircraft felt it. It is turned into the EDR of the air with the
  reporter's ICAO type:

| Class | Examples | Factor |
|---|---|---|
| light | C172, PA28, SR22, King Air, PC-12, TBM, Caravan | ×0.6 |
| regional | CRJ, ERJ, Dash 8, ATR, Citations, Learjets, Gulfstreams | ×0.85 |
| medium | A320 family, 737 family, 757, A220, MD-80 | ×1.0 |
| heavy | widebodies (747, 767, 777, 787, A330, A350, A380), C-17 | ×1.25 |
| type not determinable | blank, UNKN, an AIREP callsign, or a type not in the lists | ×0.6 (lightest, so a report never overstates) |

  The category centres for a medium aircraft are: LGT 0.15, LGT-MOD 0.22, MOD 0.30, MOD-SEV 0.40, SEV 0.50 and EXTRM 0.60. So
  "extreme" from a C172 (0.36) is less than "moderate-to-severe" from an A320 (0.40). The resulting EDR goes to X-Plane like any
  other, and X-Plane's flight model makes the user's own aircraft respond to it.
- **The floor (pilot reports only):** the turbulence is raised to the floor and never lowered by it. A NEG report raises nothing.
  - **PIREPs:** full strength within 10 nm and 30 minutes, fading to nothing at 50 nm and 90 minutes. They apply within the reported
    layer, or ±3,000 ft of the flight level when there is no layer, with a 2,000 ft taper.
  - **SIGMETs and AIRMETs are not used.** An earlier build raised turbulence inside turbulence SIGMETs (0.30) and G-AIRMET Tango
    (0.18), then only drew them on the map. Both were removed on 2026-11-06: they are broad forecast areas drawn wide on purpose.
  - **Weight:** each floor is weighted by (1 − GTGN coverage). GTGN already assimilates pilot reports, so this mainly acts outside
    the US.
  - **Off switch:** `turb_reports = 0` turns the floor off. The map layers stay.
- **Map:** one layer in X-Plane's map, with its own toggle in the map's layer list: **"WXPlane: Pilot reports (turbulence)"**. It
  puts a triangle at each report that mentions turbulence or low-level wind shear, red for an urgent report (UUA, also larger) and
  orange for any other. The label shows UUA/LLWS when present, then the intensity, flight level, type and time. Labels are hidden
  when more than 80 reports are in view. The triangles are drawn with OpenGL 1.x through X-Plane's drawing callback; the functions
  are looked up at run time, so the plugin links no OpenGL library.
- **Low-level wind shear reports:** a pilot report with LLWS (or a `/WS` group) within 15 nm and 90 minutes, reported no
  higher than 4,000 ft above the local ground, adds 0.025 to the hourly odds of a gust front and of a microburst at the windshear
  sources near it (for example 10 %/h becomes 12.5 %/h). Their strength is unchanged. It only applies where convection already makes
  a source; on its own it creates nothing. Logged as "low-level wind shear pilot report nearby".
- **Log:** a line per refresh (`reports: …`) and, when a floor sets the turbulence, `floor EDR x from PIREP MOD A320 (medium,
  x1.00) FL330, 3 nm, 0 min ago`.

## Failures and stale data

- **NOMADS throttling:** requests to NOMADS are spaced at least 0.6 s apart. On 2026-11-06 a burst during the header walk got
  HTTP 302 for one level. A throttled request (302, 429 or 503) is retried once after 20 s.
- **Files still being uploaded:** an analysis is used only from 5 minutes after its time. A file whose levels end below 45,000 ft
  is treated as incomplete; the previous analysis is kept and the file is checked again 2 minutes later. On 2026-11-06 a file read 2
  minutes after its time held only the levels up to 5,000 ft, so the climb above that fell back to the ECMWF estimate over the US.
- **Network errors:** the worker retries with a growing delay (1 to 15 min for GTGN, 2 to 30 min for ECMWF) and logs each new problem
  once.
- **Stale data:** a GTGN analysis older than 45 min, or an ECMWF step more than 4 h from now, is not used.
- **No data:** a layer with no current data keeps X-Plane's own value from before WXPlane wrote (below FL100 outside the US, for
  example); nothing is invented. Until the first data arrives, X-Plane's
  values are left alone.
- **No network on the sim's thread:** all downloads and decoding happen on the worker. The flight loop only reads the published
  cubes, once a second.

## Log line (every minute and when the source changes)

```
[WXPlane Turbulence] GTGN | data: GTGN 21:30Z (6 min old) | aircraft 33000 ft MSL: GTGN EDR 0.228; coverage 1.00, variation x1.02
  -> EDR 0.233 (x max 1.00, smoothed 0.233) -> X-Plane level 1.65 requested, 1.65 held | load factor sd 0.041 g over 240 samples
```

`diag_verbosity = 2` adds one line with every layer (`ft: EDR/level`).

## Verification done before release

- **JPEG2000 and bitmap:** a real NOAA GRIB2 message (WAVEWATCH III, NCEPLIBS-g2c test data) decodes to the reference minimum, mean
  and maximum.
- **CCSDS:** a GRIB2 message encoded with libaec using ECMWF's flags (preprocessing, MSB, block 32, RSI 128) decodes exactly.
- **Lambert:** the HRRR grid's far corner comes out at 47.842° N, 60.917° W (NOAA's grid description); position and inverse agree to
  1e-6°.
- **Model:**
  - **Ellrod TI1:** a synthetic shear-and-deformation field gives the analytic value within 3 %.
  - **GTGN interpolation:** vertical interpolation, skipping levels below the ground, edge coverage, the 3-minute fade and the
    stale-data cut-off are all checked.
  - **Variation:** it is smooth in space and time.
- **End to end** (`HARNESS_TURB=1 tests/harness`): synthetic GTGN (JPEG2000, Lambert) and ECMWF (CCSDS, index) files are served by a
  mock network. The run checks:
  - the header walk, byte ranges and the written levels at 33,000 ft;
  - that no ECMWF download happens inside the US;
  - the switch to the ECMWF estimate over Germany;
  - that X-Plane's values are put back by the menu toggle.
- **First flight with GTGN (2026-11-06, KDAB climb to FL240):**
  - the files were found and their 51 EDR levels (98–50,000 ft, 5.40, the HRRR Lambert grid) decoded;
  - EDR 0.12 at the surface and 0.06–0.08 in the climb became level 0–0.2, so the ride stayed smooth;
  - the switch between GTGN and ECMWF worked.
- **Still to confirm in the sim:**
  - the real GTGN file layout (level type, number of levels, idx-free header walk);
  - the real ECMWF fields;
  - whether X-Plane keeps the values written, given the odd read-backs in the test;
  - the response in cruise.
