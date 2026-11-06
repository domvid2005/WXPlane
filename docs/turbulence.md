# Realistic turbulence (2026-11-06)

Menu: **Enable Realistic Turbulence** (`realistic_turbulence`, on by default) and **Max Turbulence...** at the bottom of the menu
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

- **GTGN files:** they are found by trying the last 2 hours of 15-minute names directly
  (`gtgn.YYYYMMDD/gtgn.tHHMMz.3km.grib2`, from NWS SCN 26-62). If none answers, the log shows the HTTP codes and, once an hour, what
  the NOMADS directory returns.
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
- **EDR-equivalent:** each TI1 value is converted to the centre of its category: 0 → 0.01, 1 → 0.05, 2 → 0.08, 4 → 0.15, 8 → 0.25,
  12 → 0.35, and 16 or more → 0.42. That top value is held, so the estimate alone never reaches "severe".
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
   - **Assumption:** the load-factor spread a typical transport should feel is σg = 0.65 × EDR. The ~1-minute peak (about 3σ) is then
     ~0.5 g at EDR 0.25 (moderate) and ~1 g at EDR 0.5 (severe).
   - **Measured:** the X-Plane level that gives that σg, from the in-sim test (2026-11-06, one aircraft, 250 kt, 3,000 ft, last 25 s
     of each 50 s step; the smooth-air 0.022 g taken out):

| Level | 0 | 1 | 2 | 3 | 5, 7, 10 |
|---|---|---|---|---|---|
| load-factor sd measured | 0.022 g | 0.045 g | 0.213 g | 0.386 g | 0.40–0.42 g |
| used (baseline removed) | 0 | 0.039 | 0.212 | 0.385 | (saturated: 3 is the top) |

   Resulting mapping: EDR 0.02 → 0.3, 0.05 → 0.8, 0.10 → 1.15, 0.20 → 1.5, 0.30 → 1.9, 0.45 → 2.5, 0.60 and above → 3.0.

   X-Plane's response depends on the aircraft, as a real aircraft's does. The calibration targets the air, not one aircraft.
6. **Writing:** only `sim/weather/region/turbulence` is written, every 5 s. Its earlier values are saved first and put back when the
   feature is turned off, the weather is turned off, or the plugin is disabled. Storm and gust turbulence in WXPlane's weather records
   is unchanged except for the Max turbulence scale.

## Failures and stale data

- **Network errors:** the worker retries with a growing delay (1 to 15 min for GTGN, 2 to 30 min for ECMWF) and logs each new problem
  once.
- **Stale data:** a GTGN analysis older than 45 min, or an ECMWF step more than 4 h from now, is not used.
- **No data:** with no current data at a layer, zero is written there; nothing is invented. Until the first data arrives, X-Plane's
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
- **Still to confirm in the sim:**
  - the real GTGN file layout (level type, number of levels, idx-free header walk);
  - the real ECMWF fields;
  - whether X-Plane keeps the values written, given the odd read-backs in the test;
  - the response in cruise.
