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

function Run([string] $exe, [string[]] $arguments) {
    & $exe @arguments
    if ($LASTEXITCODE) { throw "$exe failed ($LASTEXITCODE)" }
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
# (DNSAPI and IPHLPAPI are Windows' own, for GLib's networking.)
Run python @('third_party/rapport/ci/check-imports.py', '--allow', 'DNSAPI.dll', '--allow', 'IPHLPAPI.DLL',
    "$build\Release\ifritview.exe")

$stage = 'dist\IfritView'
Remove-Item -Recurse -Force dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\bin" | Out-Null
Copy-Item "$build\Release\ifritview.exe" "$stage\bin\"
Copy-Item README.md, LICENSE $stage

$suffix = if ($version) { "-$version" } else { '' }
$appVersion = if ($version) { $version -replace '~.*', '' } else {
    (Select-String -Path CMakeLists.txt -Pattern 'project\(IfritView VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}
Compress-Archive -Path $stage -DestinationPath "dist\ifritview$suffix-windows-x86_64.zip"
$makensis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
if (-not $makensis) { $makensis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe" }
Run $makensis @('/V2', "/DVERSION=$appVersion", "/DSTAGE=$PWD\$stage",
    "/DOUT=$PWD\dist\ifritview$suffix-windows-x86_64-setup.exe", "$PWD\data\windows\ifritview.nsi")
Remove-Item -Recurse -Force $stage
Get-ChildItem dist
