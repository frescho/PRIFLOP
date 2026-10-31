<p align="center"><img src="images/logo.png" alt="PRIFLOP" width="536"></p>

# PRIFLOP (magyarul)

*English: [README](../../README.md)*

A PriFlop egy Floppy emulator a Microkey Primóhoz és a Commodore 64-hez. A hardver egy Raspberry Pi
Pico 2 W-on alapul. Az SD2IEC-hez hasonlóan a Primo IEC buszára csatlakozik, de a
fájlokat a Pico saját flash-memóriájában tárolja és Wi-Fi-n keresztül egy
weboldalon tudjuk a fájlokat feltölteni és kezelni.

A PRIFLOP az [sd2iec](https://www.sd2iec.de/) (Ingo Korb és társai)
átirata az RP2350-re, a következő kiegészítésekkel:

- **A Primóhoz:**
  - saját DOS, amely a programok számára CDOS 1.0-kompatibilis, felismeri a két fizikai gépverziót és
    ellenőrzőösszeggel védett gyors (turbó) átvitellel tölt és ment;
  - **PFM**, fájlválasztó, amely egy név nélküli `CMD LOAD`-dal indul,
    beállítástól függően akár reset után magától.
- **Nincs SD-kártya:** a fájlok a Pico flash-memóriájában tárolódnak, 3 MB-os
  littlefs-köteten.
- **Webes fájlkezelő** Wi-Fi-n: böngészés, feltöltés, letöltés, átnevezés,
  törlés, mappák, Wi-Fi-beállítás és a meghajtó diagnosztikája.
- **RAM-meghajtó** 9-es eszközként.
- **A C64-hez:** az sd2iec parancsai, D64, D71, D81 és DNP lemezképek,
  JiffyDOS.

## Mi kell a Primóhoz?

Az alap Primo ROM-jában nincsenek lemezkezelő rutinok. Ahhoz, hogy a gép a
meghajtóról induljon, két dolog kell:

- **A soros port.**
  - **B sorozat:** be van építve, csak a kábelt kell elkészíteni.
  - **A sorozat:** be kell forrasztani a hiányzó DIN-aljzatot, IC-ket és
    ellenállásokat (Hardver kézikönyv, opcionális Commodore soros interfész,
    76–78. oldal).
- **A CDOS boot EPROM.** A BASIC ROM négy EPROM-ban van; az elsőt (0–4 kB)
  a CDOS-t betöltő változatra kell cserélni. Ez a régi IC kiforrasztásával,
  törlésével (UV) és újraégetésével jár. A beépítés során célszerű foglalatot beforrasztani.
  A gép soros portjának címétől függően két változat létezik: `CDOSA.BIN` (0x80)
  és `CDOSA641.BIN` (0x40).
- **Vagy egy külső CDOS bővítőkártya** a bővítőporton, amely bekapcsoláskor
  tölt a lemezről, EPROM-csere nélkül. *Ezzel a PRIFLOP-ot még nem
  próbáltuk ki.*

Részletes leírás: a [Primo honlap floppy oldalán](https://primo.homeserver.hu/html/hardvercdos.html).
A PRIFLOP-ot egy Primo B64-en ('84.2 ROM) teszteltük, CDOS boot EPROM-mal.

## Gyors kezdés (Primo)

1. **Flashelés.** Tartsd lenyomva a Pico BOOTSEL gombját, dugd be USB-n egy
   számítógépbe, és másold a `priflop.uf2` fájlt a megjelenő `RP2350`
   meghajtóra.
2. **Wi-Fi.** Csatlakozz a `PRIFLOP` hálózathoz, és nyisd meg a
   `http://192.168.4.1` címet (felhasználó: `admin`, jelszó: `admin`). A
   jelszót a Settings fülön azonnal változtasd meg.
3. **A Primo fájlok feltöltése a meghajtóra.** A kicsomagolt zip könyvtárában:
   `python3 deploy.py --ip 192.168.4.1`
   Ez feltölti a boot fájlt és a PFM-et. A boot fájl neve egyetlen bájt
   (0x85), ezért a weboldalról nem tölthető fel; ehhez a `deploy.py` kell.
4. **Csatlakoztatás.** Dugd a kábelt a
   kikapcsolt Primo soros portjába. Ajánlott először a PriFlopot áram alá helyezni. 
5. **Reset a BRK nyomva tartásával.** Tartsd lenyomva a BRK gombot, eközben nyomd
   meg a resetet, majd engedd el a BRK-t. A boot fájl betöltődik és
   elindítja a DOS-t; a felirat `turbo`-t mutat, ha a gyors átvitel be van
   kapcsolva.
6. **Betöltés.** A név nélküli `CMD LOAD` elindítja a PFM-et:
   - ↑ ↓: mozgatás;
   - ← →: lapozás;
   - RETURN: betöltés, vagy belépés egy mappába;
   - C: beállítások;
   - Q: kilépés.

   A `CMD LOAD"NÉV"` és a `CMD SAVE"NÉV"` úgy működik, mint a CDOS 1.0
   alatt.
7. **Saját programok.** A weboldal Files fülén töltheted fel őket. A
   kazettás programokat (`.ptp`) előbb a `tools/primo/ptp2disk.py`-jal
   kell átalakítani PRG formátumba.

Ha a `PRIFLOP.CFG`-ben a `PFM_AUTOSTART=ON` van beállítva (vagy a PFM beállításaiban be
van kapcsolva), a PFM minden resetkor magától elindul.

## Bekötés

A meghajtó négy jelvezetéket használ, mindegyiket egy 100 Ω-os soros
ellenálláson át (ATN: GP2, CLOCK: GP3, DATA: GP6, SRQ: GP7). A kapcsolási
rajz, a DIN-dugó lábkiosztása és a Primo kábel bekötése a
[Hardware](HARDWARE.md) oldalon van.

## Dokumentáció (angolul)

| | |
|---|---|
| [Quick start: the Primo](QUICKSTART-PRIMO.md) | Gyors kezdés a Primóhoz |
| [Hardware](HARDWARE.md) | A kártya, a bekötés, alkatrészek, tápellátás |
| [The Primo](PRIMO.md) | A boot fájl, a DOS, a turbó, a PFM |
| [Getting started](SOFTWARE.md) | Flashelés, Wi-Fi, a weboldal, hibaelhárítás |
| [Test programs](TESTS.md) | A tesztprogramok: miért, hogyan, mi a jó eredmény |
| [Building from source](BUILDING.md) | Fordítás forrásból |

## Licenc

Kizárólag a GPL 2-es verziója. Az sd2iec Ingo Korb és társai munkája; a
harmadik féltől származó részek megtartják saját licencüket (lásd az angol
README-t). Az RP2350-átirat, a webes felület és a Primo szoftverei **frescho**
munkái.

Nem része a kiadásnak, és nem is kell hozzá: a CDOS, a Primo ROM-jai és a
Primo programkönyvtára. Amennyiben szükséges, a `tools/primo/fetch-library.sh` letölti a
könyvtárat a primo.homeserver.hu-ról saját használatra.
