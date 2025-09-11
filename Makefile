run: main
	./main

main: main.cpp jitir.hpp jitir_llvmapi.hpp llvmgen.hpp x86gen.hpp
	clang++ `llvm-config --libs --cflags` -o $@ $<

jitir.hpp: jitir.py jitir.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<

jitir_llvmapi.hpp: jitir.py jitir_llvmapi.tmpl.hpp
	PYTHONPATH="../lwir.cpp" python3 $<
