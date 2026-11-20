# nwsnes

Un cœur d'émulateur SNES taillé pour la calculatrice **NumWorks** (appli externe `.nwa`), dont la mémoire vive ne suffit normalement pas : une SNES utilise 192 Ko pour sa mémoire de travail et sa mémoire vidéo, une appli NumWorks en reçoit 149 au total.

> **État : version 0.3, expérimentale.** L'appli tourne sur une vraie N0110 (système 25.2.0) : la version 0.1 y faisait tourner Super Mario World à 43 % de sa vitesse, avec 7 images affichées par seconde. Le cœur a été réécrit depuis pour le petit cache de la calculatrice (0.2) ; d'après le banc d'essai le même jeu devrait approcher sa vitesse normale, mais **ce n'est pas encore mesuré sur calculatrice**. L'installation en deux parties des jeux de plus de 2,5 Mo (0.3) n'a **pas encore été essayée sur une calculatrice** non plus : sur PC seulement, contre une calculatrice simulée.

L'outil PC qui prépare un jeu et l'installe est dans un dépôt séparé : **snes2nw**.

*English summary: a SNES emulator core for the NumWorks calculator. It fits by never emulating audio and by backing emulated RAM with flash pages found by profiling the game on a PC ("content-addressed paging"). Games bigger than the app area of the flash are split over the app areas of both firmware slots. Runs on a real N0110; speed of the current version and the two-slot installation are verified on PC only so far.*

## Comment ça tient

1. **Pas de son.** La calculatrice n'a pas de haut-parleur, et le processeur audio de la SNES demande 64 Ko à lui seul. Il est remplacé par un répondeur qui tient le rôle attendu dans les échanges de démarrage (`core/apu_stub.c`).
2. **Mémoire paginée par contenu.** La mémoire émulée est découpée en pages de 256 octets. Chaque page pointe soit vers une page nulle partagée, soit vers une page d'un **dictionnaire en mémoire flash**, soit vers une vraie page de RAM. On n'écrit que dans la RAM : écrire dans une page en flash la copie d'abord. Un ramasseur compare ensuite les pages de RAM au dictionnaire et rend celles dont le contenu est connu. La RAM ne garde que ce qui est à la fois vivant et imprévisible (`core/pager.c`).
3. **Le dictionnaire se construit sur PC.** `snes2nw` fait tourner le jeu avec ce même cœur, note les contenus de page qui restent stables (graphismes décompressés, cartes de niveau, tables), et les range dans un « pack » avec la ROM. Les pages qui existent telles quelles dans la ROM ne coûtent rien.
4. **Affichage par bandes de 16 lignes**, sans image en mémoire : les écritures du jeu dans les registres vidéo sont notées, et chaque bande est peinte d'un coup, du fond vers l'avant, puis envoyée à l'écran (`core/ppu.c`).
5. **Un cœur fait pour 8 Ko de cache.** La mémoire flash de la calculatrice est lente dès qu'on sort du cache (près d'une microseconde par accès manqué) : le code est rangé pour que ce qui sert à chaque instruction émulée tienne ensemble (`core/hot.ld`), les octets d'instruction sont lus directement dans la ROM, et les boucles où le jeu ne fait qu'attendre l'image suivante sont sautées.
6. **Les deux moitiés de la mémoire flash.** La zone des applis fait 2560 Ko, moins que beaucoup de jeux. Mais la flash contient deux systèmes, celui qui tourne et celui de secours, chacun avec sa zone d'applis. Un jeu trop gros est coupé en deux : une partie est installée avec l'appli, l'autre écrite par `snes2nw` dans la zone des applis de l'autre moitié, où l'appli sait la retrouver (`core/pack.h`). Un jeu de 4 Mo tient ainsi en entier, sans compression.

L'appli elle-même garde environ 31 Ko de RAM (tampons d'affichage compris) ; tout le reste de ce que la calculatrice lui laisse devient la réserve de pages.

## Ce qui est mesuré

### Sur une vraie calculatrice (N0110, système 25.2.0)

Mesuré avec les applis `make diag` et `make probe` :

| | |
|---|---|
| RAM laissée aux applis externes | 149 Ko |
| Zone des applis en flash | 2560 Ko dans chacune des deux moitiés |
| L'autre moitié de la flash | lisible par une appli |
| Accès à la flash hors du cache | environ 0,95 µs |
| Accès à la RAM hors du cache | environ 33 ns |
| Caches du processeur | 8 Ko pour le code, 8 Ko pour les données |
| Exécuter du code depuis la RAM | impossible (la calculatrice redémarre) |

### Sur PC

Banc d'essai (`host/`), sur des ROM libres : 91 ROM de test du dépôt [PeterLemon/SNES](https://github.com/PeterLemon/SNES) et la ROM de test de ce dépôt (`tests/rom/`, programme et graphismes originaux).

| Vérification | Résultat |
|---|---|
| Cœur rapide (processeur, affichage, pagination) comparé à l'interprète et au rendu de référence, même minutage, 300 images × 91 ROM | identiques, pixels et mémoire |
| Les mêmes comparaisons sur trois jeux du commerce (Super Mario World, Donkey Kong Country, EarthBound), jusqu'à 7200 images | identiques, pixels et mémoire |
| Comparé à [LakeSnes](https://github.com/angelo-wf/LakeSnes) non modifié, 300 images × 91 ROM | 90 identiques ; la dernière diffère à la première ligne de l'écran |
| Packs abîmés (600 variantes, en une ou deux parties), sous AddressSanitizer | aucune erreur mémoire |
| Installation en deux parties contre une calculatrice simulée (dans snes2nw) : acceptée, refusée, ignorée | chaque cas se termine proprement |

Avec les raccourcis de vitesse (ce qui tourne sur la calculatrice), le minutage n'est plus exactement celui de la référence et certaines images diffèrent de quelques pixels, ou arrivent une image plus tard.

Mémoire vive nécessaire, dictionnaire construit sur une partie puis mesuré sur une autre (30 s de jeu, touches au hasard) :

| ROM | Sans dictionnaire | Avec dictionnaire | Taille du pack |
|---|---|---|---|
| ROM de test de ce dépôt (mode 1, 3 plans, 24 sprites) | 19,8 Ko | 1,2 Ko | 39 Ko |
| Petit jeu de démonstration en mode 7 | 63,0 Ko | 13,8 Ko | 588 Ko |
| Démo mode 7 « StarWars » | 54,0 Ko | 4,8 Ko | 217 Ko |
| Démo mode 7 + HDMA | 32,2 Ko | 12,0 Ko | 58 Ko |
| Image 8 bits par pixel 64×64 tuiles | 39,8 Ko | 0,2 Ko | 129 Ko |

Ces programmes sont petits. Un jeu du commerce remplit bien plus sa mémoire : le chiffre ne se connaît qu'en le mesurant avec `snes2nw`.

Vitesse, en instructions ARM par image (code de l'appli exécuté sur un Cortex-M7 émulé par QEMU, `make bench`, en plein niveau) :

| Jeu | En dessinant chaque image | Sans dessiner |
|---|---|---|
| Super Mario World | 2,0 millions | 0,8 million |
| Donkey Kong Country | 4,4 millions | 1,1 million |

Le temps réel dépend surtout des accès à la flash qui manquent le cache, que QEMU ne connaît pas : le banc d'essai les compte avec un modèle du cache (`CACHESIM`). Avec les temps mesurés sur la calculatrice, ce modèle donne environ 15 ms par image pour Super Mario World en dessinant une image sur deux (la vitesse normale, à 30 images affichées par seconde) et 24 ms pour Donkey Kong Country en dessinant une image sur trois (70 à 80 % de sa vitesse). **Ce modèle n'a pas encore été confronté à la calculatrice.**

## Limites connues

- **La vitesse de la version actuelle n'est pas mesurée sur calculatrice**, et l'installation en deux parties n'y a jamais été essayée : il se peut que la calculatrice refuse l'écriture dans son autre moitié (`snes2nw` s'arrête alors sans rien modifier).
- **Un jeu en deux parties ne survit pas à une mise à jour de la calculatrice** : elle écrit dans la moitié de secours. Il suffit de le réinstaller.
- **Il faut avoir joué le passage sur PC.** Un passage jamais vu pendant la préparation peut demander plus de RAM qu'il n'y en a ; l'appli s'arrête alors avec un message.
- Un jeu de plus de 5 Mo environ ne tient pas en entier : seule la partie de sa ROM lue pendant la préparation est installée.
- **Pas de son**, et pas de puces supplémentaires de cartouche (Super FX, SA-1, DSP…).
- **Les sauvegardes ne survivent pas** à la sortie de l'appli (la mémoire de sauvegarde est émulée, mais pas encore écrite dans le stockage de la calculatrice).
- Le faux processeur audio procède par essais quand un jeu attend une réponse inhabituelle : certains jeux peuvent rester bloqués au démarrage.
- Rendu : pas de « couleur directe », modes haute résolution affichés en 256 pixels.
- Un seul jeu installé à la fois.

## Touches

| SNES | NumWorks |
|---|---|
| Croix | Flèches |
| X, A | OK, Retour |
| Y, B | Boîte à outils, Effacer |
| Select, Start | Shift, Alpha (ou EXE) |
| L, R | eˣ, xʸ |
| Statistiques à l'écran | 0 |
| Quitter | Maison |

Les quatre touches à droite des flèches forment le même carré que les boutons de la manette. Les statistiques montrent : images affichées par seconde, vitesse en %, RAM utilisée par le jeu, RAM libre.

## Compiler

```sh
make host      # banc d'essai PC : build/nwsnes-host
make test      # vérifications sur la ROM de test
make nwa       # appli calculatrice : build/nwsnes.nwa (arm-none-eabi-gcc, Node.js)
make diag      # appli de mesure de la calculatrice (vitesse de la flash, de la RAM, cache)
make probe     # appli qui montre le découpage de la flash (les deux moitiés, leurs zones d'applis)
make wasm      # cœur + profileur en WebAssembly pour snes2nw (zig : pip install ziglang)
make sim       # l'écran de l'appli simulé sur PC
make bench PACK=jeu.pack   # instructions par image sur Cortex-M7 émulé (qemu-system-arm)
```

`build/nwsnes-host jeu.sfc --help` liste les options du banc d'essai (comparaison des rendus, mesure de la RAM minimale, création de pack).

## Organisation

| Dossier | Contenu |
|---|---|
| `core/` | le cœur, sans aucune allocation : processeur, bus et minutage, affichage, pagination, lecteur de pack |
| `numworks/` | l'appli calculatrice (API EADK), et les deux applis de mesure |
| `pc/` | côté PC uniquement : profileur, fabrication du pack, lecture de l'en-tête des ROM |
| `host/` | banc d'essai en ligne de commande, simulateur de l'appli |
| `wasm/` | interface WebAssembly utilisée par snes2nw |
| `bench/` | mesure sur Cortex-M7 émulé |
| `tests/` | ROM de test (sources en assembleur 65816 et générateur de graphismes) et vérifications |
| `tools/` | scripts : import du code LakeSnes, conversion d'images, comparaison avec un autre émulateur |
| `docs/` | [conception détaillée](docs/CONCEPTION.md) |

## ROM

Ce dépôt ne contient aucun jeu du commerce et n'en distribue pas. N'utilisez que des copies de cartouches que vous possédez. Un pack contient la ROM : ne le partagez pas.

## Licence et remerciements

Licence MIT, voir [LICENSE](LICENSE) et [THIRD-PARTY.md](THIRD-PARTY.md).

- Le processeur 65816, le DMA, les registres vidéo et le rendu de référence viennent de [LakeSnes](https://github.com/angelo-wf/LakeSnes) d'angelo_wf (MIT), adaptés par `tools/import_*.py`.
- L'idée d'un tampon de priorité par ligne vient du rendu de [snesrev](https://github.com/snesrev) (MIT).
- Les ROM de test de [PeterLemon](https://github.com/PeterLemon/SNES) ont servi à la validation ; elles ne sont pas redistribuées ici.
- La documentation [Nwagyu](https://yaya-cout.github.io/Nwagyu/) pour tout ce qui concerne les applis externes NumWorks.

NumWorks est une marque déposée de NumWorks ; SNES et Super Nintendo sont des marques de Nintendo. Ce projet n'est affilié à aucune des deux sociétés.
