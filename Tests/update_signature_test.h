//
// The updater's Authenticode check against real files: the test executable itself, which is
// unsigned, must be refused, and a Microsoft file with an embedded signature must verify at the
// WinVerifyTrust level yet still be refused as a signer, because it is not ours. Windows only.
// Revocation is checked online, so a machine without a network skips the signed half.
//

#ifndef QUARRY_UPDATE_SIGNATURE_TEST_H
#define QUARRY_UPDATE_SIGNATURE_TEST_H

#include <iostream>

#include <JuceHeader.h>

#if JUCE_WINDOWS
    #include "Components/UpdateInstaller.h"
    #include "Components/UpdatePolicy.h"
#endif

inline bool update_signature_test()
{
#if JUCE_WINDOWS
    using quarry::update::SignerInfo;
    using quarry::update::UpdateInstaller;

    bool ok = true;

    {
        const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        SignerInfo info;
        const bool read = UpdateInstaller::readSigner(self, nullptr, info);
        if (read || info.trustResult == 0
            || quarry::update::isAcceptedSigner(info.thumbprint, info.commonName)) {
            std::cout << "  FAILED: the unsigned test executable must be refused" << std::endl;
            ok = false;
        }
    }

    const char* candidates[] = {"C:\\Windows\\explorer.exe",
                                "C:\\Windows\\System32\\ntoskrnl.exe",
                                "C:\\Windows\\System32\\msiexec.exe"};
    bool verified_one = false;
    for (const auto* path: candidates) {
        const juce::File file(path);
        if (!file.existsAsFile())
            continue;

        SignerInfo info;
        if (!UpdateInstaller::readSigner(file, nullptr, info))
            continue;

        verified_one = true;
        std::cout << "  " << path << " verified, signer \"" << info.commonName.toStdString() << "\", thumbprint "
                  << info.thumbprint.toStdString() << std::endl;
        if (info.trustResult != 0 || info.thumbprint.length() != 40 || info.commonName.isEmpty()) {
            std::cout << "  FAILED: a verified file should report a signer" << std::endl;
            ok = false;
        }
        if (quarry::update::isAcceptedSigner(info.thumbprint, info.commonName)) {
            std::cout << "  FAILED: a Microsoft signer must not be accepted" << std::endl;
            ok = false;
        }
        break;
    }

    if (!verified_one)
        std::cout << "  skipped the signed file check (no candidate had an embedded signature that verified)" << std::endl;

    std::cout << (ok ? "  all passed" : "  FAILURES") << std::endl;
    return ok;
#else
    return true;
#endif
}

#endif // QUARRY_UPDATE_SIGNATURE_TEST_H
