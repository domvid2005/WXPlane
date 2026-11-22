# WXPlane investigation, 2026-11-21: "square clouds" over Europe in storms

Report (2026-11-19, three screenshots): flat-topped slabs with vertical cliffs, a flat vertical wall, a huge mass ending in a squared
wall. Question on 2026-11-21: did the European radar change of 2026-11-20 (precipitation type from the reflectivity, model tops varied,
finer squares) fix it?

**No.** Measured with the harness scenario `SQUARES` (a rain band through Madrid in the European composite, five fields with different
skies, the model's cloud 5 km deep in the band), on 5.1.4 and on the radar change alike. What goes to X-Plane, by column (whatever slot
a layer sits in):

| Rain band, cloud all around | `deck_shape = 0` (5.1.4, radar change) | `deck_shape = 1` (5.1.5) |
|---|---|---|
| Solid cloud beside a touching record with no cloud at all at its heights, 2,000 m or more | 7 records, each 5,084 m | 0 (highest 454 m) |
| Solid cloud with a stretch of 2,000 m or more open beside it (the stricter measure) | 46 records, 34 of them within 12 nm | 20, 6 within 12 nm |
| ... 3,500 m or more | 7 | 1 |
| A solid top more than 650 m per nm above the neighbour's cloud | 9 | 0 |
| Touching records of solid cloud with exactly the same top | 69 % | 4 % |
| Thin layers (10–60 %) 3,000 m deep or more in records without rain | 45 of 125 | 14 of 122 |
| Solid cloud, median depth | 5,084 m | 4,901 m |

| The same band, clear sky reported on both sides (`SQUARES_EDGE`) | `deck_shape = 0` | `deck_shape = 1` |
|---|---|---|
| No cloud at all beside, 2,000 m or more | 11 records, each 5,084 m | 4 (2,551 to 4,433 m) |
| A stretch of 2,000 m or more open beside it | 62, 34 within 12 nm | 39, 8 within 12 nm |
| ... 3,500 m or more | 11 | 1 |
| Thin layers 3,000 m deep or more | 22 of 98 | 7 of 100 |

5,084 m is the largest depth X-Plane allows a cumulus-type layer. **Nothing of this has been seen in the simulator**; the numbers are
what WXPlane hands over, not what X-Plane draws.

## Causes (all in 5.1.4 already)

1. **Pits and squared ends.** A rain record carries its rain cloud in the lowest cloud slot and the station's deck one slot up; the dry
   record beside it has that deck in the lowest slot. The cloud smoothing compared slot against slot, found none of the deck in the rain
   records around, and thinned it: 85 % reported, 8–25 % in the dry squares between and beside the rain. Each was a hole in solid cloud
   with walls as high as the deck is deep, in the shape of the radar's squares (3 to 24 nm).
2. **Level lids.** A reported deck reaches the model's cloud top (2026-11-13), limited to X-Plane's largest depth for the type. Under a
   deep model cloud every record on one base came out at base + 5,084 m: one flat top across the whole area.
3. **Towers.** The same rule gave a few or scattered clouds the whole depth: 40 % cover, 4 km deep.
4. **Steps between two fields.** The no-spires rule (2026-11-12) holds a top to 650 m per nm above the neighbour's layer *in the same
   slot*. Madrid's layer in that slot was its lower broken deck (top 1,678 m) with its overcast to 6,700 m right above it in the next
   slot; the decks of the fields around were cut to 1,678 + 650 × 2.98 nm = 3,615 m, a step of 2,500 m in the cloud top 3 nm from the
   airport, inside solid rain cloud.
5. **Ends far out.** Whether the cloud ends beside a record was judged over the smoothing's reach (8 to 20 nm); the records far out are
   10 to 30 nm wide, so the last record of an overcast saw almost only itself and kept the whole depth.

## Changes (`deck_shape`, default on; `deck_shape = 0` gives the 5.1.4 picture back, checked by `SQUARES_OFF`)

- `matchEmptyLayers` (cloud smoothing): the same deck in another slot of a neighbouring record counts as that deck (1).
- Uneven tops: every deep layer (1,500 m and more) up to a tenth of its depth lower, by smooth noise fixed to the ground, about 12 nm (2).
- `buildXp`: FEW at most 1,200 m and SCT 1,800 m deep under a model cloud; BKN and OVC keep the model's depth (3).
- No spires, by column: where another layer of the neighbour stands on its layer in this slot (starts within 400 m of that top, about
  as dense), the top is held against that one's (4). It only ever allows more height than before, never less.
- Edge: a layer gets shallower where the records around have little cloud at its heights (any slot, any base), judged over the records
  that touch this one (1.3 of its radius where that is more than the smoothing's reach) (5). Inside a wide overcast reported on
  different bases nothing changes. Records with rain are left to the radar (a thin rain cloud stops the rain in X-Plane).
- Thunderstorm cores and towering cumulus keep the radar's tops throughout.

Tried and dropped: counting a neighbour by the distance to its edge instead of its centre. It lowered the highest end far out
(4,433 to 3,338 m) but made more records step against their neighbours (39 to 50 with 2,000 m open).

## What is left

- The 6 records within 12 nm with 2,000 to 2,500 m open beside them stand where two fields report it: a deck from 1,400 m beside a
  field with "few at 4,000 ft, broken at 10,000 ft". The low cloud ends between them because the reports say so; bases are never moved.
- Far out with clear sky beside the band, the last overcast record is now 2,500 to 3,000 m deep instead of 4,800 m (one of 4,433 m is
  left, touching the clear record only at a corner). A record 20 to 40 nm wide cannot slope within itself.
- Three cloud slots: where a record has three layers and its neighbour has other ones, X-Plane blends unlike layers; not changed.

## Later on 2026-11-21: the KRCX screenshot (US radar, 5.1.4)

A flight log and screenshot from Ladysmith, WI (KRCX) in steady rain (NOAA MRMS, 202 rain records, 35 dBZ at most): a deck about
4-5 km deep ending in vertical faces with clear ground beside them, several faces stepped one behind the other. Every rain record
carried the station's overcast, 97 % from 800 to 4,956 m (the model's top, cut at X-Plane's cumulus maximum), with the same top.

The measures above count layer against layer and could not see what X-Plane does between records: it blends cover, base and top slot
by slot. A new measure (`[squares] ends`) follows that blend from a record with a solid layer in a slot to a touching record with
under 25 % in that slot, and takes the height of the cloud where its cover falls through 40 %.

| Records where X-Plane's blend ends a deck with a face of 2,000 m+ / 3,000 m+ | `deck_shape = 0` | pushed build (f09d604) | now |
|---|---|---|---|
| Rain band, cloud all around (`SQUARES`) | 107 / 35 | 76 / 5 | 20 / 3 |
| Clear sky beside the band (`SQUARES_EDGE`) | 112 / 33 | 83 / 10 | 33 / 4 |

Three causes, all changed under `deck_shape`:

1. **One deck in two slots.** A rain record carries the rain cloud and the station's deck as two layers on nearly the same base; the
   deck went up a slot while the dry record beside it had it in the lowest. Nine in ten of the places where the blend ended a deck
   beside a rain record were this. Overlapping layers of one height band are now merged into one before the slots are given out: the
   denser one's base (the deck the neighbours share: with the rain cloud's lower base the dry records beside no longer took it for
   their deck and thinned to a fifth), the highest top, the most cover, the most convective type. Ground fog and cirrus are never
   merged; station layers never overlap.
2. **Placeholders with the deck's full top.** A slot with no cloud gets an invisible 1 % layer at the height of the nearest deck, so
   X-Plane keeps the base level (KBOS cones, 2026-11-12). It took the deck's top as well: towards clear sky the cover fell under a level
   lid and the deck ended in a face as high as it is deep. The placeholder now keeps the base and has a low top (12 % of the depth,
   at least 450 m; thinner ones once made X-Plane drop an update), so the top comes down as the cover falls. This is the KRCX picture
   (an overcast running out into clear sky); the rig's band has few fully clear records beside it, so it shows little of this one.
3. **Deep thin fringes.** A 5-25 % fringe of a deck kept most of the deck's depth. With solid cloud (60 %) of that deck near, a
   fringe under half cover now gets a depth in proportion to its cover (a fifth at 10 %, at least 300 m).

Not seen in the simulator. What is left in the rig is mostly a low deck ending under a mid-level deck that carries on above it.
