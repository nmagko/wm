#!/bin/sh
#
# Make icon from png
# Copyright (C) 2026  Victor C. Salas P. (aka nmag) <nmagko@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Usage: tools/make_icon.sh icons/gnuos.png 48 > gnuosicon.h

set -e
png="$1"
size="$2"
{
    printf '/* Auto-generated from %s - do not edit. */\n' "$png"
    printf '#define APP_ICON_W %s\n#define APP_ICON_H %s\n' "$size" "$size"
    printf 'static const unsigned long gnuos_icon_argb[] = {\n'
    # ImageMagick outputs RGBA -> awk swaps to ARGB
    magick "$png" -depth 8 "rgba:-" \
        | od -An -tu1 -v \
        | awk -v n="$size" '
      BEGIN { i = 0; printf "  " }
      {
        for (k = 1; k <= NF; k++) {
          v[i % 4] = $k
          i++
          if (i % 4 == 0) {
            r = v[0]; g = v[1]; b = v[2]; a = v[3]
            printf "0x%02x%02x%02x%02xUL,", a, r, g, b
            if ((i / 4) % 8 == 0) printf "\n  "; else printf " "
          }
        }
      }
      END { printf "\n};\n" }'
}
