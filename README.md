# nwsnes

Un cœur d'émulateur SNES taillé pour la calculatrice **NumWorks** (appli externe `.nwa`), dont la mémoire vive ne suffit normalement pas : une SNES utilise 192 Ko pour sa mémoire de travail et sa mémoire vidéo, une appli NumWorks en reçoit une centaine au total.

> **État : version 0.1, expérimentale.** Tout ce qui suit a été vérifié sur PC, y compris le code ARM exécuté dans un Cortex-M7 émulé. **Rien n'a encore tourné sur une vraie calculatrice**, et aucun jeu du commerce n'a été essayé. La vitesse réelle est inconnue.

L'outil PC qui prépare un jeu et l'installe est dans un dépôt séparé : **snes2nw**.

*English summary: a SNES emulator core for the NumWorks calculator. It fits by never emulating audio and by backing emulated RAM with flash pages found by profiling the game on a PC ("content-addressed paging"). Verified on PC only so far.*

## Comment ça tient

1. **Pas de son.** La calculatrice n'a pas de haut-parleur, et le processeur audio de la SNES demande 64 Ko à lui seul. Il est remplacé par un répondeur qui tient le rôle attendu dans les échanges de démarrage (`core/apu_stub.c`).
2. **Mémoire paginée par contenu.** La mémoire émulée est découpée en pages de 256 octets. Chaque page pointe soit vers une page nulle partagée, soit vers une page d'un **dictionnaire en mémoire flash**, soit vers une vraie page de RAM. On n'écrit que dans la RAM : écrire dans une page en flash la copie d'abord. Un ramasseur compare ensuite les pages de RAM au dictionnaire et rend celles dont le contenu est connu. La RAM ne garde que ce qui est à la fois vivant et imprévisible (`core/pager.c`).
3. **Le dictionnaire se construit sur PC.** `snes2nw` fait tourner le jeu avec ce même cœur, note les contenus de page qui restent stables (graphismes décompressés, cartes de niveau, tables), et les range dans un « pack » avec la ROM. Les pages qui existent telles quelles dans la ROM ne coûtent rien.
4. **Affichage ligne par ligne**, sans image en mémoire : chaque ligne de 256 pixels part directement à l'écran (`core/ppu.c`).
5. **Raccourcis de vitesse** : octets d'instruction lus directement dans la ROM, accès rapide à la mémoire de travail, et saut des boucles où le jeu ne fait qu'attendre l'image suivante.

L'appli elle-même garde 8 Ko de RAM ; tout le reste de ce que la calculatrice lui laisse devient la réserve de pages.

## Ce qui est mesuré

Toutes les mesures viennent du banc d'essai PC (`host/`), sur des ROM libres : 91 ROM de test du dépôt [PeterLemon/SNES](https://github.com/PeterLemon/SNES) et la ROM de test de ce dépôt (`tests/rom/`, programme et graphismes originaux).

| Vérification | Résultat |
|---|---|
| Tests du processeur 65816 (23 ROM) | 23 réussis |
| Rendu rapide et pagination comparés au rendu de référence, même minutage, 300 images × 92 ROM | identiques, pixels et mémoire |
| Avec les raccourcis de vitesse, pixels seulement | 91 ROM identiques sur 92 |
| Comparé à [LakeSnes](https://github.com/angelo-wf/LakeSnes) non modifié, 300 images × 91 ROM | 90 identiques ; la dernière diffère sur 4 images (1 sans les raccourcis de vitesse), à la première ligne de l'écran |
| Paquets corrompus (300 variantes), sous AddressSanitizer | aucune erreur mémoire |

Mémoire vive nécessaire, dictionnaire construit sur une partie puis mesuré sur une autre (30 s de jeu, touches au hasard) :

| ROM | Sans dictionnaire | Avec dictionnaire | Taille du pack |
|---|---|---|---|
| ROM de test de ce dépôt (mode 1, 3 plans, 24 sprites) | 19,8 Ko | 1,2 Ko | 39 Ko |
| Petit jeu de démonstration en mode 7 | 63,0 Ko | 13,8 Ko | 588 Ko |
| Démo mode 7 « StarWars » | 54,0 Ko | 4,8 Ko | 217 Ko |
| Démo mode 7 + HDMA | 32,2 Ko | 12,0 Ko | 58 Ko |
| Image 8 bits par pixel 64×64 tuiles | 39,8 Ko | 0,2 Ko | 129 Ko |

Ces programmes sont petits. Un jeu du commerce remplit bien plus sa mémoire : le chiffre ne se connaît qu'en le mesurant avec `snes2nw`.

Vitesse, en instructions ARM par image (code de l'appli exécuté sur un Cortex-M7 émulé par QEMU, `make bench`) :

| Scène | En dessinant chaque image | Sans dessiner |
|---|---|---|
| ROM de test (mode 1) | 5,0 millions | 0,77 million |
| Jeu de démonstration en mode 7 | 7,6 millions | 0,63 million |

Le processeur de la N0110 fait 216 millions de cycles par seconde, soit 3,6 millions par image à 60 images par seconde. Si une instruction prend entre un et deux cycles, la scène de test tournerait à vitesse normale en affichant entre 15 et 30 images par seconde (l'appli saute l'affichage de certaines images quand elle est en retard). C'est une estimation : QEMU compte les instructions, pas le temps, et ne connaît ni les caches ni la mémoire flash externe de la calculatrice.

## Limites connues

- **Aucun essai sur calculatrice.** L'appli compile, se lie avec `nwlink` comme le fait l'installateur, et son code tourne sur PC avec de faux appels système (`make sim`), mais c'est tout.
- **Il faut avoir joué le passage sur PC.** Un passage jamais vu pendant la préparation peut demander plus de RAM qu'il n'y en a ; l'appli s'arrête alors avec un message.
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
make wasm      # cœur + profileur en WebAssembly pour snes2nw (zig : pip install ziglang)
make sim       # l'écran de l'appli simulé sur PC
make bench PACK=jeu.pack   # instructions par image sur Cortex-M7 émulé (qemu-system-arm)
```

`build/nwsnes-host jeu.sfc --help` liste les options du banc d'essai (comparaison des rendus, mesure de la RAM minimale, création de pack).

## Organisation

| Dossier | Contenu |
|---|---|
| `core/` | le cœur, sans aucune allocation : processeur, bus et minutage, affichage, pagination, lecteur de pack |
| `numworks/` | l'appli calculatrice (API EADK) |
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
