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
- **Événements :** conserver en RAM les échantillons avant et après un trigger, créer un Event, puis lui associer ultérieurement le contexte KNX et une sauvegarde sélective sur microSD.
- **HMI :** écran couleur tactile, Start/Stop Analysis, Dashboard, Scope, compteur d'événements, état système et diagnostics.
- **Web local :** Dashboard, Scope, Events, Sessions, Configuration et System accessibles depuis PC, tablette ou smartphone, sans cloud obligatoire.
- **Black box microSD :** sauvegarde future d'événements, sessions et captures sélectionnés. Le flux ADC brut complet ne doit pas être écrit en permanence.
- **Application PC et rapports :** import futur d'une session et rapport HTML/PDF avec chronologie, statistiques, anomalies, télégrammes, tensions et captures associées.

### Matériel et interfaces

Le prototype utilise la [Waveshare ESP32-C6-Touch-LCD-1.47](https://www.waveshare.com/esp32-c6-touch-lcd-1.47.htm) ([documentation officielle](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47)) : ESP32-C6, 8 Mo de Flash, écran IPS tactile capacitif 1,47" de 172 × 320 pixels, Wi-Fi, BLE, emplacement microSD, gestion batterie et USB-C.

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

**Implémenté et validé sur carte réelle :** support Waveshare ESP32-C6, USB/Serial, LCD, tactile capacitif, QMI8658A, provisioning Wi-Fi AP, connexion STA, serveur Web local, Dashboard responsive, Web Scope, API REST et Start/Stop depuis Web et LCD. Sur GPIO5 isolé du bus KNX : ADC continu ESP-IDF + DMA à environ **83,33 kéchantillons/s**, ring buffer de **50 000 échantillons (100 Ko)**, **35 000 avant trigger / 15 000 après** (70 % / 30 %), triggers montant, descendant et manuel, Scope LCD et Web. Les fronts GND/3V3 ont été testés physiquement (brut 0 à 3438 ; moyenne montant environ 27 → 3256, descendant environ 3256 → 396). La microSD a été détectée, sans écriture Events.

Le test de coexistence ADC + DMA + LCD + tactile + Wi-Fi + Web d'environ **10 minutes** a mesuré près de 83 333 échantillons/s, **zéro overrun ADC**, **zéro erreur de lecture DMA**, aucun redémarrage spontané ni watchdog. Environ 160 544 octets de heap étaient libres après arrêt ; le minimum observé sous charge était d'environ 124 908 octets. Une requête HTTP a expiré isolément ; le serveur a continué à fonctionner.

**Prévu, non implémenté ou non validé :** front-end analogique KNX protégé, mesure réelle du bus, VBUS sur GPIO6, TP-UART, décodage des télégrammes, ACK / NAK / BUSY, corrélation protocole/physique, classification d'anomalies, Events en RAM, stockage Events et Sessions sur SD, application PC, rapports et validation terrain complète.

### Architecture

Les liaisons en pointillés concernent exclusivement des fonctions **prévues**. L'entrée GPIO5 actuellement validée reçoit seulement un signal de test sûr, **jamais le bus KNX**.

~~~mermaid
flowchart LR
  Test["GPIO5 test signal<br/>hors bus KNX / away from KNX"] --> ADC["Continuous ADC + DMA<br/>VALIDÉ / VALIDATED"]
  ADC --> Ring["Ring + triggers<br/>VALIDÉS / VALIDATED"]
  Ring --> Scope["LCD + Web Scope<br/>VALIDÉ / VALIDATED"]
  Ring -. futur .-> Event["Events + correlation<br/>PRÉVUS / PLANNED"]
  Bus["Bus KNX TP1"] -. futur .-> AFE["Protected analog front-end<br/>PRÉVU / PLANNED"]
  AFE -. futur .-> ADC
  Bus -. futur .-> UART["TP-UART<br/>PRÉVU / PLANNED"]
  UART -. futur .-> Decoder["KNX decoder<br/>PRÉVU / PLANNED"]
  Decoder -. futur .-> Event
  Event -. futur .-> SD["Selective microSD storage<br/>PRÉVU / PLANNED"]
  Event -. futur .-> Reports["PC HTML/PDF reports<br/>PRÉVUS / PLANNED"]
~~~

### Développement et compilation

Développement sous **Windows et VS Code**, avec **Arduino CLI**, **Arduino ESP32 core 3.3.11** et FQBN esp32:esp32:esp32c6. Installer les bibliothèques Arduino GFX Library for Arduino, ArduinoJson et WebSockets compatibles avec ce core. Le code utilise aussi les bibliothèques Wi-Fi, SD et Preferences du core ESP32. Adapter le port série si nécessaire ; COM10 est celui de la carte de développement testée.

~~~powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --output-dir build firmware/KNXAnalyzer
arduino-cli upload -p COM10 --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --input-dir build firmware/KNXAnalyzer
arduino-cli monitor -p COM10 -c baudrate=115200
~~~

Au premier démarrage, l'AP KNX-Analyzer-XXXX propose la configuration locale à http://192.168.4.1/. Deux profils Wi-Fi (principal et secours) sont enregistrés en NVS ; le retour AP est prévu si aucun ne se connecte. Aucun mot de passe n'est publié dans ce dépôt. La SD existante n'est ni formatée ni utilisée pour enregistrer des captures.

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
- **Events:** retain samples before and after a trigger in RAM, create an Event, and later attach KNX context and selective microSD storage.
- **HMI:** color touch screen, Start/Stop Analysis, Dashboard, Scope, event counter, system state and diagnostics.
- **Local Web UI:** Dashboard, Scope, Events, Sessions, Configuration and System on a PC, tablet or phone, with no mandatory cloud service.
- **microSD black box:** future selective storage of important events, diagnostic sessions and captures. The complete raw ADC stream should not be written continuously.
- **PC application and reports:** future session import and HTML/PDF diagnostic reports with timeline, statistics, anomalies, telegrams, voltages and associated scope captures.

### Hardware and interfaces

The prototype uses the [Waveshare ESP32-C6-Touch-LCD-1.47](https://www.waveshare.com/esp32-c6-touch-lcd-1.47.htm) ([official documentation](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47)): ESP32-C6, 8 MB Flash, 1.47" 172 × 320 IPS capacitive touch display, Wi-Fi, BLE, microSD slot, battery management and USB-C.

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

**Implemented and validated on real hardware:** Waveshare ESP32-C6 support, USB/Serial, LCD, capacitive touch, QMI8658A, Wi-Fi AP provisioning, STA connection, local Web server, responsive Dashboard, Web Scope, REST API and Start/Stop from Web and LCD. On GPIO5 isolated from the KNX bus: ESP-IDF continuous ADC + DMA at approximately **83.33 kSamples/s**, a **50,000-sample (100 KB)** ring, **35,000 pre-trigger / 15,000 post-trigger** samples (70% / 30%), rising, falling and manual triggers, and LCD/Web Scope. Physical GND/3V3 edges were tested (raw 0 to 3438; rising mean about 27 → 3256, falling mean about 3256 → 396). The microSD card was detected; no Events were written to it.

An approximately **10-minute** ADC + DMA + LCD + touch + Wi-Fi + Web coexistence test measured near 83,333 samples/s, **zero ADC overruns**, **zero DMA read errors**, and no spontaneous reboot or watchdog. About 160,544 bytes of heap remained after stopping; the observed minimum under load was about 124,908 bytes. One isolated HTTP request timed out, after which the server continued to work.

**Planned, not implemented or not validated:** protected KNX analog front-end, actual bus measurement, GPIO6 VBUS, TP-UART, telegram decoder, ACK / NAK / BUSY analysis, protocol/physical correlation, anomaly classification, RAM Events, SD Event and Session storage, PC application, reports and complete field validation.

### Architecture

Dashed links represent **planned** functions only. The validated GPIO5 input currently receives safe test signals, **never the KNX bus**. The diagram above applies equally to this section.

### Development and build

Development uses **Windows and VS Code**, **Arduino CLI**, **Arduino ESP32 core 3.3.11**, and FQBN esp32:esp32:esp32c6. Install compatible Arduino libraries GFX Library for Arduino, ArduinoJson, and WebSockets. Wi-Fi, SD and Preferences come from the ESP32 core. Change the serial port as needed; COM10 is the tested development board.

~~~powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --output-dir build firmware/KNXAnalyzer
arduino-cli upload -p COM10 --fqbn 'esp32:esp32:esp32c6:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB' --input-dir build firmware/KNXAnalyzer
arduino-cli monitor -p COM10 -c baudrate=115200
~~~

On first boot, the KNX-Analyzer-XXXX AP offers local configuration at http://192.168.4.1/. Two Wi-Fi profiles (primary and backup) are stored in NVS; AP fallback is intended when neither connects. No password is published in this repository. Existing SD contents are neither formatted nor used to save captures.

### Method and license

Development proceeds on real hardware: develop → compile → flash → board test → measure → validate. A feature that only compiles or is merely planned is not marked as validated. **No reuse license has been selected yet.**
