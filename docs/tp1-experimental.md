# Décodeur TP1 expérimental — jalon temps réel

Le firmware reçoit uniquement le signal analogique du diviseur expérimental sur GPIO5. Il n'émet jamais sur KNX. Ce montage reste provisoire et n'est pas le front-end protégé prévu pour le projet.

## Couches séparées

- **Physique TP1 :** impulsion descendante dominante pour le bit 0, 9 600 bit/s, environ 35 µs par impulsion. Seuils ADC expérimentaux `1600/1700 RAW` avec hystérésis, propres au diviseur actuel.
- **Caractère :** start 0, huit bits LSB d'abord, parité paire, stop 1. Chaque candidat conserve ses octets tels que reconstruits et les comptes d'erreurs de parité/timing.
- **Télégramme TP1 :** regroupement par écart entre caractères. XOR de tous les octets égal à `0xFF` pour une trame à checksum correct. Pour le format standard, longueur totale `8 + (octet_routage & 0x0F)`. Les champs certains sont source, destination, type d'adresse, hop count et longueur TP.
- **Acquittement de liaison :** `CC`, `0C`, `C0` sont des caractères de liaison reconnus. Un télégramme au checksum correct dont la sémantique n'est pas décodée reste `VALID_UNKNOWN`.
- **cEMI / TP-UART :** aucun de ces formats n'est injecté dans le décodeur analogique. Aucun DPT ni configuration ETS n'est interprété.

Sources : [outil TP1 présenté par KNX Association](https://www.knx.org/projects/knx-tp-telegram-visualiser), [description détaillée des octets sur le bus et de leur différence avec cEMI](https://marcdahl.dk/knx/), [fiche Siemens TP-UART, interface distincte](https://sid.siemens.com/api/khub/documents/2~uRRw4wGcDfAiXOoB~OVA/content).

## Journal et limites

Chaque candidat porte `monotonic_us`, `date_time:null` tant qu'aucune horloge n'est disponible, classification, `raw_hex`, erreurs et champs génériques si leur structure est vérifiée. Le journal de session est `tp1-candidates.jsonl`. Les lignes passent par un tampon RAM borné de 16 Kio et sont écrites progressivement sur SD par blocs de 512 octets. Le fichier est ouvert avant le démarrage ADC et synchronisé périodiquement ; à l’arrêt, une relecture vérifie taille, lignes et CRC. `journal_drops` et `queue_drops` signalent toute perte. Les captures analogiques de 100 Kio ne sont pas déclenchées automatiquement par les télégrammes normaux.

Le Live Scope LCD est suspendu pendant l'analyse ; la page Scope affiche un moniteur texte mis à jour à 1 Hz. Le décodage ADC reste prioritaire. Les seuils, le regroupement et la classification doivent être revalidés avec le futur front-end protégé et sous trafic plus varié. Les trames Synco / LTE-Mode dont l'interprétation n'est pas connue restent `VALID_UNKNOWN` si leurs contrôles physiques et leur checksum sont corrects.

## Dernière validation sur bus réel

Pendant un essai de 55 s environ avec appuis manuels, le firmware final a compté **3 148 impulsions, 476 caractères et 92 candidats** : **42 `VALID_UNKNOWN`** (tous les 42 avec XOR `0xFF` et champs génériques présents), **46 `VALID_KNOWN`** (`CC`, acquittement positif) et **4 `INVALID_PARITY`** (8 erreurs de caractère au total). Il a mesuré **83 302 échantillons/s** en fin d'essai, **0 overrun**, **0 échantillon ADC invalide**, **0 erreur de lecture DMA**, **0 perte de file/journal**, **0 erreur SD**. Le journal de **21 032 octets** a signalé `saved:true` avant la fermeture de session. Heap libre observée **87 668 octets**, minimum **71 468 octets**. Aucun reboot/watchdog n'a été observé dans ce journal série. Le moniteur LCD est implémenté et le Live Scope mesuré à **0 FPS** durant l'analyse ; une confirmation visuelle de ce nouvel écran reste à faire.

## Consolidation du journal progressif

Le fichier `tp1-candidates.jsonl` est ouvert avant l'ADC, alimenté par blocs de 512 octets depuis un tampon RAM de 16 Kio, puis synchronisé régulièrement. Le pool DMA ADC a été porté à 48 Kio sans changer les seuils ou le décodage TP1. À l'arrêt, le firmware relit le fichier depuis SD et compare octets, nombre de lignes et CRC. Après un redémarrage ultérieur, une session de test fermée est restée accessible par l'API Sessions.

Un test SD isolé, avec **300 lignes synthétiques explicitement marquées** et 72 candidats réels apparus dans la même session, a relu **109 592 octets / 372 lignes**, CRC correct, sans perte de file, overrun ADC, erreur DMA ou SD. Le code générant ces lignes synthétiques a été retiré du firmware de production. Un essai séparé de **140 candidats réels** a relu **32 114 octets**, CRC correct, mais compté 8 overruns avec l'ancien pool DMA de 32 Kio. Le firmware de production avec pool DMA de 48 Kio a ensuite tourné environ **5 minutes** sur bus réel : **180 candidats**, dont **82 télégrammes à checksum correct**, **90 ACK**, **19 erreurs de parité** et **1 erreur de checksum**. Le journal SD a été fermé puis relu intégralement : **41 334 octets / 180 lignes**, CRC correct, contre **32 Kio** pour l'ancienne limite. Compteurs finaux : **0 overrun ADC**, **0 erreur DMA**, **0 erreur SD**, **0 perte de file ou de journal** ; **18 synchronisations**. Le débit ADC final en cours d'acquisition était **83 336 échantillons/s**. Heap libre après arrêt **88 168 octets**, minimum interne **51 544 octets**. Aucun redémarrage/watchdog n'a été observé. Une coupure USB pendant une session active a ensuite laissé **23 945 octets synchronisés / 104 lignes JSON valides** sur SD, sans ligne invalide ni fragment final. Au redémarrage, la session a été marquée `INTERRUPTED` et est restée accessible. Une nouvelle session a enregistré **21 138 octets / 92 lignes**, puis a été fermée et relue avec CRC correct, sans erreur ADC, DMA ou SD. La coupure n'a toutefois pas été synchronisée avec une écriture SD en cours : une coupure exactement au milieu d'une écriture reste un futur test de robustesse.
