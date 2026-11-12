# WXPlane 4.0 – live real-world weather for X-Plane 12

**WXPlane replaces X-Plane 12's weather with live, real-world data.** It reads airport reports, winds aloft, radar, turbulence and pilot reports, and keeps the weather around you current every 5 minutes, all the way along your route. Version 4.0 adds a full weather window inside the sim.

Free. No accounts and no API keys. Windows, macOS and Linux.

## What it does

- **Airport weather (METAR + ATIS):** the 40 nearest reports within 120 nm set the wind, visibility, fog and mist, clouds, rain and snow, temperature and pressure. The weather blends smoothly between airports, with no gaps. D-ATIS is merged in at major US airports.
- **Winds and temperatures aloft, worldwide:** GFS model data from the surface to FL600 replaces X-Plane's own, with the tropopause in the right place.
- **Clouds the airports can't see:** cirrus and mid-level decks from the GFS model, plus weather over open water where there is no airport.
- **Real radar storms:** NOAA MRMS over the US and the EUMETNET OPERA composite over Europe, updated every 5 minutes. Rain, showers and thunderstorms appear where the radar shows them, with realistic tops.
- **Realistic turbulence:** clear-air and storm turbulence at every altitude, from NOAA GTGN in the US, an ECMWF-based estimate elsewhere, and live pilot reports. A Max turbulence slider scales it to taste.
- **Realistic windshear:** gust fronts and microburst outflow around real convective storms.
- **Smooth:** downloads run in the background and every update fades in over 1–2 minutes. No freezes, no sudden jumps.

## New in 4.0: the weather window

Open it from **Plugins → WXPlane**, or bind it to a key or joystick button. It can pop out to a second monitor and works in VR.

- **Overview:** the nearest airport decoded, with a wind dial. Click the dial to switch between North Lock and Airplane Heading Lock, which shows the headwind/tailwind and crosswind components. Also shows nearby airports, a mini map, winds and temperatures aloft, and turbulence and windshear at your aircraft.
- **Map:** zoom smoothly from 1 to 160 nm with the mouse wheel, drag to pan, and re-centre on the aircraft. Layers:
  - live radar
  - built-in coastlines and borders
  - airports by flight category (click one for its METAR and ATIS)
  - a turbulence grid
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
