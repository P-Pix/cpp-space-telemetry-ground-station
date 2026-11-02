# Documentation STGS

Il y a deux niveaux de documentation dans ce dossier. Les fichiers Markdown sont là pour retrouver rapidement une décision de conception ou une commande. Les PDF et leurs sources LaTeX vont beaucoup plus loin dans le détail des chemins d'exécution et des options CLI.

## Pour commencer

| Document | Contenu |
| --- | --- |
| [README principal](../README.md) | Présentation, compilation et premiers essais |
| [Présentation du projet](PRESENTATION_DU_PROJET.md) | Ce que j'ai cherché à construire et pourquoi |
| [Notes techniques](NOTES_TECHNIQUES.md) | TCP, UDP, framing, CRC, threads, replay, limites |
| [Essais rapides](ESSAIS_RAPIDES.md) | Commandes utilisables sur une machine Linux |
| [Vérifications du 02/11/2026](VERIFICATIONS_2026-11-02.md) | Résultats observés sur le code fourni |
| [Dossier synthétique PDF](STGS_Presentation_technique.pdf) | Version courte à lire sans parcourir le dépôt |

Pour les détails plus complets, j'ai conservé les documents de référence qui étaient déjà dans le projet :

- [`TECHNICAL_DESIGN.md`](TECHNICAL_DESIGN.md) — description des composants, formats et invariants.
- [`PERFORMANCE_REPORT.md`](PERFORMANCE_REPORT.md) — périmètre et mesures du microbenchmark.
- [`VERIFICATION_REPORT.md`](VERIFICATION_REPORT.md) — compte rendu plus ancien des contrôles (à distinguer de la vérification datée ci-dessus).
- [`SHOWCASE_GUIDE.md`](SHOWCASE_GUIDE.md) — autres scénarios de démonstration.

## Manuels d'exécution

Ces PDF documentent le trajet des commandes jusqu'aux classes C++ concernées. Je les ai laissés dans le dépôt avec leurs sources `.tex` :

| Fichier | Sujet |
| --- | --- |
| `STGS_01_Architecture_et_execution.pdf` | Architecture, codec, pipeline et réseau |
| `STGS_02_Commandes_station_sol.pdf` | Options de la station sol |
| `STGS_03_Commandes_simulateur.pdf` | Options du simulateur |
| `STGS_04_Diagnostics_build_tests_benchmark.pdf` | Build, diagnostic, tests, benchmark |
| `STGS_05_Limites_invariants_entretien.pdf` | Limites et invariants discutables dans le code |
| `STGS_Manuel_execution_complet.pdf` | Ensemble des cinq volumes |

Les deux revues de code historiques `stgs_code_review_recruiter_fr.pdf` et `stgs_code_review_recruiter_en.pdf` restent disponibles pour référence. Les notes Markdown plus courtes au-dessus reflètent mieux la présentation actuelle du projet.

### Recompiler les manuels

Depuis `docs/` et avec LaTeX installé :

```bash
latexmk -pdf STGS_01_Architecture_et_execution.tex
latexmk -pdf STGS_02_Commandes_station_sol.tex
latexmk -pdf STGS_03_Commandes_simulateur.tex
latexmk -pdf STGS_04_Diagnostics_build_tests_benchmark.tex
latexmk -pdf STGS_05_Limites_invariants_entretien.tex
latexmk -pdf STGS_Manuel_execution_complet.tex
```

Le manuel complet assemble les cinq PDF : il faut donc d'abord les compiler.

## Deux options à ne pas confondre

`--auto-port` cherche un port local **disponible pour démarrer la station**. `--discover-ports`, côté simulateur, cherche un port TCP **où un listener répond déjà**. Le diagnostic peut indiquer qu'un port est ouvert, mais ne sait pas prouver que le service qui répond parle STGS : il n'y a pas de handshake d'identité dans ce protocole.
