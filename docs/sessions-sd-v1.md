# Sessions et captures SD V1 / Sessions and SD captures V1

## Français

Une pression sur **START ANALYSIS** crée une session sur la microSD, puis démarre l'acquisition ADC existante. **STOP ANALYSIS** arrête l'acquisition, termine toute écriture en attente et clôt la session. Une session restée `RUNNING` au redémarrage devient `INTERRUPTED`; les captures déjà finalisées restent lisibles. Sans SD montée, le démarrage d'une session échoue explicitement.

L'application n'écrit que sous `/knx-analyzer/` et ne formate jamais la carte. Chaque session possède un identifiant unique sans dépendre d'une heure Internet :

```text
/knx-analyzer/sessions/<session-id>/
  session.json
  events.jsonl
  captures/event-000001.bin
  captures/event-000002.bin
```

`session.json` contient l'état, les temps depuis le démarrage (la date reste `null` sans horloge fiable), les compteurs d'événements et d'erreurs, la fréquence ADC, le matériel et la version du firmware. Chaque ligne de `events.jsonl` décrit un événement et indique si son RAW a été persisté. Les métadonnées de session sont écrites via un fichier temporaire et un renommage. La récupération au démarrage peut utiliser la sauvegarde temporaire. Une interruption pendant l'écriture du RAW peut laisser un fichier `.part`; seul un `.bin` finalisé est considéré comme capture disponible.

Le ring ADC reste à 50 000 échantillons, avec 35 000 avant et 15 000 après le trigger. La RAM conserve 24 métadonnées d'Events et un seul RAW complet. À la fin d'une capture, l'ADC est arrêté et une file d'un job écrit le RAW figé sur SD par blocs de 512 échantillons, sans deuxième tampon RAW de 100 Ko. Le démarrage, le réarmement et l'effacement du Scope sont suspendus pendant cette écriture. Si l'écriture échoue, les compteurs d'erreur augmentent et l'Event indique que son RAW n'a pas été persisté. La SD fournit l'historique durable; le flux ADC continu complet n'y est jamais enregistré. Pendant que l'ADC est armé, les lectures SD potentiellement longues de l'API Sessions répondent HTTP 409 (`adc_busy_retry`). La page Web attend la fin de la capture pour reprendre la lecture de l'historique. Cela préserve le service du DMA.

### Format RAW version 1

Tous les entiers sont non signés et **little endian**. L'en-tête occupe exactement **48 octets**, suivi de `sample_count` valeurs ADC `uint16_t` brutes. Le CRC-32 IEEE (`0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`) porte uniquement sur les octets des échantillons.

| Offset | Taille | Champ |
| ---: | ---: | --- |
| 0 | 8 | Magic ASCII `KNXADC1` puis `0x00` |
| 8 | 2 | Version (`1`) |
| 10 | 2 | Taille d'en-tête (`48`) |
| 12 | 4 | Event ID |
| 16 | 4 | Fréquence d'échantillonnage mesurée, Hz |
| 20 | 4 | Nombre d'échantillons |
| 24 | 4 | Index du trigger |
| 28 | 4 | Nombre d'échantillons avant trigger |
| 32 | 4 | Nombre d'échantillons après trigger |
| 36 | 2 | Minimum ADC brut |
| 38 | 2 | Maximum ADC brut |
| 40 | 4 | CRC-32 des échantillons |
| 44 | 4 | Réservé (`0`) |

Une capture actuelle contient 50 000 échantillons et mesure **100 048 octets**. Les données ADC flottantes de GPIO5 sont du bruit de test, **pas une mesure KNX**.

### API locale

| Route | Contenu |
| --- | --- |
| `GET /api/sessions` | Liste des sessions SD |
| `GET /api/sessions/{id}` | Métadonnées de session |
| `GET /api/sessions/{id}/events` | Liste des Events persistés |
| `GET /api/sessions/{id}/events/{eventId}` | Métadonnées d'un Event |
| `GET /api/sessions/{id}/events/{eventId}/capture` | Forme d'onde décimée avec min/max, lue à la demande |
| `GET /api/sessions/{id}/events/{eventId}/raw` | Fichier binaire RAW à télécharger |
| `GET /api/status` | État SD, capacité carte/volume, espace libre, file et compteurs d'erreur |

La page `/sessions` affiche les sessions et ouvre leurs Events. La forme d'onde et les API Events existantes indiquent `RAW: RAM`, `RAW: SD` ou `unavailable`. Une ancienne capture sortie du ring RAM peut ainsi rester consultable depuis la SD. Une capture non disponible renvoie HTTP 410; un identifiant inconnu renvoie HTTP 404. Le serveur ne charge que le RAW demandé. L'état System distingue la capacité physique de la carte de la taille du volume monté; le volume utilisable peut être bien plus petit. La carte de test de 16 Go expose actuellement un volume monté d'environ 126 Mio. Le projet ne le repartitionne et ne le formate pas.

> [!CAUTION]
> Aucun front-end analogique KNX protégé n'est connecté et aucune donnée KNX réelle n'est décodée. Ne jamais connecter directement le bus KNX TP1 à un GPIO ou à l'ADC ESP32.

### Validation sur matériel

Sur la Waveshare avec Arduino ESP32 core 3.3.11, une session a conservé **50/50 captures manuelles** : 50 fichiers de 100 048 octets, en-têtes et CRC valides, et première capture relue depuis SD après rotation RAM. Une session fermée et une session de deux Events laissée ouverte ont survécu au redémarrage ; cette dernière est devenue `INTERRUPTED`.

Le stress final a duré **630 s** avec **28/28 captures persistées**. Le ring RAM indiquait 24 métadonnées, 1 RAW en RAM et 23 anciens RAW accessibles depuis SD. Débit moyen des 28 captures : **83 286 échantillons/s** (min/max **82 732 / 83 325**). Compteurs : **0 overrun ADC, 0 erreur DMA, 0 erreur d'écriture SD, 0 échec HTTP de test, 0 reboot**. Heap libre finale **118 392 octets**, minimum interne **83 204 octets**, minimum observé du plus grand bloc libre **90 100 octets**. Octets écrits par le firmware pendant ce stress : **2 822 502**. Une erreur WebSocket transitoire a été suivie d'une reconnexion. La métrique HTTP du firmware compte les réponses 4xx/5xx ; aucun timeout HTTP distinct n'est actuellement compté par le firmware.

Une révision intermédiaire avait subi des overruns lors de l'ouverture d'un ancien RAW SD pendant l'ADC. La restriction HTTP 409 a été ajoutée, puis le stress complet ci-dessus a été refait sans overrun. Le test ciblé d'une telle lecture pendant acquisition a retourné HTTP 409 sans perte ADC.

## English

**START ANALYSIS** creates a microSD session and starts the existing ADC acquisition. **STOP ANALYSIS** stops acquisition, finishes any pending write, and closes the session. A session left `RUNNING` across reboot becomes `INTERRUPTED`; completed captures remain readable. Starting a session fails explicitly when the SD card is unavailable.

The application writes only below `/knx-analyzer/` and never formats the card. Each session has a unique ID independent of Internet time and contains `session.json`, one JSON object per line in `events.jsonl`, and versioned binary captures in `captures/`. An interrupted RAW write may leave a `.part` file; only a finalized `.bin` is available for reading. Session metadata uses temporary and backup files to reduce the risk of losing it during a reset.

Potentially long Sessions API reads return HTTP 409 (`adc_busy_retry`) while ADC acquisition is armed; the Web UI resumes SD history after the capture. This protects DMA service time.

The ADC ring still contains 50,000 samples, with 35,000 before and 15,000 after the trigger. RAM retains 24 Event metadata records and one complete RAW capture. After a capture, ADC acquisition is stopped while a single queued job writes the frozen RAW to SD in 512-sample chunks. No second 100 KB RAW buffer is allocated. Rearming waits for the write to finish. SD holds the durable history, not the entire continuous ADC stream.

The **version 1 RAW format** is a 48-byte little-endian header followed by raw `uint16_t` ADC samples. The field offsets and CRC-32 definition are in the table above. Current files contain 50,000 samples and are 100,048 bytes long. The [local API table](#api-locale) also applies in English: `/sessions` lists and opens stored sessions, and a requested waveform is read from SD on demand. Old Events remain available after the RAM metadata ring rotates. A failed write is recorded as a non-persisted RAW with error counters.

### Hardware validation

On the Waveshare board with Arduino ESP32 core 3.3.11, one session persisted **50/50 manual captures**. All 50 files were 100,048 bytes with valid headers and CRCs; the oldest RAW remained readable from SD after RAM rotation. A closed session and a two-Event session left open survived reboot; the latter became `INTERRUPTED`.

The final stress ran **630 seconds** with **28/28 captures persisted**. RAM held 24 metadata records and one RAW; 23 older captures were accessible from SD. The 28 captures averaged **83,286 samples/s** (min/max **82,732 / 83,325**), with **zero ADC overruns, DMA errors, SD write errors, HTTP test failures, and reboots**. Final free heap was **118,392 bytes**, internal minimum **83,204 bytes**, and minimum observed largest free block **90,100 bytes**. The firmware wrote **2,822,502 bytes** during the run. One transient WebSocket error reconnected. The firmware counts HTTP 4xx/5xx responses, but has no separate timeout counter.

An intermediate build overran the ADC while reading an old SD RAW during acquisition. The HTTP 409 guard was added and the complete stress was repeated with zero overruns. A targeted read while ADC was armed returned HTTP 409 without ADC loss.

> [!CAUTION]
> No protected KNX analog front-end is connected and no real KNX traffic is decoded. Never connect the KNX TP1 bus directly to an ESP32 GPIO or ADC input. A floating GPIO5 produces test noise, not a KNX measurement.
