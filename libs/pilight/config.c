/*
	Copyright (C) 2013 - 2014 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <regex.h>
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>

#include "../../pilight.h"
#include "common.h"
#include "json.h"
#include "config.h"
#include "log.h"
#include "devices.h"
#include "settings.h"
#include "registry.h"
#ifdef EVENTS
	#include "rules.h"
#endif
#include "hardware.h"
#include "gui.h"

static struct config_t *config;

static void sort_list(int r) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *a = NULL;
	struct config_t *b = NULL;
	struct config_t *c = NULL;
	struct config_t *e = NULL;
	struct config_t *tmp = NULL;

	while(config && e != config->next) {
		c = a = config;
		b = a->next;
		while(a != e) {
			if((r == 0 && a->writeorder > b->writeorder) ||
			   (r == 1 && a->readorder > b->readorder)) {
				if(a == config) {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					config = b;
					c = b;
				} else {
					tmp = b->next;
					b->next = a;
					a->next = tmp;
					c->next = b;
					c = b;
				}
			} else {
				c = a;
				a = a->next;
			}
			b = a->next;
			if(b == e)
				e = a;
		}
	}
}

/* The location of the config file */
static char *configfile = NULL;

int config_gc(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct config_t *listeners;
	while(config) {
		listeners = config;
		listeners->gc();
		FREE(config->name);
		config = config->next;
		FREE(listeners);
	}
	if(config != NULL) {
		FREE(config);
	}
	FREE(configfile);
	logprintf(LOG_DEBUG, "garbage collected config library");
	return 1;
}

int config_parse(JsonNode *root) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *jconfig = NULL;
	unsigned short error = 0;

	sort_list(1);
	struct config_t *listeners = config;
	while(listeners) {
		if((jconfig = json_find_member(root, listeners->name))) {
			if(listeners->parse) {
				if(listeners->parse(jconfig) == EXIT_FAILURE) {
					error = 1;
					break;
				}
			}
		}
		listeners = listeners->next;
	}

	if(error) {
		config_gc();
		return EXIT_FAILURE;
	} else {
		return EXIT_SUCCESS;
	}
}

JsonNode *config_print(int level, const char *media) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *root = json_mkobject();

	sort_list(0);
	struct config_t *listeners = config;
	while(listeners) {
		if(listeners->sync) {
			struct JsonNode *child = listeners->sync(level, media);
			if(child != NULL) {
				json_append_member(root, listeners->name, child);
			}
		}
		listeners = listeners->next;
	}

	return root;
}

int config_write(int level, const char *media) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	struct JsonNode *root = json_mkobject();
	FILE *fp;

	sort_list(0);
	struct config_t *listeners = config;
	while(listeners) {
		if(listeners->sync) {
			struct JsonNode *child = listeners->sync(level, media);
			if(child != NULL) {
				json_append_member(root, listeners->name, child);
			}
		}
		listeners = listeners->next;
	}

	/* Overwrite config file with proper format */
	if(!(fp = fopen(configfile, "w+"))) {
		logprintf(LOG_ERR, "cannot write config file: %s", configfile);
		return EXIT_FAILURE;
	}
	fseek(fp, 0L, SEEK_SET);
	char *content = json_stringify(root, "\t");
 	fwrite(content, sizeof(char), strlen(content), fp);
	fclose(fp);
	FREE(content);
	json_delete(root);
	return EXIT_SUCCESS;
}

int config_read(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	FILE *fp = NULL;
	char *content = NULL;
	size_t bytes = 0;
	struct JsonNode *root = NULL;
	struct stat st;

	/* Read JSON config file */
	if(!(fp = fopen(configfile, "rb"))) {
		logprintf(LOG_ERR, "cannot read config file: %s", configfile);
		return EXIT_FAILURE;
	}

	fstat(fileno(fp), &st);
	bytes = (size_t)st.st_size;

	if(!(content = calloc(bytes+1, sizeof(char)))) {
		logprintf(LOG_ERR, "out of memory");
		fclose(fp);
		return EXIT_FAILURE;
	}

	if(fread(content, sizeof(char), bytes, fp) == -1) {
		logprintf(LOG_ERR, "cannot read config file: %s", configfile);
	}
	fclose(fp);

	/* Validate JSON and turn into JSON object */
	if(json_validate(content) == false) {
		logprintf(LOG_ERR, "config is not in a valid json format");
		FREE(content);
		return EXIT_FAILURE;
	}
	root = json_decode(content);

	if(config_parse(root) != EXIT_SUCCESS) {
		FREE(content);
		json_delete(root);
		return EXIT_FAILURE;
	}
	json_delete(root);
	config_write(1, "all");
	FREE(content);
	return EXIT_SUCCESS;
}

void config_register(config_t **listener, const char *name) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(!(*listener = MALLOC(sizeof(config_t)))) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}

	if(!((*listener)->name = MALLOC(strlen(name)+1))) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}
	strcpy((*listener)->name, name);
	(*listener)->parse = NULL;
	(*listener)->sync = NULL;
	(*listener)->gc = NULL;
	(*listener)->next = config;
	config = (*listener);
}

int config_set_file(char *settfile) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	if(access(settfile, R_OK | W_OK) != -1) {
		if((configfile = REALLOC(configfile, strlen(settfile)+1)) == NULL) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(configfile, settfile);
	} else {
		logprintf(LOG_ERR, "the config file %s does not exists", settfile);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

char *config_get_file(void) {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	return configfile;
}

void config_init() {
	logprintf(LOG_STACK, "%s(...)", __FUNCTION__);

	hardware_init();
	settings_init();
	devices_init();
	gui_init();
#ifdef EVENTS
	rules_init();
#endif
	registry_init();
}
