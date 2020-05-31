#!/bin/sh

g++ -g -fpermissive -I external/webview/ -I external/httpserver.h/ \
	aria2web-host.c \
	`pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0` \
	-o aria2web-host \
	|| exit 1
g++ -g -fpermissive -I external/webview/ -I external/httpserver.h/ \
	-I external/tiny-process-library \
	external/tiny-process-library/process.cpp \
	external/tiny-process-library/process_unix.cpp \
	aria2web-lv2ui.c \
	`pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0` \
	-o aria2web-lv2ui.so -shared -fPIC -Wl,--no-undefined \
	|| exit 1

if [ ! -d sfizz-aria2web/dist ] ; then
  echo "building sfizz..." ;
  mkdir sfizz-aria2web ;
  cd sfizz-aria2web ;
  cmake -DCMAKE_INSTALL_PREFIX=`pwd`/dist ../external/sfizz ;
  make ;
  make install ;
  cd .. ;
fi

DSTLV2=sfizz-aria2web/dist/lib/lv2/sfizz.lv2/
cp aria2web-lv2ui.so $DSTLV2
cp aria2web.ttl $DSTLV2
cp manifest.ttl $DSTLV2

# FIXME: change destination to the parent directory...
DSTRES=$DSTLV2/resources/
mkdir -p $DSTRES
echo "Copying resources..."
cp aria2web-host $DSTRES
cp index.html $DSTRES
cp -R ui-* $DSTRES
cp *.js $DSTRES
echo "build successfully completed."
