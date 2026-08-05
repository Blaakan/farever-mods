# Commandes Git — Farever Mods

Ce fichier concerne ce dépôt :

- Mon fork : https://github.com/patobeur/farever-mods
- Dépôt original : https://github.com/Blaakan/farever-mods
- Pull Requests originales : https://github.com/Blaakan/farever-mods/pulls
- Branche de travail utilisée : `Language_Version`

## 1. Vérifier la branche et les remotes

```powershell
git status
git branch --show-current
git remote -v
```

La configuration attendue est :

```text
origin   https://github.com/patobeur/farever-mods.git
upstream https://github.com/Blaakan/farever-mods.git
```

`origin` désigne mon fork. `upstream` désigne le dépôt original.

## 2. Enregistrer mes modifications

```powershell
git status
git diff
git add .
git diff --staged
git commit -m "Décrire brièvement les modifications"
```

Exemple :

```powershell
git commit -m "Isole les sauvegardes par compte et personnage"
```

## 3. Pousser vers mon fork

Pour envoyer la branche vers mon fork :

```powershell
git push -u origin Language_Version
```

Après le premier envoi, cette commande suffit généralement :

```powershell
git push origin Language_Version
```

## 4. Mettre à jour la branche avec le dépôt original

Récupérer les changements du dépôt original :

```powershell
git fetch upstream
```

Mettre à jour ma branche avec la branche principale originale :

```powershell
git switch Language_Version
git merge upstream/main
```

Puis pousser la branche mise à jour vers mon fork :

```powershell
git push origin Language_Version
```

En cas de conflit, corriger les fichiers indiqués, puis :

```powershell
git add .
git commit -m "Résout les conflits avec upstream/main"
git push origin Language_Version
```

## 5. Créer une Pull Request vers le dépôt original

Ouvrir cette page :

https://github.com/Blaakan/farever-mods/compare/main...patobeur:farever-mods:Language_Version

Vérifier que la comparaison est :

```text
base repository : Blaakan/farever-mods
base branch     : main
head repository : patobeur/farever-mods
compare branch  : Language_Version
```

Puis cliquer sur **Create pull request**.

Avec GitHub CLI, si elle est installée :

```powershell
gh pr create --repo Blaakan/farever-mods --head patobeur:Language_Version --base main --fill
```

## 6. Pousser directement vers le dépôt original

Cette commande ne fonctionnera que si Blaakan m’a accordé un accès en écriture :

```powershell
git push -u upstream Language_Version
```

Si GitHub répond `403 Permission denied`, je n’ai pas les droits nécessaires. Dans ce cas, pousser vers `origin` puis utiliser une Pull Request depuis le fork.

## 7. Vérifier le dernier commit envoyé

```powershell
git log -1 --oneline
git status
git ls-remote --heads origin Language_Version
git ls-remote --heads upstream Language_Version
```

## 8. Commandes utiles en cas d’erreur

Voir les fichiers modifiés :

```powershell
git status
git diff
```

Voir les fichiers déjà préparés pour le commit :

```powershell
git diff --staged
```

Annuler la préparation d’un fichier sans supprimer ses modifications :

```powershell
git restore --staged chemin\vers\fichier
```

Annuler les modifications locales d’un fichier — commande destructive :

```powershell
git restore chemin\vers\fichier
```

Changer de branche :

```powershell
git switch Language_Version
```

Créer une nouvelle branche :

```powershell
git switch -c nom-de-la-branche
```

## Règle simple à retenir

Le fonctionnement recommandé est :

```powershell
git add .
git commit -m "Description des changements"
git push origin Language_Version
```

Puis créer ou mettre à jour la Pull Request vers `Blaakan/farever-mods`.

`origin` = mon fork. `upstream` = dépôt original.
## Que signifie `upstream` ?

Dans Git, `origin` et `upstream` sont simplement des noms donnés à des dépôts distants :

- `origin` est mon dépôt distant principal, ici mon fork : `patobeur/farever-mods`.
- `upstream` est le dépôt source dont mon fork provient, ici `Blaakan/farever-mods`.

Ces noms ne donnent aucun droit particulier. Ils servent seulement à savoir vers quel dépôt une commande s’applique.

Par exemple :

```powershell
git push origin Language_Version
```

envoie ma branche vers mon fork, tandis que :

```powershell
git push upstream Language_Version
```

essaie de l’envoyer directement vers le dépôt original. Cette dernière commande nécessite une autorisation d’écriture sur le dépôt de Blaakan.

La Pull Request est le lien entre les deux : je pousse vers `origin`, puis je demande à `upstream` d’intégrer ma branche.