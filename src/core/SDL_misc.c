/*
  SDLop -- SDL_misc.h: SDL_OpenURL().

  Uses xdg-open, like SDL3 does on Linux when it cannot use gio/portal. The child
  is detached (double fork) so no zombie is left behind and the call does not
  block.
*/

#include "../sdlop_internal.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

bool SDL_OpenURL(const char *url)
{
    pid_t pid;

    if (!url || !url[0]) {
        return SDL_InvalidParamError("url");
    }

    pid = fork();
    if (pid < 0) {
        return SDL_SetError("fork() failed: %s", strerror(errno));
    }
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) {
                    close(devnull);
                }
            }
            setsid();
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    if (waitpid(pid, NULL, 0) < 0) {
        return SDL_SetError("waitpid() failed");
    }
    return true;
}
