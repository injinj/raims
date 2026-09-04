; NSIS installer for RaiMS on Windows, built on Linux with makensis (mingw32-nsis)
; by "make port_extra=-mingw dist_win".  Defines: NAME VERSION VER_BUILD SRC OUT
Unicode true
!include "MUI2.nsh"
!include "StrFunc.nsh"
${StrStr}
${UnStrRep}

Name "RaiMS ${VER_BUILD}"
OutFile "${OUT}"
InstallDir "$PROGRAMFILES64\RaiMS"
InstallDirRegKey HKLM "Software\RaiTechnology\RaiMS" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define SVC       "RaiMS"
!define REGUNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\RaiMS"
!define REGENV    "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "RaiMS server (ms_server, ms_gen_key, raisvc, dlls)" SecMain
  SectionIn RO
  ; stop a previous instance before overwriting the exe
  IfFileExists "$INSTDIR\bin\raisvc.exe" 0 +2
    nsExec::ExecToLog '"$INSTDIR\bin\raisvc.exe" stop ${SVC}'
  SetOutPath "$INSTDIR"
  File /r /x config "${SRC}\*.*"
  ; config: generate the keys only when there is no config yet -- ms_gen_key
  ; wants an empty (or nonexistent) directory, so it runs before the transport
  ; files are dropped in; those are refreshed on every install.
  IfFileExists "$INSTDIR\config\svc_eval.yaml" cfg_done 0
    CreateDirectory "$INSTDIR\config"
    nsExec::ExecToLog '"$INSTDIR\bin\ms_gen_key.exe" -d "$INSTDIR\config" -s eval -y'
  cfg_done:
  SetOutPath "$INSTDIR\config"
  File "${SRC}\config\*.yaml"
  CreateDirectory "$INSTDIR\log"
  WriteRegStr HKLM "Software\RaiTechnology\RaiMS" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\RaiTechnology\RaiMS" "Version" "${VER_BUILD}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${REGUNINST}" "DisplayName" "RaiMS"
  WriteRegStr   HKLM "${REGUNINST}" "DisplayVersion" "${VER_BUILD}"
  WriteRegStr   HKLM "${REGUNINST}" "Publisher" "Rai Technology"
  WriteRegStr   HKLM "${REGUNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${REGUNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${REGUNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${REGUNINST}" "NoRepair" 1
SectionEnd

Section "Install and start the ${SVC} Windows service" SecService
  ; windows firewall: rv and nats listen on all interfaces, redis/telnet on
  ; loopback only, so only those two need an inbound rule
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="RaiMS ms_server"'
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="RaiMS ms_server" dir=in action=allow protocol=TCP localport=7500,4222 program="$INSTDIR\bin\ms_server.exe"'
  nsExec::ExecToLog '"$INSTDIR\bin\raisvc.exe" uninstall ${SVC}'
  nsExec::ExecToLog '"$INSTDIR\bin\raisvc.exe" install ${SVC} -w "$INSTDIR" -l "$INSTDIR\log\ms_server.out" -D "Rai message service (rv 7500, nats 4222, redis 6379, telnet 2222)" -- "$INSTDIR\bin\ms_server.exe" -d "$INSTDIR\config" -l "$INSTDIR\log\ms_server.log"'
  nsExec::ExecToLog '"$INSTDIR\bin\raisvc.exe" start ${SVC}'
SectionEnd

Section "Add bin directory to the system PATH" SecPath
  ReadRegStr $0 HKLM "${REGENV}" "Path"
  ${StrStr} $1 $0 "$INSTDIR\bin"
  StrCmp $1 "" 0 +3
    WriteRegExpandStr HKLM "${REGENV}" "Path" "$0;$INSTDIR\bin"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

LangString DESC_SecMain ${LANG_ENGLISH} "The server and its runtime.  An evaluation config is created in $INSTDIR\config with a fresh service key (ms_gen_key)."
LangString DESC_SecService ${LANG_ENGLISH} "Register ms_server as the Windows service ${SVC} (automatic start, restart on failure) and start it now.  Logs in $INSTDIR\log."
LangString DESC_SecPath ${LANG_ENGLISH} "Append $INSTDIR\bin to the machine PATH (ms_server, ms_gen_key, raisvc)."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_SecMain)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecService} $(DESC_SecService)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_SecPath)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  nsExec::ExecToLog '"$INSTDIR\bin\raisvc.exe" uninstall ${SVC}'
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="RaiMS ms_server"'
  ReadRegStr $0 HKLM "${REGENV}" "Path"
  ${UnStrRep} $1 $0 ";$INSTDIR\bin" ""
  StrCmp $0 $1 +3 0
    WriteRegExpandStr HKLM "${REGENV}" "Path" $1
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\log"
  ; config (keys) is kept unless empty; delete it yourself to start over
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR\config"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${REGUNINST}"
  DeleteRegKey HKLM "Software\RaiTechnology\RaiMS"
SectionEnd
