# Packages the Vita build into a VPK. This step is NOT part of the CMake/ninja
# build (the "mdkr64" CMake target only produces the raw linked ELF) -- it has
# to be run manually after every rebuild that should reach real hardware.
#
# Usage: pwsh -File tools/package_vita.ps1 [-BuildDir build-vita] [-NoIcons]
#
# Requires $env:VITASDK to be set and the VitaSDK toolchain available on PATH.

param(
    [string]$BuildDir = "build-vita",
    # LiveArea assets (icon0/bg/startup/template.xml) are bundled by default.
    # This used to be opt-in behind -IncludeIcons because bundling them broke
    # VitaShell's fresh install on real hardware with a generic 0x80104004
    # error every time -- root-caused (2026-09) to template.xml using the
    # newer "frame-based" LiveArea schema (<frame>/<pos>/<bg><img>) with a
    # content-id attribute. That schema parses fine on some VitaShell/firmware
    # combos but silently fails LiveArea registration on others. Switched to
    # the older, simpler schema (<livearea-background><image>...</image>,
    # <gate><startup-image>...</startup-image>) -- confirmed on real hardware
    # to install and display correctly. Pass -NoIcons to fall back to a bare
    # eboot + param.sfo VPK (no LiveArea) if needed.
    [switch]$NoIcons
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repoRoot $BuildDir
$livearea = Join-Path $repoRoot "vita\livearea"
$trophyPack = Join-Path $build "TROPHY.TRP"

$env:Path = "$env:VITASDK\bin;" + $env:Path

Push-Location $build
try {
    if (-not (Test-Path "mdkr64")) {
        throw "mdkr64 (raw linked ELF) not found in $build -- run the CMake/ninja build first."
    }

    & python (Join-Path $repoRoot "tools\build_vita_trophy_pack.py") `
        --out $trophyPack --livearea-icon (Join-Path $livearea "icon0.png")
    if ($LASTEXITCODE -ne 0) { throw "Vita trophy-pack generation failed" }

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
    if ($NoIcons) {
        vita-pack-vpk -s param.sfo -b eboot.bin mdkr64.vpk
    } else {
        vita-pack-vpk -s param.sfo -b eboot.bin `
            -a "$livearea\icon0.png=sce_sys/icon0.png" `
            -a "$livearea\bg.png=sce_sys/livearea/contents/bg.png" `
            -a "$livearea\startup.png=sce_sys/livearea/contents/startup.png" `
            -a "$livearea\template.xml=sce_sys/livearea/contents/template.xml" `
            -a "$trophyPack=sce_sys/trophy/GBLN00001_00/TROPHY.TRP" `
            mdkr64.vpk
    }
    if ($LASTEXITCODE -ne 0) { throw "vita-pack-vpk failed" }

    Get-ChildItem mdkr64.elf, mdkr64.velf, eboot.bin, mdkr64.vpk | Select-Object Name, Length, LastWriteTime
}
finally {
    Pop-Location
}
