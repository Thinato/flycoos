#!/bin/zsh
# still.sh RIG SHOT FRAME CROP NAME: one frame of build/review/RIG/shots/SHOT as build/review/RIG/page/NAME.jpg
RIG=${0:A:h:h:h}/build/review/$1; mkdir -p $RIG/page
magick $RIG/shots/$2/$(printf f%04d.bmp $(( $3 / 2 * 2 ))) -crop $4 +repage -resize ${RESIZE:-900x} -quality 88 $RIG/page/$5.jpg && echo $RIG/page/$5.jpg
