# WXPlane changelog

## 5.1.1
- Cloud coverage varies within each METAR category – FEW 10–25%, SCT 35–50%, BKN 70–85%, OVC 97–100% – leaning to the heavy end next to heavier sky and to the light end next to lighter or clear sky
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
