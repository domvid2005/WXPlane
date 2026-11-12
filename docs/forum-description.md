# WXPlane 5.0 – live real-world weather for X-Plane 12

**WXPlane replaces X-Plane 12's weather with live, real-world data.** It reads airport reports, winds aloft, radar, turbulence and pilot reports, and keeps the weather around you current every 5 minutes, all the way along your route. Version 5.0 puts the winds, clouds and turbulence aloft on the map, at any altitude you choose.

Free. No accounts and no API keys. Windows, macOS and Linux.

## What it does

- **Airport weather (METAR + ATIS):** the 40 nearest reports within 120 nm set the wind, visibility, fog and mist, clouds, rain and snow, temperature and pressure. The weather blends smoothly between airports, with no gaps. D-ATIS is merged in at major US airports.
- **Winds and temperatures aloft, worldwide:** GFS model data from the surface to FL600 replaces X-Plane's own, with the tropopause in the right place.
- **Clouds the airports can't see:** cirrus and mid-level decks from the GFS model, plus weather over open water where there is no airport.
- **Real radar storms:** NOAA MRMS over the US and the EUMETNET OPERA composite over Europe, updated every 5 minutes. Rain, showers and thunderstorms appear where the radar shows them, with realistic tops.
- **Realistic turbulence:** clear-air and storm turbulence at every altitude, from NOAA GTGN in the US, an ECMWF-based estimate elsewhere, and live pilot reports. A Max turbulence slider scales it to taste.
- **Realistic windshear:** gust fronts and microburst outflow around real convective storms.
- **Smooth:** downloads run in the background and every update fades in over 1–2 minutes. No freezes, no sudden jumps.

## New in 5.0

- **Winds aloft on the map:** wind barbs (blue, deeper the stronger) at any level; click one to decode it, with the headwind and crosswind on your heading.
- **Clouds on the map:** the cloud layers WXPlane gives X-Plane, darker grey the deeper the layer.
- **Altitude slider** beside the map for winds, clouds and turbulence: ACFT follows your aircraft, SFC shows the surface, COMP overlays all altitudes. It snaps to the nearest 1,000 ft, more firmly at 5,000 and 10,000 ft (which light up amber).
- **Time zones:** a Time Zone layer draws the zone boundaries. The map's LOCAL time follows the + at the map centre when you pan away from the aircraft. Zulu, local time, sunrise and sunset are shown for every airport.
- **Units per section:** °T / °M and °C / °F switches for the airports, the map and the winds aloft. METAR and ATIS can be toggled on the home page, with magnetic winds for performance calculations.
- **Pilot reports:** every PIREP type on the home page and the Turbulence tab, sortable, with details on click.
- **Map popups:** as many as fit, never overlapping and never leaving the map; they can be dragged.
- **Turbulence add-on detection:** with ActiveSky XP, xProTurb or similar running, WXPlane leaves the turbulence to it and says so.
- **Search by FAA code:** small US fields such as 35D or 1B9 work without the K.
- Zoom out to 320 nm, European radar backup feed, and many smaller fixes.

## The weather window

Open it from **Plugins → WXPlane**, or bind it to a key or joystick button. It can pop out to a second monitor and works in VR.

- **Overview:** the nearest airport decoded, with a wind dial. Click the dial to switch between North Lock and Airplane Heading Lock, which shows the headwind/tailwind and crosswind components. Also shows nearby airports, a mini map, winds and temperatures aloft, and turbulence and windshear at your aircraft.
- **Map:** zoom smoothly from 1 to 320 nm with the mouse wheel, drag to pan, and re-centre on the aircraft. Layers:
  - live radar
  - built-in coastlines and borders
  - airports by flight category (click one for its METAR and ATIS)
  - a turbulence grid, wind barbs and clouds at the slider's altitude
  - time zone boundaries
  - PIREPs (UUA red, UA orange, click one for details)
  - colour-coded AIRMETs, SIGMETs and convective SIGMETs (click one for details)
- **Airports:** worldwide METAR/ATIS search, plus every station in use, sortable by any column.
- **Winds & Temps:** the full profile up to FL600 with ISA deviation.
- **Turbulence:** EDR by altitude and every nearby pilot report.
- **Services:** the live status of every data source.
- **Settings:** every option in one place.

## Requirements

- X-Plane 12.3 or newer.
- Internet connection.
- **Turn off X-Plane's own real-world weather.** WXPlane warns you if it is on.

## Installation

Copy the `WXPlane` folder into `X-Plane 12/Resources/plugins/` and restart X-Plane. Delete any older WXPlane folder first. A full user manual (`WXPlane_Manual.txt`, next to the plugin in its platform folder) is in the download.

## Credits

**Weather data**
- NOAA / National Weather Service – Aviation Weather Center: METARs, pilot reports, G-AIRMETs, SIGMETs and convective SIGMETs (public domain)
- NOAA MRMS radar (public domain)
- NOAA GTGN turbulence – Graphical Turbulence Guidance by NCAR for the FAA (public domain)
- Open-Meteo.com – NOAA GFS model data (CC BY 4.0)
- EUMETNET OPERA radar composite – Open Radar Data (CC BY 4.0)
- ECMWF Open Data (CC BY 4.0)
- D-ATIS by clowd.io

**Map:** Natural Earth (public domain)

**Software**
- Dear ImGui (MIT)
- IBM Plex Sans and JetBrains Mono (SIL Open Font License)
- libcurl, zlib, OpenSSL
- OpenJPEG and libaec
- X-Plane SDK by Laminar Research

Full licence texts are in `LICENSES.txt` in the download. WXPlane is not affiliated with NOAA, EUMETNET, ECMWF, Open-Meteo, clowd.io or Laminar Research. It is for flight simulation only and must not be used for real-world flight planning.

**Source and issues:** https://github.com/domvid2005/WXPlane. When reporting a problem, please attach `X-Plane 12/Log.txt`.
