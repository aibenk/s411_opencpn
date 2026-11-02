# Test data notice

The plugin code is licensed under GPL v2 or later (see `LICENSE`). The files in
`tests/fixtures/` are **not** covered by that licence where they contain real
ice charts. They are included only so that the automated tests can check the
plugin against operational data.

| Fixture | Content | Source / producer |
|---|---|---|
| `411DE00EK1250220.GML`, `411DE00EIS250217.GML`, `v1_latlon.gml`, `v1_lonlat*.gml` | Baltic ice chart of 20 Feb 2025, ice report of 17 Feb 2025, Edition 1.x samples | Bundesamt für Seeschifffahrt und Hydrographie (BSH), `ftp.bsh.de/outgoing/eisbericht/` |
| `411US01AK260701.GML` | Alaska ice chart | NOAA National Weather Service, Alaska Sea Ice Program |
| `S411_NATICE_ArcticExtent_20260701.zip` | Arctic ice extent | U.S. National Ice Center |
| `TDS1*`, `sigrid3_*`, `circumpolar.gml`, `v1_epsg3031.gml`, `forecast.gml`, `prec_*.gml`, `nested.zip` | Synthetic test data | S-411 Edition 2.0.0 specification project / this repository |
| `expected_portrayal/` | Expected drawing instructions | S-411 Edition 2.0.0 specification (Annex G), CC BY 4.0 |

The real charts are converted or unchanged copies of publicly distributed data;
the rights remain with the producing ice services. They are outdated by design
and **not for navigation**. Using them for anything other than testing this
plugin requires the permission of the respective producer.

Questions: info@aibenk.com
