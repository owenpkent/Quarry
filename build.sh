#!/usr/bin/env bash
set -euf -o pipefail
set -x
os=windows
arch="$(uname -m)"
file=onnxruntime.lib
version=v1.14.1-neuralnote.1
# SHA-256 of the v1.14.1-neuralnote.1 Windows tarball; keep in step with ONNX_SHA256 in run.py.
sha256=0fe0568469956181fafffdfb416916c10919114b0c8ce2badddd0a9a313ebab8
test_enabled=ON

if test "$(uname -s)" = "Darwin"; then
	os=macOS
	arch=universal
	file=libonnxruntime.a
	# To remove warnings about minimum macOS target versions
	version=v1.14.1-neuralnote.2
	sha256=7e943da96b195713eea605f983f8ba0b312a9a39077468593665fa33ac27f915
fi
if test "$(uname -s)" = "Linux"; then
	os=linux
	arch="$(uname -m)"
	file=libonnxruntime.lib
	# To remove warnings about minimum macOS target versions
	version=v1.14.1-neuralnote.0
	sha256=""
	test_enabled=OFF
fi
dir="onnxruntime-${version}-${os}-${arch}"
archive="$dir.tar.gz"

if test "$(uname -s)" = "Linux"; then
	# No release tagged v1.14.1-neuralnote.0 exists upstream, so there is nothing to fetch or to
	# verify. Fail here rather than copy an unchecked archive or 404 later.
	if test -z "$sha256"; then
		echo "The prebuilt onnxruntime archive for Linux is unavailable: release ${version} does not exist." >&2
		echo "Build onnxruntime yourself, or point 'version' in build.sh (and its sha256) at a release with a Linux asset." >&2
		exit 1
	fi
	cp ../libonnxruntime-neuralnote/$archive .
fi

# macOS has no sha256sum by default.
sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

# If either the library or the ort model is missing or if an archive was found
# then fetch ort model and library again.
if test ! -f "Lib/ModelData/features_model.ort" -o ! -f "ThirdParty/onnxruntime/lib/$file" -o -f "$archive"; then
	if ! test -f "$archive"; then
		curl -fsSLO "https://github.com/tiborvass/libonnxruntime-neuralnote/releases/download/${version}/${archive}"
	fi
	got="$(sha256_of "$archive")"
	if test "$got" != "$sha256"; then
		echo "The onnxruntime archive is not the expected file:" >&2
		echo "  expected sha256 $sha256" >&2
		echo "  got             $got" >&2
		rm -f "$archive"
		exit 1
	fi
	rm -rf ThirdParty/$dir ThirdParty/onnxruntime
	tar -C ThirdParty/ -xvf "$archive"
	mv "ThirdParty/$dir" ThirdParty/onnxruntime
	mv ThirdParty/onnxruntime/model.with_runtime_opt.ort Lib/ModelData/features_model.ort
	rm "$archive"
fi

ncpus="$(getconf _NPROCESSORS_ONLN || echo 1)"

config=Release
cmake -S . -B build -DCMAKE_BUILD_TYPE="${config}" -DBUILD_UNIT_TESTS="${test_enabled}" && cmake --build build -j "${ncpus}"

if [[ "$test_enabled" = "ON" ]]; then
	./build/Tests/UnitTests_artefacts/Release/UnitTests
fi

echo
if test "$(uname -s)" = "Linux"; then
	echo "Run: ./build/Quarry_artefacts/Release/Standalone/Quarry"
else
	echo "Run: ./build/Quarry_artefacts/Release/Standalone/Quarry.app/Contents/MacOS/Quarry"
fi
