; IfritView's Windows installer. Built by .github/ci/build-windows.sh:
;   makensis -DVERSION=1.2 -DSTAGE=<install tree> -DOUT=<setup.exe> ifritview.nsi
; Windows paths (Windows' makensis doesn't expand File /r with '/').
; STAGE holds the app as installed: bin\ifritview.exe (everything linked in)
; and the README and licence. Making it the default viewer is in its Settings.

Unicode true
!include "MUI2.nsh"

Name "IfritView"
OutFile "${OUT}"
InstallDir "$PROGRAMFILES64\IfritView"
InstallDirRegKey HKLM "Software\IfritView" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\IfritView"

!define MUI_ICON "${__FILEDIR__}\ifritview.ico"
!define MUI_UNICON "${__FILEDIR__}\ifritview.ico"
; "Run IfritView" starts it as the user, through Explorer: started from the
; elevated installer it would be elevated too, and Windows would refuse it
; drag and drop from Explorer.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunAsUser
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0.0"
VIAddVersionKey "ProductName" "IfritView"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "IfritView installer"
VIAddVersionKey "LegalCopyright" "Manu Evans, GPL-3.0"

Function RunAsUser
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\bin\ifritview.exe"'
FunctionEnd

Section "IfritView"
  SetShellVarContext all
  ; A reinstall replaces the old tree rather than mixing DLLs of two versions.
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\IfritView.lnk" "$INSTDIR\bin\ifritview.exe"

  WriteRegStr HKLM "Software\IfritView" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "IfritView"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "Manu Evans"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/TurkeyMan/ifritview"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\ifritview.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\IfritView.lnk"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "Software\IfritView"
SectionEnd
