/*
 * verba static fixture: the liar installer, as a static binary.
 *
 * Tracing with ptrace (rather than LD_PRELOAD) exists precisely so that
 * static binaries are in scope; this exercises that claim end to end. It lies
 * the same three ways as fixtures/liar-installer.sh.
 *
 * $1 = scratch output path (written without ever being named in the output).
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s OUT\n", argv[0]);
		return 2;
	}

	/* (1) faithless claim: say we saved a file we never touch. */
	puts("Saved configuration to /etc/verba-static-liar.conf");

	/* (2) undeclared write: write a real file we never mention. */
	FILE *f = fopen(argv[1], "w");
	if (f) {
		fputs("payload", f);
		fclose(f);
	}

	/* (3) discourse mismatch: announce success, then exit non-zero. */
	puts("Installation completed successfully.");
	return 1;
}
