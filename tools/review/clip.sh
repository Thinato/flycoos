#!/bin/zsh
# clip.sh RIG NAME FROM TO CROP SFROM STO [STEP]: from build/review/RIG/shots/NAME, makes in build/review/RIG/page/
#   NAME.gif       real speed, frames FROM..TO
#   NAME-slow.gif  4x slow, frames SFROM..STO
#   NAME-strip.jpg one row of frames SFROM..STO every STEP frames (default 8)
# Frame numbers are scene frames (even: shoot.sh writes every second one). CROP is ImageMagick WxH+X+Y on the
# 1920x1080 shot (crop both sides of a before/after pair the same). RESIZE (default 640x), STRIPW (default 260).
RIG=${0:A:h:h:h}/build/review/$1; n=$2; from=$3; to=$4; crop=$5; sfrom=$6; sto=$7; step=${8:-8}
src=$RIG/shots/$n; out=$RIG/page; mkdir -p $out/$n.jpg; rm -f $out/$n.jpg/*.jpg(N)
even() { echo $(( $1 / 2 * 2 )); }
from=$(even $from); to=$(even $to); sfrom=$(even $sfrom); sto=$(even $sto)
for ((f=from; f<=to; f+=2)); do
  [[ -f $src/$(printf f%04d.bmp $f) ]] && magick $src/$(printf f%04d.bmp $f) -crop $crop +repage -resize ${RESIZE:-640x} $out/$n.jpg/$(printf %04d $f).jpg
done
magick -delay 3 -loop 0 $out/$n.jpg/*.jpg -layers Optimize $out/$n.gif               # 2 frames a 1/30 s tick: real speed
slow=(); for ((f=sfrom; f<=sto; f+=2)); do slow+=$out/$n.jpg/$(printf %04d $f).jpg; done
magick -delay 13 -loop 0 $slow -layers Optimize $out/$n-slow.gif                   # 0.13 s a frame: about 4x slow
strip=(); for ((f=sfrom; f<=sto; f+=step)); do strip+=$out/$n.jpg/$(printf %04d $(even $f)).jpg; done
montage $strip -tile x1 -geometry ${STRIPW:-260}x+2+2 -background '#222' $out/$n-strip.jpg
echo "$n: $out/$n.gif, $n-slow.gif, $n-strip.jpg"
