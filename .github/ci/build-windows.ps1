# Windows build with MSVC, everything static (the C runtime and vcpkg's
# x64-windows-static libraries): one ifritview.exe that needs nothing but
# Windows. Release-only libraries (x64-windows-static-release) halve the
# first build. Usage, on a runner with Visual Studio 2022:
#   build-windows.ps1 configure   # VCPKG_TOOLCHAIN set; builds the libraries first time
#   build-windows.ps1 package     # builds; leaves the zip and installer in dist\
param([Parameter(Mandatory)][ValidateSet('configure', 'package')] [string] $Step)
$ErrorActionPreference = 'Stop'
$build = 'build-win'

# A tag v1.2 builds version 1.2 ('-' becomes '~', as in the Linux packages).
$version = ''
if ($env:GITHUB_REF_TYPE -eq 'tag') { $version = $env:GITHUB_REF_NAME.TrimStart('v') -replace '-', '~' }

# A failing command's error lines (or its last lines) become an annotation on
# the run, readable on its summary page and through the API.
function Run([string] $exe, [string[]] $arguments) {
    $log = New-TemporaryFile
    & $exe @arguments 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE) {
        $lines = Get-Content $log | Where-Object { $_ -match 'error|fatal|failed|not found' } | Select-Object -Last 15
        if (-not $lines) { $lines = Get-Content $log | Select-Object -Last 15 }
        Write-Output ("::error title=$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)::" + ($lines -join '%0A'))
        throw "$exe failed ($LASTEXITCODE)"
    }
}
trap {
    Write-Output "::error title=build-windows.ps1 $Step::$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
    break
}

if ($Step -eq 'configure') {
    if (-not $env:VCPKG_TOOLCHAIN) { throw 'VCPKG_TOOLCHAIN is not set' }
    $arguments = @('-S', '.', '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_TOOLCHAIN", '-DVCPKG_TARGET_TRIPLET=x64-windows-static-release')
    if ($version) { $arguments += "-DIFRIT_VERSION=$version" }
    Run cmake $arguments
    exit 0
}

Run cmake @('--build', $build, '--config', 'Release', '--target', 'ifritview', '--parallel')
& "$build\Release\ifritview.exe" --version
if ($LASTEXITCODE) { throw 'ifritview --version failed' }

# Nothing but Windows: no runtime or library DLL may be needed beside it.
Run python @('third_party/rapport/ci/check-imports.py', "$build\Release\ifritview.exe")

$stage = 'dist\IfritView'
Remove-Item -Recurse -Force dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\plugins" | Out-Null
Copy-Item "$build\Release\ifritview.exe" "$stage\"
# Format plugins go in plugins, beside the exe; the interface they're written to waits there.
Copy-Item include\ifritview_plugin.h "$stage\plugins\"
Copy-Item README.md, LICENSE $stage
# Translations, IfritView's and rapport's, in i18n beside the exe.
New-Item -ItemType Directory -Force "$stage\i18n" | Out-Null
Get-ChildItem i18n\*.po, third_party\rapport\i18n\*.po -ErrorAction SilentlyContinue | Copy-Item -Destination "$stage\i18n\"
# The licence notices of everything built into the exe: the bundled code and
# each vcpkg library linked (third-party-notices.py lists them).
$installed = (Select-String -Path "$build\CMakeCache.txt" -Pattern '^_VCPKG_INSTALLED_DIR:PATH=(.*)').Matches[0].Groups[1].Value
Run python @('.github/ci/third-party-notices.py', 'windows', "$installed\x64-windows-static-release\share", "$stage\THIRD-PARTY-NOTICES.txt")

$suffix = if ($version) { "-$version" } else { '' }
$appVersion = if ($version) { $version -replace '~.*', '' } else {
    (Select-String -Path CMakeLists.txt -Pattern 'project\(IfritView VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}
Compress-Archive -Path $stage -DestinationPath "dist\ifritview$suffix-windows-x86_64.zip"
$makensis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
if (-not $makensis) { $makensis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
if (-not (Test-Path $makensis)) {
    # Not on the runner image any more: from Chocolatey, which is.
    Run choco @('install', 'nsis', '--no-progress', '-y')
}
Run $makensis @('/V2', "/DVERSION=$appVersion", "/DSTAGE=$PWD\$stage",
    "/DOUT=$PWD\dist\ifritview$suffix-windows-x86_64-setup.exe", "$PWD\data\windows\ifritview.nsi")
Remove-Item -Recurse -Force $stage
Get-ChildItem dist
