# Compara fallos de pagina mayores del reproductor con y sin fijar paginas calientes.
$adb = "$env:LOCALAPPDATA\platform-tools\adb.exe"; $d = "192.168.1.147:5555"
$nav = @("home","tap:80,105","tap:83,295","swipe:160,420,160,80,200","swipe:160,420,160,80,200","swipe:160,420,160,80,200","back","tap:235,295","swipe:160,420,160,80,200","back","tap:83,425","swipe:160,420,160,80,200","back","home")
function majflt { $s = & $adb -s $d shell "set -- `$(cat /proc/`$(pidof hiby_player)/stat); echo `${12}"; [int]"$s".Trim() }
function run($label) {
  $a = majflt; python hiby.py @nav 2>$null | Out-Null; $b = majflt
  "{0,-22} fallos mayores: {1}" -f $label, ($b - $a)
}
Set-Location $PSScriptRoot
& $adb -s $d shell "killall hibyctl 2>/dev/null; F=/usr/data/hibymod/fast; /tmp/hibyctl pinprofile /tmp/pin.list 3072 `$F/hiby_player `$F/lib/libc-2.22.so `$F/lib/libpthread-2.22.so `$F/lib/libm-2.22.so `$F/lib/libasound.so.2.0.0 /lib/ld-2.22.so /usr/lib/libcrypto.so.1.1 /usr/lib/libssl.so.1.1"
run "sin fijar (1)"; run "sin fijar (2)"
& $adb -s $d shell "start-stop-daemon -S -b -x /tmp/hibyctl -- pin /tmp/pin.list; sleep 2; grep -E 'MemAvailable|Mlocked' /proc/meminfo | tr -s ' ' | tr '\n' ' '"
run "fijando (1)"; run "fijando (2)"
& $adb -s $d shell "grep -E 'MemAvailable|Mlocked' /proc/meminfo | tr -s ' ' | tr '\n' ' '"
