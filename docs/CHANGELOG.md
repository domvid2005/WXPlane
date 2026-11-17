# WXPlane changelog

## 5.1.3 (in progress)
- Map: every airport in the world with a current METAR (aviationweather.gov, every 10 minutes), as smaller dots by flight category; click for the METAR. Display only, never sent to X-Plane (world_metars)
- Map: in the US, the whole NOAA radar mosaic beyond the weather loaded into X-Plane (about 3 nm detail), display only
- Map: zoom out to 640, 1280 and 2500 nm
- Map beyond the weather loaded into X-Plane (display only): global-model winds aloft and cloud cover for the part of the map in view, pilot reports there, ECMWF turbulence for the whole world (1 degree), and Europe's radar (OPERA)
- AviTab tab: a WXPlane icon on AviTab's home screen (3D cockpit AviTab); tapped, the WXPlane map fills the tablet until AviTab's Home. Works alongside AviTab Browser (avitab_tab, avitab_icon_x / avitab_icon_y)
- Settings: Detailed log, off for a shorter Log.txt (main events, warnings and errors only) (detailed_log)

## 5.1.2
- Overcast (OVC) at a steady 97% coverage
- Rain at an airport: the airport's rain cloud is no longer cut to a thin sheet under the deck above it (it looked like fog, X-Plane dropped the overcast over it and no rain fell)
- Blend ring round the status dot: fills while X-Plane blends new weather in at the aircraft (shown only when the weather there is changing)
- Update notice: at start-up, a yellow notice in the window when a newer WXPlane is out on SkunkCrafts Updater, with a Dismiss button (update_check)
- Map: the local time under the cursor is right next to time-zone lines; Arizona (outside the Navajo Nation) and Saskatchewan, which keep standard time all year, are outlined in summer
- Turbulence add-on check: X-ProTurb (a FlyWithLua script) is now recognised; with XPRealistic, WXPlane's turbulence can run alongside it (Turbulence with XPRealistic, in Settings)
- Radar: the US radar download is retried after a short failure, and the rain-type check no longer flattens storms in an area just flown into
- Log.txt: when the blend settled at the aircraft, and whether X-Plane's read-back took the first load (it lags what X-Plane draws by 2-3 minutes)

## 5.1.1
- Cloud coverage varies within each METAR category – FEW 10–25%, SCT 35–50%, BKN 70–85%, OVC 97–100% (97% from 5.1.2) – leaning to the heavy end next to heavier sky and to the light end next to lighter or clear sky
- Cloud tops from the global model's humidity where it has cloud at a reported base (model_tops, on by default)
- Reported cloud bases kept exactly, also for decks reported close together; decks are never joined (with more decks than slots the lightest goes, never a storm)
- Clouds blend between neighbours only where the layers touch, start at the same height and are the same kind; no waves under the cloud, no layers out of order
- Overcast and broken decks next to an airport keep the airport's heights and coverage in the rain around it; radar squares over an airport are split finer
- Mist (BR) shown by the visibility alone, freeing a cloud slot; fog keeps its ground layer only near foggy airports
- Turbulence add-on check: the X-Crafts ERJ's EICAS no longer locks the turbulence settings on a Mac either
- Log.txt: a frame-rate check on every weather change (frame_check), and rain and model cloud cover at the aircraft

## 5.1
- Updates through SkunkCrafts Updater
- Turbulence tab: live EDR bar and 30 s g meter
- Windshear odds slider (0–1.0)
- One °T/°M and °C/°F setting for the whole window; dial, arrows, map rotation, heading lock, PIREP winds and storm bearings follow it
- Map tab: strongest cells merged within 3 nm, sortable, with a count; pilot report list beside the map; click either to fly the map there
- North/Heading Lock on the Winds & Temps and Airports tabs
- Softer storm edges, no cloud spires; smoother map radar
- Lists in whole rows with scroll bars and counts beside titles; popups never outside the map; altitude slider clear of the map; sharper logo; even tabs
- Aircraft plugins (e.g. ERJ EICAS) no longer lock turbulence settings
- Dim at night (Settings, on by default): the window dims with the daylight so the night cockpit no longer darkens while it is open
- Overcast (OVC) at 97% coverage, never thinned below 95% where it blends with its neighbours
- Broken (BKN) at 75–80% coverage (was 70%), varying from station to station
- Refresh and Pop out as tabs, with the status in a bubble above them
- All tabs stay up during the first download; a slow download shows "Still downloading… 0:47" instead of a frozen countdown
- Map popups keep off the map's buttons and can't be dragged onto each other ("max popups" fixed)
- North Lock: north is the north at the + when the map is moved off the aircraft (°M turns the map with the local variation); "Center On Aircraft"
- Pop-in opens a fresh window in the sim; sturdier METAR/ATIS reading
- Australia/New Zealand daylight saving time; rough-top clouds keep their deck's layers; many smaller fixes

## 5.0
- Winds aloft, clouds and time zones on the map, with an altitude slider (ACFT / SFC / COMP)
- Units per section, PIREPs of every type, draggable map popups
- Turbulence add-on detection (ActiveSky XP, xProTurb …), OPERA backup feed, search by FAA code, 320 nm zoom
