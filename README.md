# KNX TP1 Analyzer

Analyseur portable expérimental KNX TP1 : corréler le protocole et la couche physique.
Experimental portable KNX TP1 analyzer: correlate protocol traffic with the physical layer.

[Français](#français) · [English](#english)

## Français

> [!CAUTION]
> **⚠️ PROJET EXPÉRIMENTAL — DÉVELOPPEMENT EN COURS**
> Ce projet est actuellement un prototype de développement. L'interface analogique protégée destinée au bus KNX TP1 n'est pas encore implémentée. **Ne jamais connecter directement un GPIO ou une entrée ADC de l'ESP32-C6 au bus KNX TP1.** Ce projet n'est actuellement ni un appareil KNX certifié, ni un produit commercial fini.

### Objectif du projet

KNX TP1 Analyzer vise à devenir un analyseur portable de diagnostic pour les installations KNX TP1. L'objectif final dépasse la lecture des télégrammes : observer simultanément le trafic protocolaire, la tension réelle du bus, la forme analogique du signal TP1, les erreurs de communication, les répétitions, les réponses ACK / NAK / BUSY, les chutes de tension et les anomalies de couche physique.

La fonction centrale envisagée est leur **corrélation** : chute de tension → télégramme perturbé → absence d'ACK → répétition → capture analogique de l'événement. À terme, l'outil doit aider à diagnostiquer des problèmes d'alimentation, de câblage, de contacts intermittents, de perturbations électriques, de charge anormale du bus et de qualité du signal qu'un simple moniteur de télégrammes ne montre pas.

### Fonctions envisagées

- **Protocole :** décodage des télégrammes KNX TP1, adresses source et destination, données, ACK / NAK / BUSY, répétitions, erreurs et statistiques de trafic.
- **Couche physique :** mesure indépendante de la tension KNX, min/max, chutes et perturbations, forme d'onde, oscilloscope et déclenchement sur anomalie. Un **front-end analogique séparé et protégé** reste à concevoir.
- **Événements :** conserver les échantillons autour du trigger en RAM et sauvegarder chaque capture sur microSD ; le contexte KNX reste futur.
- **HMI :** écran couleur tactile, Start/Stop Analysis, Dashboard, Scope, compteur d'événements, état système et diagnostics.
- **Web local :** Dashboard, Scope, Events, Sessions, Configuration et System accessibles depuis PC, tablette ou smartphone, sans cloud obligatoire.
- **Black box microSD :** les sessions et leurs captures Events sont sauvegardées sur la carte. Le flux ADC brut complet n'est pas écrit en permanence.
- **Application PC et rapports :** import futur d'une session et rapport HTML/PDF avec chronologie, statistiques, anomalies, télégrammes, tensions et captures associées.

### Matériel et interfaces

Le prototype utilise la [Waveshare ESP32-C6-Touch-LCD-1.47](https://www.waveshare.com/esp32-c6-touch-lcd-1.47.htm) ([documentation officielle](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47)) : ESP32-C6, 8 Mo de Flash, écran IPS tactile capacitif 1,47" de 172 × 320 pixels, Wi-Fi, BLE, emplacement microSD, gestion batterie et USB-C.

| Ressource matérielle | Lien | Usage dans le projet |
| --- | --- | --- |
| Carte Waveshare ESP32-C6-Touch-LCD-1.47 | [Produit officiel](https://www.waveshare.com/ESP32-C6-Touch-LCD-1.47.htm) · [documentation](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47) | Prototype actuel, validé |
| Brochage et fichiers constructeur | [Ressources Waveshare](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47/Resources-And-Documents) · [schéma PDF](https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Schematic.pdf) | Vérification des GPIO, du tactile, du LCD et de la microSD |
| Microcontrôleur ESP32-C6 | [Fiche technique Espressif](https://documentation.espressif.com/esp32-c6_datasheet_en.pdf) | Limites électriques, ADC et interfaces |
| Siemens BTM 117/12 PCBA / TP-UART2 | [Module](https://www.opternus.com/en/siemens/development-tools/tp-uart2-board-btm2-pcb) · [fiche PCBA PDF](https://www.opternus.com/fileadmin/_migrated/content_uploads/PCBA_UP117-12_datasheet_v5_2012-05-30_01.pdf) · [documentation TP-UART Siemens](https://sid.siemens.com/v/u/A6V11933794) | Future voie protocolaire, non intégrée |

La carte, un câble USB-C, une microSD et un signal de test **isolé du KNX** suffisent au prototype Sessions V1. La SD est requise pour démarrer une session. Aucun front-end analogique KNX n'est encore sélectionné.

Pour la future voie **protocolaire**, l'interface envisagée est un Siemens BTM / TP-UART, référence prévue **5WG1 117-8AA12 PCBA BTM**. La [documentation technique publique du module BTM 117/12 PCBA](https://www.opternus.com/fileadmin/_migrated/content_uploads/PCBA_UP117-12_datasheet_v5_2012-05-30_01.pdf) décrit notamment l'interface série. Cette interface n'est **pas encore intégrée**. La mesure **analogique** du bus utilisera un front-end distinct, protégé et adapté aux tensions KNX ; il n'est pas encore conçu ni validé.

| Broche | Rôle du prototype | État |
| --- | --- | --- |
| GPIO5 | Entrée ADC rapide de Scope, actuellement testée hors bus KNX | Validé |
| GPIO6 | Future mesure lente VBUS | Prévu |
| GPIO16 | Futur TX vers TP-UART | Prévu |
| GPIO17 | Futur RX depuis TP-UART | Prévu |
| GPIO7 | Futur signal SAVE TP-UART | Prévu |

Aucun schéma de raccordement analogique au bus KNX n'est validé ou publié.

### État actuel

**Validé sur carte réelle :** Waveshare ESP32-C6, USB/Serial, LCD, tactile, QMI8658A, Wi-Fi AP/STA, Web local, Dashboard, Scope, Events et Start/Stop. Lors des premiers essais, GPIO5 isolé du bus KNX : ADC continu ESP-IDF + DMA à environ **83,33 kéchantillons/s**, ring de **50 000 échantillons / 100 Ko**, 70 % avant et 30 % après trigger, triggers montant, descendant et manuel. Les fronts GND/3V3 ont été testés physiquement. Le Dashboard LCD et son compteur Events ont été validés visuellement.

**Sessions + SD V1 :** START crée une session persistante et STOP la clôture. Le ring RAM conserve 24 métadonnées et une seule capture RAW complète ; la SD conserve l'historique des Events et de leurs RAW. Une session interrompue par reboot est marquée `INTERRUPTED` et ses captures finalisées restent lisibles. Une session de 50 captures manuelles a produit **50/50 fichiers RAW valides**, y compris la première capture sortie du ring RAM ; les 50 CRC et en-têtes ont été vérifiés. Une session fermée et une session interrompue sont restées accessibles après reboot.

Le firmware corrigé a tenu **630 s** avec **28 captures manuelles persistées** : fréquence moyenne des captures **83 286 échantillons/s** (min/max **82 732 / 83 325**), **0 overrun ADC**, **0 erreur DMA**, **0 erreur SD**, **0 échec HTTP**, **0 redémarrage** et **0 capture perdue**. Les 28 RAW ont également passé les contrôles d'en-tête et de CRC. La heap finale était de **118 392 octets** ; minimum interne **83 204 octets**, plus petit grand bloc libre observé **90 100 octets**. Le firmware a écrit **2 822 502 octets** sur SD pendant ce test. Une connexion WebSocket a été rétablie après une erreur transitoire. Le volume monté de la carte de 16 Go ne représente qu'environ **126 Mio** ; aucune modification de partition ni formatage n'a été effectué.

**Décodeur TP1 expérimental :** GPIO5 reçoit maintenant le bus via un diviseur passif provisoire. Sur le bus réel, l'essai long a reconstruit **82 télégrammes à checksum correct** et **90 acquittements** dans un journal SD de **41 334 octets** relu avec CRC correct, à **83,3 kéchantillons/s**, sans overrun ADC ni erreur DMA/SD. Le [fonctionnement et ses limites](docs/tp1-experimental.md) sont documentés séparément.

**Limites :** aucun front-end analogique KNX protégé, mesure VBUS, TP-UART ou décodage DPT n'est implémenté. Le journal TP1 est écrit progressivement sur SD avec un tampon RAM de 16 Kio et des points de synchronisation. L'application PC et les rapports restent futurs.

### Scope Web

Ouvrir `/scope`, cliquer sur **START ANALYSIS**, puis **ARM MANUAL** et **MANUAL TRIGGER** après remplissage du prétrigger. La capture s'affiche automatiquement avec 50 000 échantillons, min/max, fréquence mesurée, source RAW et répartition 35 000 / 15 000. L'axe temporel est calculé depuis les métadonnées de la capture : environ −420 ms / t = 0 / +180 ms à 83,3 kéchantillons/s. Les boutons indisponibles indiquent la condition requise. **STOP ANALYSIS** clôture la session. Avec GPIO5 flottant, le tracé ne représente que l'entrée ADC de test.

### LCD Live Scope V1

Pendant l’analyse TP1, le moniteur texte prend automatiquement la place du Scope live pour préserver le décodage ADC.

Sur l'écran **SCOPE**, le graphe LIVE lit une fenêtre récente du ring ADC et réduit chaque colonne à son **minimum et maximum**. Le buffer d'affichage contient 160 couples `uint16_t` (640 octets de données, 656 octets avec métadonnées) ; il ne duplique pas les 50 000 échantillons RAW. Le dessin est réparti en blocs de 16 colonnes pour laisser la priorité à l'ADC, aux Events, à la SD et au Web. Toucher la durée sous le graphe sélectionne **5, 10, 20, 50 ou 100 ms** ; toucher l'échelle bascule entre **FULL** (0–4095) et **AUTO**. Les valeurs sont **ADC RAW, sans calibration en volts**. Après un trigger, la capture figée reste consultable lorsque l'acquisition s'arrête.

Sur carte réelle, le rendu stabilisé tourne à **8 FPS** ; le test simultané LCD + Web + ADC + SD de **603 s** a mesuré **83 357 échantillons/s** en moyenne, **0 overrun ADC**, **0 erreur DMA/SD/HTTP**, **0 timeout Web** et **0 redémarrage**. Cinq captures manuelles ont été demandées pendant ce test ; les sept RAW de la session, captures pilotes incluses, ont passé les contrôles d'en-tête, CRC et statistiques. Le front-end analogique KNX protégé reste absent : **ne jamais connecter directement le bus KNX TP1 à GPIO5**.

### Events et API locale

Les routes Events existantes restent disponibles : `GET /api/events`, `GET /api/events/{id}` et `GET /api/events/{id}/capture`. Un Event encore présent dans le ring RAM peut charger son ancien RAW depuis la SD. La page `/sessions` et l'API Sessions ouvrent tout l'historique persistant. HTTP 410 indique un RAW indisponible et HTTP 404 un identifiant inconnu. Consultez le [format des captures, les routes Sessions et la reprise après reboot](docs/sessions-sd-v1.md).

### Architecture

Les liaisons en pointillés concernent exclusivement des fonctions **prévues**. L'entrée GPIO5 actuellement validée reçoit seulement un signal de test sûr, **jamais le bus KNX**.

~~~mermaid
flowchart LR
  Test["GPIO5 test signal<br/>hors bus KNX / away from KNX"] --> ADC["Continuous ADC + DMA<br/>VALIDÉ / VALIDATED"]
  ADC --> Ring["Ring + triggers<br/>VALIDÉS / VALIDATED"]
  Ring --> Scope["LCD + Web Scope<br/>VALIDÉ / VALIDATED"]
  Ring --> Event["Events RAM<br/>VALIDÉS / VALIDATED"]
  Event -. futur .-> Correlation["Corrélation protocole/physique<br/>PRÉVUE / PLANNED"]
  Bus["Bus KNX TP1"] -. futur .-> AFE["Protected analog front-end<br/>PRÉVU / PLANNED"]
  AFE -. futur .-> ADC
  Bus -. futur .-> UART["TP-UART<br/>PRÉVU / PLANNED"]
  UART -. futur .-> Decoder["KNX decoder<br/>PRÉVU / PLANNED"]
  Decoder -. futur .-> Correlation
  Event --> Writer["File d'un job / one job queue"]
  Writer --> SD["Sessions + RAW microSD<br/>VALIDÉS / VALIDATED"]
  SD --> Web["Web Sessions + API"]
  Event -. futur .-> Reports["PC HTML/PDF reports<br/>PRÉVUS / PLANNED"]
~~~

### Développement et compilation

Développement sous **Windows et VS Code**, avec **Arduino CLI**, **Arduino ESP32 core 3.3.11** et FQBN esp32:esp32:esp32c6. Installer les bibliothèques Arduino GFX Library for Arduino, ArduinoJson et WebSockets compatibles avec ce core. Le code utilise aussi les bibliothèques Wi-Fi, SD et Preferences du core ESP32. Adapter le port série si nécessaire ; COM10 est celui de la carte de développement testée.

~~~powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --output-dir build firmware/KNXAnalyzer
arduino-cli upload -p COM10 --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --input-dir build firmware/KNXAnalyzer
arduino-cli monitor -p COM10 -c baudrate=115200
~~~

Au premier démarrage, l'AP KNX-Analyzer-XXXX propose la configuration locale à http://192.168.4.1/. Deux profils Wi-Fi (principal et secours) sont enregistrés en NVS ; le retour AP est prévu si aucun ne se connecte. Aucun mot de passe n'est publié. La SD n'est jamais formatée ; le projet écrit uniquement sous `/knx-analyzer/`.

### Méthode et licence

Le développement progresse sur matériel réel : développement → compilation → flash → test sur carte → mesure → validation. Une fonction seulement compilée ou prévue n'est pas déclarée validée. **Aucune licence de réutilisation n'a encore été choisie.**

## English

> [!CAUTION]
> **⚠️ EXPERIMENTAL PROJECT — WORK IN PROGRESS**
> This project is currently a development prototype. The protected analog front-end intended for the KNX TP1 bus has not yet been implemented. **Never connect an ESP32-C6 GPIO or ADC input directly to a KNX TP1 bus.** This project is currently neither a KNX-certified device nor a finished commercial product.

### Project goal

KNX TP1 Analyzer aims to become a portable diagnostic analyzer for KNX TP1 installations. Its final goal goes beyond reading telegrams: it should observe protocol traffic, actual bus voltage, the TP1 analog waveform, communication errors, telegram repetitions, ACK / NAK / BUSY responses, voltage drops and physical-layer anomalies at the same time.

The intended core function is to **correlate** these observations: voltage disturbance → affected telegram → missing ACK → repetition → analog event capture. Ultimately, the tool should help diagnose power supply, wiring, intermittent contact, electrical interference, abnormal bus load and signal-quality problems that a basic telegram monitor cannot reveal.

### Planned capabilities

- **Protocol:** decode KNX TP1 telegrams, source and destination addresses, data, ACK / NAK / BUSY, repetitions, errors and traffic statistics.
- **Physical layer:** independently measure KNX bus voltage, min/max, drops, disturbances and waveform, with an oscilloscope and anomaly triggers. A **separate protected analog front-end** must still be designed.
- **Events:** retain samples around the trigger in RAM and store each capture on microSD; KNX context is planned.
- **HMI:** color touch screen, Start/Stop Analysis, Dashboard, Scope, event counter, system state and diagnostics.
- **Local Web UI:** Dashboard, Scope, Events, Sessions, Configuration and System on a PC, tablet or phone, with no mandatory cloud service.
- **microSD black box:** sessions and Event captures are stored on the card. The complete continuous ADC stream is not written.
- **PC application and reports:** future session import and HTML/PDF diagnostic reports with timeline, statistics, anomalies, telegrams, voltages and associated scope captures.

### Hardware and interfaces

The prototype uses the [Waveshare ESP32-C6-Touch-LCD-1.47](https://www.waveshare.com/esp32-c6-touch-lcd-1.47.htm) ([official documentation](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47)): ESP32-C6, 8 MB Flash, 1.47" 172 × 320 IPS capacitive touch display, Wi-Fi, BLE, microSD slot, battery management and USB-C.

| Hardware resource | Link | Project use |
| --- | --- | --- |
| Waveshare ESP32-C6-Touch-LCD-1.47 board | [Official product](https://www.waveshare.com/ESP32-C6-Touch-LCD-1.47.htm) · [documentation](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47) | Current validated prototype |
| Board pinout and manufacturer files | [Waveshare resources](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47/Resources-And-Documents) · [schematic PDF](https://files.waveshare.com/wiki/ESP32-C6-Touch-LCD-1.47/ESP32-C6-Touch-LCD-1.47-Schematic.pdf) | Check GPIO, touch, LCD and microSD connections |
| ESP32-C6 microcontroller | [Espressif datasheet](https://documentation.espressif.com/esp32-c6_datasheet_en.pdf) | Electrical limits, ADC and interfaces |
| Siemens BTM 117/12 PCBA / TP-UART2 | [Module](https://www.opternus.com/en/siemens/development-tools/tp-uart2-board-btm2-pcb) · [PCBA data sheet](https://www.opternus.com/fileadmin/_migrated/content_uploads/PCBA_UP117-12_datasheet_v5_2012-05-30_01.pdf) · [Siemens TP-UART documentation](https://sid.siemens.com/v/u/A6V11933794) | Future protocol path, not integrated |

The board, a USB-C cable, a microSD card and a test signal **isolated from KNX** are enough for Sessions V1. SD is required to start a session. No KNX analog front-end has been selected.

The planned **protocol** interface is a Siemens BTM / TP-UART, intended reference **5WG1 117-8AA12 PCBA BTM**. A [public technical data sheet for the BTM 117/12 PCBA](https://www.opternus.com/fileadmin/_migrated/content_uploads/PCBA_UP117-12_datasheet_v5_2012-05-30_01.pdf) describes its serial interface. This interface is **not integrated yet**. Analog bus measurement will use a **different**, protected front-end rated for KNX bus voltages; it has not been designed or validated.

| Pin | Prototype role | State |
| --- | --- | --- |
| GPIO5 | Fast Scope ADC input, currently tested away from the KNX bus | Validated |
| GPIO6 | Future slow VBUS measurement | Planned |
| GPIO16 | Future TX to TP-UART | Planned |
| GPIO17 | Future RX from TP-UART | Planned |
| GPIO7 | Future TP-UART SAVE signal | Planned |

No analog KNX bus wiring diagram has been validated or published.

### Current status

The validated **Event RAW Logger V2** is documented in
[`docs/event-raw-v2.md`](docs/event-raw-v2.md): continuous 83.3 kS/s ADC,
experimental D44 trigger, 100 ms PRE/POST, 12 x 8 KiB chunk pool, selective RAW
storage, robust finalization, and experimentally validated SD R1 recovery.

**Validated on real hardware:** Waveshare ESP32-C6, USB/Serial, LCD, touch, QMI8658A, Wi-Fi AP/STA, local Web UI, Dashboard, Scope, Events and Start/Stop. In the initial tests, GPIO5 was isolated from the KNX bus: ESP-IDF continuous ADC + DMA runs near **83.33 kSamples/s** with a **50,000-sample / 100 KB** ring, 70% before and 30% after the trigger, and rising, falling and manual triggers. GND/3V3 edges were tested physically. The LCD Dashboard and Events counter were visually validated.

**Sessions + SD V1:** START creates a persistent session and STOP closes it. RAM retains 24 metadata records and one complete RAW capture; SD retains the Event and RAW history. A session interrupted by reboot is marked `INTERRUPTED` while finalized captures remain readable. One 50-manual-capture session produced **50/50 valid RAW files**, including its oldest capture after RAM rotation; all 50 headers and CRCs were checked. Both a closed and an interrupted session remained accessible after reboot.

The corrected firmware completed **630 s** with **28 persisted manual captures**: average capture rate **83,286 samples/s** (min/max **82,732 / 83,325**), **0 ADC overruns**, **0 DMA errors**, **0 SD write errors**, **0 HTTP test failures**, **0 reboots**, and **0 lost captures**. All 28 RAW headers and CRCs were also verified. Final free heap was **118,392 bytes**; internal minimum **83,204 bytes**, lowest observed largest free block **90,100 bytes**. The firmware wrote **2,822,502 bytes** to SD in this test. One transient WebSocket error reconnected. The mounted volume on the 16 GB test card is only about **126 MiB**; no partitioning or formatting was performed.

**Experimental TP1 decoder:** GPIO5 now receives the bus through a provisional passive divider. The final real-bus test reconstructed **42 checksum-valid telegrams**, **46 acknowledgements** and **4 parity-error candidates** at **83.3 kSamples/s**, with no ADC overrun or DMA/SD error. See the [decoder scope and limits](docs/tp1-experimental.md).

**Limits:** the protected KNX analog front-end, VBUS measurement, TP-UART and DPT decoding are not implemented. The TP1 journal is written progressively to SD through a 16 KiB RAM buffer with periodic checkpoints. The PC application and reports remain future work.

### Web Scope

Open `/scope`, select **START ANALYSIS**, then **ARM MANUAL** and **MANUAL TRIGGER** once the pretrigger buffer is full. The capture appears automatically with 50,000 samples, min/max, measured rate, RAW source and the 35,000 / 15,000 split. The time axis uses capture metadata: roughly −420 ms / t = 0 / +180 ms at 83.3 kSamples/s. Disabled controls explain what is required. **STOP ANALYSIS** closes the session. With GPIO5 floating, the trace only shows the test ADC input.

### LCD Live Scope V1

During TP1 analysis, the text monitor automatically replaces Live Scope to preserve ADC decoding.

On the **SCOPE** screen, LIVE reads a recent window from the ADC ring and reduces each display column to its **minimum and maximum**. The display buffer holds 160 `uint16_t` min/max pairs (640 data bytes, 656 bytes including metadata); it does not duplicate the 50,000 RAW samples. Drawing is split into 16-column chunks to give ADC, Events, SD and Web priority. Tap the duration below the graph to select **5, 10, 20, 50 or 100 ms**; tap the scale to switch between **FULL** (0–4095) and **AUTO**. Values are **uncalibrated ADC RAW**, not volts. After a trigger, the frozen capture remains available once acquisition stops.

On real hardware, steady rendering runs at **8 FPS**; a **603 s** simultaneous LCD + Web + ADC + SD test measured **83,357 samples/s** on average, **0 ADC overruns**, **0 DMA/SD/HTTP errors**, **0 Web timeouts** and **0 reboots**. Five manual captures were requested during the test; all seven RAW files in the session, including pilot captures, passed header, CRC and sample-statistics checks. A protected KNX analog front-end is still absent: **never connect the KNX TP1 bus directly to GPIO5**.

### Events and local API

Existing Events routes remain available: `GET /api/events`, `GET /api/events/{id}` and `GET /api/events/{id}/capture`. An Event still in the RAM metadata ring can retrieve an older RAW from SD. The `/sessions` page and Sessions API provide the full persistent history. HTTP 410 means RAW unavailable and HTTP 404 means unknown ID. See the [capture format, Sessions API and reboot recovery](docs/sessions-sd-v1.md).

### Architecture

Dashed links represent **planned** functions only. The validated GPIO5 input currently receives safe test signals, **never the KNX bus**. The diagram above applies equally to this section.

### Development and build

Development uses **Windows and VS Code**, **Arduino CLI**, **Arduino ESP32 core 3.3.11**, and FQBN esp32:esp32:esp32c6. Install compatible Arduino libraries GFX Library for Arduino, ArduinoJson, and WebSockets. Wi-Fi, SD and Preferences come from the ESP32 core. Change the serial port as needed; COM10 is the tested development board.

~~~powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --output-dir build firmware/KNXAnalyzer
arduino-cli upload -p COM10 --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --input-dir build firmware/KNXAnalyzer
arduino-cli monitor -p COM10 -c baudrate=115200
~~~

On first boot, the KNX-Analyzer-XXXX AP offers local configuration at http://192.168.4.1/. Two Wi-Fi profiles (primary and backup) are stored in NVS; AP fallback is intended when neither connects. No password is published. The SD card is never formatted; the project writes only under `/knx-analyzer/`.

### Method and license

Development proceeds on real hardware: develop → compile → flash → board test → measure → validate. A feature that only compiles or is merely planned is not marked as validated. **No reuse license has been selected yet.**
