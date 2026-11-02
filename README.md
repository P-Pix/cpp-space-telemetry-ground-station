# Space Telemetry Ground Station (STGS)

Une petite station sol de télémétrie écrite en **C++20 sous Linux**.

L'idée de ce projet est de reproduire une chaîne de réception assez complète pour travailler sur des sujets que j'aime bien en C++ : les protocoles binaires, les sockets, le multithreading et la gestion des erreurs. Le satellite est simulé, mais les échanges passent par de vrais sockets TCP/UDP et le traitement est réalisé par le code C++ du dépôt.

Je me suis surtout intéressé à une question : **comment recevoir, décoder et enregistrer des trames sans perdre la maîtrise des données quand le réseau ou l'ordre d'exécution des threads se complique ?**

## Ce que fait le programme

- Un simulateur produit de la télémétrie (température, batterie, état), des messages texte ou des blocs de signal bruité.
- La station reçoit des trames en TCP ou UDP, ou les relit depuis une capture binaire STGF.
- Chaque trame STGS possède un en-tête versionné, une longueur contrôlée et un CRC-32.
- Plusieurs threads peuvent décoder les trames ; un seul thread remet les résultats dans l'ordre avant l'export CSV/JSON.
- Le programme fournit des diagnostics, un suivi de l'état de la station et quelques traitements numériques simples.

```text
Simulateur TCP/UDP --------> Réception réseau ---+
                                                 +--> File d'entrée --> Décodeurs C++ --> Résultats
Fichier de rejeu STGF -----> Lecture du fichier -+                                        |
                                                                                          v
                                                                              Réordonnancement
                                                                                          |
                                                                              CSV/JSON, état,
                                                                              texte et signaux
```

Il n'y a pas de matériel spatial derrière cette démonstration, et STGS n'implémente pas les protocoles CCSDS. C'est un projet personnel d'apprentissage et de conception logicielle, pas une station sol destinée à être déployée sur une mission.

## Essayer le projet

Il faut Linux, CMake (3.16 ou plus) et un compilateur C++20. Le programme utilise les API POSIX pour le réseau et les signaux.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### Première démo : un fichier de télémétrie

C'est le moyen le plus simple de commencer : pas de port à ouvrir, pas besoin de deux terminaux.

```bash
./build/stgs_satellite_simulator --output-file demo.stgf --count 1000 --rate 0 --seed 424242
./build/stgs_ground_station --replay demo.stgf --decoder-threads 4 --output mesures.csv
```

On peut ensuite comparer deux exécutions avec un nombre de threads différent :

```bash
./build/stgs_ground_station --replay demo.stgf --decoder-threads 1 --output un_thread.csv
./build/stgs_ground_station --replay demo.stgf --decoder-threads 8 --output huit_threads.csv
cmp un_thread.csv huit_threads.csv
```

`cmp` ne doit rien afficher et doit retourner `0`. Les workers peuvent finir dans le désordre, mais le fichier de sortie conserve l'ordre des trames. Un script fait ce contrôle avec les tests C++ :

```bash
bash scripts/verify_demo.sh
```

### Deuxième démo : en TCP sur la machine locale

Dans un premier terminal :

```bash
./build/stgs_ground_station --tcp --bind 127.0.0.1 --port 9000 --decoder-threads 4 --output mesures.csv
```

Dans un second :

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 --count 1000 --rate 500
```

La station s'arrête avec `Ctrl+C`. Il est aussi possible de passer en UDP, d'envoyer un message ou de simuler un signal bruité : les commandes correspondantes sont dans [les exemples](docs/ESSAIS_RAPIDES.md).

## Quelques points du code qui me semblent intéressants

| Sujet | Ce que j'ai mis en place | Code |
| --- | --- | --- |
| **Flux TCP** | Un tampon par client pour gérer les trames fragmentées ou regroupées dans un même `recv()` | [`NetworkServerTcp.cpp`](src/NetworkServerTcp.cpp), [`FrameCodec.cpp`](src/FrameCodec.cpp) |
| **Format binaire** | Des tailles bornées, une représentation réseau explicite, des erreurs de décodage typées et un CRC-32/ISO-HDLC | [`TelemetryFrame.hpp`](include/stgs/TelemetryFrame.hpp), [`FrameCodec.cpp`](src/FrameCodec.cpp) |
| **Threads** | Deux files de travail, des `condition_variable`, une fermeture qui réveille les threads et vide les éléments restants | [`BlockingQueue.hpp`](include/stgs/BlockingQueue.hpp) |
| **Ordre des résultats** | Un numéro de séquence avant décodage et un writer unique qui attend le résultat suivant | [`GroundStationWorkers.cpp`](apps/ground_station/GroundStationWorkers.cpp) |
| **Rejeu** | Un fichier STGF pour rejouer une capture dans la même chaîne de décodage | [`Replay.cpp`](src/Replay.cpp) |
| **Signal** | Génération de sinusoïdes bruitées, moyenne glissante et projection sur une fréquence connue | [`SignalProcessing.cpp`](src/SignalProcessing.cpp) |

La séparation réseau / décodage / écriture est un choix important ici. Par exemple, le thread qui reçoit les octets ne décide pas si une mesure est valide, et les décodeurs n'écrivent pas directement dans le CSV. C'est un peu plus de code qu'une boucle unique, mais les responsabilités sont plus faciles à tester.

## Tests effectués

Sur l'archive analysée le **2 novembre 2026**, la compilation Release avec GCC 14.2 est passée, ainsi que les **20 cas de test C++** (ils sont regroupés dans une seule entrée CTest). Un rejeu de 1 000 trames a produit 1 000 lignes décodées sans rejet ; les exports réalisés avec 1 et 8 workers étaient identiques octet pour octet. Un build Debug ASan/UBSan a également passé les tests.

Ce sont des vérifications locales, pas une qualification complète. Les commandes, l'environnement et ce qui n'a *pas* été testé sont dans [le compte rendu](docs/VERIFICATIONS_2026-11-02.md).

## Ce que je voudrais encore améliorer

La plupart des fonctions prévues pour cette version sont là, mais il reste des sujets importants avant d'imaginer un usage plus exigeant :

- Les files d'entrée et de sortie sont bornées, **pas la map de réordonnancement**. Un décodeur très en retard peut faire grossir le nombre de résultats en attente.
- Le serveur TCP ne fixe pas de quota global de clients ou de traitement par connexion.
- Le CRC détecte les modifications accidentelles ; il ne remplace ni une authentification ni un canal chiffré.
- Les benchmarks n'incluent ni le réseau ni l'écriture disque. Ils donnent des indications sur le pipeline, pas un débit garanti en conditions réelles.
- Le filtre de signal est un exercice de traitement numérique, pas une démodulation RF.

J'ai détaillé ces limites et quelques pistes de correction dans les [notes techniques](docs/NOTES_TECHNIQUES.md).

## Documentation

- [Présentation et périmètre](docs/PRESENTATION_DU_PROJET.md) — une lecture courte pour comprendre le projet.
- [Notes techniques](docs/NOTES_TECHNIQUES.md) — décisions de conception, points de code et compromis.
- [Essais rapides](docs/ESSAIS_RAPIDES.md) — commandes à lancer pour observer le fonctionnement.
- [Vérifications du 2 novembre 2026](docs/VERIFICATIONS_2026-11-02.md) — résultats et limites des tests locaux.
- [Documentation complète](docs/README_DOCUMENTATION.md) — guides plus détaillés et sources LaTeX/PDF déjà présents.

**Licence :** [MIT](LICENSE).
