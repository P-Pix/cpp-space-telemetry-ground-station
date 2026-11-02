# Vérifications du 2 novembre 2026

Cette note garde une trace des essais réalisés sur l'archive source utilisée pour préparer cette version documentaire. Elle précise ce qui a été exécuté, plutôt que de tout résumer à « tests OK ».

**Périmètre :** sources C++ du dépôt, sans modification de l'implémentation pendant cette reprise de documentation. Les résultats portent sur un environnement Linux isolé ; ils ne remplacent pas des tests sur une machine ou un réseau de production.

## Environnement

- Linux x86-64 ;
- GCC 14.2.0, CMake 3.31.6, C++20 ;
- compilation Release avec les warnings du projet traités comme erreurs ;
- second build Debug avec AddressSanitizer et UndefinedBehaviorSanitizer.

## Résultats

| Essai | Observation |
| --- | --- |
| Compilation Release | Toutes les cibles construites |
| `ctest --test-dir build --output-on-failure` | 1/1 cible CTest réussie |
| `./build/stgs_tests` | 20/20 cas internes réussis |
| Build ASan + UBSan, puis tests | 20/20 cas internes réussis |
| Génération d'un fichier STGF de 1 000 trames | 1 000 trames produites |
| Rejeu de cette capture avec 1 worker | 1 000 décodées, 0 rejet, 1 000 écrites |
| Rejeu de la même capture avec 8 workers | Même résultat |
| `cmp one.csv eight.csv` | Fichiers CSV identiques octet pour octet |
| Export JSON, puis `python3 -m json.tool` | JSON accepté par le parseur |

Une des commandes de contrôle du rejeu est :

```bash
./build/stgs_satellite_simulator --output-file demo.stgf --count 1000 --rate 0 --seed 424242
./build/stgs_ground_station --replay demo.stgf --decoder-threads 1 --output one.csv
./build/stgs_ground_station --replay demo.stgf --decoder-threads 8 --output eight.csv
cmp one.csv eight.csv
```

Le fichier CSV de référence obtenu lors de cette vérification avait l'empreinte SHA-256 suivante :

```text
59c8963dd025beba24fe51c31cd85c3dcd480bee31c374cb3eedc3f617c51dd5
```

Cette empreinte ne doit **pas** être utilisée comme valeur attendue pour toute nouvelle génération : les trames contiennent un horodatage dépendant de l'horloge du système. La comparaison valable ici est celle de deux relectures du **même** fichier STGF.

## Essai avec de vraies sockets TCP

Un test local sur `127.0.0.1` a fait passer un message texte STGA, cinq blocs de signal bruité et dix trames volontairement altérées après calcul du CRC. La station a affiché le message et les cinq blocs de signal.

Bilan observé : **15 candidats reconstitués, 6 trames décodées, 9 rejetées et 6 écrites**. Le simulateur avait pourtant envoyé 16 objets logiques. Ce n'est pas contradictoire : en touchant le magic ou une longueur, la corruption peut empêcher l'extracteur TCP de retrouver un candidat complet. Il serait donc faux d'annoncer « dix erreurs CRC » systématiques pour ce scénario.

## Benchmark : seulement une indication

Deux mesures ponctuelles ont été relevées sur 20 000 trames de payload 256 octets : environ 284 000 trames/s avec un worker, et 646 000 trames/s avec quatre. Le benchmark ne lit pas de socket et n'écrit pas de fichier. Il n'y a pas eu assez de répétitions ni de contrôle de l'environnement pour annoncer un gain stable ou un débit cible.

Le fichier [`PERFORMANCE_REPORT.md`](PERFORMANCE_REPORT.md) contient d'autres mesures de référence obtenues précédemment.

## Ce qui reste hors de cette vérification

Les jobs GitHub Actions existent dans le dépôt, mais leur exécution distante n'a pas été vérifiée ici. ThreadSanitizer, le fuzzing des entrées, une campagne de charge multi-clients et une revue de sécurité complète n'ont pas été relancés dans cette série de contrôles. Des essais TSan figurent dans un [rapport plus ancien](VERIFICATION_REPORT.md) : il faut distinguer ces deux périmètres.

Il reste notamment à caractériser la mémoire utilisée par la map de réordonnancement si une séquence est retardée, ainsi que l'équité entre clients TCP. Les détails sont dans [les notes techniques](NOTES_TECHNIQUES.md).

Cette note n'est ni une certification, ni une qualification spatiale, ni une preuve de temps réel. Elle sert à refaire les contrôles et à savoir ce qu'ils couvrent réellement.
