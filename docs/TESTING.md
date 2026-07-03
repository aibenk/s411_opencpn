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

**Soll:** `ALL TESTS PASSED (0 failures)` — 17 Tests gegen echte Daten
(BSH-Ostseekarte mit 337 Features, NWS-Alaska über 180°, Eisbericht-Stationen,
Exchange-Set als Verzeichnis und als Zip).

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

### 2.3 Aktivieren

1. OpenCPN starten (`opencpn`).
2. *Options → Plugins* → **S-411 Ice** ankreuzen → *OK*.
3. In der Werkzeugleiste erscheint der **411**-Button.

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

Der 411-Button ist ein Umschalter: **Aktivieren öffnet den Dateidialog**
(Abbrechen behält bereits Geladenes), **Deaktivieren blendet aus** (Daten
bleiben erhalten).

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

**Bekannte Grenzen (v0.2.0, kein Fehler):** keine Pick-Reports beim Klick auf
Eisflächen, Punkt-Features als generische Dreiecke statt Annex-C-Symbole,
Linien-Strichmuster sind Näherungen, kein Eisbericht-Textfenster.

---

## 4. Nativer Windows-Build (nur wenn nötig)

OpenCPN für Windows ist ein **32-bit-Build (Win32)** — das Plugin muss
identisch gebaut werden und braucht die OpenCPN-Importbibliothek:

1. Visual Studio (MSVC) mit C++-Workload installieren.
2. wxWidgets in **derselben Version/Konfiguration wie OpenCPN** bauen oder
   das OpenCPN-Plugin-SDK verwenden (liefert `opencpn.lib` + wxWidgets;
   Bezugsweg siehe [shipdriver-Template](https://github.com/Rasbats/shipdriver_pi)).
3. Bauen:
   ```bat
   cmake -B build -A Win32 -DOCPN_IMPORT_LIB=C:\pfad\zu\opencpn.lib ^
         -DwxWidgets_ROOT_DIR=C:\pfad\zu\wxWidgets
   cmake --build build --config Release
   copy build\Release\s411_pi.dll "%LOCALAPPDATA%\opencpn\plugins\"
   ```
4. Weiter wie in 2.3.

Für die erste Praxisrunde ist der WSL-Weg (Abschnitt 2) deutlich schneller
und testet exakt denselben Code.

---

## 5. Befunde melden

Bitte pro Befund notieren: Testnummer aus der Checkliste, Datensatz,
OpenGL an/aus, Screenshot. Bei Lade-Problemen zusätzlich die Meldung aus dem
Warnhinweis-Dialog — sie enthält Datei und Ursache pro fehlgeschlagener Datei.
