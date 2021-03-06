# Include
include ./include/makefile.inc

# Options
DEBUG_S3_SYMBOL = y

# Flags
SHLIB_LINK += $(COMMON_LINK_OPTIONS)
PG_CPPFLAGS += $(COMMON_CPP_FLAGS) -Iinclude -Ilib -I$(libpq_srcdir) -I$(libpq_srcdir)/postgresql/server/utils

ifeq ($(DEBUG_S3_SYMBOL),y)
	PG_CPPFLAGS += -g
endif

# Targets
MODULE_big = gps3ext
OBJS = src/gps3ext.o lib/http_parser.o lib/ini.o $(addprefix src/,$(COMMON_OBJS))

# Launch
PGXS := $(shell pg_config --pgxs)
include $(PGXS)

gpcheckcloud:
	@make -C bin/gpcheckcloud

test: format
	@make -C test test

coverage: format
	@make -C test coverage

tags:
	-ctags -R --c++-kinds=+p --fields=+ialS --extra=+q
	-cscope -Rbq
	-gtags -i

lint:
	cppcheck -v --enable=warning src/*.cpp bin/gpcheckcloud/*.cpp test/*.cpp include/*.h

format:
	@-clang-format --version \
		| grep -q 'clang-format version 3.9.0' \
		&& clang-format -style="{BasedOnStyle: Google, IndentWidth: 4, ColumnLimit: 100, AllowShortFunctionsOnASingleLine: None}" -i src/*.cpp bin/gpcheckcloud/*.cpp test/*.cpp include/*.h \
		|| echo clang-format 3.9.0 is not found.

cleanall:
	@-make clean # incase PGXS not included
	@-make -C bin/gpcheckcloud clean
	@make -C test clean
	rm -f *.o *.so *.a
	rm -f *.gcov src/*.gcov src/*.gcda src/*.gcno
	rm -f src/*.o src/*.d bin/gpcheckcloud/*.o bin/gpcheckcloud/*.d test/*.o test/*.d test/*.a lib/*.o lib/*.d

.PHONY: format lint tags test coverage cleanall
