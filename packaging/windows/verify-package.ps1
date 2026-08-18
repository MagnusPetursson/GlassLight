param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [string]$ExpectedVersion = "0.2.0"
)

$ErrorActionPreference = "Stop"
$archivePath = (Resolve-Path $Archive).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("glasslight-package-" + [guid]::NewGuid())

try {
    Expand-Archive -Path $archivePath -DestinationPath $work
    $roots = @(Get-ChildItem -Path $work -Directory)
    if ($roots.Count -ne 1) {
        throw "Expected one top-level package directory, found $($roots.Count)."
    }
    $root = $roots[0].FullName
    $expected = @(
        "GlassLight.exe",
        "README.md",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "shaders/first_light.comp.spv",
        "shaders/glass_preview.comp.spv",
        "shaders/first_light.comp.fracture.spv",
        "shaders/glass_preview.comp.fracture.spv",
        "shaders/resolve.comp.spv"
    )
    foreach ($relative in $expected) {
        if (!(Test-Path (Join-Path $root $relative) -PathType Leaf)) {
            throw "Missing packaged artifact: $relative"
        }
    }

    $actualFiles = @(Get-ChildItem -Path $root -File -Recurse | ForEach-Object {
        [System.IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')
    } | Sort-Object)
    $expectedFiles = @($expected | Sort-Object)
    if (Compare-Object $expectedFiles $actualFiles) {
        throw "The Windows package contains unexpected or missing files."
    }

    $version = (Get-Item (Join-Path $root "GlassLight.exe")).VersionInfo.ProductVersion
    if ($version -ne $ExpectedVersion) {
        throw "Expected product version $ExpectedVersion, found $version."
    }

    $headers = & dumpbin /headers (Join-Path $root "GlassLight.exe") | Out-String
    if ($headers -notmatch "Windows GUI") {
        throw "GlassLight.exe is not linked as a Windows GUI application."
    }
    $dependencies = & dumpbin /dependents (Join-Path $root "GlassLight.exe") | Out-String
    if ($dependencies -match "SDL3\.dll|VCRUNTIME[0-9_]*\.dll|MSVCP[0-9_]*\.dll") {
        throw "GlassLight.exe unexpectedly requires a packaged or MSVC runtime DLL."
    }
    if ($dependencies -notmatch "vulkan-1\.dll") {
        throw "GlassLight.exe does not declare the expected system Vulkan loader dependency."
    }
    Write-Host "Windows package contract is valid: $archivePath"
}
finally {
    if (Test-Path $work) {
        Remove-Item -Path $work -Recurse -Force
    }
}
