# Hantek 6022BL → MATLAB

Χρήση του USB παλμογράφου Hantek 6022BL ως data logger από το MATLAB (64-bit):
οι μετρήσεις έρχονται ως αριθμοί για επεξεργασία, όχι μόνο στην οθόνη.

Υπάρχουν δύο ανεξάρτητοι δρόμοι:

| | SDK path (ρίζα του repo) | libusb path (`libusb/`) |
|---|---|---|
| Πώς | MATLAB → `HantekWrapper.dll` (64-bit) → named pipe → `HantekProxy.exe` (32-bit) → `HTMarch.dll` (SDK της Hantek) | MATLAB → `HantekUSB.dll` (64-bit) → libusb → ανοιχτό firmware sigrok fx2lafw |
| Driver | της Hantek | WinUSB (Zadig), βλ. `libusb/README.md` |
| Λήψη | λήψεις (block) έως 1.047.552 δείγματα, με κενό ανάμεσα | **συνεχής χωρίς κενά**, ή block οποιουδήποτε μήκους |
| Ρυθμοί | 48, 16, 8, 4, 1 MSa/s, 500, 200, 100 kSa/s | και 30, 24, 12, 2 MSa/s |
| Volt | εργοστασιακή βαθμονόμηση από την EEPROM | `calibrate_zero_libusb.m` ή το `hantek_cal.mat` του SDK path |
| Script | `tetbench_sdk.m` | `libusb/tetbench_libusb.m` |

Το αρχικό `tetbench.mlx` έμεινε ανέγγιχτο. Το `tetbench_sdk.m` είναι το ίδιο
script ως απλό `.m`, με τις διορθώσεις. Αν προτιμάς live script: δεξί κλικ →
*Open as Live Script*.

## Πώς γυρνάς πίσω αν κάτι δεν δουλέψει

Κάθε βήμα είναι ξεχωριστό commit στο branch `claude/trusting-dijkstra-2aq3zo`.
Το `main` **δεν άλλαξε**: είναι ακόμη το αρχικό commit `32f6996`.

| Θέλω… | Εντολή |
|---|---|
| όλο το αρχικό project | `git checkout main` (ή στο GitHub: branch `main` → *Download ZIP*) |
| να δω/δοκιμάσω την κατάσταση μετά από ένα βήμα | `git checkout <commit>` (πίνακας παρακάτω) |
| να αναιρέσω βήματα στο branch | `git revert <commit>`, από το νεότερο προς το παλαιότερο (τα βήματα 1–3 αλλάζουν τα ίδια αρχεία) |
| μόνο τα αρχικά binaries | `git checkout 32f6996 -- HantekProxy.exe HantekWrapper.dll HantekWrapper.h` και χρήση του αρχικού `tetbench.mlx` (το νέο `tetbench_sdk.m` χρειάζεται τα νέα binaries) |

| Βήμα | Commit | Τι αλλάζει |
|---|---|---|
| αρχικό | `32f6996` | — |
| 1 | `ea814eb` | Σωστός πίνακας sample rate, 0 ως 5ο όρισμα στο SDK |
| 2 | `e94f129` | Έλεγχοι εισόδου, χειρισμός σφαλμάτων, static build, tests |
| 3 | `a99e6fb` | Volt/div, εργοστασιακή βαθμονόμηση, τιμές σε Volt |
| 4 | `8db3a7b` | libusb path (νέος φάκελος `libusb/`, δεν αγγίζει το SDK path) |

Ο libusb path είναι αυτόνομος: αν δεν σε ενδιαφέρει, αγνόησε ή σβήσε τον φάκελο
`libusb/`. Για τον driver (επιστροφή από WinUSB στον driver της Hantek) δες το
`libusb/README.md`.

## Τι άλλαξε στο SDK path

**Βήμα 1**
- Το `HantekProxy.exe` περνά πάντα 0 ως τελευταίο όρισμα της `dsoReadHardData_LA`.
  Δεν είναι nTimeDIV αλλά δείκτης σε εσωτερικό πίνακα 8 θέσεων, και τιμές ≥ 8
  έδιναν σκουπίδια ή crash.
- Ο πίνακας sample rate στο MATLAB είναι πλέον ο πραγματικός του DLL (δες παρακάτω).
- Ο proxy είναι πλήρως static και δεν χρειάζεται πια `libstdc++-6.dll` κ.λπ.

**Βήμα 2**
- Ο proxy ελέγχει κάθε αίτημα και απαντά `-2` σε άκυρα, αντί να μην απαντά. Πριν,
  μια άγνωστη εντολή πάγωνε το MATLAB.
- Όρια: `nReadLen` 1..1.047.552, `nTimeDIV` 0..38.
- Αν τρέχει ήδη άλλο instance, ο proxy τερματίζει με μήνυμα αντί να «κολλάει» στο 100% CPU.
- Ο proxy φορτώνει το `HTMarch.dll` από τον φάκελό του.
- Το wrapper ξαναπροσπαθεί τη σύνδεση για ~5 s και ξανασυνδέεται μετά από
  restart του proxy.
- Το MATLAB ελέγχει κάθε τιμή επιστροφής, ξεκινά τον proxy χωρίς να περιμένει,
  δείχνει τα κενά ανάμεσα στις λήψεις και προειδοποιεί για clipping.

**Βήμα 3**
- Νέες `dsoSetVoltDIV` και `dsoGetCalLevel`.
- Το MATLAB ρυθμίζει volt/div ανά κανάλι, διαβάζει την εργοστασιακή βαθμονόμηση,
  δίνει τιμές σε Volt και την αποθηκεύει στο `hantek_cal.mat` (τη χρησιμοποιεί
  και ο libusb path).

## Τι βρέθηκε μέσα στο HTMarch.dll (disassembly)

| Θέμα | Εύρημα |
|---|---|
| nTimeDIV → ρυθμός | 0–10: 48 MSa/s, 11: 16, 12: 8, 13: 4, 14–24: 1 MSa/s, 25: 500 k, 26: 200 k, 27–38: 100 kSa/s, ≥39: απορρίπτεται |
| nVoltDIV → gain | 0–7 (20 mV … 5 V/div) → gain 10, 10, 10, 5, 2, 1, 1, 1 |
| Κλίμακα | ονομαστικά 25 ADC counts/V στο gain 1, δηλαδή περιοχή ±5.12 V / gain (αξίζει ένας έλεγχος με γνωστή τάση) |
| Βαθμονόμηση | `dsoGetCalLevel`: EEPROM offset 8 (εντολή 0xA2), μηδέν στο `level[16·hs + 2·nVoltDIV + ch]`, hs = 1 στα 48 MSa/s |
| `dsoReadHardData_LA` | καταγράφει πάντα 1.048.576 δείγματα και επιστρέφει έως 1.047.552 μετά τα πρώτα 1024, οπότε στα 100 kSa/s κάθε κλήση κρατά ~10,5 s |
| Συσκευή | `\\.\d602a-N`, IOCTL 0x222059 (vendor request) και 0x22204E (bulk read) |
| Χωρίς έλεγχο στο DLL | αρνητικά nTimeDIV/nVoltDIV, `nReadLen` > 1.047.552, `nLen` > 128 (τώρα τα απορρίπτει ο proxy) |

## Build

- SDK path: `./build_sdk.sh` (MinGW-w64 32 και 64 bit, σε MSYS2 ή Linux).
- libusb path: `libusb/build_libusb.sh`.

Τα έτοιμα binaries είναι ήδη στο repo, χτισμένα από αυτόν τον κώδικα.

## Tests

- `tests/sdk/run_sdk_tests.sh`: wrapper + proxy πάνω από πραγματικό named pipe,
  με ψεύτικο `HTMarch.dll`, σε Wine. Ο νέος κώδικας περνά όλους τους ελέγχους.
  Ο αρχικός αποτυγχάνει σε 14 από τους ελέγχους του βήματος 2 (τις συναρτήσεις
  του βήματος 3 δεν τις έχει καθόλου).
- `tests/libusb/run_libusb_tests.sh`: η `HantekUSB` στο Linux, η DLL σε Wine, και
  ο κώδικας USB απέναντι σε μοντέλο του 6022BL.

Κανένα από τα δύο δεν έχει δοκιμαστεί ακόμη σε πραγματικό 6022BL, σε πραγματικά
Windows ή σε MATLAB. Τα tests ελέγχουν τη λογική και το πρωτόκολλο, όχι το υλικό.
