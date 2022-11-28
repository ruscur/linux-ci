TEST_GEN_PROGS := dexcr_test hashchk_test
TEST_GEN_FILES := lsdexcr

TEST_FILES := settings
top_srcdir = ../../../../..
include ../../lib.mk

HASHCHK_TEST_CFLAGS = -no-pie $(call cc-option,-mno-rop-protect)

$(TEST_GEN_PROGS): ../harness.c ../utils.c ./dexcr.c ./cap.c
$(TEST_GEN_FILES): ../utils.c ./dexcr.c
