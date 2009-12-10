/*
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
*/

/*! \file
 *
 * \brief Play files in background
 *
 * \author Michael Ricordeau <michael.ricordeau@gmail.com>
 *
 * \ingroup applications
*/




#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 1.1 $")

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/options.h"
#include "asterisk/module.h"
#include "asterisk/utils.h"

#define AST_MODULE "PlayBG"

#define MAX_PATH_LENGTH 256

static char *app1 = "StartPlayBG";
static char *app2 = "StopPlayBG";
static char *app3 = "ResumePlayBG";

static char *syn1 = "Play sound in background";
static char *syn2 = "Stop current sound in background";
static char *syn3 = "Resume current sound set";

static char *desc1 =
"StartPlayBG(filename1&filename2&filename3&...&filenameN)\n"
"Start playing all files (in order) separated by '&' in background.\n"
"\n"
"If another stream is played while playing background sound, current background sound is interrupted.\n"
"\n"
"To resume background sound at the right offset, use ResumePlayBG.\n"
"To unset and stop background sound, use StopPlayBG.\n"
"\n"
"If StartPlayBG is executed while another background sound is set,\n"
"start playing new background sound.\n"
;

static char *desc2 =
"StopPlayBG()\n"
"Stop background sound set\n"
;

static char *desc3 =
"ResumePlayBG()\n"
"Resume background sound set at the right offset.\n"
;


struct playbg_state {
	char **filearray;
	int pos;
	int nfiles;
	int origwfmt;
	int samples;
	int sample_queue;
};


static void playbg_state_destroy(void *data) {

	struct playbg_state *state = data;
	if (state->filearray) {
		ast_free(state->filearray);
	}
	if (state) {
		ast_free(state);
	}
	ast_log(LOG_DEBUG, "playbg state destroyed\n");
}


static const struct ast_datastore_info playbg_state_datastore_info = {
        .type = "PLAYBGSTATE",
        .destroy = playbg_state_destroy,
};


static void playbg_release(struct ast_channel *chan, void *data)
{
	struct playbg_state *state;
	struct ast_datastore *datastore;

	if (!chan) {
		return;
	}

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);
	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found\n");
		return;
	}
	state = datastore->data;
	if (!state) {
		ast_log(LOG_WARNING, "Invalid playbg state\n");
		return;
	}

	if (chan->stream) {
		ast_closestream(chan->stream);
		chan->stream = NULL;
	}
	
	if (option_verbose > 2) {
		ast_verbose(VERBOSE_PREFIX_3 "Release playbg on %s\n", chan->name);
	}

	if (state->origwfmt && ast_set_write_format(chan, state->origwfmt)) {
		ast_log(LOG_WARNING, "Unable to restore channel '%s' to format '%d'\n", chan->name, state->origwfmt);
	}
}


static int playbg_seek(struct ast_channel *chan)
{
	struct playbg_state *state = NULL;
	struct ast_datastore *datastore;
	int res;
	int curr_pos;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);
	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found\n");
		return -1;
	}
	state = datastore->data;
	if (!state) {
		ast_log(LOG_WARNING, "Invalid playbg state\n");
		return -1;
	}

	if (chan->stream) {
		ast_closestream(chan->stream);
		chan->stream = NULL;
	}

	curr_pos = state->pos;
	if (curr_pos >= state->nfiles) {
		state->pos = 0;
		curr_pos = 0;
	}
	
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Seek currentpos=%d maxpos=%d\n", curr_pos, state->nfiles);
	if (!state->filearray[curr_pos]) {
		ast_log(LOG_WARNING, "Empty file at pos %d\n", curr_pos);
		state->pos++;
		return -1;
	}
	if (! (ast_openstream_full(chan, state->filearray[curr_pos], chan->language, 1)) ) {
		ast_log(LOG_WARNING, "Unable to open file '%s': %s\n", state->filearray[curr_pos], strerror(errno));
		state->pos++;
		return -1;
	}

	if (state->samples) {
		res = ast_seekstream(chan->stream, state->samples, SEEK_SET);
	}
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "%s Opened file '%s' at offset %d\n", chan->name, state->filearray[curr_pos], state->samples);

	return 0;
}


static struct ast_frame *playbg_readframe(struct ast_channel *chan) 
{
	struct playbg_state *state = NULL;
	struct ast_datastore *datastore;
	struct ast_frame *f = NULL;

	if (!(chan->stream && (f = ast_readframe(chan->stream)))) {
		if (!playbg_seek(chan))
			f = ast_readframe(chan->stream);
	}
	if (!f) {
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Increment to next playbg file for %s\n", chan->name);
		ast_channel_lock(chan);
		datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
		ast_channel_unlock(chan);
		if (!datastore) {
			ast_log(LOG_WARNING, "No playbg state found\n");
			return NULL;
		}
		state = datastore->data;
		if (!state) {
			ast_log(LOG_WARNING, "Invalid playbg state\n");
			return NULL;
		}
		state->pos++;
		state->samples = 0;
		if (!playbg_seek(chan))
			f = ast_readframe(chan->stream);
	}

	return f;
}


static int playbg_generator(struct ast_channel *chan, void *data, int len, int samples)
{
	struct playbg_state *state = NULL;
	struct ast_frame *f = NULL;
	struct ast_datastore *datastore;
	int res = 0;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);
	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found\n");
		return -1;
	}

	state = datastore->data;
	if (!state) {
		ast_log(LOG_WARNING, "Invalid playbg state\n");
		return -1;
	}

	state->sample_queue += samples;

	while (state->sample_queue > 0) {
		if ((f = playbg_readframe(chan))) {
			state->samples += f->samples;
			state->sample_queue -= f->samples;
			res = ast_write(chan, f);
			ast_frfree(f);
			if (res < 0) {
				ast_log(LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror(errno));
				return -1;
			}
		} else
			return -1;	
	}
	return res;
}


static void *playbg_alloc(struct ast_channel *chan, void *params)
{
        struct ast_datastore *datastore = NULL;
	struct playbg_state *state = NULL;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);

	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found !\n");
		return NULL;
	} else {
		state = datastore->data;
		if (!state) {
			return NULL;
		}
		state->origwfmt = chan->writeformat;
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Using current stored playbg state for %s\n", chan->name);
		return state;
	}
	return NULL;
}


static struct ast_generator playbg_stream = {
	.alloc = playbg_alloc,
	.release = playbg_release,
	.generate = playbg_generator,
};


static int playbg_start(struct ast_channel *chan, const char *opts) 
{
	int res = -1;
	int nfiles = 0;
        struct ast_datastore *datastore = NULL;
	struct playbg_state *state = NULL;
	char *cur;
	char *opt;
	char *opt2;

	if (ast_strlen_zero(opts)) {
		return -1;
	}

	opt = ast_strdupa(opts);
	opt2 = ast_strdupa(opts);

	if (strchr(opt, '&')) {
		while ((cur = strsep(&opt, "&")) ) {
			nfiles++;
		}
	} else {
		nfiles = 1;
	}
	
	if (nfiles < 1) {
		return -1;
	}

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);

	/* if we found a datastore, override current one */
	if (datastore) {
		state = datastore->data;
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Changing playbg state with '%s' for %s\n", opt, chan->name);
		ast_channel_lock(chan);
		ast_channel_datastore_remove(chan, datastore);
		ast_channel_unlock(chan);
		ast_channel_datastore_free(datastore);
		ast_deactivate_generator(chan);
		if (chan->stream) {
			ast_closestream(chan->stream);
			chan->stream = NULL;
		}
		datastore = NULL;
	}

	/* if no datastore we can create a new one */
	ast_log(LOG_DEBUG, "Create a new playbg state\n");
	if (!(datastore = ast_channel_datastore_alloc(&playbg_state_datastore_info, "playbg"))) {
		ast_log(LOG_WARNING, "Unable to allocate new datastore\n");
		return -1;
	}

	if (!(state = ast_calloc(1, sizeof(*state)))) {
		ast_log(LOG_WARNING, "Unable to allocate memory for playbg state\n");
		ast_channel_datastore_free(datastore);
		return -1;
	}

	if (!state) {
		return -1;
	}
	if (!(state->filearray = ast_calloc(1, nfiles * sizeof(char) * MAX_PATH_LENGTH))) {
		ast_log(LOG_WARNING, "Unable to allocate memory for file array\n");
		ast_free(state);
		ast_channel_datastore_free(datastore);
		return -1;
	}
	int pos = 0;
	if (strchr(opt2, '&')) {
		while ((cur = strsep(&opt2, "&"))) {
			state->filearray[pos] = ast_strdup(cur);
			ast_log(LOG_DEBUG, "Add file '%s' at position %d\n", state->filearray[pos], pos);
			pos++;
		}
	} else {
		state->filearray[0] = ast_strdup(opt2);
		ast_log(LOG_DEBUG, "Only one file '%s' added\n", opt2);
	}


	state->nfiles = nfiles;
	state->pos = 0;

	state->origwfmt = chan->writeformat;

	datastore->data = state;

	ast_channel_datastore_add(chan, datastore);

	res = ast_activate_generator(chan, &playbg_stream, NULL);
	return res;
}


static void playbg_stop(struct ast_channel *chan)
{
	struct ast_datastore *datastore;
	struct playbg_state *state;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);
	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found\n");
		return;
	}
	state = datastore->data;
	if (!state) {
		ast_log(LOG_WARNING, "Invalid playbg state\n");
		return;
	}
	ast_channel_lock(chan);
	ast_channel_datastore_remove(chan, datastore);
	ast_channel_unlock(chan);
	ast_channel_datastore_free(datastore);

	ast_deactivate_generator(chan);
	if (chan->stream) {
		ast_closestream(chan->stream);
		chan->stream = NULL;
	}
}


static int playbg_exec_stop(struct ast_channel *chan, void *data)
{
	playbg_stop(chan);
	return 0;
}


static int playbg_exec_start(struct ast_channel *chan, void *data)
{
	int res = 0;
	const char *cur;
	char *opt;
	int nfiles = 0;

	if (!data || !strlen(data))
		return -1;

	opt = ast_strdupa(data);

	res = playbg_start(chan, opt);

	return res;
}


static int playbg_exec_resume(struct ast_channel *chan, void *data)
{
	struct ast_datastore *datastore = NULL;
	struct playbg_state *state = NULL;
	int res = 0;

	ast_channel_lock(chan);
	datastore = ast_channel_datastore_find(chan, &playbg_state_datastore_info, "playbg");
	ast_channel_unlock(chan);
	if (!datastore) {
		ast_log(LOG_WARNING, "No playbg state found\n");
		return -1;
	}
	state = datastore->data;
	if (!state) {
		ast_log(LOG_WARNING, "Invalid playbg state\n");
		return -1;
	}
	state->origwfmt = chan->writeformat;

	res = ast_activate_generator(chan, &playbg_stream, NULL);
	return res;
}


static int load_module(void)
{
	int res = 0;
	res |= ast_register_application(app1, playbg_exec_start, syn1, desc1);
	res |= ast_register_application(app2, playbg_exec_stop, syn2, desc2);
	res |= ast_register_application(app3, playbg_exec_resume, syn3, desc3);
	return res;
}


static int unload_module(void)
{
	int res = 0;
	res |= ast_unregister_application(app1);
	res |= ast_unregister_application(app2);
	res |= ast_unregister_application(app3);
	return res;
}


AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Play BG",
	.load = load_module,
	.unload = unload_module,
);

