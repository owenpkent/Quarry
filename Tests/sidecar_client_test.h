//
// SidecarClient::classifyLine: the "is this line a stage event or something else" decision both
// the transcribe()/download() response loop and start()'s ready-wait loop are built on. Covered
// here in isolation since it is static and free of instance state -- see classifyLine's own docs
// in SidecarClient.h for why it is factored out this way.
//

#ifndef SIDECAR_CLIENT_TEST_H
#define SIDECAR_CLIENT_TEST_H

#include <iostream>

#include <JuceHeader.h>

#include "SidecarClient.h"

namespace sidecar_client_test_detail
{
inline bool expect(bool inCondition, const char* inWhat, bool& ioOk)
{
    if (!inCondition) {
        std::cout << "  FAIL: " << inWhat << std::endl;
        ioOk = false;
    }

    return inCondition;
}

inline juce::var parse(const juce::String& inJson)
{
    juce::var parsed;
    juce::JSON::parse(inJson, parsed);
    return parsed;
}

inline bool testStageEventWithFraction()
{
    bool ok = true;

    const auto parsed = parse(
        R"({"event":"stage","id":"req-1","stage":"download","text":"downloading...","t":1.5,"fraction":0.42})");

    SidecarStage stage;
    const auto kind = SidecarClient::classifyLine(parsed, stage);

    expect(kind == SidecarClient::LineKind::Stage, "a stage event with fraction classifies as Stage", ok);
    expect(stage.stage == "download", "stage slug is read", ok);
    expect(stage.text == "downloading...", "stage text is read", ok);
    expect(stage.t == 1.5, "stage t is read", ok);
    expect(stage.fraction == 0.42, "stage fraction is read when present", ok);

    return ok;
}

inline bool testStageEventWithoutFraction()
{
    bool ok = true;

    const auto parsed = parse(R"({"event":"stage","id":"req-1","stage":"load-model","text":"loading kong model","t":0.1})");

    SidecarStage stage;
    const auto kind = SidecarClient::classifyLine(parsed, stage);

    expect(kind == SidecarClient::LineKind::Stage, "a stage event without fraction still classifies as Stage", ok);
    expect(stage.stage == "load-model", "stage slug is read", ok);
    expect(stage.text == "loading kong model", "stage text is read", ok);
    expect(stage.fraction == -1.0, "fraction defaults to the struct's own -1.0 sentinel when absent", ok);

    return ok;
}

inline bool testPlainResponse()
{
    bool ok = true;

    const auto parsed = parse(R"({"id":"req-1","ok":true,"engine":"kong","elapsed_s":1.2,"notes":[],"pedal":[],"warnings":[]})");

    SidecarStage stage;
    const auto kind = SidecarClient::classifyLine(parsed, stage);

    expect(kind == SidecarClient::LineKind::Other,
          "a plain response (no \"event\" field) classifies as Other, not Stage", ok);

    // start()'s "ready" line is the other real-world shape of "Other": a well-formed object with
    // an "event" field that isn't "stage". Checked here too so that path is not mistaken for one.
    const auto ready = parse(R"({"event":"ready","protocol":2,"engines":["kong"],"device":"cpu"})");

    SidecarStage ready_stage;
    const auto ready_kind = SidecarClient::classifyLine(ready, ready_stage);

    expect(ready_kind == SidecarClient::LineKind::Other, "the \"ready\" event classifies as Other, not Stage", ok);

    return ok;
}

inline bool testMalformedLine()
{
    bool ok = true;

    // Not valid JSON at all: JSON::parse leaves parsed void.
    {
        juce::var parsed;
        juce::JSON::parse("not json at all {{{", parsed);

        SidecarStage stage;
        const auto kind = SidecarClient::classifyLine(parsed, stage);
        expect(kind == SidecarClient::LineKind::Other, "unparseable JSON classifies as Other", ok);
    }

    // Valid JSON, but not an object -- a bare array, which the protocol never sends on its own.
    {
        const auto parsed = parse("[1, 2, 3]");

        SidecarStage stage;
        const auto kind = SidecarClient::classifyLine(parsed, stage);
        expect(kind == SidecarClient::LineKind::Other, "a well-formed non-object line classifies as Other", ok);
    }

    return ok;
}
} // namespace sidecar_client_test_detail

inline bool sidecar_client_test()
{
    bool ok = true;

    ok &= sidecar_client_test_detail::testStageEventWithFraction();
    ok &= sidecar_client_test_detail::testStageEventWithoutFraction();
    ok &= sidecar_client_test_detail::testPlainResponse();
    ok &= sidecar_client_test_detail::testMalformedLine();

    return ok;
}

#endif // SIDECAR_CLIENT_TEST_H
