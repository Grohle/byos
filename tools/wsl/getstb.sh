set -e
. "$(dirname "$0")/fetch.sh"
mkdir -p ~/hiby/third && cd ~/hiby/third
# stb single-file libraries, pinned to one commit of https://github.com/nothings/stb and by SHA-256:
# stb_image.h v2.30, stb_truetype.h v1.26, stb_image_write.h v1.16 (the last one is used by hibynet)
STB=https://raw.githubusercontent.com/nothings/stb/4e9b368bfc071e97206ea5c6adba68dd0cbf8897
fetch_verify $STB/stb_image.h 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 stb_image.h
fetch_verify $STB/stb_truetype.h ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab stb_truetype.h
fetch_verify $STB/stb_image_write.h cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 stb_image_write.h
ls -la
grep -m1 "stb_image - v" stb_image.h
grep -m1 "stb_truetype.h - v" stb_truetype.h
