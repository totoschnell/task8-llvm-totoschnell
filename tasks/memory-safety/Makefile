FINAL_RTLIB = build/libMemorySafetyRuntime.a

all: cmake-build rtlib
#rtlib: rust
rtlib: cpp
#rtlib: c

.PHONY clean:
	rm -rf build target

build_dir:
	mkdir -p build

rust: build_dir
	cargo build --release
	cp target/release/libmemory_safety_runtime.a $(FINAL_RTLIB)

cpp: cmake-build
	cp build/libMemorySafetyRuntimeCPP.a $(FINAL_RTLIB)
c: cmake-build
	cp build/libMemorySafetyRuntimeC.a $(FINAL_RTLIB)

cmake-build: build_dir
	cd build; cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
	ninja -Cbuild
