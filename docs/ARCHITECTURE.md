# Architecture — ExplorerTint v1

Cible : Windows 10 (build 19045), Explorer 64 bits, thème sombre. Les points marqués **[vérifié]** ont une source primaire ou un test local ; **[déduit]** reste à confirmer par un test.

## 1. Objectifs v1

| # | Objectif | Statut |
|---|---|---|
| G1 | Colorer les éléments selon leur état Git : indexé, non suivi, ignoré | phase 1 |
| G2 | Rendre discrets les éléments « caché + système » et les placer en fin de liste | phase 2 |
| G3 | Ordre personnalisé des dossiers via une colonne `#`, placée en première position | phase 3 |
| G4 | Réglages dans un onglet du ruban d'Explorer (pas d'icône dans la zone de notification) | phase 4 |
| G5 | Icônes de dossier : bleue pour un dépôt Git, badge selon le nom du dossier, langages du dépôt | phase 5 |

Style retenu pour G1 (modifiable dans les réglages) :

| État | Texte | Fond |
|---|---|---|
| Indexé | normal | normal |
| Non suivi | `#C5783D` | normal |
| Ignoré | `#C5783D` | `#9B49301F` (donne `#2F2522` sur `#202020`) |

Les fonds sont exprimés en `#RRGGBBAA` et mélangés avec le fond réel de la liste, pour rester cohérents en thème clair.

## 2. Principe général

Explorer affiche la liste des fichiers avec le moteur DirectUI (`ItemsView`), qui n'offre aucune couleur par élément. ExplorerTint bascule la vue sur l'ancien contrôle liste (`SysListView32`), puis colore chaque ligne pendant son dessin (`NM_CUSTOMDRAW`).

```
explorer.exe
 └─ ExplorerTint.dll  (chargée comme Browser Helper Object, une instance par fenêtre)
     ├─ ClassicList   refuse la création de la classe Itemsview → DefView retombe sur SysListView32
     ├─ Bho           suit la navigation de la fenêtre, attache un ViewTint à chaque vue
     ├─ ViewTint      sous-classe SHELLDLL_DefView, colore les lignes dans NM_CUSTOMDRAW
     ├─ GitStatus     cache des états par dépôt, git.exe en tâche de fond, surveillance disque
     ├─ FolderHooks   GetAttributesOf / CompareIDs / IShellIcon de CFSFolder, miniatures (phases 2, 3, 5)
     ├─ FolderIcons   construction et cache des icônes de dossier (phase 5)
     ├─ RibbonTab     onglet ExplorerTint injecté dans le ruban (phase 4)
     ├─ SettingsDialog fenêtre de réglages ouverte depuis le ruban
     └─ Settings      HKCU\Software\ExplorerTint, surveillée en continu
```

## 3. Composants

### 3.1 Chargement dans Explorer — `Bho`

- Explorer charge les Browser Helper Objects déclarés sous `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects` **[vérifié]** ([Wikipedia](https://en.wikipedia.org/wiki/Browser_Helper_Object)). Open-Shell (Classic Explorer) utilise ce mécanisme et ajoute la CLSID sous `Policies\Ext\CLSID` **[vérifié]** ([ExplorerBHO.rgs](https://github.com/Open-Shell/Open-Shell-Menu/blob/master/Src/ClassicExplorer/ExplorerBHO.rgs)).
- `IObjectWithSite::SetSite` donne `IShellBrowser` (`SID_SShellBrowser`) et `IWebBrowser2` (`SID_SWebBrowserApp`) **[vérifié]** ([ExplorerBHO.cpp](https://github.com/Open-Shell/Open-Shell-Menu/blob/master/Src/ClassicExplorer/ExplorerBHO.cpp)).
- Abonnement à `DWebBrowserEvents2` : à chaque `DocumentComplete`, la vue active est récupérée (`QueryActiveShellView`) et un `ViewTint` y est attaché.
- Le BHO ne fait rien hors de `explorer.exe` (Internet Explorer charge aussi les BHO).
- Le réglage Internet Explorer « Activer les extensions tierces » doit rester actif ; il l'est sur cette machine **[vérifié]**.

### 3.2 Bascule en liste classique — `ClassicList`

- La classe COM `Itemsview` `{1eeb5b5a-06fb-4732-96b3-975c0194eb39}` est servie par `explorerframe.dll` **[vérifié, registre local]**. Vider son `InprocServer32` fait revenir Explorer à `SysListView32` ([winclassic #126](https://winclassic.net/thread/126/make-windows-explorer-listview-control)).
- Plutôt que de modifier ce registre système, ExplorerTint intercepte `CoCreateInstance` et `CoGetClassObject` dans les tables d'import de `shell32.dll`, `windows.storage.dll` et `explorerframe.dll`, et répond `REGDB_E_CLASSNOTREG` pour cette classe. **[déduit]** : que DefView passe bien par l'un de ces appels.
- Repli si l'interception ne suffit pas : surcharge par utilisateur `HKCU\Software\Classes\CLSID\{1eeb5b5a-…}\InprocServer32` vide **[déduit]**, ou interception de `CDefView::_UseItemsView` par symboles, comme le mod Windhawk « Enable SysListView32 » ([winclassic #2059](https://winclassic.net/thread/2059/syslistview32-enabler?page=1)).

### 3.3 Coloration — `ViewTint`

- Sous-classe `SHELLDLL_DefView` (`SetWindowSubclass`) et traite `WM_NOTIFY / NM_CUSTOMDRAW` venant du `SysListView32`, après le traitement normal de DefView. QTTabBar colore ainsi les lignes dans ce mode **[vérifié]** ([ExtendedSysListView32.cs](https://github.com/indiff/qttabbar/blob/master/QTTabBar/ExtendedSysListView32.cs)).
- Index de ligne → élément : `IFolderView::Item(index)` puis `IShellFolder::GetDisplayNameOf(SHGDN_INFOLDER | SHGDN_FORPARSING)`. **[déduit]** : l'index de la liste correspond à celui de `IFolderView`.
- `CDDS_ITEMPREPAINT` : `clrText` et, sauf ligne sélectionnée, `clrTextBk`. Les couleurs `#RRGGBBAA` sont mélangées avec le fond de la liste (`ListView_GetBkColor`).
- Mode sombre : si la liste classique arrive claire, ExplorerTint impose fond, texte et thème `DarkMode_ItemsView`. **[déduit]** : rendu exact de l'en-tête et de la sélection ([xplorer² sur le ListView sombre](https://www.zabkat.com/blog/darkmode-listview.htm)).
- Rafraîchissement : un message enregistré `ExplorerTint.Refresh` déclenche `InvalidateRect` sur la liste.

### 3.4 États Git — `GitStatus`

- Racine du dépôt : remontée jusqu'au premier dossier contenant `.git` (dossier ou fichier).
- Commande : `git --no-optional-locks -C <racine> status --porcelain=v1 -z --ignored=traditional --untracked-files=normal`. `--no-optional-locks` évite que la lecture réécrive l'index, ce qui relancerait la surveillance en boucle.
- Interprétation : `??` non suivi, `!!` ignoré, première lettre ≠ espace (hors conflit `U`) indexé. Un dossier entièrement non suivi ou ignoré apparaît une seule fois (`dossier/`) ; ses descendants héritent de son état. Un dossier ne porte que son propre état (choix b.2).
- Exécution sur un fil de travail unique ; la liste n'est jamais bloquée pendant l'appel à git. Avant la première réponse, les éléments s'affichent normalement.
- Surveillance : un fil par dépôt ouvert, `ReadDirectoryChangesW` récursif sur la racine ; relance de `git status` 300 ms après la dernière modification.

### 3.5 Réglages — `Settings`

Clé `HKCU\Software\ExplorerTint` :

| Valeur | Type | Défaut | Rôle |
|---|---|---|---|
| `ClassicList` | DWORD | 1 | Bascule en liste classique |
| `Git` | DWORD | 1 | Coloration Git |
| `StagedText` / `StagedBack` | SZ | vide | Couleurs « indexé » |
| `UntrackedText` / `UntrackedBack` | SZ | `#C5783D` / vide | Couleurs « non suivi » |
| `IgnoredText` / `IgnoredBack` | SZ | `#C5783D` / `#9B49301F` | Couleurs « ignoré » |
| `GitPath` | SZ | vide | Chemin de `git.exe` (sinon recherche dans le `PATH`) |
| `SystemText` / `SystemBack` | SZ | `#80808099` / vide | Couleurs des éléments système (phase 2) |
| `SystemLast` | DWORD | 1 | Éléments système en fin de liste (phase 2) |
| `HideNames` | DWORD | 1 | Active le traitement de `HiddenNames` (phase 2) |
| `HiddenNames` | SZ | `.claude;.bin;.build;.git;.github;.ia;.idea` | Noms affichés comme cachés (phase 2) |
| `RibbonTab` | DWORD | 1 | Onglet ExplorerTint dans le ruban (phase 4, nouvelles fenêtres) |
| `RepoIcon` | DWORD | 1 | Icône bleue pour les dossiers de dépôt Git (phase 5) |
| `RepoBadge` | SZ | `ring` | Badge des dépôts : `ring` (anneau des langages), `icon` (icône du projet), `bar` (barre des langages), combinables (`icon,ring`) ; vide : aucun (phase 5) |
| `FolderIcons` | SZ | `android=android;desktop=desktop;fonts=fonts;hardware=hardware;web=web;.git=git;.claude=claude;.kk="kk"#E81123` | Badge par nom de dossier (phase 5) |
| `IconRoots` | SZ | `D:\` | Emplacements où les icônes de dossier s'appliquent ; vide : partout (phase 5) |

Couleur vide = rendu normal. Formats acceptés : `#RRGGBB`, `#RRGGBBAA`.

Prise en compte immédiate : chaque processus Explorer surveille la clé (`RegNotifyChangeKeyValue` + `RegisterWaitForSingleObject`), relit les valeurs 100 ms après la dernière modification et publie un instantané partagé (`CurrentSettings()`). Les fenêtres abonnées reçoivent le message enregistré `ExplorerTint.SettingsChanged` : recalcul du dépôt si `Git` ou `GitPath` change, `IShellView::Refresh` si `SystemLast`, `HideNames`, `HiddenNames` ou un réglage d'icône change, simple redessin sinon. Une modification faite dans une fenêtre, dans un autre processus Explorer ou directement dans le registre s'applique partout **[vérifié]**. `ClassicList` et `RibbonTab` ne valent que pour les fenêtres ouvertes ensuite.

## 4. Phases suivantes

### Phase 2 — fichiers système (G2)

- Élément discret = attributs `Hidden` **et** `System` à la fois. Couvre `$RECYCLE.BIN`, `Recovery`, `System Volume Information`, `desktop.ini` **[vérifié, D:\]** ; aucun élément de `D:\.tools` ni `D:\.projects` n'a ces deux attributs (sur 4 niveaux) **[vérifié]**.
- Rendu : couleur de texte dédiée, réglable (`SystemText`, défaut `#80808099`, gris semi-transparent lisible en clair comme en sombre).
- Noms traités comme cachés : `.claude`, `.bin`, `.build`, `.git`, `.github`, `.ia`, `.idea` s'affichent comme des éléments cachés (icône estompée), qu'ils aient l'attribut `Hidden` ou non. Liste réglable (`HiddenNames`). L'attribut réel du fichier n'est jamais modifié.
- Fin de liste : les éléments système passent après les autres, quel que soit le sens du tri.
- Moyen : remplacement de `GetAttributesOf` (ajout de `SFGAO_HIDDEN | SFGAO_GHOSTED` pour les noms listés) et de `CompareIDs` dans la table virtuelle de `IShellFolder` de `CFSFolder` (`windows.storage.dll`). Cette table est commune à toutes les instances : ni symboles ni bibliothèque d'interception. **[déduit]** : DefView estompe l'icône d'après `SFGAO_GHOSTED` et inverse le résultat de `CompareIDs` en tri décroissant ; le sens est relu par `IFolderView2::GetSortColumns` pour compenser.
- Limite : quand Explorer masque les fichiers cachés, les noms listés restent visibles (le filtrage se fait à l'énumération, sur l'attribut réel).

### Phase 3 — colonne `#` (G3)

- La liste classique de DefView est en mode `LVS_OWNERDATA` **[vérifié, style `56001348` lu sur une fenêtre ouverte]** : l'ordre des lignes est celui du modèle interne de DefView, et une colonne ajoutée directement au `SysListView32` ne pourrait pas être triée.
- Explorer lit les valeurs des colonnes par `CDefCollection::Item_GetValue`, pas par `IShellFolder2::GetDetailsEx` **[vérifié]** ([mod Windhawk](https://raw.githubusercontent.com/ramensoftware/windhawk-mods/main/mods/explorer-details-better-file-sizes.wh.cpp)) ; fournir une vraie propriété demanderait des symboles.
- `CFSFolder` expose toutes les propriétés enregistrées comme colonnes natives (342 sur cette machine, via `MapColumnToSCID`) **[vérifié]** : le tri de n'importe laquelle passe donc par `CFSFolder::CompareIDs`.
- Moyen retenu : la propriété `System.ItemFolderNameDisplay` (« Folder name », calculée sans ouvrir les fichiers) sert de colonne hôte. Elle est ajoutée en tête avec `IColumnManager::SetColumns`, uniquement dans les dossiers qui ont un ordre défini. Son titre est remplacé par `#` (interception de `LVM_INSERTCOLUMN` / `LVM_SETCOLUMN`), ses valeurs par le rang (interception de `LVN_GETDISPINFO`), et son tri par l'ordre personnalisé (le `CompareIDs` déjà remplacé en phase 2, colonne reconnue par `MapColumnToSCID`). Ni symboles ni droits administrateur. **[déduit]** : DefView appelle `CompareIDs` avec l'index de cette colonne.
- Stockage de l'ordre : fichier central `%LOCALAPPDATA%\ExplorerTint\order.ini` (UTF-8), une section `[chemin du dossier]` suivie des noms dans l'ordre. Les éléments absents de la liste passent après, triés par nom. Relu automatiquement quand il change.
- Saisie de l'ordre (choix b.3) : glisser-déposer en vue Détails, bouton gauche sans modificateur. Le dépôt de DefView est enveloppé (propriété de fenêtre `OleDropTargetInterface` **[vérifié, présente sur la liste]**, remplacée par `RevokeDragDrop` / `RegisterDragDrop`). Lâcher entre deux lignes réordonne ; lâcher au milieu d'une ligne de dossier garde le comportement normal (déplacement dans le dossier). Le premier réordonnancement d'un dossier crée son ordre à partir de l'affichage courant et ajoute la colonne `#`. Un dossier qui a déjà un ordre ne se réordonne que trié par `#`.

### Phase 4 — onglet du ruban (G4)

- Le ruban est la ressource `UIFILE / EXPLORER_RIBBON` d'`ExplorerFrame.dll` **[vérifié, ressource locale]**, au format binaire du Windows Ribbon Framework, lisible avec [UICC_BML_Parser](https://github.com/rikka0w0/UICC_BML_Parser).
- Ressource modifiée — `RibbonPatch` : l'onglet Affichage (`0x1003`) et son groupe « Afficher/masquer » (`0x2024`) sont clonés avec de nouveaux identifiants (`0x7E00` onglet, `0x7E01` groupe, `0x7E02`–`0x7E05` contrôles), puis le binaire est réécrit avec longueurs, adresses et table de pointeurs recalculées. Testé sur la ressource locale par `tests/RibbonTest.exe --check` **[vérifié]**.
- Injection — `RibbonTab` : interception de `FindResourceW/ExW`, `SizeofResource`, `LoadResource` et `LockResource` dans la table d'import de `UIRibbon.dll`, pour servir la ressource modifiée ; interception de `IUIFramework::Initialize` / `Destroy` (table virtuelle de `CLSID_UIRibbonFramework`) ; au premier `Initialize`, remplacement de `IUIApplication::OnCreateUICommand` d'Explorer pour répondre aux identifiants `0x7E02`–`0x7E05`. Chaque fenêtre hôte du ruban est sous-classée.
- Contrôles de l'onglet :

| Id | Contrôle | Effet |
|---|---|---|
| `0x7E02` | case « Couleurs Git » | `Git` |
| `0x7E03` | case « Système en fin de liste » | `SystemLast` |
| `0x7E04` | case « Noms cachés » | `HideNames` |
| `0x7E05` | bouton « Paramètres » | ouvre `SettingsDialog` |

- Libellés et infobulles en français ou en anglais selon la langue de l'interface (`GetUserDefaultUILanguage`) ; icône engrenage tirée de `shell32.dll` (index 316), adaptée au DPI. L'état des cases suit `CurrentSettings()` et se met à jour dans toutes les fenêtres à chaque changement de la clé **[vérifié]**.
- `SettingsDialog` : fenêtre modale (une seule à la fois) avec les cinq options, les huit couleurs (champ texte + aperçu sur damier, sélecteur Windows qui conserve l'opacité), la liste des noms cachés et le chemin de `git.exe`. Boutons Défauts, OK, Annuler, Appliquer ; une couleur invalide est signalée et rien n'est écrit. Thème sombre quand Explorer est sombre. Écrit toutes les valeurs de la clé, ce qui déclenche la prise en compte immédiate décrite en 3.5 **[vérifié]**.
- Limite connue : la vérification a été faite avec l'interface en anglais ; les textes français n'ont pas été vus à l'écran.

### Phase 5 — icônes de dossier (G5)

- Règles, limitées aux emplacements `IconRoots` (défaut `D:\`) :
  - dossier contenant `.git` (dossier ou fichier) → icône bleue : l'icône de dossier du système dont les teintes jaunes (15°–75°) passent à 212°, saturation et luminosité conservées, pour rester dans le ton du dossier jaune ;
  - nom correspondant à une entrée de `FolderIcons` (motif `PathMatchSpec`, insensible à la casse, première entrée gagnante) → badge en bas à droite, comme l'icône du dossier Polices ;
  - les deux se combinent : un dossier `android` qui est un dépôt est bleu avec le badge Android.
- Badges : nom intégré (`android`, `claude`, `desktop`, `fonts`, `git`, `hardware`, `web`, dessinés en vectoriel avec GDI+), texte entre guillemets avec couleur facultative (`"kk"#E81123`, bleu `#0078D7` par défaut) ou mot inconnu, fichier image (`.png`, `.jpg`, `.bmp`, `.gif`) ou emplacement d'icône `fichier[,index]` (variables d'environnement acceptées). Un liseré sombre semi-transparent détache le badge du dossier et du fond.
- Construction : base = icône de dossier du système (`SHGetStockIconInfo(SIID_FOLDER)`) ou base bleue, badge composé à chaque taille (16, 20, 24, 32, 40, 48, 64, 256), écrit en `.ico` dans `%LOCALAPPDATA%\ExplorerTint\icons\<empreinte>.ico`. L'empreinte couvre la version du dessin, le type de base, le badge et la date du fichier badge : un changement donne un nouveau fichier, donc pas d'image périmée dans le cache d'icônes du shell. Environ 12 ms par icône, une seule fois **[vérifié, journal]**.
- Branchement : `IShellIcon::GetIconOf` de `CFSFolder` (table virtuelle commune, `windows.storage.dll`) renvoie l'index obtenu par `Shell_GetCachedImageIndexW` sur ce fichier. DefView passe par cet appel en vue Détails comme en grandes icônes **[vérifié]**.
- Miniatures : en vue icônes, un dossier contenant des images affiche normalement un aperçu de son contenu, obtenu par `IThumbnailCache::GetThumbnail` (`thumbcache.dll`) et non par `GetUIObjectOf` **[vérifié, trace]**. Pour les dossiers concernés, l'appel renvoie `WTS_E_FAILEDEXTRACTION` et la vue garde l'icône, comme pour un dossier à icône `desktop.ini` **[vérifié]**.
- Langages d'un dépôt — `RepoInfo` : approximation de [linguist](https://github.com/github-linguist/linguist), l'outil des statistiques de GitHub.
  - Source : l'arbre de `HEAD` (`git ls-tree -r -z -l HEAD`), comme GitHub ; taille en octets de chaque fichier.
  - Comptés : langages de type `programming` et `markup` ; un langage groupé compte pour son parent. Exclus : chemins de `vendor.yml` et `documentation.yml`, liens symboliques, fichiers binaires (octet nul dans les 8000 premiers).
  - Langage d'un fichier : nom exact, puis extension la plus longue. `.h` suit la règle de `heuristics.yml` (motifs Objective-C, puis C++, sinon C). Autre extension ambiguë : le candidat le plus présent ailleurs dans le dépôt, sinon extension principale, puis langage de `popular.yml`.
  - Six entrées au plus, le reste regroupé en « Other ». Écart mesuré avec l'API GitHub : 0,1 point au plus sur Caterpillar et Junct **[vérifié, `gh api repos/…/languages`]**.
  - Icône du projet : `.ico` ou `.png` de l'arbre, notée d'après son nom (`icon`, `app`, `favicon`, `logo`, `ic_launcher`…), sa densité Android et sa profondeur ; un PNG doit être à peu près carré.
  - Calcul sur un fil dédié (30 à 160 ms par dépôt **[vérifié, journal]**), résultat gardé en mémoire et dans `%LOCALAPPDATA%\ExplorerTint\repos.txt` (en-tête versionné). Le commit de `HEAD` est relu dans les fichiers de `.git` toutes les 5 s au plus ; un nouveau commit relance le calcul, l'ancien résultat sert en attendant. À la fin, `SHChangeNotify(SHCNE_UPDATEITEM)` fait redessiner le dossier **[vérifié]**.
  - Données : `src/LinguistData.h`, généré par `tools/gen_linguist.ps1` depuis les fichiers YAML de linguist (licence MIT).
- Badge d'un dépôt (`RepoBadge`), dans l'emplacement en bas à droite : badge de nom > icône du projet > anneau. La barre se dessine en travers du bas du dossier, indépendamment.
- Outil : `tests\bin\IconTest.exe <dossier> [--hue N] [badge …]` écrit toutes les variantes en `.ico` pour les inspecter hors d'Explorer ; `--repo <chemin>` affiche les langages et l'icône d'un dépôt et écrit ses variantes anneau, barre et icône.
- Limites : un dossier qui devient dépôt (`git init`) ne change d'icône qu'après rafraîchissement (F5) ; l'arborescence du volet de navigation n'est pas rafraîchie quand les réglages changent.

## 5. Arborescence

```
ExplorerTint/
  build.bat            compilation MSVC (x64), même schéma que Junct
  ExplorerTint.def     exports COM
  ExplorerTint.rc      version, manifeste Common Controls v6, fenêtre de réglages
  src/                 code C++
  res/                 anciennes images de la base bleue (inutilisées)
  tools/               gen_linguist.ps1 (données linguist)
  tests/               RibbonTest (binaire du ruban, sonde UI Automation), IconTest (icônes de dossier)
  docs/                ce document
```

## 6. Installation de développement

1. `build.bat`
2. En administrateur : `regsvr32 ExplorerTint.dll` (déclare la classe COM et le BHO dans HKLM).
3. Redémarrer Explorer, ou ouvrir une nouvelle fenêtre si les fenêtres de dossier tournent dans un processus séparé.

Désinstallation : `regsvr32 /u ExplorerTint.dll`, puis redémarrer Explorer.

## 7. Risques ouverts

| Risque | Effet | Vérification |
|---|---|---|
| DefView ne passe pas par `CoCreateInstance` pour `Itemsview` | Pas de bascule en liste classique | Journal `OutputDebugString` au premier test |
| Liste classique non sombre | Liste blanche dans Explorer sombre | Premier test ; correctif prévu en 3.3 |
| Index de ligne ≠ index `IFolderView` | Couleurs décalées | Premier test, tri et regroupement inclus |
| Dépôts très volumineux | `git status` lent | Mesure ; repli par dossier si nécessaire |
| Mise à jour de Windows | Symboles et ressources modifiés (phases 2 à 4) | Contrôle de version de `shell32.dll` / `windows.storage.dll` au démarrage |
