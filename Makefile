run: main
	./main

main: main.cpp jitir.hpp llvmgen.hpp
	clang++ `llvm-config --libs --cflags` -o $@ $<

jitir.hpp: jitir.py jitir.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<
