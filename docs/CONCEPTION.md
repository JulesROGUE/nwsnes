# Conception de nwsnes

Ce document explique les choix techniques. Le code est commenté en anglais ; les noms entre `accents graves` renvoient aux fichiers.

## Le problème

| | SNES | Appli NumWorks (N0110) |
|---|---|---|
| Mémoire de travail | 128 Ko | |
| Mémoire vidéo | 64 Ko | |
| Mémoire du processeur audio | 64 Ko | |
| **Total** | **256 Ko** | **149 Ko**, données de l'appli comprises |
| ROM du jeu | jusqu'à 6 Mo | 2560 Ko de zone d'applis |

Les chiffres de droite sont mesurés sur une N0110 (système 25.2.0). Ils dépendent de la version du système : c'est lui qui indique à l'installateur les zones de RAM et de flash réservées aux applis externes. L'appli affiche la RAM réelle à son démarrage.

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

Coût en RAM : 3,6 Ko pour la table. Le reste des 31 Ko de l'appli va surtout à l'affichage : la bande de 16 lignes en cours (8 Ko), les lignes de sprites de la bande (4 Ko), les palettes et leur cache RGB565, le journal des écritures dans les registres, puis l'état du processeur et des registres.

## Le pack (`core/pack.h`, `pc/profiler.c`)

Un seul fichier, installé comme « données externes » de l'appli : en-tête, image de la ROM (complétée à une puissance de deux), index du dictionnaire trié par empreinte (8 octets par page), puis les pages qui ne se trouvent pas dans la ROM.

Le profileur, côté PC, observe à chaque image le contenu de chaque page. Un contenu qui reste identique 4 images de suite est enregistré, et crédité d'un point par image et par page où il est vu. À la fin, les contenus sont classés par points ; ceux retrouvés tels quels dans la ROM (recherche à n'importe quel décalage, par empreinte glissante) ne coûtent que 8 octets d'index, les autres 264. On remplit jusqu'au budget de flash.

`--add` dans snes2nw repart du dictionnaire d'un pack existant : les parties s'additionnent.

Quand la ROM ne tient pas dans le budget, elle est rangée par **blocs de 4 Ko**, avec une table qui donne la place de chaque bloc (version 2 du format). Les blocs de même contenu ne sont stockés qu'une fois (une ROM de 3 Mo arrive doublée à 4 Mo). L'appli garde en RAM un pointeur par bloc (4 Ko pour une ROM de 4 Mo), pris sur la réserve de pages. Un bloc peut manquer : l'appli s'arrête proprement si le jeu le lit. C'est ce qui sert aux jeux trop gros même pour les deux moitiés de la flash : seuls les blocs lus pendant la préparation, et leurs voisins, sont installés.

## Les deux moitiés de la flash (`core/pack.c`, `numworks/probe.c`, snes2nw `lib/flash2.js`)

Les 8 Mo de flash de la N0110 sont deux moitiés de 4 Mo. Chacune peut contenir un système complet : l'une tourne, l'autre est la copie de secours dans laquelle les mises à jour sont écrites. Chaque moitié a sa zone d'applis (2560 Ko mesurés), mais les applis ne sont installées que dans celle de la moitié qui tourne.

Un jeu trop gros pour cette zone est en deux parties :

- le pack proprement dit, installé avec l'appli : table des blocs, une partie des blocs, dictionnaire ;
- son **extension** : le reste des blocs de ROM, à la fin de la zone d'applis de l'autre moitié.

Ce que contient l'extension ne dépend que de la ROM (les derniers blocs, autant qu'il en tient), pas de la façon dont le jeu a été joué : un pack refait après une nouvelle partie d'essai va avec l'extension déjà en place. Les entrées de la table des blocs et du dictionnaire disent dans laquelle des deux parties se trouve leur contenu.

Chaque moitié se décrit elle-même 64 Ko après son début (version du système, bornes de sa zone d'applis). `pack_extensionAddress` en déduit l'adresse de l'extension ; l'outil PC qui l'écrit et l'appli qui la lit passent par cette même fonction, compilée dans les deux. L'appli relit et vérifie toute l'extension à chaque lancement : une mise à jour de la calculatrice, qui écrit dans cette moitié, peut l'avoir effacée.

Ce qui est établi et ce qui ne l'est pas :

- une appli peut lire l'autre moitié : vérifié sur calculatrice (`make probe`) ;
- le système déclare par USB toute l'autre moitié comme lisible, effaçable et inscriptible : c'est écrit dans son code source publié (c'est par là que passent les mises à jour) ;
- le noyau, dont le source n'est pas publié, laisse réellement écrire dans la zone d'applis de l'autre moitié : vérifié sur une N0110 (système 25.2.0, qui tournait dans la première moitié), où 2556 Ko ont été écrits, relus à l'identique, puis retrouvés par l'appli. Rien n'est connu des autres modèles et versions : si une calculatrice refuse, elle le signale, et l'installation s'arrête avant d'avoir touché aux applis en place.

## Minutage et processeur (`core/snes.c`, `core/cpu.c`)

LakeSnes avance de 2 cycles d'horloge à la fois et teste tout à chaque pas, soit dix millions de pas par seconde : trop lent ici. nwsnes laisse le processeur avancer avec un seul compteur, qui descend jusqu'à la prochaine échéance : sur une ligne, l'initialisation du HDMA, le dessin, le rafraîchissement mémoire, le HDMA, la fin de ligne, plus l'interruption programmable. Chaque accès mémoire retire son coût du compteur ; quand il passe sous zéro, l'échéance est traitée sur place.

Sur la calculatrice, ce qui coûte n'est pas tant le nombre d'instructions que les accès à la flash qui manquent le cache de 8 Ko : près d'une microseconde chacun, le temps de cent instructions. Le cœur est donc organisé autour de ça :

- le décodage se fait en deux temps, par tables : le mode d'adressage (un aiguillage commun), puis l'opération, une petite fonction par opération ;
- ces fonctions sont rangées dans la flash par ordre de fréquence mesurée, les plus courantes côte à côte avec la boucle principale (`core/hot.ld`) ;
- les tables de décodage sont en RAM, où un accès manqué coûte trente fois moins ;
- les octets d'instruction sont lus par un pointeur direct dans la ROM, et les accès à la mémoire de travail et à la ROM évitent le décodage complet du bus.

**Boucles d'attente** : quand le processeur revient en arrière au même endroit, avec exactement les mêmes registres et sans avoir rien écrit, il ne peut sortir de la boucle que par une interruption ou un drapeau matériel. On saute alors à la prochaine échéance.

Ce raccourci, et quelques autres sur le minutage, sont désactivables (`speedHacks`) : sans eux le cœur donne exactement les mêmes images et la même mémoire que l'interprète d'origine de LakeSnes, conservé pour la comparaison (`core/cpu_ref.c`, PC seulement). Avec eux, une interruption peut arriver quelques cycles plus tôt ou plus tard dans une boucle d'attente : certaines images diffèrent de quelques pixels ou arrivent une image plus tard.

## Affichage (`core/ppu.c`)

Le dessin est retardé. Les écritures du jeu dans les registres vidéo sont notées dans un journal avec la ligne où elles ont lieu ; l'image est produite par **bandes de 16 lignes**, quand la bande est complète ou qu'une lecture du jeu a besoin d'un état à jour. Une bande où rien ne change entre ses lignes est le cas courant ; sinon elle est peinte en morceaux, d'un changement au suivant.

Pour une telle bande, le chemin rapide peint directement en RGB565, du fond vers l'avant : chaque plan recouvre les précédents, ce qui évite tout tampon de priorité, et les sprites de la bande sont préparés une fois. Le mélange de couleurs avec le sous-écran, quand il porte sur une couleur fixe, passe par une palette précalculée.

Tout ce que le chemin rapide ne sait pas faire (fenêtres qui coupent une ligne, mosaïque, décalage par tuile, modes haute résolution, certains mélanges entre les deux écrans) passe par un rendu général, ligne par ligne, avec un tampon de priorité. Le banc d'essai PC compte les lignes que prend chaque chemin.

Le rendu d'origine de LakeSnes est conservé (`core/ppu_ref.c`, PC seulement). Le banc d'essai fait tourner les deux côte à côte et compare chaque image.

## Processeur audio (`core/apu_stub.c`)

Quatre ports, quatre réponses possibles : « prêt » ($AA/$BB, ce que dit la ROM de démarrage), « écho » (le protocole d'envoi de données acquitte chaque octet par un écho), « compteur », « zéro ». L'écriture de $CC en état « prêt » passe en « écho ». Si le jeu lit les ports en boucle sans rien écrire (96 lectures dans une image), il attend autre chose : on passe à la réponse suivante. Un pack peut imposer une réponse.

C'est le point le plus fragile face aux jeux du commerce.

## Ce que la validation couvre, et ce qu'elle ne couvre pas

Couvert, sur PC : identité du cœur rapide et de la référence (processeur, rendu, mémoire octet pour octet) sur 91 ROM de test et sur des sessions de trois jeux du commerce ; comparaison avec LakeSnes non modifié ; robustesse aux packs abîmés ; logique de cadence de l'appli avec une horloge simulée ; édition de liens de l'appli par `nwlink` ; installation en deux parties contre une calculatrice simulée, y compris quand elle refuse ou ignore l'écriture (tests de snes2nw).

Couvert, sur une calculatrice (N0110, système 25.2.0) : l'appli démarre et fait tourner des jeux du commerce ; tailles de la RAM et des zones d'applis ; vitesse de la flash et de la RAM ; lecture et écriture de l'autre moitié de la flash ; Donkey Kong Country en deux parties, mesuré une fois en plein niveau à 99 % de sa vitesse (39 images affichées par seconde).

Non couvert : les autres modèles et les autres versions du système ; une mesure de vitesse dans les conditions du banc d'essai, pour recaler le modèle (nombre d'instructions et d'accès manqués au cache), que la mesure ci-dessus montre environ deux fois trop pessimiste.

## Pistes

- Écrire la sauvegarde dans le stockage de la calculatrice.
- Compresser en RAM les pages froides qui ne sont pas dans le dictionnaire.
- Placer le code automatiquement d'après les accès mesurés, pour réduire encore les accès manqués au cache.
- Recompiler à l'avance le code du jeu en ARM, pour les jeux où le processeur émulé domine.
- Réglages par jeu dans `snes2nw/profiles`.
