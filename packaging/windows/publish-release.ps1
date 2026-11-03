# Publishes PitchNet Stars Clique Edition 1.0.0 on GitHub.
#
# Run from the repository root, after checking everything (see RELEASING.md):
#   powershell -ExecutionPolicy Bypass -File packaging\windows\publish-release.ps1
#
# What it does:
#   1. checks that the installer and both manuals exist;
#   2. makes the repository public (asks first);
#   3. creates the v1.0.0 release with the installer and the PDFs (asks first).

$ErrorActionPreference = 'Stop'
$repo    = '9519860664r-oss/pitchnet-stars-clique'
$tag     = 'v1.0.0'
$title   = 'PitchNet Stars Clique Edition 1.0.0'
$notes   = 'docs\release-notes-1.0.0.md'
$assets  = @(
    'Output\PitchNet-StarsClique-1.0.0-Setup.exe',
    'docs\PitchNet-StarsClique-Manual-EN.pdf',
    'docs\PitchNet-StarsClique-Manual-RU.pdf'
)

foreach ($f in @($notes) + $assets) {
    if (-not (Test-Path $f)) { throw "Missing: $f" }
}
Write-Host "Files found:" -ForegroundColor Cyan
$assets | ForEach-Object { '  {0}  ({1:N1} MB)' -f $_, ((Get-Item $_).Length / 1MB) }

$branch = git rev-parse --abbrev-ref HEAD
$pending = git status --porcelain
if ($pending) { Write-Warning "Uncommitted changes:`n$pending" }
git fetch -q mine
$ahead = git rev-list --count "mine/$branch..HEAD"
if ([int]$ahead -gt 0) { throw "$ahead local commit(s) not pushed. Run: git push mine $branch" }

$visibility = gh repo view $repo --json visibility -q .visibility
Write-Host "Repository is $visibility." -ForegroundColor Cyan
if ($visibility -ne 'PUBLIC') {
    if ((Read-Host "Make $repo PUBLIC? Everyone will see the code and the commit history (yes/no)") -ne 'yes') { return }
    gh repo edit $repo --visibility public --accept-visibility-change-consequences
    gh repo edit $repo --description 'Neural vocal pitch editor (VST3, Windows). Based on PitchNet by Session Loops & contributors, derived from HachiTune. Modified by Goth Angel (Stars Clique): multitrack editing for FL Studio, recording, tuning tools, new skin. AGPLv3.' --add-topic vst3 --add-topic pitch-correction --add-topic vocal --add-topic fl-studio --add-topic juce --add-topic pitchnet
}

if ((Read-Host "Create release $tag with the installer and both manuals? (yes/no)") -ne 'yes') { return }
gh release create $tag @assets --repo $repo --target $branch --title $title --notes-file $notes --latest
Write-Host "Done: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
