#pragma once
/**
 * @file
 * @brief walb diff virtual full image scanner.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cstdio>
#include <vector>
#include <string>
#include <memory>
#include "cybozu/option.hpp"
#include "memory_buffer.hpp"
#include "fileio.hpp"
#include "walb_diff_base.hpp"
#include "walb_diff_file.hpp"
#include "walb_diff_mem.hpp"
#include "walb_diff_merge.hpp"

namespace walb {
namespace diff {

/**
 * Virtual full image scanner.
 *
 * (1) Call readAndWriteTo() to write all the data to a file descriptor.
 * (2) Call read() multiple times for various purposes.
 */
class VirtualFullScanner
{
private:
    cybozu::util::FdReader reader_;
    const bool isInputFdSeekable_;
    std::shared_ptr<char> bufForSkip_;
    walb::diff::Merger merger_;
    uint64_t addr_; /* Indicator of previous read amount [logical block]. */
    walb::diff::RecIo recIo_; /* current diff rec IO. */
    uint16_t offInIo_; /* offset in the IO [logical block]. */
    bool isEndDiff_; /* true if there is no more wdiff IO. */
    const bool emptyWdiff_;

public:
    /**
     * @inputFd a base image file descriptor.
     *   stdin (non-seekable) or a raw image file or a block device.
     * @wdiffPaths walb diff files. Each wdiff is sorted by time.
     */
    VirtualFullScanner(int inputFd, const std::vector<std::string> &wdiffPaths)
        : reader_(inputFd)
        , isInputFdSeekable_(reader_.seekable())
        , bufForSkip_(allocateBufForSkipStatic(isInputFdSeekable_))
        , merger_()
        , addr_(0)
        , recIo_()
        , offInIo_(0)
        , isEndDiff_(false)
        , emptyWdiff_(wdiffPaths.empty()) {
        if (!emptyWdiff_) {
            merger_.addWdiffs(wdiffPaths);
            merger_.prepare();
        }
    }
    /**
     * Write all data to a specified fd.
     *
     * @outputFd output file descriptor.
     * @bufSize buffer size [byte].
     */
    void readAndWriteTo(int outputFd, size_t bufSize) {
        cybozu::util::FdWriter writer(outputFd);
        std::shared_ptr<char> buf =
            cybozu::util::allocateBlocks<char>(LOGICAL_BLOCK_SIZE, bufSize);
        size_t rSize;
        while (0 < (rSize = readSome(buf.get(), bufSize))) {
            writer.write(buf.get(), rSize);
        }
        writer.fdatasync();
    }
    /**
     * Read a specified bytes.
     * @data buffer to be filled.
     * @size size trying to read [byte].
     *   This must be multiples of LOGICAL_BLOCK_SIZE.
     *
     * RETURN:
     *   Read size really [byte].
     *   0 means that the input reached the end.
     */
    size_t readSome(void *data, size_t size) {
        assert(size % LOGICAL_BLOCK_SIZE == 0);
        /* Read up to 65535 blocks at once. */
        uint16_t blks = uint16_t(-1);
        if (size / LOGICAL_BLOCK_SIZE < blks) {
            blks = size / LOGICAL_BLOCK_SIZE;
        }

        fillDiffIo();
        if (emptyWdiff_ || isEndDiff_) {
            /* There is no remaining diff IOs. */
            return readBase(data, blks);
        }

        uint64_t diffAddr = currentDiffAddr();
        assert(addr_ <= diffAddr);
        if (addr_ == diffAddr) {
            /* Read wdiff IO partially. */
            uint16_t blks0 = std::min(blks, currentDiffBlocks());
            return readWdiff(data, blks0);
        }
        /* Read the base image. */
        uint16_t blks0 = blks;
        uint64_t blksToIo = diffAddr - addr_;
        if (blksToIo < blks) {
            blks0 = uint16_t(blksToIo);
        }
        return readBase(data, blks0);
    }
    /**
     * Try to read sized value.
     *
     * @data buffer to be filled.
     * @size size to read [byte].
     */
    void read(void *data, size_t size) {
        char *p = (char *)data;
        while (0 < size) {
            size_t r = readSome(p, size);
            if (r == 0) throw cybozu::util::EofError();
            p += r;
            size -= r;
        }
    }
private:
    /**
     * Read from the base full image.
     * @data buffer.
     * @blks [logical block].
     * RETURN:
     *   really read size [byte].
     */
    size_t readBase(void *data, size_t blks) {
        char *p = (char *)data;
        size_t size = blks * LOGICAL_BLOCK_SIZE;
        while (0 < size) {
            size_t r = reader_.readsome(p, size);
            if (r == 0) break;
            p += r;
            size -= r;
        }
        if (size % LOGICAL_BLOCK_SIZE != 0) {
            throw std::runtime_error(
                "input data is not a multiples of LOGICAL_BLOCK_SIZE.");
        }
        size_t readLb = blks - size / LOGICAL_BLOCK_SIZE;
        addr_ += readLb;
        return readLb * LOGICAL_BLOCK_SIZE;
    }
    /**
     * Read from the current diff IO.
     * @data buffer.
     * @blks [logical block]. This must be <= remaining size.
     * RETURN:
     *   really read size [byte].
     */
    size_t readWdiff(void *data, size_t blks) {
        assert(recIo_.isValid());
        const DiffRecord& rec = recIo_.record();
        const walb::diff::IoData &io = recIo_.io();
        assert(offInIo_ < rec.io_blocks);
        if (rec.isNormal()) {
            assert(!io.isCompressed());
            size_t off = offInIo_ * LOGICAL_BLOCK_SIZE;
            ::memcpy(data, io.data.data() + off, blks * LOGICAL_BLOCK_SIZE);
        } else {
            /* Read zero image for both ALL_ZERO and DISCARD.. */
            assert(rec.isDiscard() || rec.isAllZero());
            ::memset(data, 0, blks * LOGICAL_BLOCK_SIZE);
        }
        offInIo_ += blks;
        assert(offInIo_ <= rec.io_blocks);
        skipBase(blks);
        addr_ += blks;
        return blks * LOGICAL_BLOCK_SIZE;
    }
    /**
     * Skip to read the base image.
     */
    void skipBase(size_t blks) {
        if (isInputFdSeekable_) {
            reader_.lseek(blks * LOGICAL_BLOCK_SIZE, SEEK_CUR);
        } else {
            for (size_t i = 0; i < blks; i++) {
                assert(bufForSkip_);
                reader_.read(bufForSkip_.get(), LOGICAL_BLOCK_SIZE);
            }
        }
    }
    /**
     * Set recIo_ approximately.
     */
    void fillDiffIo() {
        if (emptyWdiff_ || isEndDiff_) return;
        const DiffRecord& rec = recIo_.record();
        /* At beginning time, rec.ioBlocks() returns 0. */
        assert(offInIo_ <= rec.io_blocks);
        if (offInIo_ == rec.io_blocks) {
            offInIo_ = 0;
            if (!merger_.pop(recIo_)) {
                isEndDiff_ = true;
                recIo_ = walb::diff::RecIo();
            }
        }
    }
    uint64_t currentDiffAddr() const {
        return recIo_.record().io_address + offInIo_;
    }
    uint16_t currentDiffBlocks() const {
        assert(offInIo_ <= recIo_.record().io_blocks);
        return recIo_.record().io_blocks - offInIo_;
    }

    static std::shared_ptr<char> allocateBufForSkipStatic(bool isInputFdSeekable) {
        if (isInputFdSeekable) {
            return nullptr;
        } else {
            return cybozu::util::allocateBlocks<char>(
                LOGICAL_BLOCK_SIZE, LOGICAL_BLOCK_SIZE);
        }
    }
};

}} //namespace walb::diff
