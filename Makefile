#
#  WM is a window manager (Win 3.0-style)
#  Copyright (C) 2018  Victor C. Salas P. (aka nmag) <nmagko@gmail.com>
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

PREFIX?=/usr/X11R6
CFLAGS?=-std=c11 -Wall -Wextra -Wpedantic -O2
INSTALL= install
RM     = rm
STRIP  = strip

all:
	$(CC) $(CFLAGS) -I$(PREFIX)/include -L$(PREFIX)/lib -o wm wm.c -lX11
	$(CC) $(CFLAGS) -I$(PREFIX)/include -L$(PREFIX)/lib -o win3wm win3wm.c -lX11
	$(CC) $(CFLAGS) -I$(PREFIX)/include -L$(PREFIX)/lib -o win win.c

clean:
	$(RM) -f *~
	$(RM) -f wm
	$(RM) -f win3wm
	$(RM) -f win

install:
	$(STRIP) wm
	$(INSTALL) -o root -m 755 -v wm /usr/local/bin/
	$(STRIP) win3wm
	$(INSTALL) -o root -m 755 -v win3wm /usr/local/bin/
	$(STRIP) win
	$(INSTALL) -o root -m 755 -v win /usr/local/bin/

uninstall:
	$(RM) -f /usr/local/bin/wm
	$(RM) -f /usr/local/bin/win3wm
	$(RM) -f /usr/local/bin/win
