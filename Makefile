# mini-vfx-lab: each module builds and self-tests independently.
MODULES = emitter field trail pause

.PHONY: all test clean analyze $(MODULES)

all test clean:
	@for m in $(MODULES); do $(MAKE) -C $$m $(MAKECMDGOALS); done

analyze:
	$(MAKE) -C pause analyze

$(MODULES):
	$(MAKE) -C $@
