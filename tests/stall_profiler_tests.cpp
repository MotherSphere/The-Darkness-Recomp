#include "runtime/native/stall_profiler.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <string>
#include <thread>

using namespace DarkRecomp::Native::StallProfiler;
static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
}
static std::string findLine(const std::string& log, const char* first, const char* second) {
    size_t begin = 0;
    while (begin < log.size()) {
        const auto end = log.find('\n', begin);
        const auto line = log.substr(begin, end == std::string::npos ? end : end - begin);
        if (line.find(first) != std::string::npos && line.find(second) != std::string::npos) return line;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}
static double number(const std::string& line, const char* field) {
    const auto at = line.find(field);
    if (at == std::string::npos) return -1;
    return std::strtod(line.c_str() + at + std::strlen(field), nullptr);
}
int main(int argc, char** argv) {
    // Exercise independent CRT buffers sharing one redirected OS file, as the
    // launcher does. Small buffers force flush boundaries through long lines.
    static char stdoutBuffer[1024], stderrBuffer[1024];
    std::setvbuf(stdout, stdoutBuffer, _IOFBF, sizeof(stdoutBuffer));
    std::setvbuf(stderr, stderrBuffer, _IOFBF, sizeof(stderrBuffer));
    const bool compiledOut = argc > 1 && !std::strcmp(argv[1], "--compiled-out");
    const bool disabled = argc > 1 && !std::strcmp(argv[1], "--disabled");
    SetEnvironmentVariableW(L"DARKRECOMP_STALL_PROFILE", disabled ? L"0" : L"1");
    FILE* capture = nullptr;
    check(tmpfile_s(&capture) == 0 && capture, "create log capture");
    if (!capture) return 1;
    std::fflush(stderr);
    const int original = _dup(_fileno(stderr));
    const int originalStdout = _dup(_fileno(stdout));
    check(original >= 0 && _dup2(_fileno(capture), _fileno(stderr)) == 0, "redirect log");
    check(originalStdout >= 0 && _dup2(_fileno(capture), _fileno(stdout)) == 0, "redirect shared stdout log");
    initializeFromEnvironment();
    check(enabled() == (!disabled && !compiledOut), "environment / compile-time enable switch");
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE pacing = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string liveRenderObject, liveAudioObject;
    check(gate && started && pacing, "create original wait objects");
    if (gate && started && pacing) {
        Scope display(Section::Other, "test_display");
        frameBoundary();
        std::thread guest([&] {
            Scope code(Section::Guest, "test_guest", 0x82001234, 0x82005678);
            Scope wait(Section::Wait, "test_fence_wait", 0, 0, reinterpret_cast<uint64_t>(gate), "test_fence");
            SetEvent(started);
            const auto result = WaitForSingleObject(gate, INFINITE);
            // Scope observation must leave the wait result and object alone.
            check(result == WAIT_OBJECT_0, "original wait result");
        });
        WaitForSingleObject(started, INFINITE);
        {
            Scope render(Section::Rendering, "test_render", 0x82009000, 0x82009100);
            WaitForSingleObject(pacing, 25);
            {
                Scope io(Section::FileIO, "test_io");
                WaitForSingleObject(pacing, 5);
            }
            {
                Scope audio(Section::Audio, "test_audio");
                WaitForSingleObject(pacing, 5);
            }
        }
        { Scope present(Section::Present, "test_present"); WaitForSingleObject(pacing, 5); }
        frameBoundary(); // guest wait still active: it must appear in this frame.
        SetEvent(gate);
        guest.join();
        // A separate thread's frame stream must not reset display intervals.
        std::thread producer([&] {
            Scope code(Section::Guest, "test_producer", 0x8200A000);
            frameBoundary("guest");
            WaitForSingleObject(pacing, 25);
            frameBoundary("guest");
        });
        producer.join();
        frameBoundary();

        // Isolate a fully covered frame on its own thread. Nested sections
        // must partition wall time rather than charging audio/I/O twice to
        // their rendering parent. Compare logged call self time directly so
        // scheduler jitter cannot make this a flaky fixed-duration test.
        std::thread accounting([&] {
            Scope covered(Section::Other, "accounting_covered");
            frameBoundary("accounting");
            {
                Scope render(Section::Rendering, "accounting_render");
                WaitForSingleObject(pacing, 25);
                { Scope io(Section::FileIO, "accounting_io"); WaitForSingleObject(pacing, 5); }
                { Scope audio(Section::Audio, "accounting_audio"); WaitForSingleObject(pacing, 5); }
            }
            { Scope present(Section::Present, "accounting_present"); WaitForSingleObject(pacing, 5); }
            frameBoundary("accounting");
        });
        accounting.join();

        // Keep a worker changing its active leaf and totals while another
        // thread snapshots it. Every successful snapshot must retain a whole
        // metadata tuple even across many nested scope transitions.
        std::atomic<bool> stormReady{false}, stormDone{false};
        uint64_t stormScopes = 0;
        std::thread storm([&] {
            Scope guest(Section::Guest, "snapshot_storm_guest", 0x8200C000, 0x8200C100);
            stormReady.store(true, std::memory_order_release);
            while (!stormDone.load(std::memory_order_acquire)) {
                Scope render(Section::Rendering, "snapshot_storm_render", 0x8200C200, 0x8200C300);
                { Scope audio(Section::Audio, "snapshot_storm_audio", 0x8200C400, 0x8200C500); }
                ++stormScopes;
            }
        });
        while (!stormReady.load(std::memory_order_acquire)) WaitForSingleObject(pacing, 1);
        frameBoundary("snapshot_storm_setup");
        for (unsigned i = 0; i < 12; ++i) {
            WaitForSingleObject(pacing, 25);
            frameBoundary("snapshot_storm");
        }
        stormDone.store(true, std::memory_order_release);
        storm.join();
        check(stormScopes != 0, "concurrent nested scope storm makes progress");

        // Fast transitions rarely leave an active leaf older than the 2ms
        // contributor threshold. Pre-age each reader frame, then hold a live
        // worker leaf for only 4ms and snapshot it before allowing its Scope
        // to complete. A pending contributor must therefore come from the
        // cross-thread snapshot, never a completed-call event.
        const auto liveSnapshot = [&](Section section, const char* function,
                                      const char* stream, uint32_t pc, uint32_t caller,
                                      const char* objectKind, std::string& objectTag) {
            HANDLE start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE release = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(start && ready && release, "create controlled snapshot handshake");
            if (start && ready && release) {
                char tag[128]{};
                std::snprintf(tag, sizeof(tag), "object_kind=%s object=0x%llX ",
                    objectKind, reinterpret_cast<uint64_t>(release));
                objectTag = tag;
                bool workerWaitsOk = false;
                std::thread worker([&] {
                    if (WaitForSingleObject(start, 1000) != WAIT_OBJECT_0) return;
                    Scope guest(Section::Guest, "snapshot_live_guest", 0x8200D000, 0x8200D100);
                    Scope leaf(section, function, pc, caller,
                        reinterpret_cast<uint64_t>(release), objectKind);
                    SetEvent(ready);
                    workerWaitsOk = WaitForSingleObject(release, 1000) == WAIT_OBJECT_0;
                });
                frameBoundary("snapshot_live_setup");
                for (unsigned i = 0; i < 5; ++i) WaitForSingleObject(pacing, 4);
                SetEvent(start);
                const auto readyResult = WaitForSingleObject(ready, 1000);
                check(readyResult == WAIT_OBJECT_0, "controlled live leaf published before snapshot");
                if (readyResult == WAIT_OBJECT_0) {
                    WaitForSingleObject(pacing, 4);
                    frameBoundary(stream);
                }
                SetEvent(release);
                worker.join();
                check(workerWaitsOk, "controlled live leaf completes after reader snapshot");
            }
            if (start) CloseHandle(start);
            if (ready) CloseHandle(ready);
            if (release) CloseHandle(release);
        };
        liveSnapshot(Section::Rendering, "snapshot_live_render", "snapshot_live_render",
            0x8200D200, 0x8200D300, "snapshot_render_event", liveRenderObject);
        liveSnapshot(Section::Audio, "snapshot_live_audio", "snapshot_live_audio",
            0x8200D400, 0x8200D500, "snapshot_audio_event", liveAudioObject);

        // The parent earns its self time in the preceding frame. Only its
        // nested wait occupies the target frame; its full lifetime/self time
        // must not be reused as a contributor to that later hitch.
        std::thread crossFrame([&] {
            Scope parent(Section::Guest, "cross_frame_parent", 0x8200B000, 0x8200B100);
            frameBoundary("cross_frame_setup");
            WaitForSingleObject(pacing, 25);
            {
                Scope child(Section::Wait, "cross_frame_child", 0, 0,
                    reinterpret_cast<uint64_t>(pacing), "cross_frame_event");
                frameBoundary("cross_frame_setup");
                WaitForSingleObject(pacing, 25);
                frameBoundary("cross_frame_target");
            }
        });
        crossFrame.join();

        if (!disabled && !compiledOut) {
            std::atomic<bool> noiseDone{false};
            const auto noise = [&](FILE* stream, const char* tag) {
                const std::string payload(220, 'x');
                unsigned lines = 0;
                while (!noiseDone.load(std::memory_order_relaxed)) {
                    std::fprintf(stream, "[%s] line=%u payload=%s\n", tag, lines++, payload.c_str());
                    if (!(lines % 16)) WaitForSingleObject(pacing, 1);
                }
            };
            std::thread stdoutNoise(noise, stdout, "StdoutNoise");
            std::thread stderrNoise(noise, stderr, "StderrNoise");
            // A legacy diagnostic may leave its prefix unfinished between
            // calls. Profiler records must still begin on their own line.
            std::fprintf(stderr, "[MultipartDiagnostic] prefix");
            for (unsigned i = 0; i < 32; ++i) {
                Scope call(Section::Other, "output_stress_call");
                WaitForSingleObject(pacing, 3);
            }
            // Several large static metadata records exceed one output batch.
            // Formatting must happen outside the shared CRT locks, while the
            // noisy legacy writers keep progressing between complete batches.
            static const std::string largeName = "output_large_record_" + std::string(8192, 'y');
            for (unsigned i = 0; i < 12; ++i) {
                Scope call(Section::Other, largeName.c_str());
                WaitForSingleObject(pacing, 3);
            }
            std::fprintf(stderr, " suffix\n");
            noiseDone.store(true, std::memory_order_relaxed);
            stdoutNoise.join(); stderrNoise.join();
        }
    }
    // Multiple exit paths may request shutdown concurrently. Join all callers
    // before inspecting the captured log, then verify repeated calls are safe.
    std::thread shutdownA([] { shutdown(); });
    std::thread shutdownB([] { shutdown(); });
    std::thread shutdownC([] { shutdown(); });
    shutdownA.join(); shutdownB.join(); shutdownC.join();
    shutdown();
    check(!enabled(), "concurrent and repeated shutdown disables profiling");
    std::fflush(stdout);
    std::fflush(stderr);
    _dup2(original, _fileno(stderr)); _close(original);
    _dup2(originalStdout, _fileno(stdout)); _close(originalStdout);
    std::rewind(capture);
    std::string log;
    char chunk[4096];
    while (const auto bytes = std::fread(chunk, 1, sizeof(chunk), capture)) log.append(chunk, bytes);
    std::fclose(capture);
    if (gate) CloseHandle(gate);
    if (started) CloseHandle(started);
    if (pacing) CloseHandle(pacing);
    if (disabled || compiledOut) check(log.empty(), "disabled profiler emits no log");
    else {
        check(log.find("[STALL] frame=") != std::string::npos, "slow frame marked");
        check(log.find("budget_ms=16.670") != std::string::npos, "60Hz budget");
        check(log.find("[STALL] call") != std::string::npos, "slow runtime call marked");
        check(log.find("[WAIT]") != std::string::npos, "wait logged");
        check(log.find("function=test_fence_wait") != std::string::npos, "function retained");
        check(log.find("guest_pc=0x82001234 caller=0x82005678") != std::string::npos, "inherited guest call metadata");
        check(log.find("object_kind=test_fence object=0x") != std::string::npos, "wait object retained");
        check(log.find("pending=1") != std::string::npos, "in-progress cross-frame wait reported");
        check(log.find("contributors=frame_thread") != std::string::npos, "ranked main contributors");
        check(log.find("contributors=workers") != std::string::npos, "parallel thread accounting");
        check(log.find("stream=guest") != std::string::npos, "independent guest frame stream");
        for (const auto* name : {"guest=", "rendering=", "audio=", "file_io=", "wait=", "present="})
            check(log.find(name) != std::string::npos, "all required sections present");

        const auto accountingFrame = findLine(log, "stream=accounting ", "total_ms=");
        const auto accountingTotals = findLine(log, "stream=accounting ", "contributors=frame_thread");
        check(!accountingFrame.empty() && !accountingTotals.empty(), "isolated accounting frame reported");
        double sectionSum = 0;
        for (const auto* field : {" guest=", " rendering=", " audio=", " file_io=", " wait=", " present=", " other="}) {
            const auto value = number(accountingTotals, field);
            check(value >= 0, "accounting section parsed");
            sectionSum += value;
        }
        check(std::fabs(sectionSum - number(accountingFrame, " total_ms=")) < 0.25,
              "exclusive sections partition the fully covered frame");
        const auto renderCall = findLine(log, "[STALL] call", "function=accounting_render ");
        const auto ioCall = findLine(log, "[STALL] call", "function=accounting_io ");
        const auto audioCall = findLine(log, "[STALL] call", "function=accounting_audio ");
        const auto presentCall = findLine(log, "[STALL] call", "function=accounting_present ");
        check(!renderCall.empty() && !ioCall.empty() && !audioCall.empty() && !presentCall.empty(),
              "nested measured calls reported");
        check(std::fabs(number(accountingTotals, " rendering=") - number(renderCall, " self_ms=")) < 0.25,
              "render section is exclusive of nested audio and file I/O");
        check(std::fabs(number(renderCall, " duration_ms=") - number(renderCall, " self_ms=") -
                        number(ioCall, " duration_ms=") - number(audioCall, " duration_ms=")) < 0.25,
              "inclusive rendering call equals self plus nested calls");
        check(std::fabs(number(accountingTotals, " file_io=") - number(ioCall, " self_ms=")) < 0.25 &&
              std::fabs(number(accountingTotals, " audio=") - number(audioCall, " self_ms=")) < 0.25 &&
              std::fabs(number(accountingTotals, " present=") - number(presentCall, " self_ms=")) < 0.25,
              "nested sections retain their own exclusive wall time");

        check(!findLine(log, "stream=snapshot_storm ", "total_ms=").empty(),
              "concurrent scope storm sampled across slow frames");
        check(!findLine(log, "[STALL] call", "function=snapshot_storm_guest ").empty(),
              "concurrent scope storm retains completed outer call");
        size_t stormBegin = 0;
        while (stormBegin < log.size()) {
            const auto end = log.find('\n', stormBegin);
            const auto line = log.substr(stormBegin, end == std::string::npos ? end : end - stormBegin);
            if (line.find("function=snapshot_storm_guest ") != std::string::npos)
                check(line.find("guest_pc=0x8200C000 caller=0x8200C100") != std::string::npos &&
                      line.find("section=guest ") != std::string::npos, "guest storm snapshot metadata coherent");
            if (line.find("function=snapshot_storm_render ") != std::string::npos)
                check(line.find("guest_pc=0x8200C200 caller=0x8200C300") != std::string::npos &&
                      line.find("section=rendering ") != std::string::npos, "render storm snapshot metadata coherent");
            if (line.find("function=snapshot_storm_audio ") != std::string::npos)
                check(line.find("guest_pc=0x8200C400 caller=0x8200C500") != std::string::npos &&
                      line.find("section=audio ") != std::string::npos, "audio storm snapshot metadata coherent");
            if (end == std::string::npos) break;
            stormBegin = end + 1;
        }

        const auto liveRender = findLine(log, "[STALL] contributor ", "function=snapshot_live_render ");
        const auto liveAudio = findLine(log, "[STALL] contributor ", "function=snapshot_live_audio ");
        check(!liveRender.empty() && !liveAudio.empty(),
              "controlled render and audio reader snapshots both produce contributors");
        check(liveRender.find("guest_pc=0x8200D200 caller=0x8200D300") != std::string::npos &&
              liveRender.find("section=rendering ") != std::string::npos &&
              !liveRenderObject.empty() && liveRender.find(liveRenderObject) != std::string::npos &&
              liveRender.find("pending=1 ") != std::string::npos &&
              liveRender.find("stream=snapshot_live_render contributor_group=workers ") != std::string::npos &&
              number(liveRender, " overlap_ms=") > 2,
              "live render reader snapshot retains the complete pending metadata tuple");
        check(liveAudio.find("guest_pc=0x8200D400 caller=0x8200D500") != std::string::npos &&
              liveAudio.find("section=audio ") != std::string::npos &&
              !liveAudioObject.empty() && liveAudio.find(liveAudioObject) != std::string::npos &&
              liveAudio.find("pending=1 ") != std::string::npos &&
              liveAudio.find("stream=snapshot_live_audio contributor_group=workers ") != std::string::npos &&
              number(liveAudio, " overlap_ms=") > 2,
              "live audio reader snapshot retains the complete pending metadata tuple");

        const auto targetFrame = findLine(log, "stream=cross_frame_target ", "total_ms=");
        const auto targetTotals = findLine(log, "stream=cross_frame_target ", "contributors=frame_thread");
        check(!targetFrame.empty() && !targetTotals.empty(), "cross-frame target reported");
        check(number(targetTotals, " wait=") > 16.67 && number(targetTotals, " guest=") < 2,
              "target frame contains child wait without earlier parent self time");
        const auto frameId = std::to_string(static_cast<unsigned long long>(number(targetFrame, " frame=")));
        const auto targetTag = " frame=" + frameId + " ";
        size_t childContributors = 0, parentContributors = 0, begin = 0;
        while (begin < log.size()) {
            const auto end = log.find('\n', begin);
            const auto line = log.substr(begin, end == std::string::npos ? end : end - begin);
            if (line.find(" contributor ") != std::string::npos && line.find(targetTag) != std::string::npos) {
                if (line.find("function=cross_frame_child ") != std::string::npos) ++childContributors;
                if (line.find("function=cross_frame_parent ") != std::string::npos) ++parentContributors;
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        check(childContributors == 1, "pending and completed child interval contributes once");
        check(parentContributors == 0, "earlier parent self time cannot rank in the later frame");

        size_t stressCalls = 0, largeCalls = 0;
        begin = 0;
        while (begin < log.size()) {
            const auto end = log.find('\n', begin);
            const auto line = log.substr(begin, end == std::string::npos ? end : end - begin);
            const auto stall = line.find("[STALL]");
            const auto wait = line.find("[WAIT]");
            if (stall != std::string::npos || wait != std::string::npos) {
                check(stall == 0 || wait == 0, "profiler record begins on a separate line");
                check(line.find("[StdoutNoise]") == std::string::npos &&
                      line.find("[StderrNoise]") == std::string::npos,
                      "concurrent stdout/stderr cannot split a profiler record");
                if (line.find(" thread=") != std::string::npos && line.find(" function=") != std::string::npos)
                    check(line.find(" pending=") != std::string::npos, "profiler call record retains its final fields");
                if (line.find("[STALL] call ") == 0 && line.find("function=output_stress_call ") != std::string::npos)
                    ++stressCalls;
                if (line.find("[STALL] call ") == 0 && line.find("function=output_large_record_") != std::string::npos)
                    ++largeCalls;
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        check(stressCalls == 32, "all concurrent buffered output stress calls survive intact");
        check(largeCalls == 12, "large records crossing batch boundaries retain all final fields");
        const auto reporterStats = findLine(log, "[StallProfiler] reporter ", "max_batch_bytes=");
        check(number(reporterStats, " batches=") > 1 &&
              number(reporterStats, " max_batch_bytes=") >= 8192 &&
              number(reporterStats, " max_batch_bytes=") <= 32 * 1024,
              "reporter output batches retain their fixed byte bound");
        check(number(reporterStats, " max_batch_flush_ms=") >= 0 &&
              number(reporterStats, " max_stdio_wait_ms=") >= 0 &&
              number(reporterStats, " max_stdio_hold_ms=") >= 0,
              "reporter reports I/O, lock-wait and lock-hold cost without measured-thread logging");
        check(number(reporterStats, " oversized_output_records=") == 0,
              "large profiler records survive without oversized-record loss");
        if (failures) std::printf("Captured log:\n%s", log.c_str());
    }
    std::printf("Stall profiler contract: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
