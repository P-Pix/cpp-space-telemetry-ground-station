# Pourquoi j'ai fait STGS

STGS signifie *Space Telemetry Ground Station*. C'est un projet personnel C++20, développé pour Linux, qui simule la réception et l'exploitation de données envoyées par un satellite.

Le thème spatial me plaît, mais l'intérêt du projet est surtout dans le fonctionnement interne. Je voulais quelque chose de plus concret qu'un simple exemple de socket : recevoir des octets, retrouver les trames, vérifier leur intégrité, les faire passer dans plusieurs threads et pouvoir rejouer exactement une série de mesures pour vérifier le comportement du programme.

## Le scénario

Un simulateur produit des trames de télémétrie. La station peut les recevoir en TCP, en UDP ou les relire dans un fichier binaire. Une fois la trame reconstituée, un décodeur contrôle ses champs et son CRC. La station conserve les résultats dans l'ordre d'arrivée et les exporte en CSV ou JSON.

J'ai ajouté deux types de contenu au-delà des mesures classiques : des messages texte et des blocs de signal sinusoïdal avec du bruit. Ça permet de montrer la différence entre **une trame abîmée**, qui doit être rejetée, et **une mesure bruitée contenue dans une trame valide**, qui doit être interprétée.

## Les choix que je trouve les plus instructifs

### TCP ne donne pas des messages complets

Une lecture de socket ne correspond pas forcément à une trame. J'utilise donc un extracteur par connexion, capable de conserver des octets incomplets et de reprendre après une structure manifestement invalide. C'est dans `src/NetworkServerTcp.cpp` et `src/FrameCodec.cpp`.

### Plusieurs threads, un résultat prévisible

Le décodage peut se faire sur plusieurs workers. Mais je ne voulais pas que changer `--decoder-threads` change l'ordre du CSV. Chaque candidat reçoit son numéro de séquence avant le traitement parallèle, puis un seul writer attend et écrit les résultats dans cet ordre. Le mécanisme se trouve dans `apps/ground_station/GroundStationWorkers.cpp`.

### Pouvoir reproduire un problème

Les captures STGF gardent les trames binaires complètes, CRC compris. La station peut donc relire les mêmes octets sans relancer le simulateur ou ouvrir de socket. C'est pratique pour comparer plusieurs réglages et isoler un problème de décodage.

## Ce que le projet montre, et ce qu'il ne montre pas

On y trouve du C++ moderne, des sockets POSIX, des files synchronisées, un petit protocole binaire, des tests réseau en loopback et du traitement numérique. Les choix sont visibles dans le code, et il y a des commandes pour les reproduire.

En revanche, **ce n'est pas un logiciel spatial opérationnel**. Le protocole STGS n'est pas CCSDS, il n'y a ni acquisition RF, ni chaîne de démodulation, ni mécanisme d'authentification, ni garantie de temps réel. Je préfère préciser ce périmètre plutôt que de présenter le simulateur comme un matériel qui n'existe pas.

## Pour voir le projet fonctionner

Le plus rapide est de lancer `bash scripts/verify_demo.sh` après compilation. Le script exécute les tests puis compare deux exports d'une même capture avec 1 et 8 threads de décodage.

Pour regarder les parties les plus intéressantes du code :

- [`FrameCodec.cpp`](../src/FrameCodec.cpp) : parsing, CRC et extraction de trames.
- [`NetworkServerTcp.cpp`](../src/NetworkServerTcp.cpp) : réception TCP et clients multiples.
- [`BlockingQueue.hpp`](../include/stgs/BlockingQueue.hpp) : transfert de données et arrêt des files.
- [`GroundStationWorkers.cpp`](../apps/ground_station/GroundStationWorkers.cpp) : décodage parallèle et restitution ordonnée.
- [`Replay.cpp`](../src/Replay.cpp) : lecture/écriture des captures.

Les [notes techniques](NOTES_TECHNIQUES.md) expliquent le raisonnement plus en détail, y compris les limites que je voudrais traiter ensuite.
