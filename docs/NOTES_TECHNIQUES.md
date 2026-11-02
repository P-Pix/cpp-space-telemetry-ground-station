# Notes techniques — ce qu'il y a derrière STGS

Ce document reprend les morceaux du code qui ont demandé le plus de réflexion sur la conception. Ce n'est pas une spécification de protocole spatial : c'est la description du fonctionnement actuel de STGS, avec les compromis que j'assume et ceux que je voudrais encore revoir.

## Vue d'ensemble

Le simulateur peut écrire un fichier STGF ou envoyer des octets en TCP/UDP. Dans les deux cas, la station aboutit à une suite de **candidats de trames**. Ils ne sont pas considérés comme valides avant le passage dans `decodeFrame()`.

```text
Socket TCP / socket UDP / fichier STGF
             |
             v
    Octets de trames candidates
             |
       file d'entrée
             |
       workers de décodage
       (format + CRC + champs)
             |
       file de résultats
             |
       writer unique
       (ordre de séquence)
          /    |     \
     santé   STGA   CSV/JSON
```

Cette distinction m'intéresse particulièrement : le transport n'a pas à décider si une température est plausible ou si un CRC est bon. Inversement, un décodeur ne devrait pas avoir besoin de savoir si les octets viennent du réseau ou d'une capture.

Les principales classes sont dans `src/` et `include/stgs/`. L'assemblage de l'application se trouve dans `apps/ground_station/`.

## 1. Le format binaire STGS

J'ai choisi un format volontairement petit, avec une version et des champs fixes. Ça permet de travailler sur des détails qui comptent dans le développement bas niveau : taille exacte d'un champ, boutisme, limites d'allocation et données malformées.

| Champ | Taille |
| --- | ---: |
| Magic ASCII `STGS` | 4 octets |
| Version | 1 octet |
| Identifiant satellite | 2 octets |
| Horodatage en millisecondes | 8 octets |
| Température `float32` | 4 octets |
| Batterie | 1 octet |
| Statut | 1 octet |
| Longueur du payload | 2 octets |
| Payload | 0 à 4 096 octets |
| CRC-32/ISO-HDLC | 4 octets |

L'en-tête occupe 23 octets, la plus petite trame en fait 27 et la plus grande 4 123. Les entiers sont stockés en ordre réseau (big-endian). Les constantes de taille et les offsets sont définis dans `include/stgs/TelemetryFrame.hpp` ; le codec évite d'utiliser des valeurs numériques éparpillées dans le parser.

Le CRC utilise la variante ISO-HDLC, avec le polynôme réfléchi `0xEDB88320`, une initialisation et un XOR final à `0xFFFFFFFF`. Le test de référence `123456789 -> 0xCBF43926` permet de vérifier qu'on parle bien de la même variante de CRC-32.

**Pourquoi contrôler les longueurs avant le CRC ?** Parce qu'une entrée venant du réseau n'est pas fiable. `decodeFrame()` vérifie les tailles, la version et la cohérence du message avant de lire ce qu'il ne devrait pas lire. Une longueur annoncée trop grande est rejetée avant toute allocation basée sur elle. Le résultat est un `std::variant<TelemetryFrame, FrameError>` : une trame invalide fait partie des cas attendus, ce n'est pas une exception C++.

**Ce que je renforcerais :** la sérialisation attend un `float` de 32 bits au format IEEE-754. La taille est vérifiée, mais il manque un contrôle explicite du caractère IEEE-754 (`std::numeric_limits<float>::is_iec559`) si l'on veut documenter sérieusement la portabilité.

Code : `include/stgs/TelemetryFrame.hpp`, `include/stgs/Crc32.hpp`, `src/FrameCodec.cpp`.

## 2. Pourquoi la réception TCP est plus délicate qu'elle en a l'air

Le piège classique est d'écrire du code qui suppose qu'un `recv()` rend une trame complète. TCP transporte un flux d'octets, pas des messages. Une trame peut donc arriver en trois morceaux ; à l'inverse, un seul appel peut récupérer plusieurs trames.

J'utilise un `StreamFrameExtractor` pour conserver ce qui reste à traiter. Il cherche le magic `STGS`, attend l'en-tête, examine la taille annoncée puis attend la fin du candidat. Si le préfixe ou la longueur est impossible, il tente de se resynchroniser. Chaque client TCP possède son propre extracteur : les fragments de deux connexions ne doivent surtout pas se mélanger.

Dans la boucle multi-client, une autre subtilité vient de `poll()`. Il faut que les indices de la liste de clients correspondent aux `pollfd` effectivement fournis à cet appel. Le serveur construit donc un instantané des clients, traite ce groupe, puis accepte les nouveaux clients après. Les clients de l'instantané sont parcourus en sens inverse pour permettre les suppressions. Le code lit aussi les dernières données disponibles même si `POLLHUP` est signalé.

En UDP, c'est différent : chaque datagramme est un candidat. La station utilise `MSG_TRUNC` pour ne pas accepter silencieusement une datagramme trop grande que le tampon aurait tronquée.

Code : `src/NetworkServerTcp.cpp`, `src/NetworkServerUdp.cpp`, `src/FrameCodec.cpp`.

**Limite actuelle :** cette boucle est suffisante pour les scénarios de test, mais elle ne plafonne pas le nombre total de clients et ne limite pas le temps consacré à une connexion très bavarde. Je ne prétendrais pas qu'elle résiste telle quelle à une charge arbitraire sur un réseau ouvert.

## 3. Faire travailler plusieurs threads sans changer le CSV

Les données entrantes sont placées dans une `BlockingQueue<RawWorkItem>`. Chaque élément contient ses propres octets : ce n'est pas un pointeur vers un tampon réseau réutilisé au prochain `recv()`.

J'utilise ici des objets propriétaires (`ByteVector`), et les vues comme `std::span` servent à lire temporairement des données dont la durée de vie est connue. C'est une manière d'éviter le piège d'un `std::string_view` ou d'un `span` conservé alors que le buffer d'origine a déjà disparu.

La file bloquante utilise un `std::mutex` et des `std::condition_variable`. L'attente repose sur un prédicat (file disponible **ou** fermée), ce qui gère les réveils intempestifs. Une fermeture réveille les producteurs et consommateurs ; les consommateurs peuvent encore vider les éléments qui étaient déjà présents.

Le problème suivant est l'ordre. Les workers ne mettent pas tous le même temps à décoder une trame. Si chacun écrit directement dans le CSV, l'ordre dépend de l'ordonnanceur. J'ai donc préféré :

1. affecter un numéro croissant dès l'entrée dans le pipeline ;
2. décoder en parallèle, sans écrire dans le fichier ;
3. recevoir les résultats dans une seconde file ;
4. confier l'écriture à un seul consommateur, qui attend la prochaine séquence.

Le writer garde temporairement les résultats arrivés trop tôt dans une `std::map`. Il n'écrit que `expectedSequence`, puis avance tant qu'il retrouve les séquences suivantes. Il traite aussi les rejets dans cet ordre, pour ne pas décaler les numéros.

Le test de rejeu `1 worker` contre `8 workers` vérifie une propriété visible : **à capture identique, le CSV est identique octet pour octet**. Cela ne rend pas l'exécution des threads déterministe ; cela rend la sortie ordonnée.

**Le vrai point faible est ici :** les deux files peuvent être bornées, mais la map du writer ne l'est pas indépendamment. Si une ancienne séquence prend beaucoup de retard, les résultats suivants s'accumulent. Il faudrait une fenêtre de séquences en vol ou un contrôle d'admission qui borne aussi ce stockage. Dire simplement « pipeline borné en mémoire » serait donc excessif.

Code : `include/stgs/BlockingQueue.hpp`, `apps/ground_station/GroundStationPipeline.cpp`, `apps/ground_station/GroundStationWorkers.cpp`.

## 4. L'arrêt du programme compte autant que son fonctionnement normal

Les workers et le writer sont gérés avec `std::jthread`. L'ordre de fermeture est important :

- arrêter l'admission des nouveaux candidats et fermer la file d'entrée ;
- laisser les décodeurs consommer le reste et attendre leur fin ;
- fermer la file des résultats ;
- laisser le writer finir les écritures avant de terminer.

Si une exception survient dans un thread, elle est mémorisée, les files sont fermées et elle peut être remontée après nettoyage, dans le contexte de l'appelant. C'est préférable à laisser un thread mourir silencieusement pendant que les autres restent bloqués sur une condition variable.

Côté réseau, `stop()` demande un arrêt coopératif au lieu de fermer depuis un autre thread un descripteur encore utilisé par `poll()`. Cela évite une famille de problèmes liés à la durée de vie et à la réutilisation des file descriptors. La réactivité de l'arrêt dépend toutefois du timeout de `poll()` : aucune garantie temps réel n'est associée à ce choix.

Code : `src/NetworkServer.cpp`, `apps/ground_station/GroundStationPipeline.cpp`, `include/stgs/BlockingQueue.hpp`.

## 5. Un même décodeur pour le réseau et les fichiers

Le format STGF contient les trames STGS complètes. Un enregistrement est précédé de sa longueur sur 32 bits ; le lecteur la valide avant de réserver un buffer et de lire les octets. Je voulais pouvoir reproduire une session sans que le simulateur ou le réseau deviennent une variable supplémentaire.

Les payloads STGS peuvent ensuite contenir une enveloppe applicative STGA. Pour le moment, elle sert aux messages texte et aux blocs de signal. Un payload qui n'est pas STGA reste un payload brut valide. La séparation évite de confondre **erreur de trame/CRC** et **erreur de contenu applicatif**.

Code : `src/Replay.cpp`, `src/ApplicationPayload.cpp`, `include/stgs/ApplicationPayload.hpp`.

## 6. Signal : distinguer une donnée bruitée d'une trame invalide

Le simulateur génère une sinusoïde et peut ajouter du bruit gaussien aux échantillons. Le CRC est calculé sur ces échantillons bruités : le message reste cohérent. L'option `--corrupt` intervient au contraire après l'encodage et le CRC : on teste alors le chemin de rejet.

La station propose une moyenne glissante et une projection sur une fréquence nominale déjà connue. On retrouve une composante liée au sinus, mais ce n'est ni une recherche automatique de fréquence ni un véritable récepteur radio. Le rapport signal/résidu calculé donne une indication, pas une mesure RF étalonnée.

Code : `apps/satellite_simulator/SimulatorFrameFactory.cpp`, `src/SignalProcessing.cpp`.

## 7. Ce que les tests permettent vraiment de dire

La suite couvre notamment le codec, les longueurs erronées, l'extraction TCP fragmentée, l'UDP local, le replay, la fermeture des files, les payloads STGA et le monitoring de santé. Des tests sur la loopback ouvrent de vraies sockets ; ce ne sont pas uniquement des mocks.

Un benchmark séparé mesure le décodage et la synchronisation des files sur des trames préparées en mémoire. Il ne mesure ni le réseau ni l'écriture disque. Les petits messages peuvent être dominés par la contention, donc augmenter le nombre de workers n'est pas automatiquement un gain.

Voir [les vérifications datées](VERIFICATIONS_2026-11-02.md), [`tests/`](../tests/) et [`PERFORMANCE_REPORT.md`](PERFORMANCE_REPORT.md).

## Et après ?

Si je devais pousser cette base plus loin, je commencerais par **borner le nombre de séquences en vol** puis par limiter l'admission et le traitement par client TCP. Ensuite viendraient des tests de charge sur une station complète, du fuzzing pour les entrées binaires et un contrat plus explicite sur les plateformes supportées. Les sujets comme l'authentification ou l'utilisation d'un protocole spatial standard devraient être décidés en fonction d'un vrai besoin, pas ajoutés pour faire une liste de fonctionnalités.

Ce sont des pistes, pas des fonctions que le dépôt prétend déjà fournir.
