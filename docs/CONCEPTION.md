# Conception de nwsnes

Ce document explique les choix techniques. Le code est commenté en anglais ; les noms entre `accents graves` renvoient aux fichiers.

## Le problème

| | SNES | Appli NumWorks (N0110) |
|---|---|---|
| Mémoire de travail | 128 Ko | |
| Mémoire vidéo | 64 Ko | |
| Mémoire du processeur audio | 64 Ko | |
| **Total** | **256 Ko** | **environ 100 Ko**, données de l'appli comprises |

La limite exacte dépend de la version du système : c'est le système qui indique à l'installateur la zone de RAM réservée aux applis externes. L'appli affiche la valeur réelle à son démarrage. Le « 100 Ko » vient de mesures de la communauté, pas d'une documentation officielle.

Un émulateur classique réserve tout d'un bloc et ne peut donc pas tenir. nwsnes part du constat inverse : à un instant donné, l'essentiel de cette mémoire contient des données **que l'on peut connaître à l'avance** (graphismes décompressés, cartes de niveau, tables), ou des zéros.

## Pagination par contenu (`core/pager.c`)

Les trois mémoires émulées (travail, vidéo, sauvegarde) sont découpées en 896 pages virtuelles de 256 octets. Une table donne, pour chaque page, l'adresse de ses données :

- la **page nulle** partagée, en flash ;
- une page du **dictionnaire** du pack, en flash, éventuellement directement dans l'image de la ROM ;
- une page de la **réserve** en RAM, seule inscriptible.

Lecture : un accès à la table, puis l'octet. Écriture : si la page est en RAM, écriture directe. Sinon :

1. si l'octet écrit est déjà la valeur présente, il ne se passe rien (les jeux effacent souvent de la mémoire déjà nulle) ;
2. sinon une page de la réserve est prise, le contenu y est copié, et la table mise à jour (copie à l'écriture).

Le **ramasseur** (`pager_collect`) passe sur les pages en RAM, calcule une empreinte de 32 bits, et cherche par dichotomie dans l'index du dictionnaire. Si le contenu y figure (comparaison complète pour confirmer), la page virtuelle est redirigée vers la flash et la page de RAM libérée. Il tourne quand la réserve descend sous un huitième, et en urgence quand elle est vide. Si rien ne peut être libéré, l'appli signale « mémoire pleine ».

Le dictionnaire est indépendant de la position : un même contenu sert pour n'importe quelle page de n'importe laquelle des trois mémoires.

Pourquoi 256 octets : toutes les structures que l'affichage lit d'un coup (une tuile de 16, 32 ou 64 octets, une ligne de table de tuiles) sont alignées sur leur taille et ne chevauchent donc jamais deux pages. L'affichage peut prendre un pointeur de page et lire sans autre précaution.

Coût en RAM : 3,6 Ko pour la table. Le reste des 8 Ko de l'appli : palettes (0,5 Ko + 0,5 Ko de cache RGB565), table des sprites (0,5 Ko), trois tampons de ligne (1,5 Ko), ligne de sortie (0,5 Ko), état du processeur et des registres.

## Le pack (`core/pack.h`, `pc/profiler.c`)

Un seul fichier, installé comme « données externes » de l'appli : en-tête de 64 octets, image de la ROM (complétée à une puissance de deux), index du dictionnaire trié par empreinte (8 octets par page), puis les pages qui ne se trouvent pas dans la ROM.

Le profileur, côté PC, observe à chaque image le contenu de chaque page. Un contenu qui reste identique 4 images de suite est enregistré, et crédité d'un point par image et par page où il est vu. À la fin, les contenus sont classés par points ; ceux retrouvés tels quels dans la ROM (recherche à n'importe quel décalage, par empreinte glissante) ne coûtent que 8 octets d'index, les autres 264. On remplit jusqu'au budget de flash.

`--add` dans snes2nw repart du dictionnaire d'un pack existant : les parties s'additionnent.

## Minutage (`core/snes.c`)

LakeSnes avance de 2 cycles d'horloge à la fois et teste tout à chaque pas, soit dix millions de pas par seconde : trop lent ici. nwsnes laisse le processeur avancer et ne s'arrête qu'aux positions où quelque chose se passe sur une ligne : initialisation du HDMA (16), dessin de la ligne (512), rafraîchissement mémoire (536), HDMA (1104), fin de ligne, plus l'interruption programmable. Chaque accès mémoire ajoute son coût et compare à la prochaine échéance.

Trois raccourcis, désactivables (`speedHacks`) :

- les octets d'instruction sont lus par un pointeur direct dans la ROM tant que le programme reste dans la même banque ;
- les accès à la mémoire de travail évitent le décodage complet du bus ;
- **boucles d'attente** : quand le processeur revient en arrière au même endroit, avec exactement les mêmes registres et sans avoir rien écrit, il ne peut sortir de la boucle que par une interruption ou un drapeau matériel. On saute alors à la prochaine échéance.

Ensemble, ces trois raccourcis ont fait passer le coût du processeur émulé de 4,2 à 0,5 million d'instructions ARM par image sur le jeu de démonstration en mode 7 (600 premières images).

Ce dernier raccourci déplace de quelques cycles le moment où une interruption arrive dans la boucle. Aucun pixel ne change sur 91 des 92 ROM de test ; les octets laissés sous le pointeur de pile par l'interruption peuvent différer, ce qui est sans effet.

## Affichage (`core/ppu.c`)

Chaque plan écrit dans un tampon d'une ligne où chaque pixel porte sa priorité, son plan et son indice de couleur ; le plus prioritaire gagne, quel que soit l'ordre de dessin. Une passe finale convertit en RGB565, avec le mélange de couleurs contre le sous-écran si le jeu l'utilise. Les fenêtres découpent la ligne en au plus cinq segments.

Chemins lents, pixel par pixel, repris de LakeSnes : mosaïque, décalage par tuile (modes 2, 4, 6), modes haute résolution (5, 6).

Le rendu d'origine de LakeSnes est conservé (`core/ppu_ref.c`, PC seulement). Le banc d'essai fait tourner les deux rendus côte à côte et compare chaque image.

## Processeur audio (`core/apu_stub.c`)

Quatre ports, quatre réponses possibles : « prêt » ($AA/$BB, ce que dit la ROM de démarrage), « écho » (le protocole d'envoi de données acquitte chaque octet par un écho), « compteur », « zéro ». L'écriture de $CC en état « prêt » passe en « écho ». Si le jeu lit les ports en boucle sans rien écrire (96 lectures dans une image), il attend autre chose : on passe à la réponse suivante. Un pack peut imposer une réponse.

C'est le point le plus fragile face aux jeux du commerce.

## Ce que la validation couvre, et ce qu'elle ne couvre pas

Couvert, sur PC : exactitude du processeur (ROM de test dédiées), identité du rendu rapide et du rendu de référence, transparence de la pagination (mémoire identique octet pour octet à une exécution sans pagination), comparaison avec LakeSnes non modifié, robustesse aux packs corrompus, logique de cadence de l'appli avec une horloge simulée, édition de liens de l'appli par `nwlink`.

Non couvert : tout ce qui dépend de la calculatrice réelle (vitesse, taille exacte de la RAM et de la flash, comportement des appels système), et tout jeu du commerce.

## Pistes

- Écrire la sauvegarde dans le stockage de la calculatrice.
- Compresser en RAM les pages froides qui ne sont pas dans le dictionnaire.
- Accélérer l'affichage (tables de décodage de tuiles, sprites) une fois la vitesse réelle connue.
- Recompiler à l'avance le code du jeu en ARM, pour les jeux où le processeur émulé domine.
- Réglages par jeu dans `snes2nw/profiles`.
