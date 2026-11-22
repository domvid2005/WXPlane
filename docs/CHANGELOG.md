# WXPlane changelog

## 5.1.5
- Clouds in rain and storms: no more square, slab-like clouds. Decks no longer end in vertical walls as high as the deck is deep, tops are no longer one flat level across the area, and the dry gaps between rain no longer open into square pits in solid cloud. A deck's top now comes down where it thins out, deep decks have uneven tops, and few or scattered clouds are no longer made as deep as the frontal cloud above them (deck_shape; 0 brings back 5.1.4's clouds)
- Altimeter at airports above sea level: with the reported QNH set, the altimeter reads the field elevation on a cold or hot day too. WXPlane sent the QNH as X-Plane's sea-level pressure; X-Plane works pressure out through the real temperatures, so the altimeter was 30-50 ft out at fields around 650 ft and hundreds of feet at high fields in cold or hot air (qnh_temp_correct)
- Tropopause: a front under a jet stream is no longer taken for the tropopause, and its height no longer steps by thousands of feet between neighbouring weather records across a jet stream; the temperature at the tropopause belongs to the height written (tropo_smooth)
- X-Plane's own variation and evolution of its base weather are held off while WXPlane sets the weather (put back when WXPlane is switched off), and Log.txt says when X-Plane changes its base temperatures by itself (xp_base_steady)
- METARs for other add-ons: WXPlane writes the world's current METARs into X-Plane's real-weather folder (Output/real weather) every 10 minutes, as X-Plane's own METAR file. With X-Plane's real weather off, which WXPlane needs, X-Plane stopped writing that file, and add-ons that read it (Little Navmap, Pilot2ATC and others) showed METARs days or weeks old. Not while X-Plane's own real weather is on (xp_metar_file)
- European radar: steady rain and showers told apart from the reflectivity, as the US radar does, so steady rain is no longer built as convective cells; more, smaller squares for widespread rain (radar_natural_max 400)

## 5.1.4
- XPTablet: with Alpine Aviation's XPTablet installed, the WXPlane window fills the tablet's WXPlane page, with all its tabs, touch and scrolling (no Pop out tab there). XPTablet asks for it through a plugin message (embed.h)
- AviTab tab: only with AviTab itself, never on XPTablet's screen; its full-screen click window is there only while the mouse points at the tablet, so X-Plane's own pop-up messages can be dismissed again
- Overview: in a narrow window the °C / °F switch stands after the temperature instead of being cut off

## 5.1.3
- Map, worldwide: every airport in the world with a current METAR, as smaller dots by flight category; click one for its METAR (aviationweather.gov, every 10 minutes; world_metars)
- Map beyond the weather loaded into X-Plane, for the part in view: global-model winds aloft (Winds), cloud cover as grey shading (Clouds), pilot reports (PIREPs), ECMWF turbulence for the whole world at 1 degree (Turb), the whole US radar mosaic and Europe's radar (OPERA). Display only: none of it is sent to X-Plane, so the sim's frame rate is not affected
- Map: zoom out to 640, 1280 and 2500 nm
- AviTab tab: a WXPlane icon on AviTab's home screen (AviTab in the 3D cockpit); tapped, the WXPlane map fills the tablet, with its layers, range, altitude and info windows, until AviTab's Home. Works alongside AviTab Browser, whatever order X-Plane draws the add-ons in. Only with AviTab itself: no icon when AviTab is not running, or while XPTablet runs (avitab_tab, avitab_icon_x / avitab_icon_y)
- Settings: Detailed log. Off, Log.txt gets only WXPlane's main events, warnings and errors (detailed_log)
- Blend ring: measures against the weather sent over the aircraft (rain counts), judges the sky as a whole rather than layer by layer, and compares the sea-level pressure correctly (it stopped at 1-5 % under rain); shown only while the weather at the aircraft is changing
- Rain at an airport (from 5.1.2) and the first-load check: no second send of the first load (X-Plane's read-back lags what it draws by 2-3 minutes); Log.txt notes after 4 minutes whether it caught up

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
