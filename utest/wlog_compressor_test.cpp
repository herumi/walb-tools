#include <cybozu/test.hpp>
#include <cstdio>
#include <cstring>
#include "random.hpp"
#include "walb_log_compressor.hpp"
#include "thread_util.hpp"
#include "checksum.hpp"

void testCompressedData(std::vector<char> &&v)
{
    walb::log::CompressedData cd0, cd1, cd2;
    size_t s = v.size();
    cd0.moveFrom(0, s, std::move(v));
    cd1 = cd0.compress();
    cd2 = cd1.uncompress();
    CYBOZU_TEST_EQUAL(cd0.rawSize(), cd2.rawSize());
    CYBOZU_TEST_ASSERT(::memcmp(cd0.rawData(), cd2.rawData(), cd0.rawSize()) == 0);
#if 0
    ::printf("orig size %zu compressed size %zu\n", cd0.rawSize(), cd1.rawSize());
#endif
}

CYBOZU_TEST_AUTO(compressedData)
{
    cybozu::util::Random<uint32_t> rand;
    for (size_t i = 0; i < 100; i++) {
        size_t s = rand.get16() + 32;
        std::vector<char> v(s);
        rand.fill(&v[0], 32);
        testCompressedData(std::move(v));
    }
}

void throwErrorIf(std::vector<std::exception_ptr> &&ev)
{
    for (std::exception_ptr &ep : ev) {
        std::rethrow_exception(ep);
    }
    ev.clear();
}

uint32_t calcCsum(const walb::log::CompressedData &data)
{
    return cybozu::util::calcChecksum(data.rawData(), data.rawSize(), 0);
}

CYBOZU_TEST_AUTO(compressor)
{
    using BoundedQ = cybozu::thread::BoundedQueue<walb::log::CompressedData, true>;
    size_t qs = 10;
    BoundedQ q0(qs), q1(qs), q2(qs);

    class Producer : public cybozu::thread::Runnable
    {
    private:
        BoundedQ &outQ_;
        size_t n_;
        std::vector<uint32_t> csumV_;
    public:
        Producer(BoundedQ &outQ, size_t n, std::vector<uint32_t> &csumV)
            : outQ_(outQ), n_(n), csumV_(csumV) {}
        void operator()() noexcept override try {
            cybozu::util::Random<uint32_t> rand;
            for (size_t i = 0; i < n_; i++) {
                size_t s = rand.get16() + 32;
                std::vector<char> v(s);
                rand.fill(&v[0], 32);
                walb::log::CompressedData cd;
                cd.moveFrom(0, s, std::move(v));
                csumV_.push_back(calcCsum(cd));
                outQ_.push(std::move(cd));
            }
            outQ_.sync();
            done();
        } catch (...) {
            throwErrorLater();
            outQ_.error();
        }
    };
    class Consumer : public cybozu::thread::Runnable
    {
    private:
        BoundedQ &inQ_;
        std::vector<uint32_t> csumV_;
    public:
        Consumer(BoundedQ &inQ, std::vector<uint32_t> &csumV)
            : inQ_(inQ), csumV_(csumV) {}
        void operator()() noexcept override try {
            while (!inQ_.isEnd()) {
                walb::log::CompressedData cd = inQ_.pop();
                uint32_t csum = calcCsum(cd);
                csumV_.push_back(csum);
            }
            done();
        } catch (...) {
            throwErrorLater();
            inQ_.error();
        }
    };

    std::vector<uint32_t> csumV0, csumV1;
    auto w0 = std::make_shared<Producer>(q0, 100, csumV0);
    auto w1 = std::make_shared<walb::log::CompressWorker>(q0, q1);
    auto w2 = std::make_shared<walb::log::UncompressWorker>(q1, q2);
    auto w3 = std::make_shared<Consumer>(q2, csumV1);
    cybozu::thread::ThreadRunnerSet thSet;
    thSet.add(w0);
    thSet.add(w1);
    thSet.add(w2);
    thSet.add(w3);
    thSet.start();
    throwErrorIf(thSet.join());

    CYBOZU_TEST_ASSERT(csumV0 == csumV1);
}