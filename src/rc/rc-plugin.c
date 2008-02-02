/*
   librc-plugin.c
   Simple plugin handler
   */

/*
 * Copyright 2007 Roy Marples
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "einfo.h"
#include "rc.h"
#include "rc-misc.h"
#include "rc-plugin.h"
#include "strlist.h"

#define RC_PLUGIN_HOOK "rc_plugin_hook"

bool rc_in_plugin = false;

typedef struct plugin
{
	char *name;
	void *handle;
	int (*hook) (rc_hook_t, const char *);
	struct plugin *next;
} plugin_t;

static plugin_t *plugins = NULL;

#ifndef __FreeBSD__
dlfunc_t dlfunc (void * __restrict handle, const char * __restrict symbol)
{
	union {
		void *d;
		dlfunc_t f;
	} rv;

	rv.d = dlsym (handle, symbol);
	return (rv.f);
}
#endif

void rc_plugin_load (void)
{
	DIR *dp;
	struct dirent *d;
	plugin_t *plugin = plugins;
	char *p;
	void *h;
	int (*fptr) (rc_hook_t, const char *);

	/* Don't load plugins if we're in one */
	if (rc_in_plugin)
		return;

	/* Ensure some sanity here */
	rc_plugin_unload ();

	if (! (dp = opendir (RC_PLUGINDIR)))
		return;

	while ((d = readdir (dp))) {
		if (d->d_name[0] == '.')
			continue;

		p = rc_strcatpaths (RC_PLUGINDIR, d->d_name, NULL);
		h = dlopen (p, RTLD_LAZY);
		free (p);
		if (! h) {
			eerror ("dlopen: %s", dlerror ());
			continue;
		}

		fptr = (int (*)(rc_hook_t, const char*)) dlfunc (h, RC_PLUGIN_HOOK);
		if (! fptr) {
			eerror ("%s: cannot find symbol `%s'", d->d_name, RC_PLUGIN_HOOK);
			dlclose (h);
		} else {
			if (plugin) {
				plugin->next = xmalloc (sizeof (*plugin->next));
				plugin = plugin->next;
			} else
				plugin = plugins = xmalloc (sizeof (*plugin));

			plugin->name = xstrdup (d->d_name);
			plugin->handle = h;
			plugin->hook = fptr;
			plugin->next = NULL;
		}
	}
	closedir (dp);
}

int rc_waitpid (pid_t pid)
{
	int status = 0;
	pid_t savedpid = pid;
	int retval = -1;

	errno = 0;
	while ((pid = waitpid (savedpid, &status, 0)) > 0) {
		if (pid == savedpid)
			retval = WIFEXITED (status) ? WEXITSTATUS (status) : EXIT_FAILURE;
	}

	return (retval);
}

void rc_plugin_run (rc_hook_t hook, const char *value)
{
	plugin_t *plugin = plugins;
	struct sigaction sa;
	sigset_t empty;
	sigset_t full;
	sigset_t old;

	/* Don't run plugins if we're in one */
	if (rc_in_plugin)
		return;

	/* We need to block signals until we have forked */
	memset (&sa, 0, sizeof (sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset (&sa.sa_mask);
	sigemptyset (&empty);
	sigfillset (&full);

	while (plugin) {
		int i;
		int flags;
		int pfd[2];
		pid_t pid;
		char *buffer;
		char *token;
		char *p;
		ssize_t nr;

		if (! plugin->hook) {
			plugin = plugin->next;
			continue;
		}

		/* We create a pipe so that plugins can affect our environment
		 * vars, which in turn influence our scripts. */
		if (pipe (pfd) == -1) {
			eerror ("pipe: %s", strerror (errno));
			return;
		}

		/* Stop any scripts from inheriting us.
		 * This is actually quite important as without this, the splash
		 * plugin will probably hang when running in silent mode. */
		for (i = 0; i < 2; i++)
			if ((flags = fcntl (pfd[i], F_GETFD, 0)) < 0 ||
			    fcntl (pfd[i], F_SETFD, flags | FD_CLOEXEC) < 0)
				eerror ("fcntl: %s", strerror (errno));

		sigprocmask (SIG_SETMASK, &full, &old);

		/* We run the plugin in a new process so we never crash
		 * or otherwise affected by it */
		if ((pid = fork ()) == -1) {
			eerror ("fork: %s", strerror (errno));
			break;
		}

		if (pid == 0) {
			int retval;

			/* Restore default handlers */
			sigaction (SIGCHLD, &sa, NULL);
			sigaction (SIGHUP,  &sa, NULL);
			sigaction (SIGINT,  &sa, NULL);
			sigaction (SIGQUIT, &sa, NULL);
			sigaction (SIGTERM, &sa, NULL);
			sigaction (SIGUSR1, &sa, NULL);
			sigaction (SIGWINCH, &sa, NULL);
			sigprocmask (SIG_SETMASK, &old, NULL);
			
			rc_in_plugin = true;
			close (pfd[0]);
			rc_environ_fd = fdopen (pfd[1], "w");
			retval = plugin->hook (hook, value);
			fclose (rc_environ_fd);
			rc_environ_fd = NULL;

			/* Just in case the plugin sets this to false */
			rc_in_plugin = true;
			exit (retval);
		}

		sigprocmask (SIG_SETMASK, &old, NULL);
		close (pfd[1]);
		buffer = xmalloc (sizeof (char) * BUFSIZ);
		memset (buffer, 0, BUFSIZ);

		while ((nr = read (pfd[0], buffer, BUFSIZ)) > 0) {
			p = buffer;
			while (*p && p - buffer < nr) {
				token = strsep (&p, "=");
				if (token) {
					unsetenv (token);
					if (*p) {
						setenv (token, p, 1);
						p += strlen (p) + 1;
					} else
						p++;
				}
			}
		}

		free (buffer);
		close (pfd[0]);

		rc_waitpid (pid);
		plugin = plugin->next;
	}
}

void rc_plugin_unload (void)
{
	plugin_t *plugin = plugins;
	plugin_t *next;

	while (plugin) {
		next = plugin->next;
		dlclose (plugin->handle);
		free (plugin->name);
		free (plugin);
		plugin = next;
	}
	plugins = NULL;
}
