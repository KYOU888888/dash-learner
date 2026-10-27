# Dash Learner — mod Geode pour Geometry Dash 2.2081

Un bot qui apprend à finir un niveau dans le vrai jeu, par essais et erreurs.

## Comment il apprend

Le bot décide, pour chaque tick du jeu (240 par seconde), s'il appuie ou non.

1. Premier essai : il n'appuie jamais, et meurt à un endroit.
2. Il modifie ses appuis juste avant cet endroit (décaler un saut de quelques ticks, ajouter un appui, en supprimer un, ou réinventer la zone).
3. Il rejoue tout depuis le début. Si l'essai va plus loin, il devient le nouveau meilleur. Sinon, on garde l'ancien.
4. Plus il bloque au même endroit, plus il remonte loin en arrière pour corriger (de 1/8 s jusqu'à 3 s).
5. Quand il finit le niveau, il garde la solution et la rejoue.

Comme Geometry Dash est déterministe, tout ce qui est avant la zone modifiée se rejoue exactement pareil. C'est ce qui permet au bot de progresser.

## Obtenir le fichier .geode (sans rien installer sur ton PC)

1. Crée un compte sur github.com si tu n'en as pas.
2. Crée un nouveau dépôt (bouton **New**), par exemple `dash-learner`.
3. Clique sur **uploading an existing file** et glisse **tout le contenu** du dossier (y compris le dossier `.github`).
   - Si le dossier `.github` ne passe pas par glisser-déposer, crée le fichier à la main : **Add file → Create new file**, nom `.github/workflows/build.yml`, et colle le contenu du fichier.
4. Va dans l'onglet **Actions**. La compilation démarre toute seule (environ 5 à 10 minutes).
5. Quand c'est vert, ouvre le build, descends en bas : télécharge **Build Output**. Dedans il y a `pierre.dash-learner.geode`.

## Installer dans le jeu

1. Il faut Geode installé (geode-sdk.org) sur Geometry Dash **2.2081**.
2. Copie `pierre.dash-learner.geode` dans le dossier `geode/mods` du jeu.
   - Steam : clic droit sur Geometry Dash → Gérer → Parcourir les fichiers locaux → `geode` → `mods`.
3. Relance le jeu.

## Utilisation

- Lance un niveau classique en mode normal : le bot prend la main.
- En haut à gauche : numéro d'essai, record en %, compteur de blocage.
- Menu pause : **Bot: ON/OFF**, **Oublier** (repart de zéro sur ce niveau).
- Réglages (Geode → Dash Learner → réglages) :
  - **Vitesse d'entraînement** x1 à x8. Coupe la musique dans les options du jeu si tu accélères.
  - **Mode sans sauvegarde** (activé par défaut) : les % et la complétion du bot ne comptent pas comme les tiens.
  - **Afficher les infos**.

L'apprentissage est sauvegardé par niveau dans le dossier de sauvegarde du mod. Tu peux quitter et reprendre.

## Limites

- Pas de niveaux plateforme, pas de mode practice, joueur 1 seulement (pas de dual).
- Les niveaux avec des triggers aléatoires (Random) peuvent l'embrouiller, car le rejeu n'est plus identique.
- Un mod de noclip ou un autre bot actif en même temps fausse l'apprentissage.
- Sur un niveau long et dur, ça peut prendre des milliers d'essais. La vitesse x4–x8 aide beaucoup.
- Un niveau fini par un bot ne doit pas être présenté comme fini par toi (leaderboards, records).
