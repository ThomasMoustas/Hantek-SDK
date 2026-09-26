# libusb path: Hantek 6022BL σε 64-bit MATLAB χωρίς το SDK της Hantek

Η `HantekUSB.dll` (64-bit) μιλάει απευθείας στο scope μέσω **libusb 1.0.29**
και του ανοιχτού firmware **fx2lafw του sigrok** (0.1.7). Δεν χρειάζεται
`HantekProxy.exe`, `HTMarch.dll` ή ο driver της Hantek.

- **Συνεχής λήψη χωρίς κενά**: τα δείγματα ρέουν σε ring buffer και κάθε
  δείγμα έχει ακριβή δείκτη χρόνου (`index / rate`).
- **Λήψη block** οποιουδήποτε μήκους (όχι μόνο 1.047.552 δείγματα).
- Ρυθμοί 100 kSa/s … 48 MSa/s (και 30, 24, 12, 2 MSa/s, που δεν είχε το SDK).
- Gain 1, 2, 5, 10 ανά κανάλι.

Το πρωτόκολλο (εντολές 0xE0–0xE4, bulk endpoint 0x86, upload firmware με 0xA0)
πάρθηκε από τον πηγαίο κώδικα του firmware (`include/scope.inc`) και του driver
`hantek-6xxx` του libsigrok 0.5.2, που υπάρχουν στο `third_party/`.

## Αρχεία

| Αρχείο | Τι είναι |
|---|---|
| `HantekUSB.dll` | Η βιβλιοθήκη για το MATLAB (64-bit, libusb ενσωματωμένο). |
| `HantekUSB.h` | Το header για το `loadlibrary`. |
| `HantekUSB.c` | Ο πηγαίος κώδικας. |
| `fx2lafw-hantek-6022bl.fw`, `fx2lafw-hantek-6022be.fw` | Το firmware του sigrok. Πρέπει να είναι δίπλα στη DLL. |
| `tetbench_libusb.m` | Παράδειγμα: συνεχής λήψη ή λήψη block, γράφημα σε Volt. |
| `calibrate_zero_libusb.m` | Μέτρηση του μηδενός για κάθε gain (είσοδοι στα 0 V). |
| `build_libusb.sh` | Build της DLL (και του `.so` για τα tests). |
| `third_party/` | Πηγαίος κώδικας του libusb και του firmware (για τις άδειες). |

## Εγκατάσταση driver (μία φορά, Windows)

Το libusb χρειάζεται τον driver **WinUSB** αντί για τον driver της Hantek, και
μάλιστα για **δύο** USB IDs. Η συσκευή αλλάζει ID όταν φορτωθεί το firmware.

1. Κατέβασε το [Zadig](https://zadig.akeo.ie).
2. Σύνδεσε το 6022BL. Στο Zadig: *Options → List All Devices*. Διάλεξε τη
   συσκευή με USB ID **04B4 602A** (ή 04B5 602A, αν ο driver της Hantek έχει ήδη
   φορτώσει το δικό του firmware). Επίλεξε driver **WinUSB** → *Replace Driver*.
3. Στο MATLAB, με Current Folder αυτόν τον φάκελο, τρέξε το `tetbench_libusb.m`.
   Η DLL φορτώνει το firmware και η συσκευή επανεμφανίζεται ως **1D50 608E**
   (στο Zadig με όνομα «fx2lafw»). Την πρώτη φορά τα Windows δεν έχουν driver για αυτό το ID,
   οπότε το `huOpen` αποτυγχάνει μετά από ~10 s με μήνυμα που ζητά Zadig.
4. Zadig ξανά: διάλεξε το **1D50 608E** → **WinUSB** → *Install Driver*.
5. Τρέξε ξανά το script. Από εδώ και πέρα όλα γίνονται αυτόματα, κάθε φορά που
   συνδέεις το scope.

## Επιστροφή στον driver της Hantek

Όσο είναι εγκατεστημένος ο WinUSB, το λογισμικό της Hantek και το SDK path
(`../tetbench_sdk.m`) δεν βρίσκουν τη συσκευή. Για να τα ξαναχρησιμοποιήσεις:

1. Σύνδεσε το scope. *Device Manager → Universal Serial Bus devices*: δεξί κλικ
   στη συσκευή → *Uninstall device*. Αν εμφανίζεται, τσέκαρε και το
   *Delete the driver software for this device* (Windows 10) ή
   *Attempt to remove the driver for this device* (Windows 11).
2. Αποσύνδεσε και ξανασύνδεσε το scope. Τα Windows βάζουν ξανά τον driver της
   Hantek. Αν δεν γίνει: *Update driver → Browse my computer → Let me pick* →
   ο driver της Hantek, ή τρέξε ξανά το setup του driver της Hantek.

Αν το scope είναι ακόμη συνδεδεμένο με το firmware του sigrok (1D50:608E),
αποσύνδεσέ το πρώτα: το firmware ζει μόνο στη RAM και χάνεται.

## Χρήση από το MATLAB

Στην αρχή του `tetbench_libusb.m`:

| Ρύθμιση | Τιμές |
|---|---|
| `SIMULATE` | 0 = πραγματικό scope, 1 = προσομοιωμένα σήματα (τρέχει χωρίς συσκευή) |
| `SAMPLE_RATE` | 48e6, 30e6, 24e6, 16e6, 12e6, 8e6, 4e6, 2e6, 1e6, 500e3, 200e3, 100e3 |
| `GAIN` | ανά κανάλι 1, 2, 5, 10 → περιοχή περίπου ±5.12 V / gain |
| `DURATION` | δευτερόλεπτα λήψης |
| `MODE` | `'stream'` (συνεχής) ή `'block'` |

**Volt**: `V = (raw − zero) / (25 · gain)`. Το 25 counts/V (στο gain 1) είναι η
κλίμακα που χρησιμοποιεί και το HTMarch.dll. Για το `zero` το script
προτιμά, με αυτή τη σειρά:
1. το `hantek_zero_libusb.mat`, που δημιουργεί το `calibrate_zero_libusb.m`,
2. την εργοστασιακή βαθμονόμηση `../hantek_cal.mat`, που αποθηκεύει το `../tetbench_sdk.m`
   (το firmware του sigrok δεν διαβάζει την EEPROM, γι' αυτό αξίζει να τρέξεις
   μία φορά το SDK path πριν αλλάξεις driver),
3. το 128.

Πριν κάνεις `unloadlibrary`, κάλεσε πάντα `huClose`, που σταματά το USB thread.
Τα scripts το κάνουν ήδη.

## Ρυθμοί και εύρος ζώνης USB

Και τα δύο κανάλια ρέουν πάντα, δηλαδή 2 × rate bytes/s. Το USB 2.0 bulk δίνει
στην πράξη ~35–40 MB/s:

- **≤ 8 MSa/s**: ρεαλιστικό για συνεχή λήψη.
- **16 MSa/s**: στο όριο.
- **24–48 MSa/s**: χάνονται δεδομένα, σε κάθε λειτουργία.

Στα Windows το libusb δεν ενεργοποιεί το RAW_IO του WinUSB, οπότε τα όρια ίσως
είναι χαμηλότερα. Γι' αυτό η DLL μετρά τον **πραγματικό ρυθμό** (`getQueueStatus`,
θέση 9 στο MATLAB) και το script προειδοποιεί αν είναι κάτω από το 99% του
ρυθμιζόμενου. Το firmware δεν σηματοδοτεί μόνο του τις απώλειες.

Αν το MATLAB δεν διαβάζει αρκετά γρήγορα, ο ring buffer (~10 s) γεμίζει. Τα
νεότερα δείγματα τότε χάνονται και το κενό φαίνεται (ο δείκτης πηδάει,
`getQueueStatus` θέσεις 5–6).

## API (C, `HantekUSB.h`)

| Συνάρτηση | |
|---|---|
| `huOpen()` / `huClose()` | Άνοιγμα (με upload firmware) / κλείσιμο. |
| `huSetSampleRate(rate)`, `huSetGain(ch, gain)` | Ρυθμίσεις (όχι κατά τη διάρκεια streaming). |
| `huReadBlock(ch1, ch2, n)` | n συνεχόμενα δείγματα ανά κανάλι. |
| `startStreaming(buf)`, `getStreamData(ch1, ch2, max, &first)`, `getQueueStatus(st)`, `stopStreaming()` | Συνεχής λήψη (τα ονόματα που είχε ήδη το `HantekWrapper.def`). |
| `huSetSimulation(mode)`, `huSetFirmwareDir(dir)`, `huCheckFirmware(model)`, `huLastError()` | Βοηθητικές. |

Όλες επιστρέφουν αρνητικό κωδικό `HU_ERR_*` σε σφάλμα, και το `huLastError()` το εξηγεί.

## Τι έχει ελεγχθεί

`../tests/libusb/run_libusb_tests.sh` (Linux + Wine):

1. `HantekUSB.c` ως `.so` στο Linux, σε λειτουργία προσομοίωσης: ring buffer,
   κενά, δείκτες χρόνου, ρυθμός, σφάλματα.
2. Η **ίδια η `HantekUSB.dll`** αυτού του φακέλου μέσα σε Wine: ο κώδικας για
   Windows (threads, χρονόμετρο, εύρεση firmware δίπλα στη DLL).
3. Ο κώδικας USB απέναντι σε **μοντέλο του 6022BL** (`fake_libusb.c`, γραμμένο
   από το `scope.inc`): upload firmware byte-προς-byte, renumeration, εντολές,
   12 εκατομμύρια συνεχόμενα δείγματα στα 8 MSa/s χωρίς απώλειες, αποσύνδεση
   συσκευής.

**Δεν** έχει δοκιμαστεί σε πραγματικό 6022BL, σε πραγματικά Windows ή σε MATLAB.
Αν κάτι δεν δουλεύει, το `huLastError()` και το PulseView/sigrok-cli του sigrok
(με τον ίδιο WinUSB driver και `--driver hantek-6xxx`) βοηθούν να φανεί αν το
πρόβλημα είναι στη DLL ή στον driver/συσκευή.

## Άδειες τρίτων

- **libusb 1.0.29**: LGPL-2.1-or-later, ενσωματωμένο στατικά στη DLL. Ο
  πηγαίος κώδικας είναι στο `third_party/libusb-1.0.29.tar.bz2`. Το
  `build_libusb.sh` ξαναφτιάχνει τη DLL, άρα και με τροποποιημένο libusb.
- **sigrok-firmware-fx2lafw 0.1.7** (τα `.fw`): GPL-2.0-or-later (μέρη
  LGPL-2.1-or-later). Ο πηγαίος κώδικας είναι στο
  `third_party/sigrok-firmware-fx2lafw-0.1.7.tar.gz`. Τα `.fw` είναι αυτά του
  πακέτου `sigrok-firmware-fx2lafw 0.1.7-1` του Ubuntu 24.04, χτισμένα από
  αυτόν τον κώδικα:
  - `fx2lafw-hantek-6022bl.fw` sha256 `e31eb54405e05073b39efb44968254305cd3442228f1f493a691015e04fa6c4b`
  - `fx2lafw-hantek-6022be.fw` sha256 `5a4df01996ec362b5f9956aa0eb0ba9d717d0d71b4e1b2e4ee730a5cb56132f9`
