LLVM_FLAGS := $(shell llvm-config --cflags --libs)
Z3_FLAGS := -I/usr/include/z3 -lz3
CFLAGS := ${LLVM_FLAGS} -g
HEADER_FILES := jitir.hpp jitir_llvmapi.hpp $(wildcard *.hpp)
TEST_HEADER_FILES := $(wildcard tests/*.hpp)

run: main
	./main

test: tests/test_knownbits tests/test_insts tests/test_cfg tests/test_fuzzer tests/test_opt tests/test_source
	./tests/test_knownbits
	./tests/test_insts
	./tests/test_cfg
	./tests/test_fuzzer
	./tests/test_opt
	./tests/test_source

fuzz: tests/fuzzer
	./tests/fuzzer

main: main.cpp jitir.hpp jitir_llvmapi.hpp llvmgen.hpp x86gen.hpp
	clang++ ${CFLAGS} -o $@ $<

tests/test_knownbits: tests/test_knownbits.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_insts: tests/test_insts.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_fuzzer: tests/test_fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_cfg: tests/test_cfg.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_opt: tests/test_opt.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_reader: tests/test_reader.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -g -o $@ $<

TEST_SOURCE_LL_FILES := \
	$(patsubst tests/source/%.c,tests/source/%.o0.ll,$(wildcard tests/source/*.c)) \
	$(patsubst tests/source/%.cpp,tests/source/%.o0.ll,$(wildcard tests/source/*.cpp)) \
	$(patsubst tests/source/%.c,tests/source/%.o1.ll,$(wildcard tests/source/*.c)) \
	$(patsubst tests/source/%.cpp,tests/source/%.o1.ll,$(wildcard tests/source/*.cpp))

tests/test_source: tests/test_source.cpp ${TEST_SOURCE_LL_FILES} ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/source/%.o1.ll: tests/source/%.c
	clang -emit-llvm -S -O1 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o $@ $<

tests/source/%.o1.ll: tests/source/%.cpp
	clang++ -emit-llvm -S -O1 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o $@ $<

tests/source/%.o0.ll: tests/source/%.c
	clang -emit-llvm -S -O0 -o $@ $<

tests/source/%.o0.ll: tests/source/%.cpp
	clang++ -emit-llvm -S -O0 -o $@ $<

tests/fuzzer: tests/fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ -O3 -g ${CFLAGS} -o $@ $<

tests/test_tv: tests/test_tv.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ -g ${CFLAGS} ${Z3_FLAGS} -o $@ $<

jitir.hpp: jitir.py jitir.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<

jitir_llvmapi.hpp: jitir.py jitir_llvmapi.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<

clean:
	-rm main
	-rm tests/test_knownbits
	-rm tests/test_insts
	-rm tests/test_fuzzer
	-rm tests/test_cfg
	-rm tests/test_opt
	-rm tests/test_source
	-rm tests/test_reader
	-rm tests/fuzzer
	-rm jitir.hpp
	-rm jitir_llvmapi.hpp
	-rm tests/source/*.ll
	-rm -r tests/output
	mkdir -p tests/output/test_insts
	mkdir -p tests/output/test_fuzzer
	mkdir -p tests/output/test_cfg
	mkdir -p tests/output/test_opt
	mkdir -p tests/output/test_source
	mkdir -p tests/output/test_reader
