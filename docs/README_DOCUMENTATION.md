# STGS - Documentation d'execution

Cette documentation correspond a la version modulaire de **Space Telemetry Ground Station (STGS)**.

L'objectif est de permettre une revue technique precise : pour chaque commande importante, les documents suivent le chemin d'execution depuis `main()` jusqu'aux fonctions C++ appelees, en explicitant les validations, les objets crees, les threads, les sockets, les fichiers, les effets de bord et les limites.

## Documents

- `STGS_Manuel_execution_complet.pdf` : manuel complet, 31 pages, regroupant les cinq volumes.
- `STGS_01_Architecture_et_execution.pdf` : architecture, STGS/STGA, CRC, TCP/UDP, pipeline, backpressure, ordre deterministe, signaux POSIX et terminal.
- `STGS_02_Commandes_station_sol.pdf` : commandes de `stgs_ground_station`, TCP, UDP, auto-port, replay, export, filtres et sante.
- `STGS_03_Commandes_simulateur.pdf` : commandes de `stgs_satellite_simulator`, trafic, decouverte, message, onde bruitee, corruption et capture STGF.
- `STGS_04_Diagnostics_build_tests_benchmark.pdf` : CMake, Makefile, CTest, sanitizers, port-check et benchmark.
- `STGS_05_Limites_invariants_entretien.pdf` : bornes, invariants, garanties/non-garanties et questions probables de revue technique.

Chaque PDF autonome possede une source `.tex` de meme nom. `STGS_Manuel_execution_complet.tex` assemble les cinq PDF autonomes avec `pdfpages`.

## Commandes principales couvertes

### Station sol

```bash
./build/stgs_ground_station --tcp --port 9000 --decoder-threads 4 --output telemetry.csv
./build/stgs_ground_station --udp --port 9000 --decoder-threads 4 --output telemetry.csv
./build/stgs_ground_station --tcp --auto-port 9000:9010 --output telemetry.csv
./build/stgs_ground_station --replay frames.stgf --replay-rate 100 --output replay.csv
./build/stgs_ground_station --tcp --port 9000 --output telemetry.json
./build/stgs_ground_station --tcp --port 9000 --signal-filter moving-average --filter-window 5
./build/stgs_ground_station --tcp --port 9000 --signal-filter sine-projection
```

### Simulateur

```bash
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 --count 1000 --rate 500
./build/stgs_satellite_simulator --udp --host 127.0.0.1 --port 9000 --count 1000 --rate 500
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --discover-ports 9000:9010 --count 100
./build/stgs_satellite_simulator --tcp --host 127.0.0.1 --port 9000 --message "Bonjour STGS"
./build/stgs_satellite_simulator --output-file frames.stgf --count 1000 --seed 42
```

Le volume 3 contient egalement la commande complete de generation d'une onde sinusoidale bruitee et le detail mathematique de sa generation/filtration.

### Build, tests, diagnostic et benchmark

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTGS_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel 8
make build
make test
make asan-test
make tsan-test
./build/stgs_port_check --host 127.0.0.1 --ports 9000:9010 --timeout-ms 200
make message-demo HOST=127.0.0.1 PORTS=9000:9010
make signal-demo HOST=127.0.0.1 PORTS=9000:9010
./build/stgs_benchmark_decode --frames 200000 --payload-size 256 --decoder-threads 4 --seed 123456789
```

## Recompiler les sources LaTeX

Depuis ce dossier :

```bash
latexmk -pdf STGS_01_Architecture_et_execution.tex
latexmk -pdf STGS_02_Commandes_station_sol.tex
latexmk -pdf STGS_03_Commandes_simulateur.tex
latexmk -pdf STGS_04_Diagnostics_build_tests_benchmark.tex
latexmk -pdf STGS_05_Limites_invariants_entretien.tex
latexmk -pdf STGS_Manuel_execution_complet.tex
```

La compilation du manuel complet suppose que les cinq PDF autonomes sont présents dans le même dossier.

## Point important pour une revue technique

La documentation distingue volontairement les fonctionnalites proches mais semantiquement differentes :

- `--auto-port` cherche un port **libre pour binder la station** ;
- `--discover-ports` cherche un **listener TCP local existant** pour le simulateur ;
- `stgs_port_check` produit un rapport sur une plage sans identifier le protocole applicatif ;
- un port `OPEN` ne prouve pas qu'il s'agit d'un service STGS, car STGS v1 n'implemente pas de handshake d'identite.

Les limites chiffrées et seuils de santé sont documentés comme des paramètres de démonstration du projet. Ils ne sont pas presentes comme des exigences normatives aerospatiales.
