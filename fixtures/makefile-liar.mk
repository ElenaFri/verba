# Make-spawning fixture: a Makefile whose rules lie the same three ways as
# fixtures/liar-installer.sh. Run with:
#
#   verba make -f fixtures/makefile-liar.mk all OUT=<scratch>/make.out
#
# Each recipe runs in a shell that make spawns, so this is a realistic
# follow-fork/exec workload (the reason verba traces children at all).

all: claim write lie

claim:
	@echo "Saved configuration to /etc/verba-make-liar.conf"

write:
	@printf payload > $(OUT)

lie:
	@echo "Installation completed successfully."
	@exit 1

.PHONY: all claim write lie
