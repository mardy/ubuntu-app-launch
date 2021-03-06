#!/bin/bash -e

if [ "$PATH" != "$APP_DIR/lib/64bit-amazing/bin:$APP_DIR" ] ; then
	echo "Bad PATH: $PATH"
	exit 1
fi

if [ "$LD_LIBRARY_PATH" != "$APP_DIR/lib/64bit-amazing:$APP_DIR/lib" ] ; then
	echo "Bad LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
	exit 1
fi

if [ "$QML2_IMPORT_PATH" != "$APP_DIR/lib/64bit-amazing" ] ; then
	echo "Bad QML import path: $QML2_IMPORT_PATH"
	exit 1
fi

exit 0
