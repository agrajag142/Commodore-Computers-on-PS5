# ps5.mk - read after Amiberry's libretro/Makefile (make -f Makefile -f ps5.mk):
# just its objects (the shared library is not made), and their list.
objs: $(OBJECTS)
print-objects:
	@echo $(OBJECTS)
.PHONY: objs print-objects
