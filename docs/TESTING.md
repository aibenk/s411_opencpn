# s411_pi testen — Anleitung

Diese Anleitung beschreibt drei Stufen: **(1)** die automatischen Parser-Tests
(ohne OpenCPN), **(2)** den Praxistest im laufenden OpenCPN unter Linux/WSL —
der schnellste Weg zum echten Hands-on — und **(3)** den nativen
Windows-Build. Dazu eine Test-Checkliste mit Soll-Ergebnissen.

---

## 1. Automatische Tests (ohne OpenCPN, 2 Minuten)

Die Lade-Pipeline (GML-Parser, Exchange-Sets, Zips, Antimeridian,
Triangulierung) ist headless testbar — dieselben Tests laufen bei jedem Push
im GitHub-CI.

**Linux / WSL:**

```sh
sudo apt install cmake g++ libwxgtk3.2-dev libglu1-mesa-dev
git clone https://github.com/aibenk/s411_opencpn
cd s411_opencpn
cmake -B build -DCMAKE_BUILD_TYPE=Release -DS411_BUILD_TESTS=ON
cmake --build build --parallel
./build/s411_test tests/fixtures
```

**Soll:** `ALL TESTS PASSED (0 failures)` — 53 Checks gegen echte Daten:
BSH-Ostseekarte mit 337 Features, NWS-Alaska über 180°, Eisbericht-Stationen,
Exchange-Set als Verzeichnis und als Zip, **Achsen-Erkennung** (lat/lon vs.
lon/lat, 0–360°-Längen, CRS84-`srsName` für den ambigen Ostsee-Fall),
Hit-Test fürs Feature-Picking, Attribut-Erfassung, Code-Labels und die
Ampel-Semantik (identische Tabellen wie Web-Viewer, `rules/main.xsl` und
`tools/validate.py` im Spez-Repo — dort per CI gegengeprüft).

**Windows ohne CMake:** `testbuild\s411_test.exe tests\fixtures` (wird von
den Build-Schritten in diesem Repo mitgepflegt).

---

## 2. Praxistest in OpenCPN (empfohlen: Ubuntu in WSL2)

Windows 11 mit WSL2 hat GUI-Unterstützung eingebaut (WSLg) — OpenCPN läuft
darin als normales Fenster. Das umgeht den mühsamen nativen Windows-Build.

### 2.1 Einrichtung (einmalig)

```powershell
wsl --install -d Ubuntu-24.04     # falls noch kein WSL vorhanden; danach neu starten
```

In der Ubuntu-Shell:

```sh
sudo apt update
sudo apt install -y opencpn cmake g++ libwxgtk3.2-dev libglu1-mesa-dev git
```

> Falls die Paketquelle kein OpenCPN hat: `sudo add-apt-repository ppa:opencpn/opencpn && sudo apt update && sudo apt install opencpn`

### 2.2 Plugin bauen und installieren

```sh
git clone https://github.com/aibenk/s411_opencpn
cd s411_opencpn
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Plugin-Verzeichnis der OpenCPN-Installation finden:
ls -d /usr/lib/opencpn /usr/lib/*/opencpn 2>/dev/null
sudo cp build/libs411_pi.so /usr/lib/opencpn/        # ggf. Pfad anpassen
```

> **Wichtig:** Das Plugin muss gegen dieselbe wxWidgets-Version gebaut sein
> wie OpenCPN selbst. Bei „apt-OpenCPN + apt-wxWidgets" passt das automatisch.
> Lädt OpenCPN das Plugin nicht (bleibt in der Plugin-Liste unsichtbar),
> `opencpn` einmal im Terminal starten und die Meldungen lesen.

### 2.3 Aktivieren und bedienen

1. OpenCPN starten (`opencpn`).
2. *Options → Plugins* → **S-411 Ice** ankreuzen → *OK*.
3. In der Werkzeugleiste erscheint der **411**-Button. Ein Klick öffnet nur den
   **Steuerdialog** (es wird **nichts automatisch geladen**) — wie das linke
   Panel im Web-Viewer:
   - **Show ice overlay** – ein/aus (Daten bleiben geladen)
   - **Load data… / Clear** – Datensätze hinzufügen / alle entfernen. Nach dem
     Laden **springt die Karte automatisch** zu den neuen Daten.
   - **Datensatzliste** – Checkbox = Sichtbarkeit pro Datensatz; *Zoom to
     data* (markierter Datensatz, ohne Markierung: alle sichtbaren);
     *Remove* entfernt den markierten einzeln
   - **Display mode** – *Ice concentration* / *Stage of development* /
     *Navigational (ice-class traffic light)*
   - **Vessel ice class** – nur im Navigational-Modus aktiv (No class …
     IA Super)
   - **Colour palette** – *Day* / *Dusk* / *Night*
   - **Fill opacity** – Deckkraft der Flächenfüllungen
   - **Legende** – passt sich Modus und Palette an
   Modus, Eisklasse, Palette und Deckkraft werden gespeichert und beim
   nächsten Start wiederhergestellt. Denselben Dialog gibt es unter
   *Options → Plugins → S-411 Ice → Preferences*.
4. **Feature-Report:** kurzer Klick (ohne Ziehen, < 4 px Mausbewegung) auf
   ein Eis-Feature öffnet ein Fenster mit allen Attributen; Konzentrations-
   und Entwicklungsstadium-Codes werden im Klartext decodiert
   (z. B. `totalConcentration = 92 (10/10)`). Pannen/Zoomen bleibt
   unverändert; Klick auf freie See tut nichts.

### 2.4 Testdaten beschaffen

Alle Testdatensätze liegen im Portal (Login lt. `.env` des seaice-Projekts):
**https://31-70-111-123.sslip.io/testdata/** — die Zips dort kann das Plugin
**direkt laden**, ohne Entpacken. Sinnvolle Auswahl:

| Download | Testet |
|---|---|
| `TDS4_RealData_Baltic.zip` | echte BSH-Karte, 337 Features, alle Farben |
| `TDS8_RealData_International.zip` | 5 Produzenten, inkl. **NWS Alaska** (Antimeridian!) |
| `TDS1_BalticSea_IceChart.zip` | kleiner synthetischer Satz (schneller Smoke-Test) |
| `TDS6_IceReport_GermanCoast.zip` | Punkt-Features (Eisbericht-Stationen) |

Alternativ liegen dieselben Daten im seaice-Repo unter
`S-411_Ed_2.0.0/Annex_G_TestData/` (aus WSL erreichbar unter
`/mnt/c/Users/alexa/VSCode Projects/seaice/...`) sowie zwei Sätze als
Fixtures direkt in diesem Repo (`tests/fixtures/`).

Für den **Edition-1.x-Test**: einen aktuellen Winter-Satz vom BSH-FTP laden —
`ftp://ftp.bsh.de/outgoing/eisbericht/S411/Winter2025/` (anonymous) — und das
Zip unverändert ins Plugin geben.

---

## 3. Test-Checkliste (mit Soll-Ergebnissen)

Der 411-Button öffnet den Steuerdialog; geladen wird ausschließlich über
*Load data…* dort. Zuerst der wichtigste Test — **Georeferenzierung** (der
historische Fehlerfall „Ostsee-Eis im Indischen Ozean" war eine erzwungene
lon/lat-Interpretation für Edition-1.x-Dateien):

| # | Test | Soll |
|---|---|---|
| G1 | BSH-Winter-Zip laden (`reference/real_data/S411_ek1-20250220.zip` im seaice-Repo oder frisch vom BSH-FTP) | Karte springt automatisch zur Ostsee; Eis liegt im Bottnischen/Finnischen Meerbusen (~63°N/21°E) — **nicht** am Persischen Golf/Indischen Ozean |
| G2 | `tests/fixtures/v1_lonlat_crs84.gml` laden | Fläche im Bottnischen Meerbusen (63–64°N, 21–22°E) — lon/lat-Datei, erkennbar **nur** am CRS84-`srsName` (beide Achsen ≤ 90°) |
| G3 | `tests/fixtures/v1_lonlat.gml` laden | Fläche in der Antarktis (~64°S, 56°W) — lon/lat mit 0–360°-Längen, erkannt und normalisiert |
| G4 | NWS-Alaska-Karte laden (TDS8 bzw. `international/NWS_Alaska_src/.../S411_NWS_full_20260701.gml`) | Alaska/Beringmeer, Flächen **zusammenhängend** über die Datumsgrenze |

Danach die Funktions-Checkliste:

| # | Test | Soll |
|---|---|---|
| 1 | 411-Button → `TDS1_...zip` wählen | Ostsee-Eiskarte erscheint als Overlay: Konzentrationsfarben (blau=eisfrei … dunkelrot=10/10), **Festeis grau**, schwarze Umrandungen |
| 2 | Farben mit dem Web-Viewer vergleichen (Portal → Viewer, gleicher Datensatz) | identische Flächenfarben (gleiche Farbtabelle) |
| 3 | Button aus → wieder an → *Abbrechen* im Dialog | Overlay verschwindet / erscheint wieder, ohne Neuladen |
| 4 | `TDS4_RealData_Baltic.zip` laden, in die Ostsee zoomen/pannen | flüssiges Pan/Zoom trotz 337 Features (Triangulierung passiert nur beim Laden) |
| 5 | `TDS8_...zip` laden, zur **Beringstraße/Alaska** navigieren | Eisflächen liegen korrekt beidseits der Datumsgrenze — **keine** weltumspannenden Bänder |
| 6 | `TDS6_...zip` laden, deutsche Ostseeküste | schwarze Dreiecksmarker an den Stationspositionen |
| 7 | Entpackten Exchange-Set testen: Zip entpacken, im Dialog die **CATALOG.XML** wählen | gleicher Effekt wie Zip-Laden |
| 8 | Einzelne `.GML` aus `DATASET_FILES/` wählen | lädt ebenfalls |
| 9 | **Edition 1.x**: BSH-FTP-Winter-Zip direkt laden | Karte erscheint mit korrekten Farben (iceact→Konzentration, Festeis erkannt) |
| 10 | Fehlerfall: beliebige Nicht-XML-Datei wählen | Warnhinweis, kein Absturz |
| 11 | *Options → Display → Advanced → OpenGL* umschalten, Tests 1+4 wiederholen | beide Rendering-Pfade (GL und Nicht-GL) zeigen dasselbe Bild; im Nicht-GL-Modus sind Füllungen ebenfalls halbtransparent |
| 12 | Geteilter Bildschirm (zweites Chart-Fenster) | Overlay erscheint in beiden Canvases |
| 13 | Steuerdialog → **Display mode** auf *Stage of development* | Flächen färben nach Entwicklungsstadium (Violett-/Grüntöne) statt Konzentration; Festeis in diesem Modus nach SOD, nicht grau |
| 14 | **Display mode** *Navigational* + **Vessel ice class** durchschalten | Ampel nach der Solltabelle unten; höhere Eisklasse verschiebt die Grenze (mehr grün). 10/10-Eis bleibt für jede Klasse rot |
| 15 | **Colour palette** *Dusk* / *Night* durchschalten | Füllungen abgedunkelt; Linien im Night-Modus hell/sichtbar; Legende dimmt mit |
| 16 | Vergleich mit Web-Viewer (Portal → Viewer, gleicher Datensatz, gleicher Modus/Palette/Eisklasse) | identische Portrayal — Plugin, Viewer und die normative `rules/main.xsl` nutzen seit v0.4.0 nachweislich dieselben Tabellen (CI-Paritätschecks in beiden Repos) |
| 17 | Klick auf eine Eisfläche (ohne Ziehen) | Feature-Report: Featuretyp, Datensatz, alle Attribute; `totalConcentration`/`stageOfDevelopment` mit Klartext-Label; Klick auf freie See: nichts; Pannen unverändert |
| 18 | Zweiten Datensatz laden, Checkboxen der Liste umschalten | jeweils nur die angehakten Datensätze werden gezeichnet; *Remove* entfernt einzeln; *Zoom to data* springt zum markierten bzw. zu allen sichtbaren |
| 19 | **Fill opacity** schieben, OpenCPN neu starten | Deckkraft ändert sich live und ist nach Neustart erhalten |
| 20 | Robustheit: Kopie von `tests/fixtures/v1_latlon.gml`, `iceact`-Wert auf `9+` oder leer setzen, laden | Fläche **grau „Unknown"** in Nav- und Konzentrations-Modus — nicht rot, nicht grün „eisfrei" |

**Solltabelle Navigational-Ampel** (Bewertung der maximalen Bedeckung in
Zehnteln; Entscheidung dokumentiert im seaice-Repo, `docs/DECISIONS.md` #17):

| ct | keine Klasse | IB / IC (PC6–7) | IA (PC4–5) | IA Super (PC1–3) |
|---|---|---|---|---|
| 46 (4–6/10) | gelb | gelb | **grün** | grün |
| 80 (8/10) | gelb | gelb | gelb | **grün** |
| 90 (9/10) | rot | rot | rot | **gelb** |
| 92 (10/10) | rot | rot | rot | rot |
| 02 (bergy water) | überall hellblau *Open water / bergy water* — nie „Safe (ice free)" | | | |

**Ice-Routing (v0.5.0):**

| # | Test | Soll |
|---|---|---|
| 21 | Datensatz laden, im Panel unten **Start → Set…**, auf die Karte klicken; dann **End → Set…**, klicken | die zwei Positionen erscheinen als Koordinaten im Panel |
| 22 | **Vessel ice class** wählen, **Compute ice route** | eine Route erscheint in OpenCPN (Route-Manager), die um rote (hazardous) Eisflächen herumführt; Statuszeile nennt Distanz, Meilen in Caution-Eis und Etappenzahl |
| 23 | Eisklasse von *No class* auf *IA Super* ändern, erneut Compute | die Route darf mehr Eis durchqueren (höhere Klasse → mehr grün/gelb passierbar) |
| 24 | Start oder Ziel mitten in dichtem Eis setzen, Compute | der Router lässt das Startfeld verlassen; liegt das **Ziel** komplett im roten Eis, klare Fehlermeldung |
| 25 | Kanal komplett durch Eis versperrt, Compute | Meldung „No route: …" statt Absturz oder Route quer durchs Eis |
| 26 | **Avoid caution ice too** anhaken, Compute | die Route meidet zusätzlich gelbe Flächen (oder meldet, dass keine offene Passage bleibt) |
| 27 | *(nur mit ocpn_draw_pi installiert)* **Export no-go zones** | Meldung „Exported N …"; in ocpn_draw erscheinen rote Exclusion-Grenzen um das gefährliche Eis; erneuter Export ersetzt nur die eigenen, eigene ocpn_draw-Grenzen des Nutzers bleiben |
| 28 | Danach Weather Routing: *Configuration → Detect Boundary* an, Route rechnen | die Wetter-Route umfährt die exportierten Eiszonen |
| 29 | *Export no-go zones* **ohne** ocpn_draw_pi | Hinweis, dass ocpn_draw_pi fehlt — kein Absturz |

Der Router arbeitet auf den **sichtbaren** Datensätzen und der oben gewählten
**Vessel ice class**; er nutzt die identische Ampel-Logik wie die Darstellung.
Ab v0.6.0 wird der Suchkorridor bei „keine Route im Korridor" automatisch
geweitet (×2/×4), und eine erneute Berechnung **ersetzt** die vorige Eisroute
statt sie zu stapeln.

**Bekannte Grenzen (v0.6.0, kein Fehler):** Punkt-Features als generische
Dreiecke statt Annex-C-Symbole, Linien-Strichmuster sind Näherungen; bei
geteiltem Bildschirm nutzt der Feature-Report den zuletzt gezeichneten
Canvas-Viewport. Der Router sucht in einem Korridor um Start/Ziel (kein
globales Routing über große Ozeanbecken); Start/Ziel oberhalb 85° werden
abgelehnt. **10/10-Eis (und Festeis) gilt für JEDE Eisklasse als unpassierbar**
— bewusst identisch zur Ampel-Darstellung; im Hochwinter kann daher „keine
Route" die korrekte Antwort sein. (Ein durchlässiger Modus für Höchst-
Eisklassen mit hoher Strafe wäre eine mögliche künftige Option.)

---

## 4. Nativer Windows-Build (ein Befehl)

Voraussetzungen: Visual Studio 2022 mit C++-Workload, OpenCPN (Windows-Build,
32-bit) installiert. Dann im Repo:

```powershell
powershell -ExecutionPolicy Bypass -File buildwin.ps1 -Package
```

Das Skript lädt beim ersten Lauf das passende wxWidgets-3.2.9-vc14x-SDK
(~35 MB), erzeugt die OpenCPN-Importbibliothek **direkt aus der installierten
`opencpn.exe`** (dumpbin → .def → lib — kein separates SDK nötig), kompiliert
Win32/`/MD` gegen dieselben wx-DLLs, die OpenCPN mitbringt, und baut

```
dist\s411_pi-<version>-msvc-wx32-win32.tar.gz
```

— ein Import-Paket im Format des OpenCPN-Plugin-Managers (metadata.xml mit
`target: msvc-wx32`, DLL unter `plugins/`).

**Installation über den Plugin-Manager:** OpenCPN → *Options → Plugins* →
**Import plugin…** → das `.tar.gz` aus `dist\` wählen → Plugin **S-411 Ice**
in der Liste aktivieren.

**Deinstallation:** OpenCPN → *Options → Plugins* → **S-411 Ice** anklicken →
**Uninstall**. Das Plugin lädt von sich aus nie Daten (auch nicht beim Start);
Deaktivieren oder Deinstallieren entfernt das Overlay sofort und gibt allen
Speicher frei. Falls einmal Reste bleiben (z.B. die DLL war gesperrt, weil
OpenCPN lief): OpenCPN schließen und
`powershell -ExecutionPolicy Bypass -File packaging\uninstall.ps1` ausführen —
das entfernt DLL und die Registrierung sauber. **Vor dem Import einer neuen
Version** die alte so deinstallieren.

Alternativ kopiert `buildwin.ps1 -Install` die DLL direkt nach
`C:\Program Files (x86)\OpenCPN\plugins\` (UAC) — dann aber **nicht beides**:
entweder Import ODER Direktkopie, sonst wird das Plugin doppelt gefunden.

Nach einem OpenCPN-**Update** `sdk\opencpn.lib` löschen und das Skript neu
laufen lassen (die Importbibliothek muss zur neuen exe passen).

---

## 5. Befunde melden

Bitte pro Befund notieren: Testnummer aus der Checkliste, Datensatz,
OpenGL an/aus, Screenshot. Bei Lade-Problemen zusätzlich die Meldung aus dem
Warnhinweis-Dialog — sie enthält Datei und Ursache pro fehlgeschlagener Datei.
