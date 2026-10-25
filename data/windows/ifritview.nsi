; IfritView's Windows installer. Built by .github/ci/build-windows.sh:
;   makensis -DVERSION=1.2 -DSTAGE=<install tree> -DOUT=<setup.exe> ifritview.nsi
; Windows paths (Windows' makensis doesn't expand File /r with '/').
; STAGE holds the app as installed: ifritview.exe (everything linked in),
; plugins\ifritview_plugin.h (the format plugin interface; plugin DLLs go
; beside it), and the README and licence. Making it the default viewer is in
; its Settings.

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

; Windows won't let a running exe be replaced or deleted: before either, ask
; to close the IfritView windows that are open, as installers do, and close
; them as their close button would (taskkill without /F). The exe being
; locked is the test, so another copy elsewhere doesn't count.
!macro CLOSE_RUNNING prefix
Function ${prefix}CloseRunning
  IfFileExists "$INSTDIR\ifritview.exe" 0 done
  ClearErrors
  FileOpen $0 "$INSTDIR\ifritview.exe" a
  IfErrors 0 unlocked
  MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION "IfritView is open. Setup will close it to continue." /SD IDOK IDOK close
  Abort
close:
  nsExec::Exec 'taskkill /IM ifritview.exe'
  Pop $1
  StrCpy $1 0
wait:
  Sleep 250
  ClearErrors
  FileOpen $0 "$INSTDIR\ifritview.exe" a
  IfErrors 0 unlocked
  IntOp $1 $1 + 1
  IntCmp $1 20 0 wait 0  ; five seconds
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "IfritView is still open. Close it, then choose Retry." /SD IDCANCEL IDRETRY close
  Abort
unlocked:
  FileClose $0
done:
FunctionEnd
!macroend
!insertmacro CLOSE_RUNNING ""
!insertmacro CLOSE_RUNNING "un."

Function RunAsUser
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\ifritview.exe"'
FunctionEnd

Section "IfritView"
  SetShellVarContext all
  Call CloseRunning
  ; A reinstall replaces the app rather than mixing two versions' files, but
  ; keeps the format plugins people put in plugins (the stage's copy of
  ; ifritview_plugin.h, there for plugin authors, then replaces the old).
  ; Older versions had it all in bin\: their plugins move across.
  CreateDirectory "$INSTDIR\plugins"
  IfFileExists "$INSTDIR\bin\plugins\*.*" 0 +2
    CopyFiles /SILENT "$INSTDIR\bin\plugins\*.*" "$INSTDIR\plugins"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\i18n"  ; a language dropped since goes too
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\IfritView.lnk" "$INSTDIR\ifritview.exe"

  WriteRegStr HKLM "Software\IfritView" "InstallDir" "$INSTDIR"
  ; Found by name, as Windows finds programs (Run, ShellExecute) and as
  ; other tools look for it (a diff tool offering it for images).
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\ifritview.exe" "" "$INSTDIR\ifritview.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\ifritview.exe" "Path" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "IfritView"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "Manu Evans"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/TurkeyMan/ifritview"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\ifritview.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  ; Explorer keeps icons by path: an upgrade's new one shows only once it's
  ; told something changed (SHCNE_ASSOCCHANGED).
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Call un.CloseRunning
  Delete "$SMPROGRAMS\IfritView.lnk"
  Delete "$INSTDIR\ifritview.exe"
  ; Plugins people added stay, and so the folder with them.
  Delete "$INSTDIR\plugins\ifritview_plugin.h"
  RMDir "$INSTDIR\plugins"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  RMDir /r "$INSTDIR\i18n"
  RMDir /r "$INSTDIR\licenses"  ; an earlier version's
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "Software\IfritView"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\ifritview.exe"
  ; What the app registered for the user when offered as the default viewer.
  ; (Extensions' "Open with" entries naming IfritView.Image are left: with
  ; the ProgID gone, Windows passes over them.)
  DeleteRegKey HKCU "Software\Classes\IfritView.Image"
  DeleteRegKey HKCU "Software\Classes\Applications\ifritview.exe"
  DeleteRegValue HKCU "Software\RegisteredApplications" "IfritView"
  DeleteRegKey HKCU "Software\IfritView"
SectionEnd
