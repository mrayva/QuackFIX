PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=quackfix
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_cpp test_cpp_debug test_cpp_reldebug

# Keep the standalone C++ checks in the normal test path alongside SQLLogicTests.
test: test_cpp

test_cpp:
	cmake --build build/release --target quackfix_tokenizer_test quackfix_dictionary_test
	./build/release/quackfix_tokenizer_test
	./build/release/quackfix_dictionary_test

test_cpp_debug: debug
	./build/debug/quackfix_tokenizer_test
	./build/debug/quackfix_dictionary_test

test_cpp_reldebug: reldebug
	./build/reldebug/quackfix_tokenizer_test
	./build/reldebug/quackfix_dictionary_test

test_release_internal: test_cpp
test_debug_internal: test_cpp_debug
test_reldebug_internal: test_cpp_reldebug
