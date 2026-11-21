# WXPlane investigation, 2026-11-21: "square clouds" over Europe in storms

Report (2026-11-19, three screenshots): flat-topped slabs with vertical cliffs, a flat vertical wall, a huge mass ending in a squared
wall. Question on 2026-11-21: did the European radar change of 2026-11-20 (precipitation type from the reflectivity, model tops varied,
finer squares) fix it?

**No.** Measured with the harness scenario `SQUARES` (a rain band through Madrid in the European composite, five fields with different
skies, the model's cloud 5 km deep in the band), on 5.1.4 and on the radar change alike. What goes to X-Plane, by column:

| | 5.1.4 / radar change (`deck_shape = 0`) | 5.1.5 (`deck_shape = 1`) |
|---|---|---|
| Solid cloud beside a touching record with no cloud at its heights, 2,000 m high or more | 7 records, each 5,084 m | 0 (highest 454 m) |
| Touching records of solid cloud with exactly the same top | 69 % | 3 % |
| Thin layers (10–60 %) 3,000 m deep or more in records without rain | 45 of 125 | 14 of 122 |
| Solid cloud, median depth | 5,084 m | 4,879 m |

5,084 m is the largest depth X-Plane allows a cumulus-type layer. **Nothing of this has been seen in the simulator**; the numbers are
what WXPlane hands over, not what X-Plane draws.

## Causes (all three in 5.1.4 already)

1. **Pits and squared ends.** A rain record carries its rain cloud in the lowest cloud slot and the station's deck one slot up; the dry
   record beside it has that deck in the lowest slot. The cloud smoothing compared slot against slot, found none of the deck in the rain
   records around, and thinned it: 85 % reported, 8–25 % in the dry squares between and beside the rain. Each was a hole in solid cloud
   with walls as high as the deck is deep, in the shape of the radar's squares (3 to 24 nm).
2. **Level lids.** A reported deck reaches the model's cloud top (2026-11-13), limited to X-Plane's largest depth for the type. Under a
   deep model cloud every record on one base came out at base + 5,084 m: one flat top across the whole area.
3. **Towers.** The same rule gave a few or scattered clouds the whole depth: 40 % cover, 4 km deep.

## Changes (`deck_shape`, default on; `deck_shape = 0` gives the 5.1.4 picture back, checked by `SQUARES_OFF`)

- `matchEmptyLayers` (cloud smoothing): the same deck in another slot of a neighbouring record counts as that deck.
- `buildXp`: FEW at most 1,200 m and SCT 1,800 m deep under a model cloud; BKN and OVC keep the model's depth.
- Uneven tops: every deep layer (1,500 m and more) up to a tenth of its depth lower, by smooth noise fixed to the ground (about 12 nm).
- Edge: a layer gets shallower where the records around have little cloud at its heights (any slot, any base); inside a wide overcast
  reported on different bases nothing changes. Records with rain are left to the radar (a thin rain cloud stops the rain in X-Plane).
- Thunderstorm cores and towering cumulus keep the radar's tops throughout.

## What is left

- With clear sky reported on both sides of the band (`SQUARES_EDGE`), 5 records far out still end 2,000 m high or more (11 before; now up to
  4,849 m). The records there are 10 to 30 nm wide and the smoothing reaches 8 nm, so the deck keeps its depth up to the last record
  that reports overcast.
- Two neighbouring stations with different decks (an overcast to 6,700 m beside a broken deck to 3,600 m, 3 nm apart) still step by up
  to 860 m per nm in 9 of 2,800 comparisons, as before.
- Three cloud slots: where a record has three layers and its neighbour has other ones, X-Plane blends unlike layers; not changed.
