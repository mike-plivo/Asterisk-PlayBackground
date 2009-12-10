#
# Asterisk -- A telephony toolkit for Linux.
#
# Makefile for Asterisk-playbg
#
# Copyright (C) 1999-2008, Digium, Inc.
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#
CC=gcc

LDFLAGS=

SOLINK=-shared

build:
	$(CC) -c apps/app_playbg.c
	$(CC) $(SOLINK) app_playbg.o -o app_playbg.so $(LDFLAGS)

clean:
	rm -f app_playbg.o app_playbg.so

