; A3D STEP Thumbnails — installer (NSIS, builds on Linux or Windows)
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef BUILD_DIR
  !define BUILD_DIR "..\build-win"
!endif

!define APPNAME   "A3D STEP Thumbnails"
!define COMPANY   "A3D (AlexPrint3D)"
!define CLSID     "{AE41D446-CE3B-42C7-9539-7513CF65E803}"
!define THUMB_IID "{e357fccd-a995-4576-b01f-234630154e96}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\A3DStepThumbnails"
!define APP_KEY   "Software\A3D\StepThumbnails"

Name "${APPNAME}"
OutFile "A3D-STEP-Thumbnails-${VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\A3D\STEP Thumbnails"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "${COMPANY}"
VIAddVersionKey "FileDescription" "${APPNAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "${COMPANY}"

!define MUI_ABORTWARNING
!define MUI_ICON   "..\assets\stepthumb.ico"
!define MUI_UNICON "..\assets\stepthumb.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"   ; default
!insertmacro MUI_LANGUAGE "Ukrainian" ; picked automatically on Ukrainian Windows

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "A3D STEP Thumbnails requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; Register our handler on one extension (.step / .stp).
; If the extension's ProgID (e.g. from SolidWorks/eDrawings) already declares a
; different thumbnail handler, it is backed up and redirected to ours.
!macro RegisterExt EXT
  WriteRegStr HKLM "Software\Classes\${EXT}\ShellEx\${THUMB_IID}" "" "${CLSID}"
  WriteRegStr HKLM "Software\Classes\SystemFileAssociations\${EXT}\ShellEx\${THUMB_IID}" "" "${CLSID}"
  ReadRegStr $0 HKCR "${EXT}" ""
  ${If} $0 != ""
    ReadRegStr $1 HKCR "$0\ShellEx\${THUMB_IID}" ""
    ${If} $1 != ""
    ${AndIf} $1 != "${CLSID}"
      WriteRegStr HKLM "${APP_KEY}\Backup" "$0" "$1"
    ${EndIf}
    WriteRegStr HKLM "${APP_KEY}\ProgIds" "$0" "1"
    WriteRegStr HKLM "Software\Classes\$0\ShellEx\${THUMB_IID}" "" "${CLSID}"
  ${EndIf}
!macroend

!macro UnregisterExt EXT
  DeleteRegKey HKLM "Software\Classes\${EXT}\ShellEx\${THUMB_IID}"
  DeleteRegKey /ifempty HKLM "Software\Classes\${EXT}\ShellEx"
  DeleteRegKey HKLM "Software\Classes\SystemFileAssociations\${EXT}\ShellEx\${THUMB_IID}"
  DeleteRegKey /ifempty HKLM "Software\Classes\SystemFileAssociations\${EXT}\ShellEx"
  DeleteRegKey /ifempty HKLM "Software\Classes\SystemFileAssociations\${EXT}"
!macroend

Section "Install"
  SetRegView 64
  SetOutPath "$INSTDIR"
  File "${BUILD_DIR}\stepthumb.dll"
  File "${BUILD_DIR}\stepthumb-render.exe"
  File "..\README.md"
  File "..\assets\stepthumb.ico"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; COM server
  WriteRegStr HKLM "Software\Classes\CLSID\${CLSID}" "" "${APPNAME}"
  WriteRegStr HKLM "Software\Classes\CLSID\${CLSID}\InprocServer32" "" "$INSTDIR\stepthumb.dll"
  WriteRegStr HKLM "Software\Classes\CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${CLSID}" "${APPNAME}"

  !insertmacro RegisterExt ".step"
  !insertmacro RegisterExt ".stp"

  ; Add/Remove Programs
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${COMPANY}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://alexprint3d.com.ua/"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\stepthumb.ico"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ; Tell Explorer that associations changed
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  SetRegView 64
  !insertmacro UnregisterExt ".step"
  !insertmacro UnregisterExt ".stp"

  ; ProgIDs we touched: restore the previous handler or remove ours
  StrCpy $2 0
  loop:
    EnumRegValue $0 HKLM "${APP_KEY}\ProgIds" $2
    StrCmp $0 "" done
    ReadRegStr $1 HKLM "${APP_KEY}\Backup" "$0"
    ${If} $1 != ""
      WriteRegStr HKLM "Software\Classes\$0\ShellEx\${THUMB_IID}" "" "$1"
    ${Else}
      ReadRegStr $3 HKLM "Software\Classes\$0\ShellEx\${THUMB_IID}" ""
      ${If} $3 == "${CLSID}"
        DeleteRegKey HKLM "Software\Classes\$0\ShellEx\${THUMB_IID}"
        DeleteRegKey /ifempty HKLM "Software\Classes\$0\ShellEx"
      ${EndIf}
    ${EndIf}
    IntOp $2 $2 + 1
    Goto loop
  done:

  DeleteRegKey HKLM "Software\Classes\CLSID\${CLSID}"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${CLSID}"
  DeleteRegKey HKLM "${APP_KEY}"
  DeleteRegKey /ifempty HKLM "Software\A3D"
  DeleteRegKey HKLM "${UNINST_KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ; The DLL may still be loaded by Explorer: delete on reboot if needed
  Delete /REBOOTOK "$INSTDIR\stepthumb.dll"
  Delete /REBOOTOK "$INSTDIR\stepthumb-render.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\stepthumb.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  RMDir "$PROGRAMFILES64\A3D"
SectionEnd
