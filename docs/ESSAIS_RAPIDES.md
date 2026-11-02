# Quelques essais à faire avec STGS

Les commandes ci-dessous sont celles que j'utilise pour vérifier le comportement du programme. Toutes fonctionnent en local sous Linux. Aucune connexion extérieure n'est nécessaire.

## 1. Compiler et lancer les tests

Depuis la racine du dépôt :

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/stgs_tests
```

Il y a 20 cas de test dans `stgs_tests`, mais un seul test enregistré auprès de CTest. C'est la sortie de l'exécutable qui indique le détail des 20 cas.

## 2. Vérifier l'ordre de sortie avec plusieurs threads

```bash
./build/stgs_satellite_simulator --output-file demo.stgf --count 1000 --rate 0 --seed 424242
./build/stgs_ground_station --replay demo.stgf --decoder-threads 1 --output one.csv
./build/stgs_ground_station --replay demo.stgf --decoder-threads 8 --output eight.csv
cmp one.csv eight.csv
```

Si `cmp` retourne `0`, les deux fichiers sont identiques. C'est un contrôle simple mais utile : les décodeurs peuvent terminer dans un ordre différent et pourtant l'écriture reste ordonnée. Pour voir le mécanisme, ouvrir `GroundStationPipeline::writerLoop()` dans `apps/ground_station/GroundStationWorkers.cpp`.

Le script `bash scripts/verify_demo.sh` reprend ce scénario, lance les tests et contrôle aussi un export JSON si Python 3 est disponible.

## 3. Voir la station fonctionner en TCP

**Terminal 1** : lancer la station sur la loopback.

```bash
./build/stgs_ground_station --tcp --bind 127.0.0.1 --port 9000 \
    --decoder-threads 4 --output telemetry.csv
```

**Terminal 2** : envoyer de la télémétrie.

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
    --count 1000 --rate 500
```

Le second terminal se termine après l'envoi. La station, elle, reste à l'écoute jusqu'à `Ctrl+C`. Regarder les compteurs de trames reçues, décodées, rejetées et écrites et le fichier CSV.

On peut aussi envoyer un message texte :

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
    --message "Bonjour depuis le simulateur" --count 1
```

## 4. Envoyer un signal bruité

Redémarrer la station avec le filtre de projection, puis envoyer cinq blocs de mesures :

```bash
# Terminal 1
./build/stgs_ground_station --tcp --bind 127.0.0.1 --port 9000 \
    --signal-filter sine-projection --output signal.csv

# Terminal 2
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
    --signal --sample-rate 200 --signal-frequency 5 \
    --signal-amplitude 1.0 --signal-samples 256 \
    --noise-stddev 0.35 --count 5 --rate 2 --seed 1234
```

Le bruit est ajouté aux **échantillons du signal**, avant le calcul du CRC. Les trames sont donc valides. Le filtre connaît la fréquence nominale de 5 Hz : il ne fait pas de recherche de porteuse RF.

## 5. Tester les trames corrompues

Avec la station TCP en marche :

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 \
    --count 50 --rate 100 --seed 42 --corrupt 1
```

Ici, la modification a lieu **après** l'encodage et le calcul du CRC. Il est normal d'observer des rejets, mais pas forcément 50 rejets CRC : si le préfixe STGS ou la longueur est touché, l'extracteur TCP peut perdre un candidat pendant sa recherche de la prochaine trame.

## 6. Essayer UDP

```bash
# Terminal 1
./build/stgs_ground_station --udp --bind 127.0.0.1 --port 9001 --output udp.csv

# Terminal 2
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9001 --count 100
```

En UDP, le datagramme fournit une frontière de message. Le programme détecte toutefois les datagrammes trop gros avec `MSG_TRUNC`. En TCP, il faut au contraire retrouver les frontières des trames dans un flux d'octets.

## Si quelque chose ne fonctionne pas

- Vérifier que les exécutables existent dans `build/`.
- En TCP, vérifier que la station est démarrée avant le simulateur et que les deux utilisent le même port.
- Si le port 9000 est déjà utilisé, choisir un autre port dans les deux commandes.
- Pour isoler un problème réseau, revenir à la démonstration par fichier STGF : elle utilise le même décodeur sans socket.
