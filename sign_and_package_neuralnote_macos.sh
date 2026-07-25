#!/usr/bin/env bash -e

set -euo pipefail

# First argument gives the path to the dir containing the Standalone, AU and VST3 subdirectories.
# Typically cmake-build-release/NeuralNote_artefacts/Release or build/NeuralNote_artefacts/Release
PLUG_DIR=${1:-}

if [[ $# -eq 0 ]]; then
	>&2 echo "usage: $0 <release_dir>"; exit 1
fi

for dir in "$PLUG_DIR"/{Standalone/NeuralNoteVideo.app,AU/NeuralNoteVideo.component,VST3/NeuralNoteVideo.vst3}; do
	if ! test -d "$dir"; then
		>&2 echo "Could not find $dir"
		exit 1
	fi
done

signingID=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | cut -d'"' -f2)
if test -z "$signingID"; then
	>&2 echo "No signing certificates found in keychain. You need to import the Apple generated .p12 certificates into Keychain"
	exit 1
fi

# Notarization credentials are read from a notarytool keychain profile rather than passed on the
# command line, where the app-specific password would be visible to any local process listing.
NOTARY_PROFILE="${NOTARY_PROFILE:-NeuralNoteVideo}"

if ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
	echo "No notarytool keychain profile '$NOTARY_PROFILE' found, creating it."
	read -p "Enter your Apple ID: " APPLE_USERNAME
	APPLE_TEAMID=$(echo "$signingID" | cut -d'(' -f2 | cut -d')' -f1)
	if test -z "$APPLE_TEAMID"; then
		read -p "Enter your Apple Team ID: " APPLE_TEAMID
	fi
	# store-credentials prompts for the app-specific password on stdin.
	xcrun notarytool store-credentials "$NOTARY_PROFILE" --apple-id "$APPLE_USERNAME" --team-id "$APPLE_TEAMID"
fi

chmod +x "$PLUG_DIR"/{Standalone/NeuralNoteVideo.app,AU/NeuralNoteVideo.component,VST3/NeuralNoteVideo.vst3}/Contents/MacOS/NeuralNoteVideo

echo "Signing Standalone, AU and VST3"
codesign --remove-signature "$PLUG_DIR"/{Standalone/NeuralNoteVideo.app,AU/NeuralNoteVideo.component,VST3/NeuralNoteVideo.vst3} || true
codesign --entitlements entitlements.plist --options=runtime -s "$signingID" "$PLUG_DIR"/{Standalone/NeuralNoteVideo.app,AU/NeuralNoteVideo.component,VST3/NeuralNoteVideo.vst3}

# Check signature
printf "\nVerifying signature app\n"
codesign -dv --verbose=4 "$PLUG_DIR"/Standalone/NeuralNoteVideo.app

printf "\nVerifying signature VST3\n"
codesign -dv --verbose=4 "$PLUG_DIR"/VST3/NeuralNoteVideo.vst3

printf "\nVerifying signature AU\n"
codesign -dv --verbose=4 "$PLUG_DIR"/AU/NeuralNoteVideo.component

# Build installer
echo "Building installer"
packagesbuild -F "$PLUG_DIR" Installers/Mac/NeuralNoteVideo.pkgproj
mv Installers/Mac/build/NeuralNoteVideo.pkg Installers/Mac/build/NeuralNoteVideo_unsigned.pkg

# Sign installer
echo "Signing installer"
product_sign_ID=$(security find-identity -v -p basic | grep "Developer ID Installer" | head -1 | cut -d'"' -f2)
productsign --sign "$product_sign_ID" Installers/Mac/build/NeuralNoteVideo_unsigned.pkg Installers/Mac/build/NeuralNoteVideo.pkg
rm Installers/Mac/build/NeuralNoteVideo_unsigned.pkg

# Notarize the pkg and staple it
echo "Notarize and staple installer"
xcrun notarytool submit --keychain-profile "$NOTARY_PROFILE" --wait Installers/Mac/build/NeuralNoteVideo.pkg
xcrun stapler staple Installers/Mac/build/NeuralNoteVideo.pkg
