#include <stdio.h>
#include <string.h>

#include "model.h"
#include "util.h"

static void usage(void)
{
	printf("usage: kora <command> [args]\n"
	       "\n"
	       "commands:\n"
	       "  chat            interactive chat\n"
	       "  code            agentic coding assistant\n"
	       "  pull <model>    download a model\n"
	       "  list            list downloaded models\n"
	       "  rm <model>      remove a model\n"
	       "  sessions        list sessions\n"
	       "  version         print version\n");
}

static void version(void)
{
	printf("kora %s\n", VERSION);
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage();
		return 1;
	}

	if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
		version();
		return 0;
	}

	if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
		usage();
		return 0;
	}

	if (kora_init_dirs() != 0) {
		fprintf(stderr, "kora: failed to initialize ~/.kora/\n");
		return 1;
	}

	if (strcmp(argv[1], "pull") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: kora pull <model|url>\n");
			return 1;
		}
		return model_pull(argv[2]);
	}

	if (strcmp(argv[1], "list") == 0) {
		return model_list();
	}

	if (strcmp(argv[1], "rm") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: kora rm <model>\n");
			return 1;
		}
		return model_rm(argv[2]);
	}

	if (strcmp(argv[1], "chat") == 0) {
		printf("chat: not yet implemented\n");
		return 1;
	}

	if (strcmp(argv[1], "code") == 0) {
		printf("code: not yet implemented\n");
		return 1;
	}

	if (strcmp(argv[1], "sessions") == 0) {
		printf("sessions: not yet implemented\n");
		return 1;
	}

	fprintf(stderr, "kora: unknown command '%s'\n", argv[1]);
	usage();
	return 1;
}
