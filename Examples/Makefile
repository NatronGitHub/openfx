SUBDIRS = Basic ChoiceParams ColourSpace Custom DepthConverter DrawSuite Invert OpenGL Overlay Rectangle Test

all: subdirs

.PHONY: subdirs clean $(SUBDIRS)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean :
	for i in $(SUBDIRS) ; do \
	  $(MAKE) -C $$i clean; \
	done
