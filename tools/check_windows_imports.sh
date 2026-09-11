#!/usr/bin/env bash
# Fail if a Windows release binary imports anything outside the stock OS.
set -euo pipefail

die() {
  printf 'check_windows_imports: FAIL — %s\n' "$*" >&2
  exit 1
}

filter_bad_imports() {
  local system_dlls
  system_dlls='^('
  system_dlls+='KERNEL32|USER32|SHELL32|GDI32|ADVAPI32|NTDLL|USERENV|'
  system_dlls+='ole32|OLEAUT32|COMBASE|RPCRT4|PROPSYS|COMDLG32|COMCTL32|SHLWAPI|'
  system_dlls+='WS2_32|IPHLPAPI|BCRYPTPRIMITIVES|BCRYPT|CRYPT32|'
  system_dlls+='IMM32|SETUPAPI|VERSION|WINMM|CFGMGR32|HID|POWRPROF|DWMAPI|UXTHEME|DWRITE|'
  # Both ship in System32 on every Windows install. The pinned Khronos glTF
  # validator (Dart AOT) imports them, which is why they are here.
  system_dlls+='PSAPI|DBGHELP|'
  system_dlls+='OPENGL32|DINPUT8|d3dcompiler_47|dxgi|D3D12|MSVCRT|UCRTBASE'
  system_dlls+=')\.dll$|^api-ms-win-.*\.dll$'
  grep -viE "${system_dlls}" || true
}

# The stock-DLL allowlist above deliberately admits both CRTs, because either
# one is a legitimate Windows DLL. Which one the binary links is a different
# question, and a release-critical one: v1.2.1 shipped msvcrt-linked, where
# fopen's exclusive "wbx" mode returns EINVAL, and every save silently failed.
# The build pins msystem UCRT64 for exactly this reason (see the comments in
# .github/workflows/release.yml), but that pin lives in CI. A locally produced
# MINGW64 binary satisfies every other check in this file, so assert the CRT
# against the artifact itself rather than trusting the environment that built
# it.
check_crt() {
  local imports="$1"
  local msvcrt ucrt
  msvcrt="$(printf '%s\n' "${imports}" | grep -ciE '^MSVCRT\.dll$' || true)"
  ucrt="$(printf '%s\n' "${imports}" | grep -ciE '^(UCRTBASE|api-ms-win-crt-.*)\.dll$' || true)"
  if [[ "${msvcrt}" -gt 0 ]]; then
    die "binary imports MSVCRT.dll: this is a MINGW64 build, not UCRT64. \
Exclusive fopen(\"wbx\") returns EINVAL against the legacy CRT and every save \
fails silently. Rebuild under msystem UCRT64."
  fi
  if [[ "${ucrt}" -eq 0 ]]; then
    die "binary imports no UCRT (UCRTBASE.dll or api-ms-win-crt-*.dll); the \
CRT could not be identified, so the save path is unverified"
  fi
}

if [[ "${1:-}" == "--self-test" ]]; then
  safe_imports=$'KERNEL32.dll\nMSVCRT.dll\nDINPUT8.dll\nD3D12.dll\napi-ms-win-core-file-l1-1-0.dll'
  [[ -z "$(printf '%s\n' "${safe_imports}" | filter_bad_imports)" ]] ||
    die "stock-system positive control was rejected"
  unsafe_imports=$'SDL2.dll\nlibgcc_s_seh-1.dll'
  rejected="$(printf '%s\n' "${unsafe_imports}" | filter_bad_imports)"
  [[ "${rejected}" == "${unsafe_imports}" ]] ||
    die "non-system broken control escaped the allowlist"
  # The CRT check has to bite in both directions, or it is decoration.
  ( check_crt $'KERNEL32.dll\nUCRTBASE.dll' ) ||
    die "UCRT positive control was rejected"
  ( check_crt $'KERNEL32.dll\napi-ms-win-crt-stdio-l1-1-0.dll' ) ||
    die "api-ms-win-crt positive control was rejected"
  ( check_crt $'KERNEL32.dll\nMSVCRT.dll' 2>/dev/null ) &&
    die "msvcrt-linked broken control was accepted"
  ( check_crt $'KERNEL32.dll' 2>/dev/null ) &&
    die "CRT-less broken control was accepted"
  # --static-crt must still refuse a DYNAMIC legacy CRT, or the flag is a way
  # to smuggle the v1.2.1 defect past the check that exists for it.
  [[ -z "$(printf '%s\n' $'KERNEL32.dll\nPSAPI.DLL\ndbghelp.dll' | filter_bad_imports)" ]] ||
    die "stock PSAPI/DBGHELP positive control was rejected"
  printf 'check_windows_imports: self-test PASS\n'
  exit 0
fi

# --static-crt: this binary links the CRT statically, so it imports none, and
# the assertion below would reject it for the one thing that is not a defect.
# The frozen Character Workshop importer is the case: its bootloader comes from
# the hash-pinned PyInstaller wheel, which is built /MT, and so imports exactly
# USER32, KERNEL32 and ADVAPI32 on every machine that has ever built it -- CI
# included. Asserting a CRT import there is unpassable by construction, not a
# finding. What the assertion exists to catch is v1.2.1: the GAME binary linked
# against the legacy msvcrt, where exclusive fopen("wbx") returns EINVAL and
# every save fails silently. That binary still gets the full check, and the
# stock-DLL allowlist above still applies to everything.
static_crt=0
if [[ "${1:-}" == "--static-crt" ]]; then
  static_crt=1
  shift
fi

[[ $# -eq 1 ]] || die "usage: $0 [--static-crt] WINDOWS.exe"
binary="$1"
[[ -f "${binary}" ]] || die "binary not found: ${binary}"
objdump_bin="${OBJDUMP:-objdump}"
command -v "${objdump_bin}" >/dev/null 2>&1 ||
  die "required tool not found: ${objdump_bin}"

if ! dump="$("${objdump_bin}" -p "${binary}" 2>&1)"; then
  printf '%s\n' "${dump}" >&2
  die "could not inspect PE imports"
fi
imports="$(printf '%s\n' "${dump}" | awk '/DLL Name/{print $3}')"
[[ -n "${imports}" ]] || die "PE import table was empty or unreadable"
bad_imports="$(printf '%s\n' "${imports}" | filter_bad_imports)"
if [[ -n "${bad_imports}" ]]; then
  printf '%s\n' "${bad_imports}" >&2
  die "non-system DLL import found; the portable package must be one executable"
fi

if (( static_crt )); then
  # Still refuse a DYNAMIC legacy CRT here: --static-crt says "expect no CRT
  # import", not "accept any CRT". A binary that claims a static CRT and then
  # imports msvcrt.dll is the v1.2.1 defect wearing a flag.
  if printf '%s\n' "${imports}" | grep -qiE '^MSVCRT\.dll$'; then
    die "binary was declared --static-crt but imports MSVCRT.dll"
  fi
  printf 'check_windows_imports: PASS — stock Windows DLLs only, CRT linked statically\n'
  exit 0
fi

check_crt "${imports}"

printf 'check_windows_imports: PASS — stock Windows DLLs only, UCRT linked\n'
