LLVM_FLAGS := $(shell llvm-config --cflags --libs)
Z3_FLAGS := -I/usr/include/z3 -lz3
CFLAGS := ${LLVM_FLAGS} -g
HEADER_FILES := jitir.hpp jitir_llvmapi.hpp genext.hpp $(wildcard *.hpp)
TEST_HEADER_FILES := $(wildcard tests/*.hpp)
TEST_CFLAGS := ${CFLAGS} -DMETAJIT_DEBUG
COVERAGE_CFLAGS := ${TEST_CFLAGS} -fprofile-instr-generate -fcoverage-mapping

COVERAGE_TESTS := test_knownbits test_insts test_interpreter test_clone test_cfg test_fuzzer test_opt test_reentry test_mem2reg test_source test_genext

run: main
	./main

test: tests/test_knownbits tests/test_insts tests/test_interpreter tests/test_clone tests/test_cfg tests/test_fuzzer tests/test_opt tests/test_reentry tests/test_mem2reg tests/test_source tests/test_genext
	./tests/test_knownbits
	./tests/test_insts
	./tests/test_interpreter
	./tests/test_clone
	./tests/test_cfg
	./tests/test_fuzzer
	./tests/test_opt
	./tests/test_reentry
	./tests/test_mem2reg
	./tests/test_source
	./tests/test_genext

fuzz: tests/fuzzer
	./tests/fuzzer

main: main.cpp ${HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_knownbits: tests/test_knownbits.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_insts: tests/test_insts.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_interpreter: tests/test_interpreter.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_clone: tests/test_clone.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_fuzzer: tests/test_fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_cfg: tests/test_cfg.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_opt: tests/test_opt.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_reader: tests/test_reader.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -g -o $@ $<

tests/test_reentry: tests/test_reentry.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/test_mem2reg: tests/test_mem2reg.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

TEST_SOURCE_LL_FILES := \
	$(patsubst tests/source/%.c,tests/source/%.o0.ll,$(wildcard tests/source/*.c)) \
	$(patsubst tests/source/%.cpp,tests/source/%.o0.ll,$(wildcard tests/source/*.cpp)) \
	$(patsubst tests/source/%.c,tests/source/%.o1.ll,$(wildcard tests/source/*.c)) \
	$(patsubst tests/source/%.cpp,tests/source/%.o1.ll,$(wildcard tests/source/*.cpp))

tests/test_source: tests/test_source.cpp ${TEST_SOURCE_LL_FILES} ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

tests/source/%.o1.ll: tests/source/%.c
	clang -emit-llvm -S -O1 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o $@ $<

tests/source/%.o1.ll: tests/source/%.cpp
	clang++ -emit-llvm -S -O1 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o $@ $<

tests/source/%.o0.ll: tests/source/%.c
	clang -emit-llvm -S -O0 -o $@ $<

tests/source/%.o0.ll: tests/source/%.cpp
	clang++ -emit-llvm -S -O0 -o $@ $<

tests/fuzzer: tests/fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ -O3 -g ${CFLAGS} ${Z3_FLAGS} -o $@ $<

tests/test_tv: tests/test_tv.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ -g ${CFLAGS} ${Z3_FLAGS} -o $@ $<

tests/test_genext: tests/test_genext.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${TEST_CFLAGS} -o $@ $<

jitir.hpp jitir_llvmapi.hpp genext.hpp &: jitir.py jitir.tmpl.hpp jitir_llvmapi.tmpl.hpp genext.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 jitir.py

COVERAGE_PROFRAW_FILES := $(patsubst %,tests/coverage/%.profraw,$(COVERAGE_TESTS))

tests/coverage/%.profraw: tests/%.cpp jitir.hpp jitir_llvmapi.hpp genext.hpp ${HEADER_FILES} ${TEST_HEADER_FILES} | tests/coverage
	clang++ $(COVERAGE_CFLAGS) -o tests/coverage/$* $<
	LLVM_PROFILE_FILE=$@ tests/coverage/$*

tests/coverage/test_source.profraw: ${TEST_SOURCE_LL_FILES}

tests/coverage/merged.profdata: $(COVERAGE_PROFRAW_FILES)
	llvm-profdata-20 merge -sparse $^ -o $@

tests/coverage:
	mkdir -p tests/coverage

coverage: tests/coverage/merged.profdata
	llvm-cov-20 show --format=html --instr-profile=$< \
		$(patsubst %,-object tests/coverage/%,$(COVERAGE_TESTS)) \
		--ignore-filename-regex="^/usr" \
		> tests/coverage/report.html
	@echo "Coverage report: tests/coverage/report.html"

coverage-report: tests/coverage/merged.profdata
	llvm-cov-20 report --instr-profile=$< \
		$(patsubst %,-object tests/coverage/%,$(COVERAGE_TESTS)) \
		--ignore-filename-regex="^/usr|/tests/|lwir_utils\.hpp|unittest\.hpp"

clean:
	-rm -r tests/coverage
	-rm main
	-rm tests/test_knownbits
	-rm tests/test_insts
	-rm tests/test_interpreter
	-rm tests/test_clone
	-rm tests/test_fuzzer
	-rm tests/test_cfg
	-rm tests/test_opt
	-rm tests/test_source
	-rm tests/test_reader
	-rm tests/test_reentry
	-rm tests/test_genext
	-rm tests/fuzzer
	-rm jitir.hpp
	-rm jitir_llvmapi.hpp
	-rm genext.hpp
	-rm tests/source/*.ll
	-rm -r tests/output
	mkdir -p tests/output/test_insts
	mkdir -p tests/output/test_fuzzer
	mkdir -p tests/output/test_cfg
	mkdir -p tests/output/test_opt
	mkdir -p tests/output/test_source
	mkdir -p tests/output/test_reader
	mkdir -p tests/output/test_mem2reg
	mkdir -p tests/output/test_genext
	mkdir -p tests/coverage
