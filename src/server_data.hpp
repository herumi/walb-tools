/**
 * @file
 * @brief Server data.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <map>
#include <memory>
#include <sstream>
#include "cybozu/file.hpp"
#include "cybozu/atoi.hpp"
#include "lvm.hpp"
#include "file_path.hpp"
#include "wdiff_data.hpp"
#include "meta.hpp"

#ifndef WALB_TOOLS_SERVER_DATA_HPP
#define WALB_TOOLS_SERVER_DATA_HPP

namespace walb {

const std::string VG_NAME = "vg";
const std::string VOLUME_PREFIX = "i_";
const std::string RESTORE_PREFIX = "r_";

/**
 * Data manager for a volume in a server.
 * This is not thread-safe.
 */
class ServerData
{
private:
    const cybozu::FilePath baseDir_;
    const std::string vgName_;
    const std::string name_;
    std::shared_ptr<WalbDiffFiles> wdiffsP_;
    MetaSnap baseRecord_; /* snapshot information of the volume. */

public:
    /**
     * @baseDirStr base directory path string.
     * @name volume identifier.
     * @vgName volume group name.
     */
    ServerData(const std::string &baseDirStr, const std::string &name,
               const std::string &vgName = VG_NAME)
        : baseDir_(baseDirStr)
        , vgName_(vgName)
        , name_(name)
        , wdiffsP_()
        , baseRecord_() {
        if (!baseDir_.stat().isDirectory()) {
            throw std::runtime_error("Directory not found: " + baseDir_.str());
        }
        wdiffsP_ = std::make_shared<WalbDiffFiles>(getDir().str(), true);

        if (!cybozu::lvm::exists(vgName_, VOLUME_PREFIX + name_)) {
            /* There does not exist the logical volume. */
            /* TODO. */
        }

        /* initialize base record. */
        if (!loadBaseRecord()) {
            /* TODO: gid can be specified. */
            reset(0);
        }
    }
    /**
     * CAUSION:
     *   All data inside the directory will be removed.
     *   The volume will be removed if exists.
     */
    void reset(uint64_t gid) {
        baseRecord_.init();
        baseRecord_.raw().gid0 = gid;
        baseRecord_.raw().gid1 = gid;
        saveBaseRecord();

        wdiffsP_->reset(gid);

        /* TODO: remove the volume. */
    }
    bool initialized() const {
        /* now editing */
        return false;
    }
    /**
     * Get volume data.
     */
    cybozu::lvm::Lv volume() const {
        std::string lvName = VOLUME_PREFIX + name_;
        std::vector<cybozu::lvm::Lv> v =
            cybozu::lvm::findLv(vgName_, lvName);
        if (v.empty()) {
            throw std::runtime_error(
                "Volume does not exist: " + vgName_ + "/" + lvName);
        }
        return v[0];
    }
    const WalbDiffFiles &diffs() const {
        assert(wdiffsP_);
        return *wdiffsP_;
    }
    /**
     * Get restored snapshots.
     */
    std::map<uint64_t, cybozu::lvm::Lv> restores() const {
        std::map<uint64_t, cybozu::lvm::Lv> map;
        std::string prefix = RESTORE_PREFIX + name_ + "_";
        for (cybozu::lvm::Lv &lv : volume().snapshotList()) {
            if (cybozu::util::hasPrefix(lv.snapName(), prefix)) {
                std::string gidStr
                    = cybozu::util::removePrefix(lv.snapName(), prefix);
                uint64_t gid = cybozu::atoi(gidStr);
                map.insert(std::make_pair(gid, lv));
            }
        }
        return std::move(map);
    }
    template <typename OutputStream>
    void print(OutputStream &os) const {

        MetaSnap oldest = baseRecord_;
        MetaSnap latest = wdiffsP_->latest();
        std::string oldestState = oldest.isDirty() ? "dirty" : "clean";
        std::string latestState = latest.isDirty() ? "dirty" : "clean";
        /* now editing */

        os << "vg: " << vgName_ << std::endl;
        os << "name: " << name_ << std::endl;
        os << "sizeLb: " << volume().sizeLb() << std::endl;
        os << "oldest: (" << oldest.gid0() << ", " << oldest.gid1() << ") "
           << oldestState << std::endl;
        os << "latest: (" << latest.gid0() << ", " << latest.gid1() << ") "
           << latestState << std::endl;

        os << "----------restored snapshots----------" << std::endl;
        for (auto &pair : restores()) {
            cybozu::lvm::Lv &lv = pair.second;
            lv.print(os);
        }

        os << "----------diff files----------" << std::endl;
        std::vector<std::string> v = wdiffsP_->listName();
        for (std::string &fileName : v) {
            os << fileName << std::endl;
        }
        os << "----------end----------" << std::endl;
    }
    void print(::FILE *fp) const {
        std::stringstream ss;
        print(ss);
        std::string s(ss.str());
        if (::fwrite(&s[0], 1, s.size(), fp) < s.size()) {
            throw std::runtime_error("fwrite failed.");
        }
        ::fflush(fp);
    }
    void print() const { print(::stdout); }
    /**
     * Create a restore volume as a snapshot.
     * @gid restored snapshot will indicates the gid.
     */
    bool restore(uint64_t gid = uint64_t(-1)) {
        if (gid == uint64_t(-1)) {
            gid = getLatestCleanSnapshot();
        } else {
            if (canRestore(gid)) {

            }
        }
        std::string suffix
            = cybozu::util::formatString("_%" PRIu64 "", gid);
        std::string snapName = RESTORE_PREFIX + name_ + suffix;
        if (volume().hasSnapshot(snapName)) return false;
        cybozu::lvm::Lv snap = volume().takeSnapshot(snapName);
        return snap.exists();
    }
    /**
     * Whether a specified gid can be restored.
     * That means wdiffs has the clean snaphsot for the gid.
     */
    bool canRestore(uint64_t gid) const {
        for (const MetaDiff &diff : wdiffsP_->listDiff()) {
            if (diff.gid1() == gid && !diff.isDirty()) {
                return true;
            }
        }
        return false;
    }
    /**
     * Get the latest clean snapshot.
     *
     * RETURN:
     *   gid that means the latest clean snapshot.
     */
    uint64_t getLatestCleanSnapshot() const {
        /* now editing */
        return uint64_t(-1);
    }
    /**
     * Drop a restored snapshot.
     */
    bool drop(const std::string &name) {
        std::string snapName = RESTORE_PREFIX + name;
        if (!volume().hasSnapshot(snapName)) {
            return false;
        }
        volume().getSnapshot(snapName).remove();
        return true;
    }
    /**
     * Apply all diffs before gid into the original lv.
     * Applied diff will be deleted after the application completed successfully.
     */
    void apply(uint64_t gid) {
        throw RT_ERR("%s: not yet implemented.", __func__);
        /* now editing */
    }
    /**
     * You must prepare wdiff file before calling this.
     * Add a wdiff.
     */
    void add(const MetaDiff &diff) {
        throw RT_ERR("%s: not yet implemented.", __func__);
        /* now editing */
    }
    /**
     * Delete dangling diffs.
     */
    void cleanup() {
        wdiffsP_->cleanup();
    }
private:
    cybozu::FilePath getDir() const {
        return baseDir_ + cybozu::FilePath(name_);
    }
    cybozu::FilePath baseRecordPath() const {
        return getDir() + cybozu::FilePath("base");
    }
    bool loadBaseRecord() {
        if (!baseRecordPath().stat().isFile()) return false;
        cybozu::util::FileReader reader(baseRecordPath().str(), O_RDONLY);
        cybozu::load(baseRecord_, reader);
        checkBaseRecord();
        return true;
    }
    void saveBaseRecord() const {
        checkBaseRecord();
        cybozu::TmpFile tmpFile(getDir().str());
        cybozu::save(tmpFile, baseRecord_);
        tmpFile.save(baseRecordPath().str());
    }
    void checkBaseRecord() const {
        if (!baseRecord_.isValid()) {
            throw std::runtime_error("baseRecord is not valid.");
        }
    }
};

} //namespace walb

#endif /* WALB_TOOLS_SERVER_DATA_HPP */
