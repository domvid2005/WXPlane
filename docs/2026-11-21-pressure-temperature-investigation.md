# WXPlane investigation, 2026-11-21: altimeter setting, temperatures over the ocean, tropopause

Reports: (1) a customer over the North Pacific (forum, ToLiss, WXPlane 5.1.4): sim SAT −36 °C / ISA+19 at FL364 where WXPlane's own
table for the spot (GFS point N42 E162) had −49 °C at FL360; (2) customers "still reporting erroneous pressure readings and altitude
jumps"; (3) a customer log and two screenshots from Wichita (KICT), "clouds look like older X-Plane versions".

Evidence available: the KICT customer's Log.txt (a 30-minute ground session), the forum screenshot, and the weather-watch lines of
every earlier log in this project (694 pressure read-backs, 547 of them settled). **No log of the Pacific flight.**
Each item is marked **confirmed** (measured in logs), **worked out** (follows from the code, shown in a test) or **not proven**.

## 1. Altimeter setting at fields above sea level — confirmed

X-Plane works the pressure at any height out from the sea-level pressure through the real temperature column. In 547 settled
read-backs, X-Plane's altimeter setting for the aircraft's height (the `pressure_alt` of `XPLMGetWeatherAtLocation`) leaves its
sea-level pressure by about 1 to 1.6 hPa per 1,000 ft per 10 °C off standard (theory: 1.28). A hydrostatic model of the same column
reproduces X-Plane's value within 0.5 hPa on average (1.2 at worst) below 5,000 ft.

A QNH is the station pressure brought down to sea level through the *standard* column. Since 2026-11-04 WXPlane sent the QNH as
X-Plane's sea-level pressure. On the ground, with the reported QNH set:

| Field | Elevation | Air | X-Plane's setting minus the report | Altimeter |
|---|---|---|---|---|
| KDTW | about 650 ft | 7 °C colder than standard at FL050 | −1.4 hPa (62 samples) | 38 ft high |
| ENGM | about 650 ft | 10 °C colder | −1.7 hPa (10 samples) | 46 ft high |
| KORD | about 650 ft | 8 °C colder | −1.3 hPa (14 samples) | 35 ft high |
| KDAB, KBOS | sea level | 5–10 °C warmer | 0.2–0.4 hPa | right |

It grows with the elevation: for Denver (1,655 m) 20 °C colder than standard the same model gives 15.6 hPa, 430 ft. X-Plane's own
ATIS works its altimeter setting out the same way, so it disagreed with the real one by the same amount.

**Fix (5.1.5):** `wx::seaLevelHpaForQnh` gives the sea-level pressure that puts the QNH's station pressure at the field, through the
temperatures the record itself carries (the levels X-Plane will hold). `smoothPressure` builds its smooth field from those values
(they differ far less between a valley and a mountain station than the QNHs do); a station's own record keeps its own value within
2 hPa of the field, as before. Records keep the QNH in `pressure_alt` (logs, windows) and carry the pressure to send in `pressure_sl`.
Over water (elevation 0) nothing changes. `qnh_temp_correct = 0` goes back. Tests: `test_wx` (standard air, Denver cold and hot,
back up through the column, below sea level) and the harness scenario `HIGHFIELD` (four fields near Denver: the altimeter setting
needed at each field comes out within 1 ft of the reported one, computed the way X-Plane does it).

Assumption to confirm in the sim: X-Plane integrates through the temperature levels of the record, below the field's elevation too
(the dataref description says it fills those with the surface's difference from standard when it builds them itself). The
weather-watch line now says on the ground: "with the reported QNH the altimeter reads N ft against the field".

## 2. Temperatures over the ocean — not proven

What is known: WXPlane's table for the customer's spot is the profile of the record sent for it (the same `buildXp`), and −36 °C at
FL364 is not on that profile anywhere near that height, nor on a blend of it with a neighbouring one. So X-Plane was not using
WXPlane's temperatures at the aircraft. ISA+19 is what a standard atmosphere moved by a 34 °C sea-level temperature gives.

- At cruise the temperatures come from X-Plane's region ("base") weather, which WXPlane writes every 20 s
  (`pushRegionWinds`; before that was added, X-Plane "drifted back to its own" a few miles from an airport record, log 2026-11-04).
  Over water there is no airport record at all, so everything rests on the base.
- X-Plane changes the base by itself (Laminar, "Weather Datarefs in X-Plane 12"): it "varies with distance from a reference point",
  it has deliberate variation (`sim/weather/region/variability_pct`, "Variation across region") and a trend over time (`change_mode`,
  "Evolution over time"). WXPlane did not touch either. Every log so far shows `change_mode` 3 (static); the variation was never logged.
- **Possible cause, not proven:** the customer's X-Plane varying or drifting its base away from what WXPlane wrote, far from the
  departure airport.

**Changes (5.1.5):**
- `baseSteady`: while WXPlane sets the weather and X-Plane's real weather is off, variation 0 and evolution static; the user's values
  are put back when WXPlane is switched off. `xp_base_steady = 0` leaves them alone.
- The log line "X-Plane weather source …" now shows the variation as well.
- `pushRegionWinds` reads the region temperatures back before writing: if X-Plane changed them by itself since the last write, the
  log says by how much ("X-Plane changed its base temperatures by itself …").

**Needed to settle it:** the Log.txt of such a flight (the weather-watch lines print WXPlane's and X-Plane's temperatures at seven
levels and both tropopauses), and the customer's X-Plane weather settings.

## 3. Tropopause — worked out

- `findTropopause` took a front under a jet stream for the tropopause: with the 500–400 hPa layer of the customer's profile 3 °C more
  stable (2.7 °C/km as it was) it returned FL175 at −23 °C. Now the 4 km above a candidate must stay at 2.5 °C/km or less, and a
  candidate warmer than −38 °C is passed over (`test_wx`).
- X-Plane holds the temperature at the tropopause height to the tropopause temperature (log 2026-11-10) and blends height and
  temperature of neighbouring records separately. Across a jet the tropopause steps from about FL285 to FL455 between neighbouring
  records (3,239 m within 15 nm in the harness scenario `JET`); the blend is on neither profile. Now the height is a smooth field
  (`smoothTropopause`, 442 m in the same scenario) and each record's temperature there is its own. The region tropopause is written
  with the profile's temperature at the height written (height and temperature used to move separately). `tropo_smooth = 0` goes back.

Whether either of these was behind the customer's −36 °C is **not proven**; both removed a temperature error near the tropopause.

## 4. The KICT clouds — from the log

- WXPlane's weather at KICT in that session: few cloud at 5,000 m (cumulus type, 12 %), scattered at 7,134 m (cirrus type, 35 %), no
  low cloud. X-Plane's read-back at the aircraft matched it.
- X-Plane's own real weather was switched **on** twice in the session (0:07:37–0:09 and 0:12:05–0:15:25 of 0:29:42), each time through
  X-Plane's weather settings. While it is on X-Plane blends its own clouds with WXPlane's, and each switch makes it rebuild them. The
  model X-Plane uses had 41 % cloud at 3,400 ft there; the low dark deck in the screenshots is not in WXPlane's records for KICT.
- Which state the screenshots show cannot be told from the log. Nothing was changed in the cloud code for this.
