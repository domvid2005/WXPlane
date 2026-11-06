# Realistic windshear (2026-11-06)

Menu: **Enable Realistic Windshear** (off by default; saved as `windshear = 1` in `WXPlane.cfg`) and **Windshear Test: event ahead now**.
Code: `src/windshear.h/.cpp` (the model, no X-Plane calls) and `windshearTick` in `src/plugin.cpp` (risk, sending, encounter log).
Log lines start with `[WXPlane Windshear]`.

## What is simulated, and what reaches X-Plane

| Phenomenon | In the model | Reaches X-Plane | How |
|---|---|---|---|
| Vertical wind gradients: low-level jet, inversion, directional shear with height | from the data | yes | The station and winds-aloft profile, already sent. The record grid now has a 1,500 ft level and the region array 1,000/2,000/3,000/4,500 ft, so a jet near 2,000 ft is not smeared. Nothing synthetic is added. |
| Fronts, wind shifts and gusts between airports | from the data | yes | Station records with their own winds and gusts; X-Plane blends between them. No synthetic field (it would double-count). |
| Gust fronts (convective outflow boundaries) | yes | yes | Fixed 1.2 nm wind cells behind the moving front, each adding the outflow at its spot to the ambient wind below 3,000 ft AGL. |
| Microburst outflow | yes | yes, coarse | Fixed 1.2 nm wind cells around the centre, blowing outward with the model's outflow profile, below 3,000 ft AGL. Crossing it gives headwind, then tailwind; off-centre a crosswind. |
| Microburst / convective downdraft | yes (logged) | **no** | X-Plane's weather records have no vertical-wind field. Applying it would mean pushing the aircraft, which WXPlane never does. |
| Sea-breeze, terrain-driven boundaries | no | — | No reliable data to place them. |

## Sources and confidence (`assess`)

- **Source:** a 10 nm square holding a measured radar echo of 30+ dBZ, or an airport within 15 nm reporting TS / VCTS / SQ.
  Synthetic radar cells (rims, hole fills) never count.
- **Strong convection:** ≥50 dBZ, or TS with gusts ≥30 kt. Gust fronts 15 %/h, microbursts 2 %/h.
- **Convection:** ≥40 dBZ, or TS. Gust fronts 10 %/h, microbursts 0.7 %/h.
- **Dry-microburst environment:** a 30–44 dBZ echo, a temperature/dewpoint spread of at least 15 °C at the nearest airport, and a cloud base
  at or above 8,000 ft AGL. All three are required; a large spread alone is not enough. Gust fronts 2 %/h, microbursts 0.5 %/h, halved
  for low confidence.
- **Confidence:**
  - high: ≥45 dBZ together with reported TS, or ≥40 dBZ with TS and gusts ≥25 kt;
  - medium: one kind of evidence;
  - low: the dry-microburst proxy.
- **No evidence, no source:** calm or clear weather forms nothing. The 0.05 %/h background in the specification is not used, because
  there is nothing to place such an event on.
- **Range:** only sources within 100 nm of the aircraft are simulated. A field that has formed keeps existing when the aircraft leaves.

## Formation, lifecycle, movement

- **Formation:** a hazard rate per source, `λ = −ln(1 − P_hour)/3600`, evaluated in 30 s buckets of simulation time. The random draw is a
  hash of (seed, source, bucket), so the same weather and seed give the same fields at any frame rate (test K). Pausing stops simulation
  time. A jump of more than 10 minutes is not back-filled.
- **Limits:** one field of each kind per source at a time; the next can form after it dissipates. At most 12 fields.
- **Microburst:**
  - lifecycle: builds over 5 min, 3 min at peak, gone by 15 min (AIM 7-1-24);
  - size: outflow ring radius 0.7 → 1.3 nm (about 2.5 statute miles across);
  - strength: outflow 8–30 kt from the reflectivity, ×1.5 in 5 % of microbursts in ≥50 dBZ storms, capped at 45 kt;
  - downdraft: 600 + 90 × outflow kt fpm, capped at 6,000; modelled only.
- **Gust front:**
  - life 45–75 min: builds over 10 min, weakens over the last 15;
  - spreads 12 kt from its parent storm, 2 nm out at formation;
  - outflow 10–35 kt, from reflectivity and gusts, toward the storm's motion ±40°.
- **Movement:** storms drift with 70 % of the 10,000 ft wind. That is an assumption: one radar picture gives no measured storm motion.

## X-Plane delivery (from the probe, 2026-11-06)

- **What the probe measured:** X-Plane starts fading a record in after 30–70 s and reaches full strength after ~2 min. It delivers 60–80 %
  of the wind, and a record acts over ~0.75 of its radius.
- **Fixed cells:** the records are wind cells on a fixed 1.2 nm lattice, each 1 nm in radius. Each carries the model's horizontal outflow
  at 500 ft AGL at its spot, wherever that is at least 3 kt.
- **Why fixed:** the first in-sim test (13:48) used records that moved with the field. A moved record is a new one, and each new one
  restarted X-Plane's 2-minute fade, so the gust front reached the aircraft at about a third of its strength. Fixed cells only change
  their wind, which X-Plane updates in place.
- **Timing and strength:** cells are sent for the state `windshear_lead_s` (120 s) ahead, `windshear_gain` (1.4) times stronger.
- **Updates:** every 30 s, only for cells whose wind changed by 2 kt or 5°. The 80 cells nearest the aircraft within 40 nm are sent.
- **Re-sending and removal:** cells are re-sent with any of WXPlane's replacing updates. They are removed when windshear is turned off,
  when the plugin is disabled, or when they drop below 3 kt.

## Test event (menu: Windshear Test: event ahead now)

Each choice alternates between a microburst and a gust front. The event is placed ahead on the track, at the distance the aircraft
covers in ~150 s (3–12 nm), so it is faded in when the aircraft gets there. It is already mature, does not move, and lives its normal
lifecycle.
- **Microburst:** 30 kt outflow (42 kt sent), 3,300 fpm downdraft modelled only.
- **Gust front:** 25 kt headwind outflow.

It works with the windshear toggle on or off. Fly it below 3,000 ft AGL.

Encounter log:
- "ENTERS" when the model adds more than 3 kt;
- every 4 s, the model's headwind change next to the headwind X-Plane actually added (its wind against the wind at entry), with GS,
  IAS and VS;
- "EXITS", with both totals and the strongest (unapplied) downdraft.

## Categories (spec §19)

1. **Documented values (FAA AIM 7-1-24):** downdraft under ~1 statute mile; outflow ~2.5 statute miles; up to ~6,000 fpm and ~45 kt
   outflow (90 kt head-to-tail) in extreme cases; intensifies for ~5 min after reaching the ground, lasts up to ~15 min.
2. **Plausible physical approximations:** outflow profiles (linear to the ring, exponential beyond); full strength up to half the
   outflow depth, then tapering; gust front width ~1.5 nm; storm motion at 70 % of the 10,000 ft wind.
3. **Tunable assumptions, not measured rates:** all hourly probabilities; the intensity draws; gain and lead.
4. **Unsupported:**
   - downdrafts (no vertical wind in the API);
   - measured storm motion (single radar snapshot);
   - sea-breeze and terrain boundaries;
   - outflow boundaries identified in radar data (not available).

## Calibration (seeded runs, `tests/test_wx.cpp`)

- **Moderate storm (42 dBZ, TS), one source, 200 h:** 23 gust fronts, 1 microburst.
- **Intense storm (55 dBZ, +TSRA, gusts 35 kt), one source, 2,000 h:** 31 microbursts; 18 had 20+ kt outflow and 1 had 40+ kt.

These count fields that formed, not aircraft encounters. An aircraft meets a field only when its track crosses one.
