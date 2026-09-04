# Packages the Vita build into a VPK. This step is NOT part of the CMake/ninja
# build (the "mdkr64" CMake target only produces the raw linked ELF) -- it has
# to be run manually after every rebuild that should reach real hardware.
#
# Usage: pwsh -File tools/package_vita.ps1 [-BuildDir build-vita] [-IncludeIcons]
#
# Requires $env:VITASDK to be set and the VitaSDK toolchain available on PATH.

param(
    [string]$BuildDir = "build-vita",
    # KNOWN ISSUE: bundling the LiveArea assets (icon0/bg/startup/template.xml)
    # via vita-pack-vpk's -a flag currently breaks VitaShell's install on the
    # real hardware this project is tested against (confirmed twice: once
    # before the -Wl,-q linker fix, and again after it, so -Wl,-q was NOT the
    # actual cause as originally suspected -- something about these specific
    # assets or how vita-pack-vpk embeds them is still the problem). Leave
    # this off (bare eboot + param.sfo only) until that's root-caused; only
    # pass -IncludeIcons for a deliberate one-off test of that specific bug.
    [switch]$IncludeIcons
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repoRoot $BuildDir
$livearea = Join-Path $repoRoot "vita\livearea"

$env:Path = "$env:VITASDK\bin;" + $env:Path

Push-Location $build
try {
    if (-not (Test-Path "mdkr64")) {
        throw "mdkr64 (raw linked ELF) not found in $build -- run the CMake/ninja build first."
    }

    Copy-Item mdkr64 mdkr64.elf.unstripped -Force
    Copy-Item mdkr64 mdkr64.elf -Force
    arm-vita-eabi-strip -g mdkr64.elf

    vita-elf-create mdkr64.elf mdkr64.velf
    if ($LASTEXITCODE -ne 0) { throw "vita-elf-create failed" }

    # -c: compress the eboot (matches the original working build's size profile).
    # -pm 0x2000000 (32MB): physically-contiguous memory budget. Without this,
    # vitaGL's vglInitExtended silently fails (returns GL_FALSE) despite
    # reporting healthy free-memory-pool numbers afterward -- the game boots,
    # audio/input/game logic all run fine, but nothing ever renders. This was
    # diagnosed via mdkr_vita_boot_log instrumentation around vglInitExtended.
    # No -s: default is full/"unsafe" permissions, needed for the 256MB
    # newlib heap (_newlib_heap_size_user in main_pc.c).
    Remove-Item eboot.bin -Force -ErrorAction SilentlyContinue
    vita-make-fself -c -pm 0x2000000 mdkr64.velf eboot.bin
    if ($LASTEXITCODE -ne 0) { throw "vita-make-fself failed" }

    Remove-Item mdkr64.vpk -Force -ErrorAction SilentlyContinue
    if ($IncludeIcons) {
        vita-pack-vpk -s param.sfo -b eboot.bin `
            -a "$livearea\icon0.png=sce_sys/icon0.png" `
            -a "$livearea\bg.png=sce_sys/livearea/contents/bg.png" `
            -a "$livearea\startup.png=sce_sys/livearea/contents/startup.png" `
            -a "$livearea\template.xml=sce_sys/livearea/contents/template.xml" `
            mdkr64.vpk
    } else {
        vita-pack-vpk -s param.sfo -b eboot.bin mdkr64.vpk
    }
    if ($LASTEXITCODE -ne 0) { throw "vita-pack-vpk failed" }

    Get-ChildItem mdkr64.elf, mdkr64.velf, eboot.bin, mdkr64.vpk | Select-Object Name, Length, LastWriteTime
}
finally {
    Pop-Location
}
