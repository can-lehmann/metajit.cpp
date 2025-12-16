CFLAGS := $(shell llvm-config --cflags --libs)
HEADER_FILES := jitir.hpp jitir_llvmapi.hpp $(wildcard *.hpp)
TEST_HEADER_FILES := $(wildcard tests/*.hpp)

run: main
	./main

test: tests/test_insts tests/test_fuzzer
	./tests/test_insts
	./tests/test_fuzzer

fuzz: tests/fuzzer
	./tests/fuzzer

main: main.cpp jitir.hpp jitir_llvmapi.hpp llvmgen.hpp x86gen.hpp
	clang++ ${CFLAGS} -o $@ $<

tests/test_insts: tests/test_insts.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/test_fuzzer: tests/test_fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

tests/fuzzer: tests/fuzzer.cpp ${HEADER_FILES} ${TEST_HEADER_FILES}
	clang++ ${CFLAGS} -o $@ $<

jitir.hpp: jitir.py jitir.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<

jitir_llvmapi.hpp: jitir.py jitir_llvmapi.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<
