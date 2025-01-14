#!/bin/bash

if [ '!' "$EUID" = 0 ] && [ '!' `id -u` = 0 ] ; then
	echo "Warning: you need to be root to run this!"
	# we do not exit as other mechanisms exist that allows to do this than
	# being root. let the errors speak for themselves.
	exit 1
fi

rmmod afl_snapshot && echo Successfully unloaded the module
