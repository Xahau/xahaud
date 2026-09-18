#include <test/unit_test/SuiteJournal.h>

#include <xrpl/beast/unit_test.h>

#include <algorithm>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace beast {

class SuiteLog_test : public unit_test::suite
{
    class CaptureRunner : public unit_test::runner
    {
    public:
        std::vector<std::string> messages;

    private:
        void
        on_log(std::string const& text) override
        {
            // runner::log serializes these callbacks.
            messages.push_back(text);
        }
    };

    class Interleaved : public unit_test::suite
    {
        void
        run() override
        {
            std::promise<void> prefixWritten;
            std::promise<void> mainFlushed;
            auto release = mainFlushed.get_future();
            std::thread worker([&] {
                log << "worker-prefix";
                prefixWritten.set_value();
                release.wait();
                log << "-suffix" << std::endl;
            });
            prefixWritten.get_future().wait();
            log << "main" << std::endl;
            mainFlushed.set_value();
            worker.join();
            pass();
        }
    };

    class Unflushed : public unit_test::suite
    {
        void
        run() override
        {
            std::thread worker([&] { log << "worker-tail"; });
            worker.join();
            log << "main-tail";
            pass();
        }
    };

    static constexpr int workers = 4;
    static constexpr int messagesPerWriter = 200;

    static std::string
    message(int writer, int sequence)
    {
        return std::to_string(writer) + ":" + std::to_string(sequence) + ":" +
            std::string(256, static_cast<char>('a' + writer));
    }

    class Concurrent : public unit_test::suite
    {
        void
        run() override
        {
            ripple::test::SuiteJournalSink sink(
                "worker", severities::kInfo, *this);
            std::promise<void> ready;
            auto start = ready.get_future().share();
            std::vector<std::thread> threads;
            for (int writer = 1; writer <= workers; ++writer)
                threads.emplace_back([&, writer] {
                    start.wait();
                    for (int sequence = 0; sequence < messagesPerWriter;
                         ++sequence)
                        sink.writeAlways(
                            severities::kInfo, message(writer, sequence));
                });
            ready.set_value();
            // Direct suite logging does not take SuiteJournalSink's mutex.
            for (int sequence = 0; sequence < messagesPerWriter; ++sequence)
            {
                log << message(0, sequence);
                log.put('!');
                log << std::endl;
            }
            for (auto& thread : threads)
                thread.join();
            pass();
        }
    };

    template <class Probe>
    std::vector<std::string>
    capture()
    {
        CaptureRunner runner;
        auto const failed = runner.run(unit_test::make_suite_info<Probe>(
            "probe", "unit_test", "beast", false, 0));
        BEAST_EXPECT(!failed);
        return std::move(runner.messages);
    }

    void
    run() override
    {
        testcase("one writer's flush preserves another writer's partial line");
        auto const interleaved = capture<Interleaved>();
        BEAST_EXPECT(
            interleaved ==
            std::vector<std::string>({"main\n", "worker-prefix-suffix\n"}));

        testcase("destruction flushes every writer's unfinished text");
        auto tails = capture<Unflushed>();
        std::sort(tails.begin(), tails.end());
        BEAST_EXPECT(
            tails == std::vector<std::string>({"main-tail", "worker-tail"}));

        // Keep the old implementation's counterfactual deterministic: once
        // isolation fails, do not proceed into a known stringbuf data race.
        if (interleaved !=
            std::vector<std::string>({"main\n", "worker-prefix-suffix\n"}))
            return;

        testcase("journal threads and direct suite writes retain all messages");
        auto actual = capture<Concurrent>();
        std::vector<std::string> expected;
        for (int writer = 0; writer <= workers; ++writer)
            for (int sequence = 0; sequence < messagesPerWriter; ++sequence)
                expected.push_back(
                    (writer == 0 ? "" : "INF:worker ") +
                    message(writer, sequence) + (writer == 0 ? "!\n" : "\n"));
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        BEAST_EXPECT(actual == expected);
    }
};

BEAST_DEFINE_TESTSUITE(SuiteLog, unit_test, beast);

}  // namespace beast
