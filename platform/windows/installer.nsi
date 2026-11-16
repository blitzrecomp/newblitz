; New Blitz for Windows: NewBlitz-v<version>-Windows-Setup.exe (tools/release/make_downloads.sh builds it with makensis on
; the Mac; docs/RELEASE.md). Per user, no admin prompt: the app goes to %LOCALAPPDATA%\Programs\New Blitz, with a Start
; menu shortcut, an optional desktop shortcut and an uninstaller in Settings -> Apps (HKCU).
;
; The player's game data lives elsewhere (%LOCALAPPDATA%\Blitz2026Recompiled, and BlitzRecompiled for the 1999 mode)
; and is never touched: the uninstaller removes only what this installer put in its own folder. Installing over an
; older version replaces the program files in place; the data stays.
;
;   makensis -INPUTCHARSET UTF8 -DVERSION=0.9.5 -DVIVERSION=0.9.5.0 -DSRC=<the staged "New Blitz" folder>
;            -DICON=<blitz.ico> -DOUTFILE=<NewBlitz-v0.9.5-Windows-Setup.exe> platform/windows/installer.nsi

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user
ManifestDPIAware true

!macro Need name
    !ifndef ${name}
        !error "${name} isn't set: tools/release/make_downloads.sh passes VERSION, VIVERSION, SRC, ICON and OUTFILE"
    !endif
!macroend
!insertmacro Need VERSION
!insertmacro Need VIVERSION
!insertmacro Need SRC
!insertmacro Need ICON
!insertmacro Need OUTFILE

; BRANDING: the Windows installer
!define APPNAME "New Blitz"
!define EXE "New Blitz.exe"
!define PUBLISHER "Blitz Recomp"
!define UNINSTALLER "Uninstall New Blitz.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NewBlitz"

Name "${APPNAME}"
OutFile "${OUTFILE}"
; Fixed, per user (no folder page): the uninstaller's folder is always one this installer made for itself.
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VIVERSION}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} ${VERSION} Setup"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "GPL-3.0. No game files included."

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APPNAME}"

!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME} ${VERSION} for you (no administrator needed).$\r$\n$\r$\nYou'll need your own NFL Blitz 2000 (USA) v1.1 cartridge dump: the app asks for it when it opens. No game files are included.$\r$\n$\r$\nInstalling over an earlier version keeps your saves and settings."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "This removes the ${APPNAME} program. Your saves, settings, ROM copy and season paks stay (in %LOCALAPPDATA%\Blitz2026Recompiled), so a reinstall picks up where you left off."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; The program's own files and folders, the ones an upgrade replaces and the uninstaller removes. Nothing else in
; $INSTDIR is touched.
!macro RemoveProgramFiles
    Delete "$INSTDIR\${EXE}"
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\recompcontrollerdb.txt"
    Delete "$INSTDIR\icon.png"
    RMDir /r "$INSTDIR\assets"
    RMDir /r "$INSTDIR\packs"
    RMDir /r "$INSTDIR\art"
    RMDir /r "$INSTDIR\paks"
    RMDir /r "$INSTDIR\devices"
!macroend

Section "${APPNAME}" SecApp
    SectionIn RO
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    ; An upgrade: the old version's files go first, so nothing it shipped and this one doesn't lingers.
    IfFileExists "$INSTDIR\${EXE}" 0 fresh
        !insertmacro RemoveProgramFiles
    fresh:
    File /r "${SRC}/*"
    WriteUninstaller "$INSTDIR\${UNINSTALLER}"

    CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0

    WriteRegStr HKCU "${UNINSTKEY}" "DisplayName" "${APPNAME}"
    WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${EXE},0"
    WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\${UNINSTALLER}"'
    WriteRegStr HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\${UNINSTALLER}" /S'
    WriteRegStr HKCU "${UNINSTKEY}" "URLInfoAbout" "https://newblitz.com"
    WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
    SetShellVarContext current
    CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    !insertmacro RemoveProgramFiles
    Delete "$INSTDIR\${UNINSTALLER}"
    RMDir "$INSTDIR"  ; only if empty: anything else someone put there stays
    Delete "$SMPROGRAMS\${APPNAME}.lnk"
    Delete "$DESKTOP\${APPNAME}.lnk"
    DeleteRegKey HKCU "${UNINSTKEY}"
SectionEnd
