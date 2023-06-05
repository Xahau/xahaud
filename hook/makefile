all: reward govern mint
reward:
	wasmcc reward.c -o reward.wasm -Oz -Wl,--allow-undefined  -I../
	wasm-opt reward.wasm -o reward.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner reward.wasm
	wasm-opt reward.wasm -o reward.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner reward.wasm
	guard_checker reward.wasm
govern:
	wasmcc govern.c -o govern.wasm -Oz -Wl,--allow-undefined  -I../
	wasm-opt govern.wasm -o govern.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner govern.wasm
	wasm-opt govern.wasm -o govern.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner govern.wasm
	guard_checker govern.wasm
mint:
	wasmcc mint.c -o mint.wasm -Oz -Wl,--allow-undefined  -I../
	wasm-opt mint.wasm -o mint.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner mint.wasm
	wasm-opt mint.wasm -o mint.wasm \
		--shrink-level=100000000 \
		--coalesce-locals-learning \
		--vacuum \
		--merge-blocks \
		--merge-locals \
		--flatten \
		--ignore-implicit-traps \
		-ffm \
		--const-hoisting \
		--code-folding \
		--code-pushing \
		--dae-optimizing \
		--dce \
		--simplify-globals-optimizing \
		--simplify-locals-nonesting \
		--reorder-locals \
		--rereloop \
		--precompute-propagate \
		--local-cse \
		--remove-unused-brs \
		--memory-packing \
		-c \
		--avoid-reinterprets \
		-Oz
	hook-cleaner mint.wasm
	guard_checker mint.wasm
