#Requires -Version 7
<#
.SYNOPSIS
  LA_AlphaPolyLimit harness tests without starting Gothic.
  Reads Gothic2.exe and the vob 3DS files of your Gothic II folder READ-ONLY, writes logs to -OutDir.
.PARAMETER Test
  patch : code fingerprints + both patch sites original -> patched -> restored (+ foreign-patch case)
  degen : synthetic meshes through the engine's zCMesh::RemoveDegeneratePolys, original (double free must show) and patched
  alpha : engine GetFreeAlphaWorldPoly wrap + AddAlphaSortObject (hang/cycle must show) vs. LA_AlphaPolyLimit allocator; stub registers
  files : real vob 3DS: an optional list (-VobList, one 3DS path per line, world order) and all vob 3DS under -MeshDir;
          orig stops at the first file with a predicted double free and shows the pool corruption, patched runs all files
  all   : everything
.PARAMETER GameDir  Gothic II folder with System\Gothic2.exe (Steam 2.6 fix). Mandatory, only read.
.PARAMETER OutDir   logs (default: test\out next to this script)
.PARAMETER MeshDir  folder searched for vob 3DS files (default: <GameDir>\_work\Data\Meshes)
.PARAMETER VobList  optional text file with 3DS paths to test in that order
.EXAMPLE
  pwsh -NoProfile -File test\run_tests.ps1 -GameDir '<Gothic II folder>' -Test all
#>
param([Parameter(Mandatory = $true)][string]$GameDir,
    [ValidateSet('patch', 'degen', 'alpha', 'files', 'all')][string]$Test = 'all',
    [string]$OutDir = '', [string]$MeshDir = '', [string]$VobList = '')
$ErrorActionPreference = 'Stop'
$out = if ($OutDir) { $OutDir } else { Join-Path $PSScriptRoot 'out' }
New-Item -ItemType Directory -Force $out | Out-Null
$harness = Join-Path $PSScriptRoot '..\build\laap_harness.exe'
if (-not (Test-Path $harness)) { & cmd /c (Join-Path $PSScriptRoot '..\build.bat'); if ($LASTEXITCODE) { exit 1 } }
$exe = Join-Path $GameDir 'System\Gothic2.exe'
$fail = 0
function Run([string]$name, [string[]]$a) {
    $log = Join-Path $out "test_$name.log"
    Write-Host "== $name -> $log"
    & $harness $exe @a *> $log
    $code = $LASTEXITCODE
    Get-Content $log | Select-String '^RESULT|FATAL|FAIL' | ForEach-Object { Write-Host "   $_" }
    if ($code -ne 0) { Write-Host "   exit $code"; $script:fail++ }
}
if ($Test -in 'patch', 'all') {
    Run 'fingerprint' @('fingerprint')
    Run 'patchcheck' @('patchcheck')
}
if ($Test -in 'degen', 'all') {
    Run 'degen_orig' @('degen', 'orig')
    Run 'degen_patched' @('degen', 'patched')
}
if ($Test -in 'alpha', 'all') { Run 'alpha' @('alpha') }
if ($Test -in 'files', 'all') {
    $lists = @()
    if ($VobList) { if (Test-Path $VobList) { $lists += , @('vobload', $VobList) } else { Write-Host "   $VobList missing" } }
    # all vob 3DS: everything under MeshDir except the level mesh (Level\<world>\*.3DS) and its tiles
    $meshes = if ($MeshDir) { $MeshDir } else { Join-Path $GameDir '_work\Data\Meshes' }
    $all = Join-Path $out 'list_allvobs.txt'
    Get-ChildItem $meshes -Recurse -Filter *.3DS | Where-Object {
        $_.FullName -notmatch '\\WK_TILES\\' -and -not ($_.Directory.Parent -and $_.Directory.Parent.Name -eq 'Level')
    } | Sort-Object FullName | ForEach-Object FullName | Set-Content $all
    $lists += , @('allvobs', $all)
    foreach ($l in $lists) {
        Run "files_$($l[0])_orig" @('files', $l[1], 'orig')
        Run "files_$($l[0])_patched" @('files', $l[1], 'patched')
    }
}
exit $fail
