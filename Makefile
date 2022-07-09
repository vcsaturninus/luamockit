LUA_VERSION:=lua5.3                   # lua version to compile for and link with

CC:=gcc
C_STANDARD:=c99

# Pedantic compliance with a specified C standard means various rudimentary
# features (e.g. sigaction, sigsetjmp etc) may be left out. Specify the feature
# test macros to use for deciding what features to include and which ones to omit
FEATURE_TEST_MACROS:=-D_XOPEN_SOURCE -D_POSIX_C_SOURCE=200112L 
CFLAGS:= -g -Wall -Werror -std=$(C_STANDARD) -pedantic -fstrict-aliasing -Wcast-align=strict -O3
INCLUDES:=includes/
CPPFLAGS:=-I$(INCLUDES) $(FEATURE_TEST_MACROS)

# the realtime library is needed for the POSIX clocks and interval timers API
# the pthead library is needed for semaphore and mutex suport
LDFLAGS:=-l$(LUA_VERSION) -lpthread -lrt

CWD:=$(shell pwd)
OUT_DIR:=$(CWD)/out
SRC_DIR:=$(CWD)/src
TESTS_DIR:=$(CWD)/tests/
LUA_TESTS_FILE:=tests.lua
C_TESTS_FILE:=tests.c
C_TESTS_BIN:=ctests

MOCKIT_SOURCES:=$(SRC_DIR)/mockit.c
LUA_MOCKIT_SOURCES:=$(SRC_DIR)/luamockit.c
CLIB_SONAME:=mockit.so
LUALIB_SONAME:=lua$(CLIB_SONAME)

# always run the all target even when there are no apparent changes
.PHONY: all

# always clean first
all : clean make_dirs build_clib build_lualib tests done

mockit: clean make_dirs build_clib

luamockit: clean make_dirs build_lualib

build_clib: $(MOCKIT_SOURCES)
	@ echo "[ ] Building C library (mockit.so) ..."
	@ $(CC) $^ -shared -fPIC $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
		-o $(OUT_DIR)/$(CLIB_SONAME)
	@ echo ""

build_lualib: $(MOCKIT_SOURCES) $(LUA_MOCKIT_SOURCES)
	@ echo "[ ] Building lua library (luamockit.so) ..."
	@ $(CC) $^ -shared -fPIC $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
		-o $(OUT_DIR)/$(LUALIB_SONAME)
	@ echo ""

done :
	@ echo "[ ] DONE."
	@ echo ""

make_dirs:
	@ echo "[ ] Creating directories ... "
	@ mkdir -p $(OUT_DIR)
	@ echo ""

clean:
	@ echo "[ ] Cleaning artifacts ... "
	@ rm -rf $(OUT_DIR) 
	@ echo ""

.PHONY : tests ctests luatests

tests: ctests luatests

ctests: make_dirs build_clib
	@ echo "[ ] Running C tests (mockit)..."
	@ $(CC) $(CFLAGS) $(CPPFLAGS) -Isrc $(TESTS_DIR)/$(C_TESTS_FILE) -L$(OUT_DIR) -l:mockit.so -o out/$(C_TESTS_BIN)
	@ LD_LIBRARY_PATH=$(realpath $(OUT_DIR)/):$(LD_LIBRARY_PATH) $(OUT_DIR)/$(C_TESTS_BIN)
	@ echo ""

luatests: make_dirs build_lualib
	@ echo "[ ] Running lua tests (luamockit)..."
	@ $(TESTS_DIR)/$(LUA_TESTS_FILE)
	@ echo ""

VALGRIND_REPORT:=valgrind.txt
grind:
	LD_LIBRARY_PATH=$(realpath $(OUT_DIR)/):$(LD_LIBRARY_PATH) \
	valgrind --leak-check=full --show-leak-kinds=all \
		--track-origins=yes --verbose \
		--log-file=$(VALGRIND_REPORT) \
		$(OUT_DIR)/$(C_TESTS_BIN)
		#$(TESTS_DIR)/$(LUA_TESTS_FILE)
		#$(OUT_DIR)/$(C_TESTS_BIN)
