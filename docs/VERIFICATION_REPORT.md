# Rapport de vérification STGS — version vitrine

Ce document conserve les contrôles exécutés sur la version livrée du projet. Il s'agit d'une
validation de qualité logicielle pour un projet de démonstration C++/systèmes ; elle ne constitue
ni une qualification avionique/spatiale, ni une certification de sûreté.

## Environnement de contrôle

- Linux 6.18.35 x86_64
- GCC 14.2.0
- Clang 17.0.0
- CMake 3.31.6
- C++20

## Compilation stricte

Deux compilateurs ont construit l'ensemble des cibles Release avec les warnings du projet traités
comme erreurs :

- GCC 14.2.0 : OK ;
- Clang 17.0.0 : OK.

Le jeu de warnings inclut notamment `-Wall`, `-Wextra`, `-Wpedantic`, `-Wshadow`,
`-Wconversion`, `-Wsign-conversion` et `-Wformat=2`.

## Tests unitaires et d'intégration

La suite contient 20 tests et passe sous GCC comme sous Clang. Elle couvre notamment :

- vecteur de référence CRC-32/ISO-HDLC ;
- encode/decode STGS et rejets de trames malformées ;
- rejet des températures non finies ;
- extraction TCP fragmentée et resynchronisation ;
- payloads STGA message/signal et métadonnées signal invalides ;
- filtre moving-average et projection sinusoïdale ;
- assainissement des contrôles ANSI reçus ;
- lecture/écriture STGF et bornes de tailles ;
- hystérésis du moniteur de santé ;
- backpressure de la file bornée ;
- diagnostic de ports limité au loopback ;
- TCP loopback réel avec fragmentation et fermeture immédiate ;
- arrêt coopératif de `NetworkServer` pendant `poll()` ;
- UDP loopback réel.

## Sanitizers

- AddressSanitizer + UndefinedBehaviorSanitizer : 20/20 tests, OK ;
- détection de fuites ASan activée : OK ;
- ThreadSanitizer : 20/20 tests, aucune data race détectée dans la suite, OK.

ASan/UBSan et TSan utilisent volontairement des builds séparés.

## Analyse statique complémentaire

`clang++ --analyze` a été exécuté en parallèle sur toutes les unités de production sous `src/` et
`apps/`, y compris les nouveaux sous-modules de la station et du simulateur, sans diagnostic remonté.
Le benchmark reste validé par compilation stricte et exécution de contrôle, mais n'est pas inclus
dans ce passage d'analyseur statique.

## Découpage modulaire

La révision finale a supprimé les principaux fichiers monolithiques sans changer le contrat public :

- `apps/ground_station.cpp` est désormais un `main` minimal ; CLI, signaux POSIX, export, pipeline,
  workers, entrée réseau/replay et traitement STGA sont séparés par responsabilité ;
- `apps/satellite_simulator.cpp` délègue CLI, sockets, génération de trames et orchestration à des
  modules dédiés ;
- `NetworkServer` est séparé en cycle de vie commun, réception TCP et réception UDP ;
- la suite de tests est répartie entre codec, STGA/signal, replay/santé et réseau, avec un support
  commun et un runner minimal.

Après découpage, aucun fichier `.cpp` ou `.hpp` de production/tests ne dépasse 300 lignes dans la
révision livrée ; les fichiers les plus longs restent les parseurs CLI et composants déjà fortement
cohésifs. Le replay de référence produit exactement le même CSV avec l'ancien binaire et la version
refactorée, ce qui fournit un contrôle de non-régression fonctionnelle supplémentaire.

## Scénario réseau réel

Un scénario local TCP a été exécuté avec :

- station en `--auto-port 9450:9454` ;
- diagnostic des cinq ports ;
- découverte du listener depuis le simulateur ;
- message STGA ;
- deux blocs de signal sinusoïdal bruité avec filtre `sine-projection` ;
- 1 500 trames de télémétrie à 500 trames/s ;
- quatre décodeurs et deux files bornées à 256 entrées ;
- arrêt final par SIGINT.

Résultat station : 1 503 trames reçues, 1 503 décodées, 0 rejet, 1 503 écrites,
0 erreur applicative. Le message a été affiché et les deux signaux ont produit leurs sparklines et
métriques. Les compteurs périodiques du pipeline ont été visibles pendant le trafic.

## Déterminisme et formats de sortie

Un replay déterministe de 1 000 trames a été exporté avec 1 puis 8 workers. Les deux CSV sont
identiques octet pour octet (`cmp`). Un export JSON du même replay est accepté par
`python3 -m json.tool`.

## Benchmark de contrôle

Le benchmark Release a traité 20 000/20 000 trames, payload 256 octets, avec quatre workers.
Le débit chiffré dépend fortement du matériel et de l'environnement : il n'est pas présenté comme
une garantie contractuelle de performance.

## Limites assumées

- le protocole STGS/STGA est un format de démonstration, pas CCSDS ;
- le CRC détecte les corruptions accidentelles et n'apporte pas d'authenticité cryptographique ;
- la découverte de ports est volontairement bornée à IPv4 loopback et ne prétend pas identifier le
  service distant ;
- le traitement de signal illustre génération, bruit et filtrage numérique, pas une chaîne RF ;
- les seuils de santé sont des valeurs de démonstration configurables, pas des seuils mission.
