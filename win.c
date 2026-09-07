/*
 * WIN.COM loader for WIN3WM (Windows 3.0-style)
 * Copyright (C) 2018  Victor C. Salas P. (aka nmag) <nmagko@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include "wmver.h"

/* ================================================================ */
/* Helpers */
/* ================================================================ */

static int count_monitors (void) {
  FILE *fp;
  char line[512];
  int n = 0;
  fp = popen("grep -l '^connected$' /sys/class/drm/*/status 2>/dev/null", "r");
  if (!fp) return -1;
  while (fgets(line, sizeof(line), fp)) n++;
  if (pclose(fp) == -1 && n == 0) return -1;
  return n;
}

int backupfile (char *source, char *target) {
  FILE *src = fopen(source, "r");
  FILE *dst = fopen(target, "w");
  if (!src || !dst) {
    if (src) fclose(src);
    if (dst) fclose(dst);
    return EXIT_FAILURE;
  }
  char buffer[8192];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, src)) > 0) {
    if (fwrite(buffer, 1, n, dst) != n) {
      fclose(src);
      fclose(dst);
      return EXIT_FAILURE;
    }
  }
  fclose(src);
  fclose(dst);
  return EXIT_SUCCESS;
}

void CmdLineHelp (void) {
  printf("%s\nUsage: %s [/setup | /reset]\n"
         "\n  /setup   Initialize the .xinitrc configuration\n"
         "\n  /reset   Reset the .xinitrc configuration and go back to the previous one if it exists\n\n",
         WM_TUI_VERSION_STRING, program_invocation_short_name);
}

/* ================================================================ */
/* Main launcher */
/* ================================================================ */

int main (int argc, char *argv[]) {
  int n = -1;
  const char *home = getenv("HOME");
  char winrc[PATH_MAX], xinitrc[PATH_MAX], backup[PATH_MAX];
  char xinitcontent[4096];
  char monitor[32] = {0};
  char manager[64] = {0};
  char ans = 'n';
  int num = 0;

  if (argc > 2 || (argc == 2 && !(strcmp(argv[1], "/setup") == 0 || strcmp(argv[1], "--setup") == 0 ||
                                  strcmp(argv[1], "/reset") == 0 || strcmp(argv[1], "--reset") == 0))) {
    CmdLineHelp();
    return EXIT_FAILURE;
  }

  if (!home) {
    fprintf(stderr, "%s: error getting home directory.\n",
            program_invocation_short_name);
    return EXIT_FAILURE;
  }

  snprintf(winrc, sizeof winrc, "%s/.winrc", home);
  snprintf(xinitrc, sizeof xinitrc, "%s/.xinitrc", home);
  snprintf(backup, sizeof backup, "%s/.xinitrc-win~", home);

  if (argc == 2 && (strcmp(argv[1], "/reset") == 0 || strcmp(argv[1], "--reset") == 0)) {
    if (remove(winrc) != 0) {
      fprintf(stderr, "%s: error removing %s. Was it initialized previously?\n",
              program_invocation_short_name, winrc);
      return EXIT_FAILURE;
    }
    if (access(backup, F_OK) == -1) {
      fprintf(stderr, "Warning: there is no %s backup stage\n", backup);
      return EXIT_FAILURE;
    } else {
      printf("Restoring %s...", backup);
      if (backupfile(backup, xinitrc) == 0) {
        printf(" done.\n");
        if (remove(backup) != 0) {
          fprintf(stderr, "Warning: removing backup %s failed.\n", backup);
          return EXIT_FAILURE;
        }
      } else {
        printf(" failed.\n");
        return EXIT_FAILURE;
      }
    }
    return EXIT_SUCCESS;
  }

  if (access(winrc, F_OK) == -1 || (argc == 2 && (strcmp(argv[1], "/setup") == 0 ||
                                                  strcmp(argv[1], "--setup") == 0))) {
    n = count_monitors();
    if (n <= 0) {
      fprintf(stderr, "%s: error reading connected outputs. Is grep installed?\n",
              program_invocation_short_name);
      return EXIT_FAILURE;
    }
    if (n > 1) {
      while (ans != 'e' && ans != 'm' && ans != 's') {
        ans = 'e';
        printf("\nMulti-monitors have been found (CTRL+C=Exit)\n"
               "Do you wanna (e)xtend, (m)irror, or use a (s)pecific one? [%c] ", ans);
        ans = getchar();
        if (ans != '\n') while (getchar() != '\n');
        if (ans == '\n') ans = 'e';
      }
      if (ans == 'm')
        snprintf(monitor, sizeof monitor, "/mirror");
      if (ans == 's') {
        while (num <= 0 || num > n) {
          printf("\nCounted monitors %d (CTRL+C=Exit)\n", n);
          printf("Choose one from 1 to %d: ", n);
          scanf("%d", &num);
          while (getchar() != '\n');
        }
        snprintf(monitor, sizeof monitor, "/%d", num);
      }
    }
    ans = 'n';
    while (ans != 'x' && ans != 'e') {
      ans = 'x';
      printf("\nProgram manager (CTRL+C=Exit)\n");
      printf("Which one do you wanna use, (x)term or (e)macs? [%c] ", ans);
      ans = getchar();
      if (ans != '\n') while (getchar() != '\n');
      if (ans == '\n') ans = 'x';
    }
    if (ans == 'x')
      snprintf(manager, sizeof manager, "xterm");
    if (ans == 'e')
      snprintf(manager, sizeof manager, "emacs");
    snprintf(xinitcontent, sizeof xinitcontent, "/usr/local/bin/win3wm %s &\nexec %s\n", monitor, manager);
    printf("Initializing...");
    FILE *file = fopen(winrc, "w");
    if (!file) {
      printf(" failed.\n");
      return EXIT_FAILURE;
    }
    fputs(xinitcontent, file);
    fclose(file);
    printf(" done.\n");
  } else goto exec;
  /* Yes, goto. Now, what are you gonna do about it? mf */

  if (access(xinitrc, F_OK) != -1) {
    if (access(backup, F_OK) == -1) {
      printf("Backing %s up...", xinitrc);
      if (backupfile(xinitrc, backup) == 0)
        printf(" done.\n");
      else
        printf(" failed.\n");
    }
    printf("Saving config %s...", xinitrc);
    if (backupfile(winrc, xinitrc) == 0)
      printf(" done.\n");
    else
      printf(" failed.\n");
  }

 exec:
  execlp("startx", "startx", (char *)NULL);
  fprintf(stderr, "%s: error starting the environment. Is Xorg installed?\n",
          program_invocation_short_name);
  return EXIT_SUCCESS;
}
