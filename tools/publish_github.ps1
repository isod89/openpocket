# OpenPocket: publish on GitHub, from this folder (PUBLISH-GITHUB.bat, PUBLISH-SOURCE.bat).
#   1. the source: GitHub's branch main gets exactly the files of THIS folder (v36: whatever was on GitHub before):
#      the commits of openpocket.bundle are merged in when it is here (history), then the folder's files are
#      committed on top. Never forced: GitHub's own history (edits made on the site) is kept underneath.
#      The tag vNN is set on that commit.
#   2. the release vNN: the .fwsc packages build.bat made (docs\firmware), notes from docs\WHATS-NEW-*.md
#   3. the web installer on GitHub Pages (branch gh-pages: docs\ with the packages)
#   -SourceOnly: step 1 only (no build needed)
# Uses your own Git and GitHub CLI (gh) and your own GitHub login. gh-pages is generated: it is replaced each time.
param([string]$Owner = "", [string]$Repo = "openpocket", [switch]$SourceOnly)
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot)
function Fail([string]$m) { Write-Host ""; Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function Step([string]$m) { Write-Host "== $m" -ForegroundColor Cyan }

$ver = (Select-String -Path "firmware\openpocket.c" -Pattern '#define OP_VERSION_NUM "(\d+)"').Matches[0].Groups[1].Value
if (-not $ver) { Fail "OP_VERSION_NUM not found in firmware\openpocket.c" }
$Tag = "v$ver"
Write-Host ""
Write-Host "  O P E N P O C K E T   $Tag   ->   GitHub" -ForegroundColor White

function Find-Tool([string]$t) {
  if (Get-Command $t -ErrorAction SilentlyContinue) { return $true }
  $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
  if (Get-Command $t -ErrorAction SilentlyContinue) { return $true }
  $dirs = if ($t -eq "gh") { @("$env:ProgramFiles\GitHub CLI", "$env:LOCALAPPDATA\Programs\GitHub CLI") }
          else { @("$env:ProgramFiles\Git\cmd", "$env:LOCALAPPDATA\Programs\Git\cmd") }
  foreach ($d in $dirs) { if ($d -and (Test-Path (Join-Path $d "$t.exe"))) { $env:Path = "$d;$env:Path"; return $true } }
  return $false
}
$ids = @{ git = "Git.Git"; gh = "GitHub.cli" }
foreach ($t in @("git", "gh")) {
  if (Find-Tool $t) { continue }
  if (Get-Command winget -ErrorAction SilentlyContinue) {
    $a = Read-Host "$t is not installed. Install it now with winget? (Y/N)"
    if ($a -match '^[yYoO]') {
      winget install --id $ids[$t] -e --source winget --accept-package-agreements --accept-source-agreements
      if (Find-Tool $t) { continue }
      Fail "$t is installed but not visible yet: close this window and run PUBLISH-GITHUB.bat again"
    }
  }
  Fail "$t not found. Install it, then run PUBLISH-GITHUB.bat again."
}
gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
  Step "Signing in to GitHub (the browser will open)"
  gh auth login --web --git-protocol https
  if ($LASTEXITCODE -ne 0) { Fail "signing in to GitHub failed" }
}
gh auth setup-git *> $null
if (-not $Owner) { $Owner = "$(gh api user --jq .login)".Trim() }
if (-not $Owner) { Fail "could not read your GitHub user name" }
$full = "$Owner/$Repo"
$pages = "https://$Owner.github.io/$Repo/"
$email = "$Owner@users.noreply.github.com"
$work = Join-Path "build" "publish"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null

# 1. the source: GitHub's main + (the bundle's history) + exactly this folder's files
$src = (Get-Location).Path
$repoDir = Join-Path $work "repo"
Step "The source of $Tag -> $full (branch main)"
gh repo view $full *> $null
$exists = $LASTEXITCODE -eq 0
if (-not $exists) {
  Step "Creating the public repository $full"
  gh repo create $full --public --description "OpenPocket: open-source guitar firmware for the Donner Pocket X / M-VAVE BlackBox" --homepage $pages
  if ($LASTEXITCODE -ne 0) { Fail "creating the repository $full" }
}
$hasMain = $exists -and ("$(git ls-remote --heads "https://github.com/$full.git" main)".Trim() -ne "")
if ($hasMain) {
  # core.autocrlf=false from the clone on: with Git for Windows' default (true) the files would be checked out with CRLF,
  # then the config below makes them all "modified" and the history merge refuses ("local changes would be overwritten")
  git -c core.autocrlf=false clone --quiet --branch main "https://github.com/$full.git" $repoDir
  if ($LASTEXITCODE -ne 0) { Fail "git clone of https://github.com/$full" }
  Push-Location $repoDir
} else {
  New-Item -ItemType Directory -Force $repoDir | Out-Null
  Push-Location $repoDir
  git init --quiet
  git checkout --quiet -b main
  git remote add origin "https://github.com/$full.git"
}
git config user.name "$Owner"
git config user.email "$email"
git config core.autocrlf false
if ($hasMain) { git reset --quiet --hard HEAD }   # (the working files exactly as committed, whatever was checked out)
# the history: the bundle's commits (each version's own commit messages), when the bundle is here
$bundle = Join-Path $src "openpocket.bundle"
$bundleOk = $false
if (Test-Path $bundle) {
  git fetch --quiet $bundle "main:refs/bundle/main"
  if ($LASTEXITCODE -eq 0) {
    $bundleOk = $true
    if ($hasMain) { git merge --quiet --no-edit -X ours --allow-unrelated-histories -m "OpenPocket ${Tag}: merge the version history" refs/bundle/main }
    else { git reset --quiet --hard refs/bundle/main }
    if ($LASTEXITCODE -ne 0) { git merge --abort *> $null; Write-Host "  (the history could not be merged: only the files are published)" -ForegroundColor Yellow }
  }
}
# the files: the version's file list (the bundle's), each one taken from this folder; without a bundle, the whole
# folder. (.gitignore keeps builds, packages and backups out; files left over from older versions are not published.)
$list = @()
if ($bundleOk) { $list = @(git ls-tree -r --name-only refs/bundle/main) }
git rm -r -q --cached . *> $null
Get-ChildItem -Force | Where-Object { $_.Name -ne ".git" } | Remove-Item -Recurse -Force
if ($list.Count -gt 0) {
  git checkout --quiet refs/bundle/main -- .
  foreach ($f in $list) {
    $from = Join-Path $src ($f -replace '/', '\')
    if (Test-Path -LiteralPath $from -PathType Leaf) { Copy-Item -LiteralPath $from -Destination ($f -replace '/', '\') -Force }
  }
} else {
  robocopy $src . /E /XD (Join-Path $src ".git") (Join-Path $src "build") (Join-Path $src "firmware\build") /XF openpocket.bundle /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Pop-Location; Fail "copying the folder's files (robocopy $LASTEXITCODE)" }
}
git add -A
$changes = "$(git status --porcelain)".Trim()
if ($changes) {
  Write-Host "  files that differ from GitHub (+ the history merged):" -ForegroundColor DarkGray
  git diff --cached --stat HEAD | Select-Object -Last 12
  git commit --quiet -m "OpenPocket ${Tag}: the source of the release"
  if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "git commit" }
}
Step "Pushing the main branch to $full"
git push -u origin main
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail "git push was refused (someone pushed meanwhile?): run it again. Nothing was forced." }
# the tag vNN on that commit (moved if it already pointed elsewhere: v30..v33 pointed at an old main)
$head = "$(git rev-parse HEAD)".Trim()
$remoteTag = "$(git ls-remote --tags origin "refs/tags/$Tag^{}")".Trim()
if (-not $remoteTag) { $remoteTag = "$(git ls-remote --tags origin "refs/tags/$Tag")".Trim() }
if (-not $remoteTag -or -not $remoteTag.StartsWith($head)) {
  if ($remoteTag) { git push --quiet origin ":refs/tags/$Tag" }
  git tag -f -a $Tag -m "OpenPocket $Tag"
  git push --quiet origin $Tag
  if ($LASTEXITCODE -ne 0) { Write-Host "  (the tag $Tag could not be pushed)" -ForegroundColor Yellow }
  # moving a tag turns its release (if there is one) into a draft on GitHub, and the list then shows an older one as Latest:
  # published again here too, so that the source-only way leaves the release as it was
  elseif ($remoteTag) {
    gh release view $Tag --repo $full *> $null
    if ($LASTEXITCODE -eq 0) {
      gh release edit $Tag --repo $full --draft=false --tag $Tag --latest *> $null
      if ($LASTEXITCODE -eq 0) { Write-Host "  release $Tag published again (moving its tag had made it a draft)" }
    }
  }
}
Pop-Location
Write-Host "Source:      https://github.com/$full/tree/main   (tag $Tag)" -ForegroundColor Green
if ($SourceOnly) { Write-Host ""; Write-Host "Source only: no release, no installer update (PUBLISH-GITHUB.bat does all three)."; exit 0 }

# 2. the release
$pk = @(Get-ChildItem "docs\firmware\*.fwsc" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
if ($pk.Count -eq 0) { Fail "no package in docs\firmware: run build.bat first" }
$notes = Join-Path $work "notes.md"
$text = "OpenPocket $Tag`n`nInstall from the browser (Chrome / Edge): $pages`nPreset editor (v32 and newer): ${pages}editor.html`nWithout internet: INSTALL-OFFLINE.bat in the source.`n`nPackages: PocketX_9$ver.fwsc (Donner Pocket X), BlackBox_9$ver.fwsc (M-VAVE BlackBox).`nSource: https://github.com/$full/tree/$Tag"
$wn = Get-ChildItem "docs\WHATS-NEW-*.md" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if ($wn) { $text += "`n`n" + [IO.File]::ReadAllText($wn.FullName) }
[IO.File]::WriteAllText($notes, $text, (New-Object Text.UTF8Encoding $false))
gh release view $Tag --repo $full *> $null
if ($LASTEXITCODE -eq 0) {
  Step "The release $Tag exists: updating its files and notes"
  gh release upload $Tag @pk --repo $full --clobber
  # moving the tag (above) turns its release into a draft on GitHub, and a draft cannot be "latest": published again here
  if ($LASTEXITCODE -eq 0) { gh release edit $Tag --repo $full --draft=false --tag $Tag --notes-file $notes --latest }
} else {
  Step "Creating the release $Tag"
  gh release create $Tag @pk --repo $full --latest --title "OpenPocket $Tag" --notes-file $notes
}
if ($LASTEXITCODE -ne 0) { Fail "the release $Tag" }

# 3. GitHub Pages: the installer (docs\ with the packages)
Step "The web installer on GitHub Pages"
$pg = Join-Path $work "pages"
New-Item -ItemType Directory -Force $pg | Out-Null
Copy-Item -Recurse "docs\*" $pg
New-Item -ItemType File -Force (Join-Path $pg ".nojekyll") | Out-Null
Push-Location $pg
git init --quiet
git checkout --quiet -b gh-pages
git add -A
git -c user.name="$Owner" -c user.email="$email" commit --quiet -m "OpenPocket ${Tag}: the web installer"
git push --force "https://github.com/$full.git" gh-pages
$ok = $LASTEXITCODE -eq 0
Pop-Location
if (-not $ok) { Fail "pushing the gh-pages branch" }
gh api -X POST "repos/$full/pages" -f "source[branch]=gh-pages" -f "source[path]=/" *> $null   # (already on: refused, fine)

Write-Host ""
Write-Host "Repository:  https://github.com/$full" -ForegroundColor Green
Write-Host "Release:     https://github.com/$full/releases/tag/$Tag" -ForegroundColor Green
Write-Host "Installer:   $pages   (the first time, GitHub takes a minute or two)" -ForegroundColor Green
Write-Host "Editor:      ${pages}editor.html" -ForegroundColor Green
