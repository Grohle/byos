mkdir -p ~/hiby/third && cd ~/hiby/third
for f in stb_image.h stb_truetype.h; do
  curl -sSfL -o "$f" "https://raw.githubusercontent.com/nothings/stb/master/$f" && echo "ok $f"
done
ls -la
grep -m1 "stb_image - v" stb_image.h
grep -m1 "stb_truetype.h - v" stb_truetype.h
