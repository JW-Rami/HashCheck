PATH=%PATH%;%WSDK81%\bin\x86;C:\dev\Progs\NSIS

rem sign using SHA-256
signtool sign /v /sha1 04141E4EA6D9343CEC994F6C099DC09BDD8937C9 /ac GlobalSign_SHA256_EV_CodeSigning_CA.cer /fd sha256 /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256 "Bin\Win32\Release\HashCheck.dll" "Bin\x64\Release\HashCheck.dll" 

makensis.exe installer\HashCheck.nsi

signtool sign /v /sha1 04141E4EA6D9343CEC994F6C099DC09BDD8937C9 /ac GlobalSign_SHA256_EV_CodeSigning_CA.cer /fd sha256 /tr http://timestamp.globalsign.com/?signature=sha2 /td SHA256  "installer\HashCheckSetup-v2.5.0.1.exe"

pause
