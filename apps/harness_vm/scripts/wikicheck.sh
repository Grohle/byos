for u in https://en.wikipedia.org/wiki/Ingenic_Semiconductor "https://en.wikipedia.org/w/index.php?title=Telegram_(software)&action=raw" "https://en.wikipedia.org/api/rest_v1/page/html/Ingenic_Semiconductor"; do
  echo "$u -> $(curl -sL -o /tmp/p.html -w '%{http_code} %{size_download}' -A "NetSurf/3.11 (Linux)" "$u")"
done
curl -sL -A "NetSurf/3.11 (Linux)" -o /dev/null -w '%{http_code}\n' https://en.m.wikipedia.org/wiki/Ingenic_Semiconductor
